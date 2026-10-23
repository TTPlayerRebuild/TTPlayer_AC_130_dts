#include "dts_core.h"
namespace ttp::ac3dts {
namespace {
class DtsDecoder final : public Decoder {
  LONG refs_{1};
  DtsCore core_;
  AVFrame *frame_{av_frame_alloc()};
  AVPacket *packet_{av_packet_alloc()};
  WAVEFORMATEX output_{};
  DWORD input_size_{};
  Bytes input_;
  std::vector<double> pcm_;
  size_t consumed_{};
  bool initialized_{};
  HRESULT failure_{S_OK};
  ~DtsDecoder() {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
  }
  bool ready() {
    check(failure_);
    require(initialized_, E_UNEXPECTED);
    if (consumed_ < pcm_.size())
      return true;
    pcm_.clear();
    consumed_ = 0;
    size_t offset = 0, bytes = 0;
    while (offset + 20 <= input_.size()) {
      const auto header =
          dts_header(input_.data() + offset, input_.size() - offset);
      if (header.bytes) {
        if (header.bytes <= input_.size() - offset)
          bytes = header.bytes;
        break;
      }
      ++offset;
    }
    if (offset)
      input_.erase(input_.begin(), input_.begin() + offset);
    if (!bytes)
      return false;
    require(bytes <= INT_MAX);
    av_packet_unref(packet_);
    check_dts(av_new_packet(packet_, int(bytes)));
    memcpy(packet_->data, input_.data(), bytes);
    auto hr = protect([&]() -> HRESULT {
      check_dts(core_.Send(packet_));
      av_packet_unref(packet_);
      for (;;) {
        const int result = core_.Receive(frame_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
          break;
        check_dts(result);
        require(frame_->sample_rate == int(output_.nSamplesPerSec), E_NOTIMPL);
        auto next = core_.Convert(frame_, output_.nChannels);
        pcm_.insert(pcm_.end(), next.begin(), next.end());
        av_frame_unref(frame_);
        require(pcm_.size() <= 16 * 1024 * 1024);
      }
      require(!pcm_.empty());
      return S_OK;
    });
    input_.erase(input_.begin(), input_.begin() + bytes);
    failure_ = hr;
    check(hr);
    return true;
  }

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    if (!same(id, IID_IUnknown) && !same(id, iid_decoder))
      return E_NOINTERFACE;
    *p = static_cast<Decoder *>(this);
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
  HRESULT STDMETHODCALLTYPE Initialize(const WAVEFORMATEX *in,
                                       WAVEFORMATEX *out) override {
    return protect([&]() -> HRESULT {
      require(in && out, E_POINTER);
      require(frame_ && packet_, E_OUTOFMEMORY);
      GUID subtype{
          in->wFormatTag, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
      if (in->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        require(in->cbSize >= 22, E_INVALIDARG);
        subtype = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(in)->SubFormat;
      }
      require(same(subtype, dts_wave) || same(subtype, dts_subtype),
              E_INVALIDARG);
      require(in->nSamplesPerSec > 0 && in->nSamplesPerSec <= 768000,
              E_INVALIDARG);
      const unsigned channels = out->nChannels ? out->nChannels : in->nChannels;
      require(channels > 0 && channels <= 32, E_INVALIDARG);
      core_.Open(nullptr, true);
      input_.clear();
      pcm_.clear();
      consumed_ = 0;
      failure_ = S_OK;
      input_size_ = std::max<DWORD>(2048, in->nBlockAlign);
      output_ = {3,
                 WORD(channels),
                 in->nSamplesPerSec,
                 in->nSamplesPerSec * channels * 8,
                 WORD(channels * 8),
                 64,
                 0};
      *out = output_;
      initialized_ = true;
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE BufferSizes(DWORD *in, DWORD *out) override {
    if (!in || !out)
      return E_POINTER;
    *in = *out = 0;
    if (!initialized_)
      return E_UNEXPECTED;
    *in = input_size_;
    *out = 1536 * output_.nBlockAlign;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE NeedsInput() override {
    return protect([&] { return ready() ? S_FALSE : S_OK; });
  }
  HRESULT STDMETHODCALLTYPE OutputAvailable() override {
    return protect([&] { return ready() ? S_OK : S_FALSE; });
  }
  HRESULT STDMETHODCALLTYPE Input(Buffer *b) override {
    return protect([&]() -> HRESULT {
      require(b, E_POINTER);
      require(initialized_, E_UNEXPECTED);
      check(failure_);
      BYTE *bytes{};
      DWORD length{}, capacity{};
      check(b->Data(&bytes, &length));
      check(b->Capacity(&capacity));
      require(length <= capacity && (!length || bytes), E_INVALIDARG);
      require(input_.size() + length <= 16 * 1024 * 1024,
              HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));
      if (length)
        input_.insert(input_.end(), bytes, bytes + length);
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE Output(Buffer *b) override {
    return protect([&]() -> HRESULT {
      require(b, E_POINTER);
      check(b->SetLength(0));
      require(initialized_, E_UNEXPECTED);
      BYTE *bytes{};
      DWORD length{}, capacity{};
      check(b->Data(&bytes, &length));
      check(b->Capacity(&capacity));
      require(bytes && capacity >= output_.nBlockAlign, E_INVALIDARG);
      if (!ready())
        return S_FALSE;
      const size_t samples =
          std::min<size_t>((capacity / output_.nBlockAlign) * output_.nChannels,
                           pcm_.size() - consumed_);
      memcpy(bytes, pcm_.data() + consumed_, samples * sizeof(double));
      consumed_ += samples;
      check(b->SetLength(DWORD(samples * sizeof(double))));
      return S_OK;
    });
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    return protect([&]() -> HRESULT {
      require(initialized_, E_UNEXPECTED);
      core_.Open(nullptr, true);
      input_.clear();
      pcm_.clear();
      consumed_ = 0;
      av_frame_unref(frame_);
      av_packet_unref(packet_);
      failure_ = S_OK;
      return S_OK;
    });
  }
};
} // namespace
HRESULT make_dts_decoder(void **out) {
  if (!out)
    return E_POINTER;
  *out = nullptr;
  return protect([&]() -> HRESULT {
    *out = static_cast<Decoder *>(new DtsDecoder);
    return S_OK;
  });
}
} // namespace ttp::ac3dts
