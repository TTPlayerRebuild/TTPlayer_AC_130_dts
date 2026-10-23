#include "common.h"
#include "ttpcomm/audio_format_probe_impl.h"
namespace ttp::ac3dts {
HMODULE module{};
StandardContent standard_content() {
  for (auto name : {L"ttpctrl.dll", L"soundcore.dll",
                    static_cast<const wchar_t *>(nullptr)})
    if (auto h = GetModuleHandleW(name))
      if (auto p = GetProcAddress(h, "CreateStdContent"))
        return reinterpret_cast<StandardContent>(p);
  return nullptr;
}
template <bool IsDecoder>
class Factory final
    : public std::conditional_t<IsDecoder, DecoderCreator, ReaderCreator>,
      public ttp::format::ProbeSupport {
protected:
  DWORD ProbeFamilies() const noexcept override {
    return IsDecoder ? ttp::format::None
                     : (dts_ ? ttp::format::Dts : ttp::format::Ac3);
  }

private:
  LONG refs_{1};
  bool dts_;
  bool eac3_{};

public:
  explicit Factory(bool dts, bool eac3 = false) : dts_(dts), eac3_(eac3) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **p) override {
    if (!p)
      return E_POINTER;
    if (!IsDecoder && IsEqualIID(iid, ttp::format::iid_probe)) {
      *p = static_cast<ttp::format::Probe *>(this);
      AddRef();
      return S_OK;
    }
    if (!p)
      return E_POINTER;
    *p = nullptr;
    if (!same(iid, IID_IUnknown) &&
        !same(iid, IsDecoder ? cat_decoder : cat_reader))
      return E_NOINTERFACE;
    *p = this;
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
  HRESULT STDMETHODCALLTYPE Create(void **p) override {
    if constexpr (IsDecoder)
      return make_decoder(dts_, p);
    else
      return eac3_  ? make_eac3_reader(p)
             : dts_ ? make_dts_reader(p)
                    : make_reader(p);
  }
  HRESULT STDMETHODCALLTYPE Name(wchar_t **p) override {
    return text(IsDecoder ? (dts_ ? L"DVD DTS Decoder (libdts)"
                                  : L"Dolby AC3 Decoder (liba52)")
                : eac3_   ? L"Dolby Digital Plus Reader"
                          : L"DTS Reader",
                p);
  }
  HRESULT STDMETHODCALLTYPE Supports(const GUID *p) {
    if (!p)
      return E_POINTER;
    return (same(*p, dts_ ? dts_subtype : ac3_subtype) ||
            same(*p, dts_ ? dts_wave : ac3_wave))
               ? S_OK
               : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Extensions(wchar_t **p) {
    return text(eac3_  ? L"Dolby Digital Plus(*.eac3;*.ec3;*.ac3)"
                : dts_ ? L"DTS Audio(*.dts;*.dtshd;*.dtswav;*.wav;*.wave)"
                       : L"Dolby AC3 Audio(*.ac3;*.a52)",
                p);
  }
  HRESULT STDMETHODCALLTYPE ProbeFormat(const ttp::format::Request *q,
                                        ttp::format::Result *out) override {
    if constexpr (IsDecoder) {
      return E_NOINTERFACE;
    } else {
      if (!q || !out)
        return E_POINTER;
      if (q->size != sizeof(*q) || q->version != ttp::format::abi_version ||
          out->size != sizeof(*out) ||
          out->version != ttp::format::abi_version ||
          q->bytes > ttp::format::prefix_limit || (!q->data && q->bytes))
        return E_INVALIDARG;
      if (eac3_) {
        *out = {};
        auto frame = [](const BYTE *p, size_t n) -> size_t {
          if (n < 7 || p[0] != 0x0b || p[1] != 0x77 || (p[5] >> 3) < 11 ||
              (p[5] >> 3) > 16 || (p[2] >> 6) == 3)
            return 0;
          if ((p[4] >> 6) == 3 && ((p[4] >> 4) & 3) == 3)
            return 0;
          const size_t size = 2 * (1 + ((size_t(p[2] & 7) << 8) | p[3]));
          return size >= 7 ? size : 0;
        };
        const size_t n = frame(q->data, q->bytes);
        if (!n)
          return S_FALSE;
        out->confidence = n <= q->bytes && frame(q->data + n, q->bytes - n)
                              ? ttp::format::Strong
                              : ttp::format::Tentative;
        out->family = ttp::format::Ac3;
        ttp::format::Copy(out->container, "eac3");
        ttp::format::Copy(out->codec, "eac3");
        ttp::format::Copy(out->extension, L".eac3");
        ttp::format::Copy(out->aliases, L".ec3;.ac3");
        ttp::format::Copy(out->display, L"Dolby Digital Plus");
        return S_OK;
      }
      auto result = ttp::format::Identify(
          *q, dts_ ? (ttp::format::Dts | ttp::format::Wave) : ttp::format::Ac3);
      if (dts_ && result.family == ttp::format::Wave &&
          strcmp(result.codec, "dts"))
        result = {};
      if (dts_ && result.family == ttp::format::Dts &&
          ttp::format::HasDtsFrames({q->data, q->bytes}))
        result.confidence = ttp::format::Strong;
      *out = result;
      return result.confidence >= ttp::format::Tentative ? S_OK : S_FALSE;
    }
  }
};
class Plugin final : public AddIn {
  LONG refs_{1};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    if (!same(iid, IID_IUnknown) && !same(iid, iid_addin))
      return E_NOINTERFACE;
    *p = this;
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
  HRESULT STDMETHODCALLTYPE Enum(DWORD i, GUID *cat, void **p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    if (!cat)
      return E_POINTER;
    if (i >= 5)
      return E_INVALIDARG;
    return protect([&] {
      *cat = i < 2 ? cat_decoder : cat_reader;
      if (i < 2)
        *p = static_cast<DecoderCreator *>(new Factory<true>(i == 1));
      else
        *p = static_cast<ReaderCreator *>(new Factory<false>(i == 3, i == 4));
      return S_OK;
    });
  }
};
} // namespace ttp::ac3dts
extern "C" HRESULT WINAPI ttpGetSoundAddIn(void **p) {
  if (!p)
    return E_POINTER;
  *p = nullptr;
  return ttp::ac3dts::protect([&] {
    *p = static_cast<ttp::ac3dts::AddIn *>(new ttp::ac3dts::Plugin);
    return S_OK;
  });
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    ttp::ac3dts::module = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}
