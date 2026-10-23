#pragma once
#include "common.h"
#include <cstdint>
extern "C" {
#include <a52.h>
}
namespace ttp::ac3dts {
struct ChannelMap { BYTE count; BYTE channel[6]; };
extern const ChannelMap ac3_channels[22];
inline const ChannelMap& channel_map(int flags) {
  unsigned mode=flags&A52_CHANNEL_MASK;
  require(mode<11,HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
  return ac3_channels[mode+((flags&A52_LFE)?11:0)];
}
struct Frame { int bytes{},flags{},rate{},bitrate{},samples{},bits{},channels{}; };
// AC3 remains on the recovered liba52 implementation. DTS lives in DtsCore.
class Core {
  a52_state_t* state_{};
public:
  Core():state_(a52_init(0)) { require(state_!=nullptr,E_OUTOFMEMORY); }
  ~Core(){ if(state_) a52_free(state_); }
  Core(const Core&)=delete; Core& operator=(const Core&)=delete;
  Frame sync(const BYTE* p) {
    Frame f;f.bytes=a52_syncinfo(const_cast<BYTE*>(p),&f.flags,&f.rate,&f.bitrate);
    f.samples=1536;f.bits=24;return f;
  }
  void decode(BYTE* p,int mode,std::vector<double>& out,unsigned channels) {
    double level=1.;int flags=mode;
    require(!a52_frame(state_,p,&flags,&level,0));
    const auto& map=channel_map(flags);
    require(map.count==channels&&level!=0);
    out.resize(size_t(6)*256*channels);auto dest=out.data();
    for(int block=0;block<6;++block){
      require(!a52_block(state_));auto samples=a52_samples(state_);
      for(int i=0;i<256;++i)for(unsigned c=0;c<channels;++c)
        *dest++=samples[map.channel[c]*256+i]/level;
    }
  }
};
} // namespace ttp::ac3dts
