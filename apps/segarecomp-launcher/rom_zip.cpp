#include "rom_zip.hpp"

#include "segarecomp/sha256.hpp"

#include "stb_image.h"  // declarations only: the implementation (with its zlib decoder) is compiled once in pixel_assets.cpp

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace launcher {

namespace {

constexpr std::uintmax_t kMaxZipBytes = 64U << 20U;     // the whole archive is read into memory
constexpr std::uint32_t kMaxRomBytes = 16U << 20U;       // far above any Genesis or Master System cartridge

std::uint32_t le16(const std::vector<std::uint8_t> &b, std::size_t at) { return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8U; }
std::uint32_t le32(const std::vector<std::uint8_t> &b, std::size_t at) { return le16(b, at) | le16(b, at + 2) << 16U; }

std::uint32_t crc32_of(const std::uint8_t *data, std::size_t size) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFFU] ^ (c >> 8U);
  return c ^ 0xFFFFFFFFU;
}

std::string lower_ext(const std::string &name) {
  const auto dot = name.find_last_of('.');
  std::string ext = dot == std::string::npos ? std::string() : name.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

bool rom_extension(const std::string &ext) { return ext == "md" || ext == "bin" || ext == "gen" || ext == "smd" || ext == "sms"; }

struct Entry { std::string name; std::uint32_t flags, method, crc, csize, usize, local; };

} // namespace

bool is_zip_path(const fs::path &path) {
  const auto u8 = path.extension().u8string();
  return lower_ext("x" + std::string(reinterpret_cast<const char *>(u8.data()), u8.size())) == "zip";
}

ZipRom extract_zip_rom(const fs::path &zip, const fs::path &roms_dir) {
  ZipRom out;
  std::error_code ec;
  const std::uintmax_t size = fs::file_size(zip, ec);
  if (ec) { out.error = "the archive cannot be read"; return out; }
  if (size > kMaxZipBytes) { out.error = "the archive is larger than 64 MiB"; return out; }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  {
    std::ifstream in(zip, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) { out.error = "the archive cannot be read"; return out; }
  }
  // End of central directory: the last record with its signature (a trailing comment can follow it).
  std::size_t eocd = std::string::npos;
  for (std::size_t i = bytes.size() >= 22 ? bytes.size() - 22 : 0; bytes.size() >= 22; --i) {
    if (le32(bytes, i) == 0x06054b50U) { eocd = i; break; }
    if (i == 0 || bytes.size() - i > 22 + 0xFFFF) break;
  }
  if (eocd == std::string::npos) { out.error = "not a zip archive (or a damaged one)"; return out; }
  const std::uint32_t count = le16(bytes, eocd + 10), cd_size = le32(bytes, eocd + 12), cd_offset = le32(bytes, eocd + 16);
  if (cd_offset == 0xFFFFFFFFU || count == 0xFFFFU) { out.error = "zip64 archives are not supported"; return out; }
  if (static_cast<std::uint64_t>(cd_offset) + cd_size > eocd) { out.error = "the archive's directory is damaged"; return out; }
  std::vector<Entry> roms;
  std::size_t p = cd_offset;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (p + 46 > eocd || le32(bytes, p) != 0x02014b50U) { out.error = "the archive's directory is damaged"; return out; }
    Entry e{};
    e.flags = le16(bytes, p + 8);
    e.method = le16(bytes, p + 10);
    e.crc = le32(bytes, p + 16);
    e.csize = le32(bytes, p + 20);
    e.usize = le32(bytes, p + 24);
    const std::uint32_t name_len = le16(bytes, p + 28), extra_len = le16(bytes, p + 30), comment_len = le16(bytes, p + 32);
    e.local = le32(bytes, p + 42);
    if (p + 46 + name_len > eocd) { out.error = "the archive's directory is damaged"; return out; }
    e.name.assign(reinterpret_cast<const char *>(&bytes[p + 46]), name_len);
    p += 46 + name_len + extra_len + comment_len;
    const bool directory = !e.name.empty() && (e.name.back() == '/' || e.name.back() == '\\');
    const bool resource_fork = e.name.rfind("__MACOSX/", 0) == 0 || e.name.find("/._") != std::string::npos || e.name.rfind("._", 0) == 0;
    if (!directory && !resource_fork && rom_extension(lower_ext(e.name))) roms.push_back(e);
  }
  if (roms.empty()) { out.error = "the archive contains no Genesis / Mega Drive (.md .bin .gen .smd) or Master System (.sms) ROM"; return out; }
  if (roms.size() > 1) { out.error = "the archive contains " + std::to_string(roms.size()) + " ROMs; extract the one you want and add it directly"; return out; }
  const Entry &e = roms.front();
  out.inner_name = e.name;
  if ((e.flags & 1U) != 0U) { out.error = "encrypted archives are not supported"; return out; }
  if (e.method != 0 && e.method != 8) { out.error = "the ROM uses an unsupported compression method (only stored and deflate)"; return out; }
  if (e.usize == 0 || e.usize > kMaxRomBytes) { out.error = "the ROM inside has an implausible size"; return out; }
  if (static_cast<std::uint64_t>(e.local) + 30 > bytes.size() || le32(bytes, e.local) != 0x04034b50U) { out.error = "the archive's ROM entry is damaged"; return out; }
  const std::size_t data = static_cast<std::size_t>(e.local) + 30 + le16(bytes, e.local + 26) + le16(bytes, e.local + 28);
  if (static_cast<std::uint64_t>(data) + e.csize > bytes.size()) { out.error = "the archive is truncated"; return out; }
  std::vector<std::uint8_t> rom;
  if (e.method == 0) {
    if (e.csize != e.usize) { out.error = "the archive's ROM entry is damaged"; return out; }
    rom.assign(bytes.begin() + static_cast<std::ptrdiff_t>(data), bytes.begin() + static_cast<std::ptrdiff_t>(data + e.usize));
  } else {
    int got = 0;
    char *inflated = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char *>(&bytes[data]), static_cast<int>(e.csize), &got);
    if (inflated == nullptr || static_cast<std::uint32_t>(got) != e.usize) { stbi_image_free(inflated); out.error = "the ROM inside could not be decompressed"; return out; }
    rom.assign(reinterpret_cast<std::uint8_t *>(inflated), reinterpret_cast<std::uint8_t *>(inflated) + got);
    stbi_image_free(inflated);
  }
  if (crc32_of(rom.data(), rom.size()) != e.crc) { out.error = "the ROM inside fails its CRC check (damaged archive)"; return out; }
  const std::string digest = segarecomp::sha256_hex({rom.data(), rom.size()});
  fs::create_directories(roms_dir, ec);
  const fs::path target = roms_dir / (digest + "." + lower_ext(e.name));
  if (fs::is_regular_file(target, ec) && fs::file_size(target, ec) == rom.size()) { out.ok = true; out.extracted = target; return out; }
  const fs::path temp = roms_dir / (digest + ".tmp");
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file.write(reinterpret_cast<const char *>(rom.data()), static_cast<std::streamsize>(rom.size()))) { fs::remove(temp, ec); out.error = "the ROM could not be written to the library folder"; return out; }
  }
  fs::rename(temp, target, ec);
  if (ec) { fs::remove(temp, ec); out.error = "the ROM could not be written to the library folder"; return out; }
  out.ok = true;
  out.extracted = target;
  return out;
}

RomView inspect_rom_or_zip(const fs::path &path, const Layout &layout, const fs::path &roms_dir) {
  if (!is_zip_path(path)) return inspect_rom_file(path, layout);
  const ZipRom zip = extract_zip_rom(path, roms_dir);
  if (!zip.ok) {
    RomView refused;
    refused.path = path;
    refused.error = "zip: " + zip.error;
    return refused;
  }
  RomView view = inspect_rom_file(zip.extracted, layout);
  // A title that fell back to the file name would be the digest of the extracted copy: use the name inside the archive instead.
  const auto stem = zip.extracted.stem().u8string();
  if (view.title.empty() || view.title == std::string(reinterpret_cast<const char *>(stem.data()), stem.size())) {
    std::string inner = zip.inner_name.substr(zip.inner_name.find_last_of("/\\") == std::string::npos ? 0 : zip.inner_name.find_last_of("/\\") + 1);
    if (const auto dot = inner.find_last_of('.'); dot != std::string::npos) inner.erase(dot);
    if (const std::string cleaned = clean_title(inner); !cleaned.empty()) view.title = cleaned;
  }
  return view;
}

} // namespace launcher
