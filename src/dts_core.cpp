#include "dts_core.h"
#include "ttpcomm/audio_format_probe_impl.h"
#include <cmath>
extern "C" {
#include <libavutil/opt.h>
}
namespace ttp::ac3dts {
void check_dts(int result) {
  if (result < 0)
    throw Failure{result == AVERROR(ENOMEM)
                      ? E_OUTOFMEMORY
                      : HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
}
void DtsCore::Close() noexcept {
  swr_free(&converter_);
  av_channel_layout_uninit(&source_);
  av_channel_layout_uninit(&destination_);
  avcodec_free_context(&context_);
  rate_ = 0;
  sample_format_ = -1;
}
void DtsCore::Open(const AVCodecParameters *parameters, bool coreOnly) {
  Close();
  const AVCodecID id = parameters ? parameters->codec_id : AV_CODEC_ID_DTS;
  require(id == AV_CODEC_ID_DTS || id == AV_CODEC_ID_EAC3, E_NOTIMPL);
  const AVCodec *codec = avcodec_find_decoder(id);
  require(codec, E_NOTIMPL);
  context_ = avcodec_alloc_context3(codec);
  require(context_, E_OUTOFMEMORY);
  if (parameters)
    check_dts(avcodec_parameters_to_context(context_, parameters));
  // The legacy Decoder negotiates its rate/channels before seeing a packet.
  // Preserve the core format there; the file Reader negotiates after decoding
  // the first frame and can therefore expose HD rate/layout safely.
  if (coreOnly)
    check_dts(av_opt_set_int(context_->priv_data, "core_only", 1, 0));
  context_->thread_count = 1;
  context_->err_recognition = AV_EF_CRCCHECK | AV_EF_EXPLODE;
  check_dts(avcodec_open2(context_, codec, nullptr));
}
std::vector<double> DtsCore::Convert(const AVFrame *frame, unsigned channels) {
  require(frame && frame->nb_samples > 0 && frame->nb_samples <= 1048576);
  require(frame->sample_rate > 0 && frame->sample_rate <= 768000);
  require(frame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE &&
              frame->ch_layout.nb_channels > 0 &&
              frame->ch_layout.nb_channels <= 32 &&
              frame->ch_layout.u.mask <= MAXDWORD,
          E_NOTIMPL);
  if (!converter_) {
    rate_ = frame->sample_rate;
    sample_format_ = frame->format;
    check_dts(av_channel_layout_copy(&source_, &frame->ch_layout));
    if (!channels || channels == unsigned(source_.nb_channels))
      check_dts(av_channel_layout_copy(&destination_, &source_));
    else {
      require(channels <= 32, E_INVALIDARG);
      av_channel_layout_default(&destination_, int(channels));
      require(destination_.order == AV_CHANNEL_ORDER_NATIVE, E_NOTIMPL);
    }
    check_dts(swr_alloc_set_opts2(
        &converter_, &destination_, AV_SAMPLE_FMT_DBL, rate_, &source_,
        AVSampleFormat(sample_format_), rate_, 0, nullptr));
    check_dts(swr_init(converter_));
  }
  require(frame->sample_rate == rate_ && frame->format == sample_format_ &&
              !av_channel_layout_compare(&source_, &frame->ch_layout) &&
              (!channels || channels == unsigned(destination_.nb_channels)),
          E_NOTIMPL);
  const int capacity = swr_get_out_samples(converter_, frame->nb_samples);
  check_dts(capacity);
  require(capacity <= 2097152);
  std::vector<double> samples(size_t(capacity) * destination_.nb_channels);
  uint8_t *output = reinterpret_cast<uint8_t *>(samples.data());
  const int count = swr_convert(
      converter_, &output, capacity,
      const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
  check_dts(count);
  // No rate conversion here: a frame must not be delayed or silently dropped.
  require(count == frame->nb_samples, E_UNEXPECTED);
  samples.resize(size_t(count) * destination_.nb_channels);
  // The old libdts QMF divisor is 49152 (LFE has its own unchanged divisor).
  // Preserve that core playback level in both adapters. Lossless/HD profiles
  // keep FFmpeg's normal amplitude; never attenuate lossless PCM to mimic core.
  const bool legacy_core = context_->codec_id == AV_CODEC_ID_DTS &&
                           (context_->profile == AV_PROFILE_DTS ||
                            context_->profile == AV_PROFILE_DTS_ES ||
                            context_->profile == AV_PROFILE_DTS_96_24);
  constexpr double core_level = 0.94280904158206336587;
  for (size_t i = 0; i < samples.size(); ++i) {
    require(std::isfinite(samples[i]));
    const auto channel = av_channel_layout_channel_from_index(
        &destination_, unsigned(i % destination_.nb_channels));
    if (legacy_core && channel != AV_CHAN_LOW_FREQUENCY &&
        channel != AV_CHAN_LOW_FREQUENCY_2)
      samples[i] *= core_level;
  }
  return samples;
}
DtsHeader dts_header(const BYTE *data, size_t size) noexcept {
  using namespace ttp::format;
  unsigned identity{};
  const size_t bytes = DtsFrame({data, size}, 0, identity);
  if (!bytes || size < 20)
    return {};
  const bool packed = data[0] == 0x1f || data[0] == 0xff;
  const bool little = data[0] == 0xfe || data[0] == 0xff;
  BYTE h[1024]{};
  unsigned at = 0;
  for (unsigned i = 0; i + 1 < std::min<size_t>(size, sizeof(h)); i += 2) {
    unsigned word =
        little ? (data[i] | data[i + 1] << 8) : (data[i] << 8 | data[i + 1]);
    for (int bit = packed ? 13 : 15; bit >= 0; --bit, ++at)
      h[at / 8] |= BYTE(((word >> bit) & 1) << (7 - at % 8));
  }
  auto bits = [&](unsigned start, unsigned n) {
    unsigned v = 0;
    while (n--) {
      v = (v << 1) | ((h[start / 8] >> (7 - start % 8)) & 1);
      ++start;
    }
    return v;
  };
  constexpr unsigned rates[]{0,     8000,  16000, 32000, 0, 0,
                             11025, 22050, 44100, 0,     0, 12000,
                             24000, 48000, 96000, 192000};
  constexpr unsigned counts[]{1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
  constexpr unsigned depths[]{16, 16, 20, 20, 0, 24, 24, 0};
  const unsigned depth = depths[bits(95 + (bits(38, 1) ? 16 : 0), 3)];
  // Historical packed streams omit the final partial 14-bit transport word.
  // Decode that minimum length; the scanner skips any optional padding word.
  const size_t transport =
      packed ? (size_t(bits(46, 14) + 1) * 8 / 14) * 2 : bytes;
  bool independent = bits(87, 1) == 0;
  // A stream may set predictor_history even when no subband in the first
  // subframe uses prediction (FFmpeg's encoder does this). Such a subframe
  // overwrites all four ADPCM history samples before any later subframe can
  // use them. Parse only this bounded side-information, not compressed audio.
  if (!independent) {
    unsigned p = 104 + (bits(38, 1) ? 16 : 0);
    bool valid = true;
    auto take = [&](unsigned n) {
      if (p > at || n > at - p) {
        valid = false;
        return 0u;
      }
      auto v = bits(p, n);
      p += n;
      return v;
    };
    take(4);
    const unsigned ch = take(3) + 1;
    unsigned bands[8]{}, sel[10][8]{};
    valid = valid && ch == counts[identity & 255];
    for (unsigned c = 0; c < ch; ++c) {
      bands[c] = take(5) + 2;
      valid = valid && bands[c] <= 32;
    }
    for (unsigned c = 0; c < ch; ++c)
      take(5);
    for (unsigned c = 0; c < ch; ++c)
      take(3);
    for (unsigned c = 0; c < ch; ++c)
      take(2);
    for (unsigned c = 0; c < ch; ++c)
      take(3);
    for (unsigned c = 0; c < ch; ++c)
      take(3);
    constexpr unsigned widths[]{1, 2, 2, 2, 2, 3, 3, 3, 3, 3};
    constexpr unsigned groups[]{1, 3, 3, 3, 3, 7, 7, 7, 7, 7};
    for (unsigned n = 0; n < 10; ++n)
      for (unsigned c = 0; c < ch; ++c)
        sel[n][c] = take(widths[n]);
    for (unsigned n = 0; n < 10; ++n)
      for (unsigned c = 0; c < ch; ++c)
        if (sel[n][c] < groups[n])
          take(2);
    if (bits(38, 1))
      take(16);
    take(5);
    bool predicted = false;
    for (unsigned c = 0; c < ch; ++c)
      for (unsigned b = 0; b < bands[c]; ++b)
        predicted |= take(1) != 0;
    independent = valid && !predicted;
  }
  return {transport,
          rates[identity >> 8],
          counts[identity & 255] + (bits(85, 2) ? 1u : 0u),
          (bits(39, 7) + 1) * 32,
          depth ? depth : 16,
          independent};
}
} // namespace ttp::ac3dts
