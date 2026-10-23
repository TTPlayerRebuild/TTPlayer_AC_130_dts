#include "wave_metadata.h"
#include <cctype>
#include <shlwapi.h>
namespace ttp::ac3dts {
namespace {
constexpr size_t tag_limit = 64 * 1024 * 1024;
struct Mapping {
  const char *key;
  const char *frame;
  const char *info;
};
constexpr Mapping mappings[] = {
    {"title", "TIT2", "INAM"},    {"artist", "TPE1", "IART"},
    {"album", "TALB", "IPRD"},    {"album_artist", "TPE2", ""},
    {"track", "TRCK", "ITRK"},    {"date", "TDRC", "ICRD"},
    {"genre", "TCON", "IGNR"},    {"comment", "COMM", "ICMT"},
    {"composer", "TCOM", "IMUS"}, {"copyright", "TCOP", "ICOP"},
    {"lyrics", "USLT", ""},       {"disc", "TPOS", ""},
    {"encoder", "TENC", "ISFT"}};
std::string lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string canonical(std::string s) {
  s = lower(std::move(s));
  if (s == "year")
    s = "date";
  if (s == "albumartist" || s == "album artist")
    s = "album_artist";
  if (s == "tracknumber")
    s = "track";
  if (s == "discnumber")
    s = "disc";
  return s;
}
uint32_t sync(const BYTE *p) {
  require(!((p[0] | p[1] | p[2] | p[3]) & 128));
  return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3];
}
void setsync(Bytes &b, size_t at, uint32_t n) {
  require(n < 0x10000000);
  for (unsigned i = 0; i < 4; ++i)
    b[at + i] = BYTE(n >> ((3 - i) * 7) & 127);
}
void setle(Bytes &b, size_t at, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    b[at + i] = BYTE(n >> (i * 8));
}
void setle64(Bytes &b, size_t at, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i)
    b[at + i] = BYTE(n >> (i * 8));
}
std::wstring string_data(const BYTE *p, size_t n, BYTE encoding) {
  if (encoding == 0) {
    std::wstring s;
    for (size_t i = 0; i < n && p[i]; ++i)
      s += wchar_t(p[i]);
    return s;
  }
  if (encoding == 3) {
    size_t end = 0;
    while (end < n && p[end])
      ++end;
    return wide(std::string(reinterpret_cast<const char *>(p), end));
  }
  require(encoding == 1 || encoding == 2, E_NOTIMPL);
  bool big = encoding == 2;
  if (n >= 2 && p[0] == 0xff && p[1] == 0xfe) {
    p += 2;
    n -= 2;
    big = false;
  } else if (n >= 2 && p[0] == 0xfe && p[1] == 0xff) {
    p += 2;
    n -= 2;
    big = true;
  }
  std::wstring s;
  for (size_t i = 0; i + 1 < n; i += 2) {
    auto c = big ? be16(p + i) : le16(p + i);
    if (!c)
      break;
    s += wchar_t(c);
  }
  return s;
}
std::string frame_key(const Bytes &b) {
  std::string id(reinterpret_cast<const char *>(b.data()), 4);
  for (const auto &m : mappings)
    if (id == m.frame || (id == "TYER" && std::string(m.key) == "date"))
      return m.key;
  if (id == "TXXX" && b.size() > 11 && b[8] == 0 && b[9] == 0)
    return canonical(utf8(string_data(b.data() + 11, b.size() - 11, b[10])));
  return {};
}
void append(Bytes &b, const void *p, size_t n) {
  require(n <= tag_limit && b.size() <= tag_limit - n);
  auto first = static_cast<const BYTE *>(p);
  b.insert(b.end(), first, first + n);
}
void string_bytes(Bytes &b, const std::wstring &s, BYTE version,
                  bool terminate) {
  if (version == 4) {
    const auto value = utf8(s);
    append(b, value.data(), value.size());
    if (terminate)
      b.push_back(0);
  } else {
    b.push_back(0xff);
    b.push_back(0xfe);
    for (auto c : s) {
      b.push_back(BYTE(c));
      b.push_back(BYTE(c >> 8));
    }
    if (terminate) {
      b.push_back(0);
      b.push_back(0);
    }
  }
  require(b.size() <= tag_limit);
}
void addframe(Bytes &b, const char *id, const Bytes &payload, BYTE version) {
  Bytes h(10);
  memcpy(h.data(), id, 4);
  if (version == 4)
    setsync(h, 4, DWORD(payload.size()));
  else
    set32(h, 4, DWORD(payload.size()));
  append(b, h.data(), h.size());
  append(b, payload.data(), payload.size());
}
class Temporary {
  wchar_t path_[MAX_PATH]{};
  bool keep_{};

public:
  ComPtr<IStream> stream;
  Temporary() {
    wchar_t dir[MAX_PATH]{};
    auto n = GetTempPathW(MAX_PATH, dir);
    require(n && n < MAX_PATH, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
    require(GetTempFileNameW(dir, L"tda", 0, path_) != 0,
            HRESULT_FROM_WIN32(GetLastError()));
    auto hr = SHCreateStreamOnFileEx(
        path_, STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE,
        FILE_ATTRIBUTE_TEMPORARY, FALSE, nullptr, stream.put());
    if (FAILED(hr)) {
      DeleteFileW(path_);
      throw Failure{hr};
    }
  }
  void Preserve() {
    keep_ = true;
    OutputDebugStringW(L"ttp_ac3dts recovery copy: ");
    OutputDebugStringW(path_);
    OutputDebugStringW(L"\n");
  }
  ~Temporary() {
    stream.reset();
    if (!keep_)
      DeleteFileW(path_);
  }
};
void resize(IStream *s, uint64_t size) {
  ULARGE_INTEGER n{};
  n.QuadPart = size;
  check(s->SetSize(n));
}
void copy(IStream *source, uint64_t start, uint64_t length, IStream *target) {
  seek(source, start);
  Bytes buffer(65536);
  while (length) {
    auto n = DWORD(std::min<uint64_t>(length, buffer.size()));
    read_exact(source, buffer.data(), n);
    write_exact(target, buffer.data(), n);
    length -= n;
  }
}
void chunk(IStream *s, const char *id, const Bytes &b) {
  Bytes h(8);
  memcpy(h.data(), id, 4);
  setle(h, 4, DWORD(b.size()));
  write_exact(s, h.data(), 8);
  if (!b.empty())
    write_exact(s, b.data(), DWORD(b.size()));
  if (b.size() & 1) {
    BYTE z = 0;
    write_exact(s, &z, 1);
  }
}
} // namespace
std::string metadata_key(std::string key) { return canonical(std::move(key)); }
void refresh_art(std::vector<Art> &art) {
  for (auto &p : art) {
    auto type = p.view.type;
    p.view = {sizeof(Picture),      p.mime.c_str(), p.description.c_str(),
              DWORD(p.data.size()), p.data.data(),  type};
  }
}
Art copy_art(const Picture *p) {
  require(p && p->size == sizeof(Picture), E_INVALIDARG);
  require(p->data && p->bytes && p->bytes <= 16 * 1024 * 1024 && p->type <= 20,
          E_INVALIDARG);
  require(p->mime, E_INVALIDARG);
  const std::wstring mime = p->mime;
  require(mime == L"image/png" || mime == L"image/jpeg" ||
              mime == L"image/webp",
          E_INVALIDARG);
  Art a{Bytes(p->data, p->data + p->bytes),
        mime,
        p->description ? p->description : L"",
        {}};
  a.view.type = p->type;
  require(a.description.size() <= 65536, E_INVALIDARG);
  return a;
}
void WaveMetadata::Open(IStream *s) {
  STATSTG st{};
  check(s->Stat(&st, STATFLAG_NONAME));
  writable_ = (st.grfMode & 3) == STGM_READWRITE;
  size_ = st.cbSize.QuadPart;
  auto h = read_at(s, 0, 12);
  require(!memcmp(h.data() + 8, "WAVE", 4));
  large_ = !memcmp(h.data(), "RF64", 4) || !memcmp(h.data(), "BW64", 4);
  require(large_ || !memcmp(h.data(), "RIFF", 4));
  require(large_ || uint64_t(le32(h.data() + 4)) + 8 == size_, E_NOTIMPL);
  uint64_t at = 12, large_data = 0;
  bool ds64 = false, id3 = false;
  while (at < size_) {
    require(chunks_.size() < 65536 && size_ - at >= 8);
    h = read_at(s, at, 8);
    uint64_t bytes = le32(h.data() + 4);
    const DWORD id = be32(h.data());
    if (bytes == MAXDWORD) {
      require(id == fourcc('d', 'a', 't', 'a') && ds64, E_NOTIMPL);
      bytes = large_data;
    }
    require(bytes <= size_ - at - 8 && bytes + (bytes & 1) <= size_ - at - 8);
    chunks_.push_back({at, bytes, id});
    if (id == fourcc('L', 'I', 'S', 'T') && bytes > tag_limit) {
      const auto kind = read_at(s, at + 8, 4);
      require(memcmp(kind.data(), "INFO", 4) != 0, E_NOTIMPL);
    }
    if (id == fourcc('d', 's', '6', '4')) {
      require(large_ && !ds64 && bytes >= 28);
      auto data = read_at(s, at + 8, 28);
      require((uint64_t(le32(data.data() + 4)) << 32 | le32(data.data())) ==
                  size_ - 8,
              E_NOTIMPL);
      large_data =
          uint64_t(le32(data.data() + 12)) << 32 | le32(data.data() + 8);
      ds64 = true;
    }
    if (id == fourcc('i', 'd', '3', ' ') || id == fourcc('I', 'D', '3', ' ')) {
      require(!id3 && bytes >= 10 && bytes <= tag_limit, E_NOTIMPL);
      id3 = true;
      auto b = read_at(s, at + 8, size_t(bytes));
      require(!memcmp(b.data(), "ID3", 3) && (b[3] == 3 || b[3] == 4) &&
                  !b[4] && !b[5],
              E_NOTIMPL);
      version_ = b[3];
      auto length = sync(b.data() + 6);
      require(length <= b.size() - 10);
      size_t p = 10, end = 10 + length;
      while (p < end && b[p]) {
        require(end - p >= 10);
        for (unsigned i = 0; i < 4; ++i)
          require((b[p + i] >= 'A' && b[p + i] <= 'Z') ||
                  (b[p + i] >= '0' && b[p + i] <= '9'));
        auto n =
            version_ == 4 ? sync(b.data() + p + 4) : be32(b.data() + p + 4);
        require(n <= end - p - 10);
        frames_.emplace_back(b.begin() + p, b.begin() + p + 10 + n);
        p += 10 + n;
      }
      for (; p < end; ++p)
        require(!b[p]);
    }
    at += 8 + bytes + (bytes & 1);
  }
  require(!large_ || ds64, E_NOTIMPL);
  for (const auto &frame : frames_)
    if (!memcmp(frame.data(), "APIC", 4)) {
      require(frame.size() > 14 && !frame[8] && !frame[9], E_NOTIMPL);
      const BYTE encoding = frame[10];
      require(encoding <= 3);
      size_t p = 11;
      while (p < frame.size() && frame[p])
        ++p;
      require(p < frame.size() - 2 && p - 11 <= 128);
      const auto mime = wide(std::string(
          reinterpret_cast<const char *>(frame.data() + 11), p - 11));
      ++p;
      const auto type = frame[p++];
      require(type <= 20);
      const size_t begin = p, width = (encoding == 1 || encoding == 2) ? 2 : 1;
      while (p + width <= frame.size()) {
        if (!frame[p] && (width == 1 || !frame[p + 1]))
          break;
        p += width;
      }
      require(p + width < frame.size());
      const auto description =
          string_data(frame.data() + begin, p - begin, encoding);
      p += width;
      require(frame.size() - p <= 16 * 1024 * 1024 &&
              pictures_original_.size() < 32);
      Art a{Bytes(frame.begin() + p, frame.end()), mime, description, {}};
      a.view.type = type;
      pictures_original_.push_back(std::move(a));
    }
  refresh_art(pictures_original_);
  stream_ = s;
}
void WaveMetadata::Set(const char *key, const wchar_t *value) {
  require(stream_, E_UNEXPECTED);
  require(writable_, STG_E_ACCESSDENIED);
  if (!key) {
    require(!value, E_INVALIDARG);
    all_ = true;
    changes_.clear();
    removed_.clear();
    return;
  }
  auto k = canonical(key);
  require(!k.empty() && k.size() <= 1024, E_INVALIDARG);
  for (auto c : k)
    require(static_cast<unsigned char>(c) >= 32, E_INVALIDARG);
  if (!value) {
    changes_.erase(k);
    removed_.insert(k);
  } else {
    require(wcslen(value) <= 1024 * 1024, E_INVALIDARG);
    changes_[k] = value;
    removed_.erase(k);
  }
}
Bytes WaveMetadata::make_id3(const std::vector<Art> &art) const {
  Bytes b(10);
  memcpy(b.data(), "ID3", 3);
  b[3] = version_;
  for (const auto &f : frames_) {
    const auto k = frame_key(f);
    if ((!k.empty() && (all_ || changes_.count(k) || removed_.count(k))) ||
        (pictures_ && !memcmp(f.data(), "APIC", 4)))
      continue;
    append(b, f.data(), f.size());
  }
  for (const auto &change : changes_) {
    const char *id = "TXXX";
    for (const auto &m : mappings)
      if (change.first == m.key)
        id = m.frame;
    if (version_ == 3 && !strcmp(id, "TDRC"))
      id = "TYER";
    Bytes p{BYTE(version_ == 4 ? 3 : 1)};
    if (!strcmp(id, "TXXX"))
      string_bytes(p, wide(change.first), version_, true);
    if (!strcmp(id, "COMM") || !strcmp(id, "USLT")) {
      append(p, "eng", 3);
      string_bytes(p, L"", version_, true);
    }
    string_bytes(p, change.second, version_, false);
    addframe(b, id, p, version_);
  }
  if (pictures_)
    for (const auto &artItem : art) {
      Bytes p{BYTE(version_ == 4 ? 3 : 1)};
      const auto mime = utf8(artItem.mime);
      append(p, mime.data(), mime.size());
      p.push_back(0);
      p.push_back(BYTE(artItem.view.type));
      string_bytes(p, artItem.description, version_, true);
      append(p, artItem.data.data(), artItem.data.size());
      addframe(b, "APIC", p, version_);
    }
  setsync(b, 6, DWORD(b.size() - 10));
  return b;
}
Bytes WaveMetadata::info_chunk(const Chunk &c) const {
  auto b = read_at(stream_, c.at + 8, size_t(c.bytes));
  if (b.size() < 4 || memcmp(b.data(), "INFO", 4))
    return b;
  Bytes result(b.begin(), b.begin() + 4);
  size_t at = 4;
  while (at < b.size()) {
    require(b.size() - at >= 8);
    auto n = le32(b.data() + at + 4);
    require(uint64_t(n) + (n & 1) <= b.size() - at - 8);
    std::string key;
    for (const auto &m : mappings)
      if (strlen(m.info) == 4 && !memcmp(b.data() + at, m.info, 4)) {
        key = m.key;
        break;
      }
    if (key.empty() || (!all_ && !changes_.count(key) && !removed_.count(key)))
      append(result, b.data() + at, 8 + size_t(n) + (n & 1));
    at += 8 + size_t(n) + (n & 1);
  }
  return result;
}
void WaveMetadata::Save(const std::vector<Art> &art) {
  if (!Dirty())
    return;
  require(stream_, E_UNEXPECTED);
  require(writable_, STG_E_ACCESSDENIED);
  require(stream_size(stream_) == size_, STG_E_REVERTED);
  const auto id3 = make_id3(art);
  Temporary staged, backup;
  auto *target = staged.stream.p;
  copy(stream_, 0, 12, target);
  uint64_t ds64_at = 0;
  for (const auto &c : chunks_) {
    if (c.id == fourcc('i', 'd', '3', ' ') ||
        c.id == fourcc('I', 'D', '3', ' '))
      continue;
    if (c.id == fourcc('L', 'I', 'S', 'T') && c.bytes <= tag_limit) {
      const auto b = info_chunk(c);
      chunk(target, "LIST", b);
    } else {
      if (c.id == fourcc('d', 's', '6', '4'))
        ds64_at = tell(target) + 8;
      copy(stream_, c.at, 8 + c.bytes + (c.bytes & 1), target);
    }
  }
  if (id3.size() > 10)
    chunk(target, "id3 ", id3);
  const auto length = tell(target);
  require(length >= 12 && (large_ || length - 8 < MAXDWORD), E_NOTIMPL);
  Bytes value(large_ ? 8 : 4);
  if (large_) {
    require(ds64_at);
    setle64(value, 0, length - 8);
    seek(target, ds64_at);
  } else {
    setle(value, 0, DWORD(length - 8));
    seek(target, 4);
  }
  write_exact(target, value.data(), DWORD(value.size()));
  copy(stream_, 0, size_, backup.stream.p);
  commit_stream(backup.stream.p);
  commit_stream(target);
  // Keep the complete old bytes on disk until write, truncate and Commit
  // succeed.
  auto result = protect([&]() {
    seek(stream_, 0);
    copy(target, 0, length, stream_);
    resize(stream_, length);
    commit_stream(stream_);
    return S_OK;
  });
  if (FAILED(result)) {
    auto restored = protect([&]() {
      seek(stream_, 0);
      copy(backup.stream.p, 0, size_, stream_);
      resize(stream_, size_);
      commit_stream(stream_);
      return S_OK;
    });
    // A second physical I/O failure cannot promise recovery. Return that
    // failure explicitly; never report a successful save or retry on final
    // Release.
    if (FAILED(restored))
      backup.Preserve();
    check(restored);
    throw Failure{result};
  }
}
} // namespace ttp::ac3dts
