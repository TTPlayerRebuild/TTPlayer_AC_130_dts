#include "core.h"
// Upstream reinitialises global IMDCT tables for every reader/decoder. Serialize
// once so parallel metadata scans cannot race with active decoding.
extern "C" void ttplayer_a52_imdct_init_impl(uint32_t);
extern "C" void a52_imdct_init(uint32_t flags){static volatile LONG state=0;if(InterlockedCompareExchange(&state,1,0)==0){ttplayer_a52_imdct_init_impl(flags);InterlockedExchange(&state,2);}else while(InterlockedCompareExchange(&state,2,2)!=2)Sleep(0);}
namespace ttp::ac3dts {
class SoundDecoder final:public Decoder {
 LONG refs_{1};std::unique_ptr<Core> core_;WAVEFORMATEX format_{};DWORD input_size_{};int mode_{};
 Bytes compressed_;std::vector<double> pcm_;size_t offset_{};
 // Leave an incomplete header buffered, including split 14-bit DTS headers.
 Frame frame(bool discard){if(!core_)return {};size_t p=0;Frame f;while(compressed_.size()-p>=20){f=core_->sync(compressed_.data()+p);if(f.bytes>0&&f.bytes<=32768&&f.rate>0&&f.bitrate>0)break;++p;f={};}if(discard&&p)compressed_.erase(compressed_.begin(),compressed_.begin()+p);return f;}
 bool ready(){if(offset_<pcm_.size())return true;auto f=frame(true);return f.bytes>0&&compressed_.size()>=size_t(f.bytes);}
public:
 SoundDecoder()=default;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**p)override{if(!p)return E_POINTER;*p=nullptr;if(!same(iid,IID_IUnknown)&&!same(iid,iid_decoder))return E_NOINTERFACE;*p=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Initialize(const WAVEFORMATEX*in,WAVEFORMATEX*out)override{return protect([&]{require(in&&out,E_POINTER);GUID subtype{in->wFormatTag,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};if(in->wFormatTag==WAVE_FORMAT_EXTENSIBLE){require(in->cbSize>=22,E_INVALIDARG);subtype=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(in)->SubFormat;}require(same(subtype,ac3_wave)||same(subtype,ac3_subtype),E_INVALIDARG);require(in->nSamplesPerSec>0&&in->nSamplesPerSec<=384000,E_INVALIDARG);unsigned channels=out->nChannels?out->nChannels:in->nChannels;if(channels<1||channels>6)channels=2;const int ac3[]{1,2,4,6,7,0x17};mode_=ac3[channels-1]|A52_ADJUST_LEVEL;core_=std::make_unique<Core>();format_={3,WORD(channels),in->nSamplesPerSec,in->nSamplesPerSec*channels*8,WORD(channels*8),64,0};*out=format_;input_size_=std::max<DWORD>(2048,in->nBlockAlign);compressed_.clear();pcm_.clear();offset_=0;return S_OK;});}
 HRESULT STDMETHODCALLTYPE BufferSizes(DWORD*in,DWORD*out)override{if(!in||!out)return E_POINTER;*in=*out=0;if(!core_)return E_UNEXPECTED;*in=input_size_;*out=1536*format_.nBlockAlign;return S_OK;}
 HRESULT STDMETHODCALLTYPE NeedsInput()override{return protect([&]{require(core_!=nullptr,E_UNEXPECTED);return ready()?S_FALSE:S_OK;});}
 HRESULT STDMETHODCALLTYPE OutputAvailable()override{return protect([&]{require(core_!=nullptr,E_UNEXPECTED);return ready()?S_OK:S_FALSE;});}
 HRESULT STDMETHODCALLTYPE Input(Buffer*b)override{return protect([&]{require(core_!=nullptr,E_UNEXPECTED);require(b,E_POINTER);BYTE*p{};DWORD n{},cap{};check(b->Data(&p,&n));check(b->Capacity(&cap));require(n<=cap&&(p||!n),E_INVALIDARG);require(compressed_.size()+n<=16*1024*1024,HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW));if(n)compressed_.insert(compressed_.end(),p,p+n);return S_OK;});}
 HRESULT STDMETHODCALLTYPE Output(Buffer*b)override{return protect([&]()->HRESULT{require(b,E_POINTER);check(b->SetLength(0));require(core_!=nullptr,E_UNEXPECTED);BYTE*p{};DWORD n{},cap{};check(b->Data(&p,&n));check(b->Capacity(&cap));require(p,E_POINTER);require(cap>=format_.nBlockAlign,E_INVALIDARG);if(offset_==pcm_.size()){pcm_.clear();offset_=0;auto f=frame(true);if(f.bytes<=0||compressed_.size()<size_t(f.bytes))return S_FALSE;require(DWORD(f.rate)==format_.nSamplesPerSec,HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));auto hr=protect([&]{core_->decode(compressed_.data(),mode_,pcm_,format_.nChannels);return S_OK;});compressed_.erase(compressed_.begin(),compressed_.begin()+f.bytes);check(hr);}size_t samples=std::min<size_t>((cap/format_.nBlockAlign)*format_.nChannels,pcm_.size()-offset_);memcpy(p,pcm_.data()+offset_,samples*sizeof(double));offset_+=samples;check(b->SetLength(DWORD(samples*sizeof(double))));return samples?S_OK:S_FALSE;});}
 HRESULT STDMETHODCALLTYPE Reset()override{return protect([&]{require(core_!=nullptr,E_UNEXPECTED);core_=std::make_unique<Core>();compressed_.clear();pcm_.clear();offset_=0;return S_OK;});}
};
HRESULT make_decoder(bool dts,void**p){if(dts)return make_dts_decoder(p);if(!p)return E_POINTER;*p=nullptr;return protect([&]{*p=static_cast<Decoder*>(new SoundDecoder);return S_OK;});}
}
