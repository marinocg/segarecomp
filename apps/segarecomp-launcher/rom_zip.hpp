#pragma once

// Zipped ROMs: a .zip that contains exactly one Genesis / Mega Drive or Master System ROM is unpacked once into the library's `roms/` folder
// (named by the ROM's SHA-256), and that extracted file is what the library tracks and builds. The zip itself is only read.
// Supported: classic zip (no zip64), stored or deflate entries (inflated by stb_image's zlib decoder, already part of the launcher), CRC-checked.
// Refused with a plain reason: no ROM inside, several ROMs, encryption, other compression methods, oversize entries, damaged archives.

#include "launcher_core.hpp"

#include <filesystem>
#include <string>

namespace launcher {

[[nodiscard]] bool is_zip_path(const std::filesystem::path &path);

struct ZipRom {
  bool ok = false;
  std::filesystem::path extracted;  // <roms_dir>/<sha256>.<ext> of the ROM inside
  std::string inner_name;           // the entry's name inside the archive
  std::string error;                // why it was refused
};

// Reads `zip`, finds its single ROM entry and writes it to `roms_dir` (created when missing; an identical file already there is reused).
[[nodiscard]] ZipRom extract_zip_rom(const std::filesystem::path &zip, const std::filesystem::path &roms_dir);

// The import route for one dropped/picked/command-line path: a zip is unpacked into `roms_dir` first, then the ROM is inspected like any other.
// A refused zip comes back as a RomView with `error` set (and `path` = the zip).
[[nodiscard]] RomView inspect_rom_or_zip(const std::filesystem::path &path, const Layout &layout, const std::filesystem::path &roms_dir);

} // namespace launcher
