#include "ttpcomm/audio_format_probe_impl.h"
#include "io.h"
#include "core.h"
#include "dts_core.h"
namespace ttp::ac3dts {
class SoundReader final:public Reader,public Metadata, public ttp::format::InfoSupport {
 LONG refs_{1};std::unique_ptr<Input> input_;ComPtr<Metadata> tags_;WAVEFORMATEX format_{};WORD bits_{};
 uint64_t start_{},length_{},position_{};Bytes prefix_;size_t prefix_position_{};bool opened_{},dts_{};
 template<class T>HRESULT property(T*p,T value){if(!p)return E_POINTER;*p={};if(!opened_)return E_UNEXPECTED;*p=value;return S_OK;}
public:
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**p)override{
        if (!p) return E_POINTER;
        if (IsEqualIID(iid, ttp::format::iid_info)) { *p=static_cast<ttp::format::SourceInfo*>(this); AddRef(); return S_OK; }
if(!p)return E_POINTER;*p=nullptr;if(same(iid,IID_IUnknown)||same(iid,iid_reader))*p=static_cast<Reader*>(this);else if(same(iid,iid_metadata))*p=static_cast<Metadata*>(this);else if(tags_)return tags_->QueryInterface(iid,p);else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Open(IStream* probe_stream, DWORD probe_flags) override {
        const auto probed = PrepareFormatInfo(probe_stream, ttp::format::Ac3 | ttp::format::Dts);
        if (FAILED(probed)) return probed;
        return CompleteFormatInfo(OpenImpl(probe_stream, probe_flags));
    }
    HRESULT OpenImpl(IStream*s,DWORD){if(opened_)return E_UNEXPECTED;auto hr=protect([&]{input_=std::make_unique<Input>();input_->open(s);start_=input_->base;length_=input_->size>=start_?input_->size-start_:0;
  if(input_->seekable){if(auto host=standard_content()){ULONGLONG bytes{};if(SUCCEEDED(host(s,4,tags_.put(),&bytes))){start_=tell(s);length_=bytes;}else{tags_.reset();input_->at(start_);}}}
  prefix_.resize(32768);size_t got=0;while(got<prefix_.size()){auto n=input_->read(prefix_.data()+got,DWORD(prefix_.size()-got));got+=n;if(!n)break;}prefix_.resize(got);
  Core ac3;Frame found;size_t skip=0;
  for(;skip+20<=prefix_.size();++skip){auto a=ac3.sync(prefix_.data()+skip);if(a.bytes>0&&a.rate>0&&a.bitrate>0&&size_t(a.bytes)<=prefix_.size()-skip){found=a;dts_=false;break;}auto dh=dts_header(prefix_.data()+skip,prefix_.size()-skip);Frame d{int(dh.bytes),0,int(dh.rate),dh.samples?int(uint64_t(dh.bytes)*8*dh.rate/dh.samples):0,int(dh.samples),int(dh.bits),int(dh.channels)};if(d.bytes>0&&d.rate>0&&d.bitrate>0&&size_t(d.bytes)<=prefix_.size()-skip){found=d;dts_=true;break;}}
  require(found.bytes>0);auto channels=dts_?found.channels:channel_map(found.flags).count;format_={WORD(dts_?0x2001:0x2000),WORD(channels),DWORD(found.rate),DWORD(found.bitrate/8),WORD(found.bytes),0,0};bits_=WORD(found.bits);
  start_+=skip;if(length_)length_-=std::min<uint64_t>(skip,length_);
  if(!input_->seekable&&skip)prefix_.erase(prefix_.begin(),prefix_.begin()+skip);
  // Original 60051a78 skips the first synchronised frame's leading bytes.
  // Both reader factories sniff either codec; the host chooses its decoder.
  if(input_->seekable){input_->at(start_);prefix_.clear();}prefix_position_=0;position_=0;opened_=true;return S_OK;});if(FAILED(hr)){tags_.reset();input_.reset();prefix_.clear();}return hr;}
 HRESULT STDMETHODCALLTYPE Capabilities(DWORD*p)override{return property(p,input_?(input_->capabilities(false)&(tags_?~0u:~4u)):0u);}
 HRESULT STDMETHODCALLTYPE Duration(DWORD*p)override{return property(p,format_.nAvgBytesPerSec?dword(length_*1000/format_.nAvgBytesPerSec):0u);}
 HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX**p)override{if(!p)return E_POINTER;*p=nullptr;if(!opened_)return E_UNEXPECTED;*p=static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(format_)));if(!*p)return E_OUTOFMEMORY;**p=format_;return S_OK;}
 HRESULT STDMETHODCALLTYPE BufferSize(DWORD*p)override{return property(p,DWORD(32768));}
 HRESULT STDMETHODCALLTYPE CodecName(wchar_t**p)override{return text(dts_?L"DTS|Digital Theater Systems":L"AC3|Dolby Digital",p);}
 HRESULT STDMETHODCALLTYPE Bitrate(DWORD*p)override{return property(p,format_.nAvgBytesPerSec*8);}
 HRESULT STDMETHODCALLTYPE BitDepth(WORD*p)override{return property(p,bits_);}
 HRESULT STDMETHODCALLTYPE SetCallback(IUnknown*p)override{return input_?input_->callback(p):E_UNEXPECTED;}
 HRESULT STDMETHODCALLTYPE Start()override{if(!opened_)return E_UNEXPECTED;ResetEvent(input_->event);return S_OK;}
 HRESULT STDMETHODCALLTYPE Stop()override{if(!opened_)return E_UNEXPECTED;SetEvent(input_->event);return S_OK;}
 HRESULT STDMETHODCALLTYPE Read(Buffer*b)override{return protect([&]()->HRESULT{require(b,E_POINTER);check(b->SetLength(0));require(opened_,E_UNEXPECTED);require(WaitForSingleObject(input_->event,0)!=WAIT_OBJECT_0,E_ABORT);BYTE*p{};DWORD ignored{},cap{};check(b->Data(&p,&ignored));check(b->Capacity(&cap));require(p,E_POINTER);require(cap>0,E_INVALIDARG);if(length_)cap=DWORD(std::min<uint64_t>(cap,length_-std::min(position_,length_)));DWORD done=DWORD(std::min<size_t>(cap,prefix_.size()-prefix_position_));if(done){memcpy(p,prefix_.data()+prefix_position_,done);prefix_position_+=done;}if(done<cap)done+=input_->read(p+done,cap-done);position_+=done;check(b->SetLength(done));return done?S_OK:S_FALSE;});}
 HRESULT STDMETHODCALLTYPE Seek(DWORD*p)override{return protect([&]{require(p,E_POINTER);require(opened_,E_UNEXPECTED);require(input_->seekable,STG_E_INVALIDFUNCTION);uint64_t duration=length_*1000/format_.nAvgBytesPerSec;require(duration>0,STG_E_INVALIDFUNCTION);uint64_t target=*p>=duration?length_:length_**p/duration;input_->at(start_+target);position_=target;prefix_.clear();prefix_position_=0;ResetEvent(input_->event);return S_OK;});}
 HRESULT STDMETHODCALLTYPE Count(DWORD*p)override{if(!p)return E_POINTER;*p=0;return tags_?tags_->Count(p):S_OK;}
 HRESULT STDMETHODCALLTYPE At(DWORD i,wchar_t**k,wchar_t**v)override{if(!k||!v)return E_POINTER;*k=*v=nullptr;return tags_?tags_->At(i,k,v):E_INVALIDARG;}
 HRESULT STDMETHODCALLTYPE Get(const char*k,wchar_t**v)override{if(!k||!v)return E_POINTER;*v=nullptr;return tags_?tags_->Get(k,v):E_INVALIDARG;}
 HRESULT STDMETHODCALLTYPE Set(const char*k,const wchar_t*v)override{return tags_?tags_->Set(k,v):E_NOTIMPL;}
};
HRESULT make_reader(void**p){if(!p)return E_POINTER;*p=nullptr;return protect([&]{*p=static_cast<Reader*>(new SoundReader);return S_OK;});}
}
