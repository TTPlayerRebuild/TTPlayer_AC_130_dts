#include "dts_core.h"
#include "io.h"
#include "ttpcomm/audio_format_probe_impl.h"
#include "wave_metadata.h"
#include <atomic>
#include <cmath>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
namespace ttp::ac3dts {
namespace {
constexpr GUID pcm_guid{1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
constexpr GUID float_guid{3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
class AudioReader final : public Reader,
                          public Metadata,
                          public Thumbnail,
                          public MetadataCommit,
                          public ttp::format::SourceInfo {
  LONG refs_{1};
  Input input_;
  ComPtr<Metadata> host_tags_;
  ComPtr<Thumbnail> host_art_;
  uint64_t audio_end_{};
  uint64_t wave_payload_{};
  AVFormatContext *container_{};
  AVIOContext *io_{};
  DtsCore core_;
  AVPacket *packet_{av_packet_alloc()};
  AVFrame *frame_{av_frame_alloc()};
  WAVEFORMATEXTENSIBLE wave_{};
  ttp::format::Result source_info_{};
  int stream_index_{-1}, input_rate_{}, input_sample_format_{};
  std::vector<Tag> tags_;
  std::vector<Art> art_;
  Bytes pcm_;
  size_t consumed_{};
  uint64_t position_{}, total_output_{}, decoded_samples_{};
  uint64_t expected_input_{};
  DWORD duration_{}, bitrate_{};
  WORD depth_{};
  HRESULT io_error_{S_OK};
  bool opened_{}, draining_{}, decoder_end_{}, eof_{}, floating_{};
  std::atomic_bool stopped_{};
  std::wstring codec_name_;
  bool eac3_{}, committed_{};
  HRESULT commit_result_{S_OK}, wave_edit_error_{E_NOTIMPL};
  std::unique_ptr<WaveMetadata> wave_edit_;
  struct SeekPoint {
    uint64_t sample;
    int64_t offset;
  };
  std::vector<SeekPoint> index_;
  uint64_t index_interval_{};
  int64_t check_seek_offset_{-1};
  void read_wave_size() {
    // RIFF duration describes the PCM carrier, not decoded DTS samples.
    // Read chunk headers only; never scan the audio body to open a file.
    BYTE header[12]{};
    input_.at(input_.base);
    input_.exact(header, 12);
    if (memcmp(header + 8, "WAVE", 4)) {
      input_.at(input_.base);
      return;
    }
    uint64_t at = input_.base + 12, large_data = 0;
    for (unsigned chunk = 0;
         chunk < 4096 && at <= input_.size && input_.size - at >= 8; ++chunk) {
      input_.at(at);
      BYTE h[8]{};
      input_.exact(h, 8);
      uint64_t bytes = le32(h + 4);
      at += 8;
      if (!memcmp(h, "data", 4)) {
        if (bytes == MAXDWORD)
          bytes = large_data;
        if (bytes <= input_.size - at)
          wave_payload_ = bytes;
        break;
      }
      if (bytes > input_.size - at)
        break;
      if (!memcmp(h, "ds64", 4) && bytes >= 28) {
        BYTE sizes[16]{};
        input_.exact(sizes, 16);
        large_data =
            uint64_t(le32(sizes + 8)) | (uint64_t(le32(sizes + 12)) << 32);
      }
      at += bytes + (bytes & 1);
    }
    input_.at(input_.base);
  }
  static int read_callback(void *opaque, uint8_t *data, int bytes) noexcept {
    auto &s = *static_cast<AudioReader *>(opaque);
    ULONG got{};
    auto hr = protect([&]() -> HRESULT {
      require(bytes > 0, E_INVALIDARG);
      require(!s.stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      DWORD count = DWORD(bytes);
      if (s.audio_end_) {
        const auto pos = tell(s.input_.stream.p);
        count = DWORD(std::min<uint64_t>(
            count, s.audio_end_ > pos ? s.audio_end_ - pos : 0));
      }
      if (count)
        got = s.input_.read(data, count);
      return S_OK;
    });
    if (FAILED(hr)) {
      s.io_error_ = hr;
      return AVERROR(EIO);
    }
    return got ? int(got) : AVERROR_EOF;
  }
  static int64_t seek_callback(void *opaque, int64_t offset,
                               int whence) noexcept {
    auto &s = *static_cast<AudioReader *>(opaque);
    int64_t result = -1;
    if (whence == AVSEEK_SIZE)
      return (s.audio_end_ ? s.audio_end_ : s.input_.size) >= s.input_.base &&
                     (s.audio_end_ || s.input_.size)
                 ? int64_t((s.audio_end_ ? s.audio_end_ : s.input_.size) -
                           s.input_.base)
                 : AVERROR(ENOSYS);
    auto hr = protect([&]() -> HRESULT {
      require(s.input_.seekable, STG_E_INVALIDFUNCTION);
      const int origin = whence & ~AVSEEK_FORCE;
      require(origin >= SEEK_SET && origin <= SEEK_END, E_INVALIDARG);
      LARGE_INTEGER distance{};
      distance.QuadPart = offset;
      int seek_origin = origin;
      if (origin == SEEK_END && s.audio_end_) {
        require(s.audio_end_ <= INT64_MAX && offset <= 0 &&
                offset >= -int64_t(s.audio_end_ - s.input_.base));
        distance.QuadPart = int64_t(s.audio_end_) + offset;
        seek_origin = SEEK_SET;
      }
      if (origin == SEEK_SET) {
        require(offset >= 0 && uint64_t(offset) <= INT64_MAX - s.input_.base);
        distance.QuadPart += s.input_.base;
      }
      ULARGE_INTEGER pos{};
      check(s.input_.stream->Seek(distance, DWORD(seek_origin), &pos));
      require(pos.QuadPart >= s.input_.base && pos.QuadPart <= INT64_MAX);
      result = int64_t(pos.QuadPart - s.input_.base);
      return S_OK;
    });
    if (FAILED(hr)) {
      s.io_error_ = hr;
      return AVERROR(EIO);
    }
    return result;
  }
  void check_av(int result) {
    check(io_error_);
    if (result < 0)
      throw Failure{result == AVERROR(ENOMEM)
                        ? E_OUTOFMEMORY
                        : HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
  }
  void close_container() {
    core_.Close();
    if (packet_)
      av_packet_unref(packet_);
    if (frame_)
      av_frame_unref(frame_);
    avformat_close_input(&container_);
    if (io_) {
      av_freep(&io_->buffer);
      avio_context_free(&io_);
    }
  }
  bool decoded_frame() {
    if (decoder_end_)
      return false;
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
      require(!stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      int result = core_.Receive(frame_);
      if (result == 0) {
        const auto h =
            dts_header(packet_->data, size_t(std::max(0, packet_->size)));
        if (!eac3_ && core_.Context()->profile == AV_PROFILE_DTS &&
            h.independent && h.samples == unsigned(frame_->nb_samples) &&
            packet_->pos >= 0 &&
            (index_.empty() ||
             decoded_samples_ >= index_.back().sample + index_interval_)) {
          if (!index_interval_)
            index_interval_ = frame_->sample_rate;
          if (index_.size() == 65536) {
            for (size_t i = 0; i < index_.size() / 2; ++i)
              index_[i] = index_[i * 2];
            index_.resize(index_.size() / 2);
            index_interval_ *= 2;
          }
          index_.push_back({decoded_samples_, packet_->pos});
        }
        decoded_samples_ += frame_->nb_samples;
        return true;
      }
      if (result == AVERROR_EOF) {
        if (expected_input_)
          require(decoded_samples_ == expected_input_, STG_E_READFAULT);
        decoder_end_ = true;
        return false;
      }
      if (result != AVERROR(EAGAIN))
        check_av(result);
      require(!draining_);
      do {
        av_packet_unref(packet_);
        result = av_read_frame(container_, packet_);
        check(io_error_);
      } while (result >= 0 && packet_->stream_index != stream_index_);
      if (result == AVERROR_EOF) {
        draining_ = true;
        check_av(core_.Send(nullptr));
      } else {
        check_av(result);
        if (check_seek_offset_ >= 0) {
          require(packet_->pos == check_seek_offset_, E_NOTIMPL);
          check_seek_offset_ = -1;
        }
        check_av(core_.Send(packet_));
      }
    }
    throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
  }
  void convert(bool flush = false) {
    consumed_ = 0;
    if (flush) {
      pcm_.clear();
      return;
    }
    require(frame_->sample_rate == int(wave_.Format.nSamplesPerSec) &&
                frame_->ch_layout.nb_channels == wave_.Format.nChannels,
            E_NOTIMPL);
    const auto samples = core_.Convert(frame_);
    pcm_.resize(samples.size() * (floating_ ? 8 : 3));
    for (size_t i = 0; i < samples.size(); ++i) {
      if (floating_)
        memcpy(pcm_.data() + i * 8, &samples[i], 8);
      else {
        const int32_t value = int32_t(std::llround(
            std::clamp(samples[i], -1.0, 8388607.0 / 8388608.0) * 8388608.0));
        for (unsigned b = 0; b < 3; ++b)
          pcm_[i * 3 + b] = BYTE(uint32_t(value) >> (b * 8));
      }
    }
    av_frame_unref(frame_);
  }
  bool next() {
    while (!eof_) {
      if (decoded_frame())
        convert();
      else {
        convert(true);
        if (pcm_.empty()) {
          if (total_output_)
            require(position_ >= total_output_, STG_E_READFAULT);
          eof_ = true;
        }
      }
      if (!pcm_.empty())
        return true;
    }
    return false;
  }
  void identify_source() {
    using namespace ttp::format;
    source_info_ = {};
    source_info_.confidence = Opened;
    const bool wave = std::string(container_->iformat->name) == "wav";
    source_info_.family = wave ? Wave : eac3_ ? Ac3 : Dts;
    Copy(source_info_.container, wave ? "wave" : eac3_ ? "eac3" : "dts");
    Copy(source_info_.codec, eac3_ ? "eac3" : "dts");
    Copy(source_info_.extension, wave ? L".wav" : eac3_ ? L".eac3" : L".dts");
    Copy(source_info_.aliases, wave    ? L".wave;.bwf;.dtswav"
                               : eac3_ ? L".ec3;.ac3"
                                       : L".dtshd");
    Copy(source_info_.display, wave    ? L"DTS-WAV"
                               : eac3_ ? L"Dolby Digital Plus"
                                       : L"DTS");
  }
  void open_container() {
    decoded_samples_ = 0;
    io_error_ = S_OK;
    draining_ = decoder_end_ = eof_ = false;
    pcm_.clear();
    consumed_ = 0;
    auto *buffer = static_cast<unsigned char *>(av_malloc(32768));
    require(buffer, E_OUTOFMEMORY);
    io_ = avio_alloc_context(buffer, 32768, 0, this, read_callback, nullptr,
                             input_.seekable ? seek_callback : nullptr);
    if (!io_) {
      av_free(buffer);
      throw Failure{E_OUTOFMEMORY};
    }
    io_->seekable = input_.seekable ? AVIO_SEEKABLE_NORMAL : 0;
    container_ = avformat_alloc_context();
    require(container_, E_OUTOFMEMORY);
    container_->pb = io_;
    container_->flags |= AVFMT_FLAG_CUSTOM_IO;
    container_->probesize = 1024 * 1024;
    container_->max_analyze_duration = 2 * AV_TIME_BASE;
    check_av(avformat_open_input(&container_, nullptr, nullptr, nullptr));
    check_av(avformat_find_stream_info(container_, nullptr));
    stream_index_ = -1;
    for (unsigned i = 0; i < container_->nb_streams; ++i) {
      const auto *p = container_->streams[i]->codecpar;
      if (p->codec_type == AVMEDIA_TYPE_AUDIO &&
          p->codec_id == (eac3_ ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_DTS)) {
        stream_index_ = int(i);
        break;
      }
    }
    require(stream_index_ >= 0, E_NOTIMPL);
    auto *st = container_->streams[stream_index_];
    const auto *p = st->codecpar;
    core_.Open(p);
    require(decoded_frame());
    input_rate_ = frame_->sample_rate;
    input_sample_format_ = frame_->format;
    require(input_rate_ > 0 && input_rate_ <= 768000 &&
            frame_->ch_layout.nb_channels > 0 &&
            frame_->ch_layout.nb_channels <= 32);
    require(frame_->ch_layout.order == AV_CHANNEL_ORDER_NATIVE &&
                frame_->ch_layout.u.mask <= MAXDWORD,
            E_NOTIMPL);
    const DWORD rate = DWORD(input_rate_);
    wave_ = {};
    auto &f = wave_.Format;
    f.nChannels = WORD(frame_->ch_layout.nb_channels);
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = floating_ ? 64 : 24;
    f.nBlockAlign = WORD(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    f.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.cbSize = 22;
    wave_.Samples.wValidBitsPerSample = f.wBitsPerSample;
    wave_.dwChannelMask = DWORD(frame_->ch_layout.u.mask);
    wave_.SubFormat = floating_ ? float_guid : pcm_guid;
    depth_ = WORD(p->bits_per_raw_sample ? p->bits_per_raw_sample : 24);
    bitrate_ = dword(uint64_t(std::max<int64_t>(0, p->bit_rate)));
    int64_t duration =
        st->duration != AV_NOPTS_VALUE
            ? av_rescale_q(st->duration, st->time_base, {1, 1000})
            : container_->duration / 1000;
    duration_ = duration > 0 ? dword(uint64_t(duration)) : 0;
    // FFmpeg's raw DTS duration estimate uses the unpacked core bitrate even
    // for 14-bit transport. When the file is an exact number of equal-sized
    // core packets, compute duration from samples, not that inflated estimate.
    const auto header =
        dts_header(packet_->data, size_t(std::max(0, packet_->size)));
    if (header.bytes && core_.Context()->profile == AV_PROFILE_DTS)
      depth_ = WORD(header.bits);
    const auto end = audio_end_ ? audio_end_ : input_.size;
    const uint64_t bytes = wave_payload_
                               ? wave_payload_
                               : (end >= input_.base ? end - input_.base : 0);
    if (input_.seekable &&
        (wave_payload_ || std::string(container_->iformat->name) == "dts") &&
        core_.Context()->profile == AV_PROFILE_DTS && header.bytes &&
        packet_->size >= int(header.bytes) &&
        packet_->size <= int(header.bytes + 2) && bytes &&
        bytes % unsigned(packet_->size) == 0) {
      const uint64_t samples = bytes / unsigned(packet_->size) * header.samples;
      duration_ = milliseconds(samples, rate);
      bitrate_ = dword(uint64_t(packet_->size) * 8 * rate / header.samples);
    }
    if (!bitrate_ && duration_ && input_.size <= UINT64_MAX / 8000)
      bitrate_ = dword(input_.size * 8000 / duration_);
    codec_name_ =
        eac3_ ? L"E-AC3|Dolby Digital Plus" : L"DTS|Digital Theater Systems";
    identify_source();
    convert();
  }
  void read_metadata() {
    const auto add = [&](AVDictionary *dictionary) {
      AVDictionaryEntry *t = nullptr;
      while ((t = av_dict_get(dictionary, "", t, AV_DICT_IGNORE_SUFFIX)))
        tags_.push_back({metadata_key(t->key), wide(t->value)});
    };
    add(container_->metadata);
    add(container_->streams[stream_index_]->metadata);
    for (unsigned i = 0; i < container_->nb_streams; ++i) {
      const auto *stream = container_->streams[i];
      const auto &p = stream->attached_pic;
      if (!(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) || p.size <= 0 ||
          p.size > 16 * 1024 * 1024 || art_.size() >= 32)
        continue;
      std::wstring mime;
      if (stream->codecpar->codec_id == AV_CODEC_ID_PNG)
        mime = L"image/png";
      else if (stream->codecpar->codec_id == AV_CODEC_ID_MJPEG)
        mime = L"image/jpeg";
      else if (stream->codecpar->codec_id == AV_CODEC_ID_WEBP)
        mime = L"image/webp";
      else
        continue;
      art_.push_back({Bytes(p.data, p.data + p.size), std::move(mime), {}, {}});
      if (auto *t = av_dict_get(stream->metadata, "title", nullptr, 0))
        art_.back().description = wide(t->value);
    }
    for (auto &p : art_)
      p.view = {sizeof(Picture),      p.mime.c_str(), p.description.c_str(),
                DWORD(p.data.size()), p.data.data(),  3};
  }
  ~AudioReader() {
    if (wave_edit_ && !committed_)
      Commit();
    close_container();
    av_packet_free(&packet_);
    av_frame_free(&frame_);
  }

public:
  explicit AudioReader(bool eac3 = false) : eac3_(eac3) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (same(id, IID_IUnknown) || same(id, iid_reader))
      *out = static_cast<Reader *>(this);
    else if (same(id, iid_metadata))
      *out = static_cast<Metadata *>(this);
    else if (same(id, iid_thumbnail))
      *out = static_cast<Thumbnail *>(this);
    else if (same(id, ttp::format::iid_info))
      *out = static_cast<ttp::format::SourceInfo *>(this);
    else if (same(id, iid_metadata_commit) && wave_edit_ &&
             wave_edit_->Writable())
      *out = static_cast<MetadataCommit *>(this);
    else if (host_tags_)
      return host_tags_->QueryInterface(id, out);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = InterlockedDecrement(&refs_);
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Open(IStream *stream, DWORD flags) override {
    if (!stream)
      return E_POINTER;
    if (input_.stream)
      return E_UNEXPECTED;
    return protect([&]() -> HRESULT {
      require(packet_ && frame_, E_OUTOFMEMORY);
      floating_ = (flags & 2) != 0;
      input_.open(stream);
      if (input_.seekable) {
        BYTE signature[4]{};
        ULONG got = 0;
        while (got < 4) {
          auto n = input_.read(signature + got, 4 - got);
          if (!n)
            break;
          got += n;
        }
        input_.at(input_.base);
        // Preserve the old host tag editor for raw DTS. A WAVE container must
        // not be rewritten as a raw stream with an appended ID3/APE tag.
        const bool wave = got == 4 && (!memcmp(signature, "RIFF", 4) ||
                                       !memcmp(signature, "RF64", 4) ||
                                       !memcmp(signature, "BW64", 4));
        if (wave) {
          read_wave_size();
          if (input_.base == 0 && !input_.remote) {
            wave_edit_error_ = protect([&]() {
              auto edit = std::make_unique<WaveMetadata>();
              edit->Open(stream);
              wave_edit_ = std::move(edit);
              return S_OK;
            });
            input_.at(input_.base);
          }
        }
        if (!wave)
          if (auto host = standard_content()) {
            ULONGLONG length{};
            if (SUCCEEDED(host(stream, 4, host_tags_.put(), &length))) {
              const auto start = tell(stream);
              require(start >= input_.base && length <= INT64_MAX - start);
              if (length) {
                input_.base = start;
                audio_end_ = start + length;
              }
              if (host_tags_)
                host_tags_->QueryInterface(
                    iid_thumbnail, reinterpret_cast<void **>(host_art_.put()));
            } else
              host_tags_.reset();
            input_.at(input_.base);
          }
      }
      open_container();
      read_metadata();
      if (wave_edit_) {
        art_ = wave_edit_->OriginalPictures();
        refresh_art(art_);
      }
      opened_ = true;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE Capabilities(DWORD *out) override {
    if (!out)
      return E_POINTER;
    DWORD maximum{};
    const bool host_cover =
        host_art_ && SUCCEEDED(host_art_->MaximumCount(&maximum)) && maximum;
    *out = input_.capabilities(host_cover || bool(wave_edit_));
    if (!host_tags_ && !wave_edit_)
      *out &= ~(4u | 16u);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Duration(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = duration_;
    return opened_ ? S_OK : E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (!opened_)
      return E_UNEXPECTED;
    auto *p = static_cast<WAVEFORMATEX *>(CoTaskMemAlloc(sizeof(wave_)));
    if (!p)
      return E_OUTOFMEMORY;
    memcpy(p, &wave_, sizeof(wave_));
    *out = p;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE BufferSize(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = (wave_.Format.nSamplesPerSec / 4) * wave_.Format.nBlockAlign;
    return opened_ ? S_OK : E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE CodecName(wchar_t **out) override {
    return text(codec_name_, out);
  }
  HRESULT STDMETHODCALLTYPE Bitrate(DWORD *out) override {
    if (!out)
      return E_POINTER;
    *out = bitrate_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE BitDepth(WORD *out) override {
    if (!out)
      return E_POINTER;
    *out = depth_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetCallback(IUnknown *p) override {
    return input_.stream ? input_.callback(p) : E_UNEXPECTED;
  }
  HRESULT STDMETHODCALLTYPE Start() override {
    stopped_ = false;
    ResetEvent(input_.event);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stop() override {
    stopped_ = true;
    SetEvent(input_.event);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Read(Buffer *buffer) override {
    if (!buffer)
      return E_POINTER;
    return protect([&]() -> HRESULT {
      check(buffer->SetLength(0));
      require(opened_, E_UNEXPECTED);
      require(!stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      DWORD capacity{}, unused{};
      BYTE *out{};
      check(buffer->Capacity(&capacity));
      check(buffer->Data(&out, &unused));
      capacity -= capacity % wave_.Format.nBlockAlign;
      require(capacity, E_INVALIDARG);
      require(out, E_POINTER);
      if (consumed_ == pcm_.size() && !next())
        return S_FALSE;
      auto n = DWORD(std::min<size_t>(capacity, pcm_.size() - consumed_));
      memcpy(out, pcm_.data() + consumed_, n);
      consumed_ += n;
      position_ += n / wave_.Format.nBlockAlign;
      return buffer->SetLength(n);
    });
  }
  HRESULT STDMETHODCALLTYPE Seek(DWORD *ms) override {
    if (!ms)
      return E_POINTER;
    return protect([&]() -> HRESULT {
      require(opened_, E_UNEXPECTED);
      require(input_.seekable, STG_E_INVALIDFUNCTION);
      require(!stopped_, HRESULT_FROM_WIN32(ERROR_CANCELLED));
      const uint64_t desired =
          uint64_t(*ms) * wave_.Format.nSamplesPerSec / 1000;
      // Only independent ordinary DTS Core frames are indexed. HD/ES/96-24
      // histories and E-AC3 dither/SPX are not guessed from a byte offset.
      if (desired < position_) {
        const uint64_t warmup = uint64_t(wave_.Format.nSamplesPerSec) * 2;
        const SeekPoint *anchor = nullptr;
        if (desired > warmup)
          for (const auto &point : index_) {
            if (point.sample > desired - warmup)
              break;
            anchor = &point;
          }
        HRESULT fast = E_NOTIMPL;
        if (anchor && anchor->sample)
          fast = protect([&]() {
            const auto point = *anchor;
            check_av(
                av_seek_frame(container_, -1, point.offset, AVSEEK_FLAG_BYTE));
            avformat_flush(container_);
            av_packet_unref(packet_);
            av_frame_unref(frame_);
            core_.Open(container_->streams[stream_index_]->codecpar);
            draining_ = decoder_end_ = eof_ = false;
            io_error_ = S_OK;
            pcm_.clear();
            consumed_ = 0;
            position_ = decoded_samples_ = point.sample;
            check_seek_offset_ = point.offset;
            require(next());
            return S_OK;
          });
        if (FAILED(fast)) {
          check_seek_offset_ = -1;
          close_container();
          input_.at(input_.base);
          position_ = 0;
          open_container();
        }
      }
      while (position_ < desired) {
        if (consumed_ == pcm_.size() && !next())
          break;
        auto n = std::min<uint64_t>(desired - position_,
                                    (pcm_.size() - consumed_) /
                                        wave_.Format.nBlockAlign);
        consumed_ += size_t(n) * wave_.Format.nBlockAlign;
        position_ += n;
      }
      *ms = milliseconds(position_, wave_.Format.nSamplesPerSec);
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE GetFormatInfo(ttp::format::Result *out) override {
    if (!out)
      return E_POINTER;
    if (out->size != sizeof(*out) || out->version != ttp::format::abi_version)
      return E_INVALIDARG;
    *out = {};
    if (!opened_)
      return E_UNEXPECTED;
    *out = source_info_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Count(DWORD *out) override {
    if (host_tags_)
      return host_tags_->Count(out);
    if (!out)
      return E_POINTER;
    *out = DWORD(tags_.size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE At(DWORD n, wchar_t **key,
                               wchar_t **value) override {
    if (host_tags_)
      return host_tags_->At(n, key, value);
    if (!key || !value)
      return E_POINTER;
    *key = *value = nullptr;
    if (n >= tags_.size())
      return E_INVALIDARG;
    return protect([&]() -> HRESULT {
      check(text(wide(tags_[n].key), key));
      auto hr = text(tags_[n].value, value);
      if (FAILED(hr)) {
        CoTaskMemFree(*key);
        *key = nullptr;
      }
      return hr;
    });
  }
  HRESULT STDMETHODCALLTYPE Get(const char *key, wchar_t **out) override {
    if (host_tags_)
      return host_tags_->Get(key, out);
    if (!key || !out)
      return E_POINTER;
    *out = nullptr;
    return protect([&]() -> HRESULT {
      const auto normalized = metadata_key(key);
      for (const auto &t : tags_)
        if (normalized == t.key)
          return text(t.value, out);
      return E_INVALIDARG;
    });
  }
  HRESULT STDMETHODCALLTYPE Set(const char *key,
                                const wchar_t *value) override {
    if (host_tags_)
      return host_tags_->Set(key, value);
    return protect([&]() {
      require(!committed_, E_UNEXPECTED);
      check(wave_edit_error_);
      require(bool(wave_edit_), E_NOTIMPL);
      wave_edit_->Set(key, value);
      if (!key)
        tags_.clear();
      else {
        const auto normalized = metadata_key(key);
        auto it = std::remove_if(tags_.begin(), tags_.end(), [&](const Tag &t) {
          return t.key == normalized;
        });
        tags_.erase(it, tags_.end());
        if (value)
          tags_.push_back({normalized, value});
      }
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE Commit() override {
    if (committed_)
      return commit_result_;
    committed_ = true;
    close_container();
    opened_ = false;
    commit_result_ = protect([&]() {
      require(bool(wave_edit_), E_NOTIMPL);
      wave_edit_->Save(art_);
      return S_OK;
    });
    return commit_result_;
  }
  HRESULT STDMETHODCALLTYPE PictureCount(DWORD *out) override {
    if (host_art_)
      return host_art_->PictureCount(out);
    if (!out)
      return E_POINTER;
    *out = DWORD(art_.size());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD *out) override {
    if (host_art_)
      return host_art_->MaximumBytes(out);
    if (!out)
      return E_POINTER;
    *out = wave_edit_ ? 16 * 1024 * 1024 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MaximumCount(DWORD *out) override {
    if (host_art_)
      return host_art_->MaximumCount(out);
    if (!out)
      return E_POINTER;
    *out = wave_edit_ ? 32 : 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PictureAt(DWORD n, const Picture **out) override {
    if (host_art_)
      return host_art_->PictureAt(n, out);
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (n >= art_.size())
      return E_INVALIDARG;
    *out = &art_[n].view;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD n, const Picture *picture,
                                           DWORD flags) override {
    if (host_art_)
      return host_art_->ReplacePicture(n, picture, flags);
    return protect([&]() {
      require(!committed_, E_UNEXPECTED);
      check(wave_edit_error_);
      require(wave_edit_->Writable(), STG_E_ACCESSDENIED);
      require(n < art_.size() && picture && picture->size >= sizeof(Picture) &&
                  !(flags & ~15u),
              E_INVALIDARG);
      auto a = art_[n];
      if (flags & 1)
        a.mime = picture->mime ? picture->mime : L"";
      if (flags & 2)
        a.description = picture->description ? picture->description : L"";
      if (flags & 4) {
        require(picture->type <= 20, E_INVALIDARG);
        a.view.type = picture->type;
      }
      if (flags & 8) {
        Picture merged{sizeof(Picture), a.mime.c_str(), a.description.c_str(),
                       picture->bytes,  picture->data,  a.view.type};
        a = copy_art(&merged);
      }
      require(!a.mime.empty() && a.mime.size() <= 128 &&
                  a.description.size() <= 65536,
              E_INVALIDARG);
      art_[n] = std::move(a);
      refresh_art(art_);
      wave_edit_->Pictures();
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE AddPicture(const Picture *picture) override {
    if (host_art_)
      return host_art_->AddPicture(picture);
    return protect([&]() {
      require(!committed_, E_UNEXPECTED);
      check(wave_edit_error_);
      require(wave_edit_->Writable(), STG_E_ACCESSDENIED);
      require(art_.size() < 32, E_INVALIDARG);
      auto a = copy_art(picture);
      art_.push_back(std::move(a));
      refresh_art(art_);
      wave_edit_->Pictures();
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE RemovePicture(DWORD n) override {
    if (host_art_)
      return host_art_->RemovePicture(n);
    return protect([&]() {
      require(!committed_, E_UNEXPECTED);
      check(wave_edit_error_);
      require(wave_edit_->Writable(), STG_E_ACCESSDENIED);
      require(n < art_.size(), E_INVALIDARG);
      art_.erase(art_.begin() + n);
      refresh_art(art_);
      wave_edit_->Pictures();
      return S_OK;
    });
  }
};
} // namespace
HRESULT make_dts_reader(void **out) {
  if (!out)
    return E_POINTER;
  *out = nullptr;
  return protect([&]() -> HRESULT {
    *out = static_cast<Reader *>(new AudioReader);
    return S_OK;
  });
}
HRESULT make_eac3_reader(void **out) {
  if (!out)
    return E_POINTER;
  *out = nullptr;
  return protect([&]() {
    *out = static_cast<Reader *>(new AudioReader(true));
    return S_OK;
  });
}
} // namespace ttp::ac3dts
