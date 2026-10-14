#pragma once

// Zipped ROMs: a .zip that contains exactly one Genesis / Mega Drive or Master System ROM is unpacked once into the library's `roms/` folder
// (named by the ROM's SHA-256), and that extracted file is what the library tracks and builds. The zip itself is only read.
// Supported: classic zip (no zip64), stored or deflate entries (inflated by stb_image's zlib decoder, already part of the launcher), CRC-checked.
// Refused with a plain reason: no ROM inside, several ROMs, encryption, other compression methods, oversize entries, damaged archives.

#include "launcher_core.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace launcher {

// The largest ROM a zip may declare (far above any Genesis or Master System cartridge); larger declarations are refused before any allocation.
inline constexpr std::uint32_t kMaxZipRomBytes = 16U << 20U;

[[nodiscard]] bool is_zip_path(const std::filesystem::path &path);

struct ZipRom {
  bool ok = false;
  std::filesystem::path extracted;  // <roms_dir>/<sha256>.<ext> of the ROM inside
  std::string inner_name;           // the entry's name inside the archive
  std::string error;                // why it was refused
  std::size_t output_capacity = 0;  // bytes reserved for the ROM: exactly its declared size (0 when refused before decoding)
};

// Reads `zip`, finds its single ROM entry, decodes it into a buffer of exactly its declared size, checks its CRC-32 and writes it to
// `roms_dir/<sha256>.<ext>` through a temporary file and a rename. An existing file there is reused only when its SHA-256 matches; a damaged
// one is replaced; a link or folder in its place is refused.
[[nodiscard]] ZipRom extract_zip_rom(const std::filesystem::path &zip, const std::filesystem::path &roms_dir);

// The import route for one dropped/picked/command-line path: a zip is unpacked into `roms_dir` first, then the ROM is inspected like any other.
// A refused zip comes back as a RomView with `error` set (and `path` = the zip).
[[nodiscard]] RomView inspect_rom_or_zip(const std::filesystem::path &path, const Layout &layout, const std::filesystem::path &roms_dir);

// Test seam: makes the next extraction writes fail half-way (proves a failed write never leaves a ROM, or a partial file, behind).
void set_zip_write_failure_for_tests(bool fail);

} // namespace launcher
