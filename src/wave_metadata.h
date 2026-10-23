#pragma once
#include "io.h"
#include <map>
#include <set>
namespace ttp::ac3dts {
struct Tag {
  std::string key;
  std::wstring value;
};
struct Art {
  Bytes data;
  std::wstring mime, description;
  Picture view{};
};
// RIFF metadata editing is independent of the demuxer's buffered file position.
// Only the host-provided writable stream is used; Stat.pwcsName is not
// authority to reopen a different file. Audio/unknown chunks are copied
// verbatim to disk.
class WaveMetadata {
  struct Chunk {
    uint64_t at{}, bytes{};
    DWORD id{};
  };
  IStream *stream_{};
  uint64_t size_{};
  bool large_{}, all_{}, pictures_{}, writable_{};
  BYTE version_{4};
  std::vector<Chunk> chunks_;
  std::vector<Bytes> frames_;
  std::vector<Art> pictures_original_;
  std::map<std::string, std::wstring> changes_;
  std::set<std::string> removed_;
  Bytes make_id3(const std::vector<Art> &art) const;
  Bytes info_chunk(const Chunk &) const;

public:
  void Open(IStream *stream);
  void Set(const char *key, const wchar_t *value);
  void Pictures() {
    require(writable_, STG_E_ACCESSDENIED);
    pictures_ = true;
  }
  bool Writable() const noexcept { return writable_; }
  bool Dirty() const noexcept {
    return all_ || pictures_ || !changes_.empty() || !removed_.empty();
  }
  const std::vector<Art> &OriginalPictures() const noexcept {
    return pictures_original_;
  }
  void Save(const std::vector<Art> &art);
};
void refresh_art(std::vector<Art> &art);
Art copy_art(const Picture *picture);
std::string metadata_key(std::string key);
} // namespace ttp::ac3dts
