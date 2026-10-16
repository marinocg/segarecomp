#pragma once

// SEG-063-T001 (ADR 0132): build-time ISO9660 file-extent lookup, the minimum needed to name a disc-loaded module by path.
//
// Supported subset (ECMA-119): the volume descriptor set from logical block 16 (exactly one Primary Volume Descriptor, an optional
// Boot Record or Supplementary/Partition descriptors which are skipped, then the Set Terminator), 2048-byte logical blocks, and
// ordinary directory records of single-extent files. Everything outside it is rejected with a typed error instead of guessed:
// extended attribute records, interleaved files (file unit size / gap), multi-extent files, associated files, volume sets (sequence
// number other than 1), records crossing a sector boundary, both-endian fields whose halves disagree, extents outside the volume,
// directories larger than kIso9660MaxDirectoryBytes, paths deeper than kIso9660MaxDepth, and two records with one identifier in a
// directory. Identifiers are compared byte for byte, version suffix included ("MODA.BIN;1"); no case folding, no Joliet, no Rock
// Ridge (the System Use area is not interpreted). The generated runtime never parses ISO9660 (ADR 0129).

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/disc.hpp"

namespace segarecomp {

inline constexpr std::uint32_t kIso9660BlockBytes = 2048U;
inline constexpr std::uint32_t kIso9660FirstDescriptor = 16U;
inline constexpr std::uint32_t kIso9660MaxDescriptors = 16U;
inline constexpr std::uint32_t kIso9660MaxDirectoryBytes = 64U * 1024U;
inline constexpr std::uint32_t kIso9660MaxDepth = 8U;

enum class Iso9660Error : std::uint8_t {
  none,
  read_failed,                // a block outside the track, or unreadable
  no_primary_descriptor,      // no "CD001" Primary Volume Descriptor before the terminator
  descriptor_invalid,         // bad standard identifier or version, or the set is not terminated within kIso9660MaxDescriptors
  duplicate_primary,          // more than one Primary Volume Descriptor
  both_endian_mismatch,       // the little- and big-endian halves of a field differ
  unsupported_block_size,     // logical block size other than 2048
  volume_space_invalid,       // volume space larger than the track, or smaller than the descriptor set
  unsupported_volume_set,     // volume set size or sequence number other than 1
  record_invalid,             // a directory record shorter than its fixed part or its identifier, or a bad "." / ".." entry
  record_crosses_sector,      // a directory record that does not end inside its sector
  unsupported_feature,        // extended attributes, interleave, multi-extent or associated file
  directory_invalid,          // a directory extent whose size is zero, not a multiple of 2048 or larger than the bound
  extent_out_of_range,        // an extent leaving the volume space
  path_invalid,               // empty, not absolute, empty component, or deeper than kIso9660MaxDepth
  path_not_found,
  path_ambiguous,             // two records with the same identifier in one directory
  not_a_file,                 // the last component names a directory
  not_a_directory,            // an intermediate component names a file
};

[[nodiscard]] const char *iso9660_error_name(Iso9660Error error) noexcept;

// Reads logical block `block` of the volume (0-based inside the data track) into `out`; false when the block does not exist.
using Iso9660BlockReader = std::function<bool(std::uint32_t block, std::span<std::uint8_t, kIso9660BlockBytes> out)>;

struct Iso9660Extent {
  std::uint32_t block{};  // first logical block, relative to the volume (track) start
  std::uint32_t size{};   // data length in bytes
};

struct Iso9660Volume {
  std::string volume_id;  // 32 bytes at PVD offset 40, trailing spaces removed
  std::uint32_t volume_space{};
  Iso9660Extent root;
};

struct Iso9660VolumeResult {
  std::optional<Iso9660Volume> volume;
  Iso9660Error error{Iso9660Error::none};
};

struct Iso9660Entry {
  std::string identifier;  // as recorded
  bool directory{};
  Iso9660Extent extent;
};

struct Iso9660ListResult {
  std::vector<Iso9660Entry> entries;  // in recorded order, without "." and ".."
  Iso9660Error error{Iso9660Error::none};
};

struct Iso9660LookupResult {
  std::optional<Iso9660Extent> extent;
  Iso9660Error error{Iso9660Error::none};
};

[[nodiscard]] Iso9660VolumeResult read_iso9660_volume(const Iso9660BlockReader &read, std::uint32_t track_blocks);
[[nodiscard]] Iso9660ListResult list_iso9660_directory(const Iso9660BlockReader &read, const Iso9660Volume &volume,
                                                       const Iso9660Extent &directory);
// `path` is absolute: "/OVL/MODA.BIN;1".
[[nodiscard]] Iso9660LookupResult lookup_iso9660_file(const Iso9660BlockReader &read, const Iso9660Volume &volume, std::string_view path);

// The block reader of a disc's data track `track_index` (blocks are that track's Mode-1 user blocks).
[[nodiscard]] Iso9660BlockReader iso9660_track_reader(const DiscImage &disc, std::size_t track_index);

}  // namespace segarecomp
