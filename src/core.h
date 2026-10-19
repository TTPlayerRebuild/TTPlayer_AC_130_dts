#pragma once
#include "common.h"
#include <cstdint>
extern "C" {
#include <a52.h>
#include <dca.h>
}
namespace ttp::ac3dts {
struct ChannelMap {BYTE count;BYTE channel[6];};
extern const ChannelMap ac3_channels[22],dts_channels[20];
inline const ChannelMap& channel_map(bool dts,int flags) {
 unsigned mode=dts?(flags&DCA_CHANNEL_MASK):(flags&A52_CHANNEL_MASK);
 require(mode<unsigned(dts?10:11),HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
 return dts?dts_channels[mode+((flags&DCA_LFE)?10:0)]:ac3_channels[mode+((flags&A52_LFE)?11:0)];
}
struct Frame {int bytes{},flags{},rate{},bitrate{},samples{},bits{};};
inline int dts_depth(const BYTE*p){
 bool packed=p[0]==0x1f||p[0]==0xff;bool little=p[0]==0xfe||p[0]==0xff;BYTE bits[20]{};unsigned count=0;
 for(unsigned i=0;i<20;i+=2){unsigned word=little?(p[i]|p[i+1]<<8):(p[i]<<8|p[i+1]);for(int bit=(packed?13:15);bit>=0;--bit,++count)bits[count/8]|=BYTE(((word>>bit)&1)<<(7-count%8));}
 unsigned at=95+(((bits[4]>>1)&1)?16:0),v=0;for(int i=0;i<3;++i,++at)v=(v<<1)|((bits[at/8]>>(7-at%8))&1);
 const int depth[]{16,16,20,20,0,24,24,0};return depth[v]?depth[v]:16;
}
class Core {
 bool dts_;
 a52_state_t *a_{};dca_state_t *d_{};
public:
 explicit Core(bool dts):dts_(dts){if(dts_)d_=dca_init(0);else a_=a52_init(0);require(dts_?d_!=nullptr:a_!=nullptr,E_OUTOFMEMORY);}
 ~Core(){if(a_)a52_free(a_);if(d_)dca_free(d_);}
 Core(const Core&)=delete;Core& operator=(const Core&)=delete;
 Frame sync(const BYTE *p) {Frame f;if(dts_){f.bytes=dca_syncinfo(d_,const_cast<BYTE*>(p),&f.flags,&f.rate,&f.bitrate,&f.samples);if(f.bytes>0)f.bits=dts_depth(p);}else{f.bytes=a52_syncinfo(const_cast<BYTE*>(p),&f.flags,&f.rate,&f.bitrate);f.samples=1536;f.bits=24;}return f;}
 void decode(BYTE *p,int mode,std::vector<double>& out,unsigned channels) {
  double level=1.;int flags=mode;
  require(!(dts_?dca_frame(d_,p,&flags,&level,0):a52_frame(a_,p,&flags,&level,0)));
  const auto& map=channel_map(dts_,flags);
  require(map.count==channels&&level!=0);
  const int blocks=dts_?dca_blocks_num(d_):6;require(blocks>0&&blocks<=32);
  out.resize(size_t(blocks)*256*channels);auto dest=out.data();
  for(int block=0;block<blocks;++block){require(!(dts_?dca_block(d_):a52_block(a_)));auto samples=dts_?dca_samples(d_):a52_samples(a_);for(int i=0;i<256;++i)for(unsigned c=0;c<channels;++c)*dest++=samples[map.channel[c]*256+i]/level;}
 }
};
}
