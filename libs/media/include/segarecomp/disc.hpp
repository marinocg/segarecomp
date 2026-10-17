#pragma once

// SEG-062-T002 (ADR 0129): build-time CUE/BIN disc ingestion and the Sega CD system-area header.
//
// The one CUE parser of the project. It accepts a strict subset (FILE "name" BINARY; TRACK nn MODE1/2048 | MODE1/2352 | AUDIO;
// INDEX 00 (an in-file pregap, SEG-063-T003); INDEX 01; PREGAP; REM/TITLE/PERFORMER/CATALOG/FLAGS ignored), resolves files strictly inside the CUE's directory, verifies the
// sync pattern and header of every raw data sector, and yields a normalized track table plus a content identity: the SHA-256 of
// every referenced file and the SHA-256 of the normalized table text (which includes the file digests). Every failure is a typed
// DiscError; a failed load never returns a partial disc. Reads are bounds-checked and return only Mode-1 user data.
//
// The generated runtime never parses a CUE: it receives this table as constant data (ADR 0129).

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp {

inline constexpr std::uint32_t kDiscUserBytes = 2048U;
inline constexpr std::uint32_t kDiscRawBytes = 2352U;
inline constexpr std::uint32_t kDiscMsfLeadIn = 150U;           // MSF 00:02:00 is LBA 0 (ECMA-130)
inline constexpr std::uint32_t kDiscMaxSectors = 360000U;        // 80 minutes
inline constexpr std::uintmax_t kCueMaxBytes = 64U * 1024U;

enum class DiscTrackType : std::uint8_t { mode1_2048, mode1_2352, audio };

enum class DiscError : std::uint8_t {
  none,
  cue_unreadable,          // missing, not a regular file, or larger than kCueMaxBytes
  cue_syntax,              // malformed line, bad quoting, bad number or time
  cue_unsupported,         // a directive or mode outside the supported subset (INDEX 02+, POSTGAP, MODE2, WAVE, INDEX 00 on track 1, ...)
  file_missing,            // a TRACK before any FILE, or a FILE with no TRACK
  file_ambiguous,          // the same file named twice
  path_unsafe,             // absolute path, separator, '..', drive letter, symlink, or resolution outside the CUE directory
  file_unreadable,         // referenced file missing or unreadable
  file_size_not_multiple,  // file size is not a multiple of its tracks' sector size
  file_truncated,          // an INDEX points at or beyond the end of its file
  mixed_sector_sizes,      // tracks of different sector sizes in one file
  track_numbering,         // tracks not numbered 1, 2, 3, ... or a track without INDEX 01
  track_layout,            // overlapping or gapped INDEX positions, or a track with no sectors
  index_order,             // a repeated INDEX, INDEX 00 after INDEX 01, or INDEX 00 later than its INDEX 01 (SEG-063-T003)
  pregap_conflict,         // one track declares both PREGAP and INDEX 00 (SEG-063-T003)
  disc_too_large,          // more than kDiscMaxSectors
  raw_sync,                // a raw data sector without the 12-byte sync pattern
  raw_header,              // a raw data sector whose header MSF or mode disagrees with its LBA
  header_conflict,         // a cooked (2048) track whose data begins with a raw sync pattern
  identity_mismatch,       // the computed identity differs from the expected one
  lba_out_of_range,        // read outside every track
  lba_audio,               // read of an audio sector as data
  io_error,
};

[[nodiscard]] const char *disc_error_name(DiscError error) noexcept;

struct DiscFile {
  std::string name;           // as written in the CUE (a plain file name)
  std::filesystem::path path; // resolved, inside the CUE directory
  std::uint64_t size{};
  std::string sha256;         // lowercase hex
};

struct DiscTrack {
  unsigned number{};
  DiscTrackType type{DiscTrackType::mode1_2048};
  std::uint32_t file_index{};
  std::uint32_t sector_size{};
  std::uint64_t file_offset{};  // byte offset of the track's INDEX 01 sector in its file
  std::uint32_t pregap{};       // PREGAP sectors (not stored in the file) before first_lba
  // SEG-063-T003: INDEX 00 pregap sectors stored in the file immediately before file_offset (INDEX 01 - INDEX 00). They occupy
  // the LBAs [first_lba - index0_sectors, first_lba), belong to no track's readable area and are never part of the previous
  // track. Zero for a track without INDEX 00.
  std::uint32_t index0_sectors{};
  std::uint32_t first_lba{};
  std::uint32_t sector_count{};
};

class DiscImage {
 public:
  [[nodiscard]] const std::vector<DiscFile> &files() const noexcept { return files_; }
  [[nodiscard]] const std::vector<DiscTrack> &tracks() const noexcept { return tracks_; }
  [[nodiscard]] std::uint32_t total_sectors() const noexcept { return total_sectors_; }
  // The normalized table (deterministic text, contains the file digests) and its SHA-256: the disc identity.
  [[nodiscard]] const std::string &normalized_table() const noexcept { return table_; }
  [[nodiscard]] const std::string &identity() const noexcept { return identity_; }
  [[nodiscard]] const DiscTrack *track_of(std::uint32_t lba) const noexcept;
  // One Mode-1 user block (2048 bytes) by LBA; audio and out-of-range LBAs are typed errors.
  [[nodiscard]] DiscError read_user_data(std::uint32_t lba, std::span<std::uint8_t, kDiscUserBytes> out) const;

 private:
  friend struct DiscLoader;
  std::vector<DiscFile> files_;
  std::vector<DiscTrack> tracks_;
  std::uint32_t total_sectors_{};
  std::string table_;
  std::string identity_;
};

struct DiscLoadResult {
  std::optional<DiscImage> disc;  // set only when error == none
  DiscError error{DiscError::none};
  std::string detail;  // short, content-free (line number, track number)
};

// `expected_identity`, when given, is checked after loading (the integrity boundary of ADR 0129).
[[nodiscard]] DiscLoadResult load_cue_disc(const std::filesystem::path &cue, std::optional<std::string_view> expected_identity = {});

[[nodiscard]] const char *disc_track_type_name(DiscTrackType type) noexcept;
[[nodiscard]] std::string disc_bcd_msf(std::uint32_t lba);  // "MM:SS:FF" of lba + 150

// ---------------------------------------------------------------------------------------------------------------------------------
// Sega CD system area (contract section 9). Parsed from the first 16 user blocks (32 KiB) of track 1.

inline constexpr std::uint32_t kSegaCdSystemAreaBytes = 16U * kDiscUserBytes;
inline constexpr std::uint32_t kSegaCdHeaderEnd = 0x200U;  // IP and SP must not overlap the header

enum class SegaCdBootError : std::uint8_t {
  none,
  first_track_not_data,  // track 1 is audio
  system_area_short,     // track 1 holds fewer than 16 blocks
  system_id,             // sector 0 does not begin with "SEGADISCSYSTEM  "
  descriptor_zero_size,  // IP or SP load size is zero
  descriptor_odd,        // odd disc address, size or entry offset
  descriptor_outside,    // extent overlaps the header or leaves the system area
  descriptor_overlap,    // IP and SP extents overlap
  entry_outside,         // entry offset not inside the loaded bytes
};

[[nodiscard]] const char *sega_cd_boot_error_name(SegaCdBootError error) noexcept;

struct SegaCdBootDescriptor {
  std::uint32_t disc_address{};  // byte offset inside the system area
  std::uint32_t load_size{};
  std::uint32_t entry_offset{};
  std::uint32_t work_ram_size{};
};

struct SegaCdBootHeader {
  std::string system_id;   // 16 bytes
  std::string volume_name; // 11 bytes at $010 (the disc ID)
  std::string system_name; // 11 bytes at $020
  SegaCdBootDescriptor ip;
  SegaCdBootDescriptor sp;
  std::string hardware_id; // 16 bytes at $100
  std::string product_code;// 14 bytes at $180
  std::string region;      // printable characters of $1F0..$1F2
};

struct SegaCdBootResult {
  std::optional<SegaCdBootHeader> header;
  SegaCdBootError error{SegaCdBootError::none};
};

// Pure parse of a 32 KiB system area (no I/O).
[[nodiscard]] SegaCdBootResult parse_sega_cd_system_area(std::span<const std::uint8_t> system_area);
// Reads the system area of `disc` (track 1 must be data) and parses it.
[[nodiscard]] SegaCdBootResult read_sega_cd_boot(const DiscImage &disc, std::vector<std::uint8_t> *system_area_out = nullptr);

}  // namespace segarecomp
