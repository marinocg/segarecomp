#pragma once

// Portable patch package v1 (`.segapatch`, ADR 0266): a bounded ZIP holding `manifest.json` and exactly one standard BPS payload.
// The package is DATA ONLY. Applying it produces a patched input image that the normal static pipeline then processes; nothing
// in a package is ever executed. Every function fails closed with a readable `error`. The byte-span functions touch no file; the
// *_file helpers at the end are the one shared file-level seam used by both the CLI and the launcher.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace segarecomp::patch {

inline constexpr std::size_t kMaxPackageBytes = 32U << 20U;   // the whole archive
inline constexpr std::size_t kMaxManifestBytes = 16U << 10U;
inline constexpr std::size_t kMaxPatchBytes = 16U << 20U;     // the BPS payload
inline constexpr std::size_t kMaxImageBytes = 16U << 20U;     // source and target image
inline constexpr std::size_t kMaxArchiveEntries = 8;

struct BpsResult {
  bool ok = false;
  std::string error;
  std::vector<std::uint8_t> output;
};

// Standard BPS1 (byuu): applies `patch` to `source`. Verifies the declared source size, the three CRC-32 values, every copy offset
// and the exact end of the command stream. The output never exceeds `max_output`.
[[nodiscard]] BpsResult apply_bps(std::span<const std::uint8_t> patch, std::span<const std::uint8_t> source, std::size_t max_output = kMaxImageBytes);

// Payload types this release understands (closed list; a package naming another type, or requiring a capability not listed in
// `kSupportedCapabilities`, is refused with a readable error). Only the payload types proven necessary are implemented: the `rom`
// payload (a BPS delta producing a patched input image). A native widescreen descriptor payload is deliberately NOT defined yet.
struct PackageInfo {
  std::string id, version, name, platform, purpose;
  std::string source_sha256;
  bool has_rom = false;                 // payloads.rom present
  std::string rom_file, rom_payload_sha256, target_sha256;   // target_sha256 = the patched image digest (set when has_rom)
  std::vector<std::string> requires_capabilities;
  int widescreen_width_max = -1;        // highest validated Widescreen Width (0..100) the author claims, -1 = not stated
  std::string identity;  // SHA-256 of the manifest bytes: the package identity. The manifest names every payload digest, so it covers ALL variant inputs
};

struct Package {
  bool ok = false;
  std::string error;
  PackageInfo info;
  std::vector<std::uint8_t> payload;  // the BPS bytes, SHA-256 verified against the manifest
};

inline constexpr const char *const kSupportedCapabilities[] = {nullptr};  // none required by any supported payload today

// Validates the ZIP structure (entries, names, sizes, CRC), the manifest schema and the payload SHA-256.
[[nodiscard]] Package read_package(std::span<const std::uint8_t> zip);

struct Applied {
  bool ok = false;
  std::string error;
  std::vector<std::uint8_t> image;  // the patched image, SHA-256 verified against the manifest
};

// `rom` must have exactly the manifest's source SHA-256; BPS source/target/patch checksums and the target SHA-256 are verified.
[[nodiscard]] Applied apply_package(const Package &package, std::span<const std::uint8_t> rom);

// File-level seam (CLI `segarecomp patch ...` and the launcher call exactly these). Bounded reads; the output is written to a temporary
// sibling and renamed, so a failure never leaves a partial or half-patched file. `out` may be empty (verify only).
[[nodiscard]] Package read_package_file(const std::filesystem::path &package);
struct FileApplied {
  bool ok = false;
  std::string error;
  PackageInfo info;
  std::string rom_sha256;     // exact original image digest
  std::string target_sha256;  // verified patched image digest
};
[[nodiscard]] FileApplied apply_package_file(const std::filesystem::path &package, const std::filesystem::path &rom, const std::filesystem::path &out);
// Package identity of an already-read package file, or empty on failure.

// Test/authoring support: CRC-32 (IEEE) used by ZIP and BPS.
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes);

}  // namespace segarecomp::patch
