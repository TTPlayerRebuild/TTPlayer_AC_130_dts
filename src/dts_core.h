#pragma once
#include "common.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}
namespace ttp::ac3dts {
// One DCA implementation shared by the file Reader and the legacy Decoder ABI.
class DtsCore {
  AVCodecContext *context_{};
  SwrContext *converter_{};
  AVChannelLayout source_{}, destination_{};
  int rate_{}, sample_format_{-1};

public:
  DtsCore() = default;
  ~DtsCore() { Close(); }
  DtsCore(const DtsCore &) = delete;
  DtsCore &operator=(const DtsCore &) = delete;
  void Open(const AVCodecParameters *parameters = nullptr,
            bool coreOnly = false);
  void Close() noexcept;
  AVCodecContext *Context() const noexcept { return context_; }
  int Send(const AVPacket *packet) {
    return avcodec_send_packet(context_, packet);
  }
  int Receive(AVFrame *frame) { return avcodec_receive_frame(context_, frame); }
  std::vector<double> Convert(const AVFrame *frame, unsigned channels = 0);
};
void check_dts(int result);
HRESULT make_dts_reader(void **out);
HRESULT make_dts_decoder(void **out);
struct DtsHeader {
  size_t bytes{};
  unsigned rate{}, channels{}, samples{}, bits{};
  bool independent{};
};
DtsHeader dts_header(const BYTE *data, size_t bytes) noexcept;
} // namespace ttp::ac3dts
