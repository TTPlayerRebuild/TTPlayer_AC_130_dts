#include "ttpcomm/audio_format_probe_impl.h"
#include "common.h"
namespace ttp::ac3dts {
HMODULE module{};
StandardContent standard_content(){for(auto name:{L"ttpctrl.dll",L"soundcore.dll",static_cast<const wchar_t*>(nullptr)})if(auto h=GetModuleHandleW(name))if(auto p=GetProcAddress(h,"CreateStdContent"))return reinterpret_cast<StandardContent>(p);return nullptr;}
template<bool IsDecoder> class Factory final:public std::conditional_t<IsDecoder,DecoderCreator,ReaderCreator>, public ttp::format::ProbeSupport {
protected:
    DWORD ProbeFamilies() const noexcept override { return IsDecoder ? ttp::format::None : (dts_ ? ttp::format::Dts : ttp::format::Ac3); }
private:

 LONG refs_{1};bool dts_;
public:
 explicit Factory(bool dts):dts_(dts){}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**p)override{
        if (!p) return E_POINTER;
        if (!IsDecoder && IsEqualIID(iid, ttp::format::iid_probe)) { *p=static_cast<ttp::format::Probe*>(this); AddRef(); return S_OK; }
if(!p)return E_POINTER;*p=nullptr;if(!same(iid,IID_IUnknown)&&!same(iid,IsDecoder?cat_decoder:cat_reader))return E_NOINTERFACE;*p=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Create(void**p)override{if constexpr(IsDecoder)return make_decoder(dts_,p);else return make_reader(p);}
 HRESULT STDMETHODCALLTYPE Name(wchar_t**p)override{return text(IsDecoder?(dts_?L"DVD DTS Decoder (libdts)":L"Dolby AC3 Decoder (liba52)"):L"DTS Reader",p);}
 HRESULT STDMETHODCALLTYPE Supports(const GUID*p){if(!p)return E_POINTER;return (same(*p,dts_?dts_subtype:ac3_subtype)||same(*p,dts_?dts_wave:ac3_wave))?S_OK:S_FALSE;}
 HRESULT STDMETHODCALLTYPE Extensions(wchar_t**p){return text(dts_?L"DTS Audio(*.dts;*.dtswav)":L"Dolby AC3 Audio(*.ac3;*.a52)",p);}
};
class Plugin final:public AddIn{
 LONG refs_{1};
public:
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**p)override{if(!p)return E_POINTER;*p=nullptr;if(!same(iid,IID_IUnknown)&&!same(iid,iid_addin))return E_NOINTERFACE;*p=this;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE Enum(DWORD i,GUID*cat,void**p)override{if(!p)return E_POINTER;*p=nullptr;if(!cat)return E_POINTER;if(i>=4)return E_INVALIDARG;return protect([&]{*cat=i<2?cat_decoder:cat_reader;if(i<2)*p=static_cast<DecoderCreator*>(new Factory<true>(i==1));else *p=static_cast<ReaderCreator*>(new Factory<false>(i==3));return S_OK;});}
};
}
extern "C" HRESULT WINAPI ttpGetSoundAddIn(void**p){if(!p)return E_POINTER;*p=nullptr;return ttp::ac3dts::protect([&]{*p=static_cast<ttp::ac3dts::AddIn*>(new ttp::ac3dts::Plugin);return S_OK;});}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){ttp::ac3dts::module=h;DisableThreadLibraryCalls(h);}return TRUE;}
