#pragma once

// Portable patch package (`.segapatch`, ADR 0266 schema 1, ADR 0267 schema 2): a bounded ZIP holding `manifest.json` and the declared payloads: a standard BPS payload
// (`rom`, schema 1 and 2) and/or, in schema 2, ONE portable C11 provider source (`native`, a trusted native-source patch). A BPS-only package is DATA ONLY. A package with a
// `native` payload CARRIES EXECUTABLE SOURCE: the build compiles it into the separate Patched program through the public SDK (platforms/genesis/sdk); the product contains no
// game-specific provider. Parsing and digests are not a sandbox: such a package must come from a trusted source and requires explicit user approval of its exact identity. Every function fails closed with a readable `error`. The byte-span functions touch no file; the
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

// Deterministic BPS1 writer (SEG-136-T006, ADR 0293): the canonical command order is a single left-to-right scan. A run of at least kBpsMinCopyRun positions where target[i] == source[i]
// becomes one SourceRead; every other position is TargetRead literal bytes (adjacent literals merge). No SourceCopy/TargetCopy and no metadata, so equal inputs give equal bytes on every host.
// The result is verified by applying it back; `ok` is false (with `error`) if the patch would exceed kMaxPatchBytes or an image exceeds kMaxImageBytes.
inline constexpr std::size_t kBpsMinCopyRun = 4;
[[nodiscard]] BpsResult build_bps(std::span<const std::uint8_t> source, std::span<const std::uint8_t> target);   // BpsResult::output holds the PATCH bytes

// Host capabilities of the native patch ABI (equal to the SDK's GENESIS_CAP_*) and the ABI version this release provides. A package declares what its provider needs; a release
// refuses a package that needs an unknown capability or another ABI version. Capabilities describe what the host can do, never which game is patched.
inline constexpr unsigned kNativeAbiVersion = 1U;
inline constexpr unsigned kCapFrameWorkRam = 1U;
inline constexpr unsigned kCapFrameVdp = 2U;
inline constexpr unsigned kCapSceneryWords = 4U;
inline constexpr unsigned kCapOverlayRgba = 8U;
inline constexpr std::size_t kMaxProviderSourceBytes = 256U << 10U;
inline constexpr const char *kProviderFileName = "provider.c";
// The capability names of the manifest -> bit, or 0 for an unknown name.
[[nodiscard]] unsigned native_capability_bit(const std::string &name);

// Payload types this release understands (closed list; a package naming another type, or a capability or ABI version this release does not provide, is refused with a readable
// error): `rom` (a BPS delta producing a patched input image) and, in schema 2, `native` (the provider source).
struct PackageInfo {
  int schema_version = 1;
  std::string id, version, name, platform, purpose;
  std::string source_sha256;
  bool has_rom = false;                 // payloads.rom present
  std::string rom_file, rom_payload_sha256, target_sha256;   // target_sha256 = the patched image digest (set when has_rom)
  // Schema 2 `native` payload: ONE C11 provider source (executable code compiled into the Patched program). A native-only package leaves the image unchanged.
  bool has_native = false;
  std::string native_file, native_sha256;           // the provider source's file name and SHA-256 (part of the package identity and of every variant identity)
  unsigned native_abi = 0, native_caps = 0;         // the ABI version it was written for and the host capabilities it requires (kCap* mask)
  std::string author, license;                      // manifest metadata shown before the user approves the code
  std::vector<std::string> requires_capabilities;
  // Optional typed presentation defaults (closed value sets, validated by the parser): applied by the viewer only while this verified Patched variant runs, as a default
  // the user can change. They are never provenance: a patch that prefers Level 3 does not make any frame authored.
  std::string default_widescreen, default_side_style;   // "" = not stated
  int widescreen_width_max = -1;        // highest validated Widescreen Width (0..100) the author claims, -1 = not stated
  std::string identity;  // SHA-256 of the manifest bytes: the package identity. The manifest names every payload digest, so it covers ALL variant inputs
};

struct Package {
  bool ok = false;
  std::string error;
  PackageInfo info;
  std::vector<std::uint8_t> payload;  // the BPS bytes, SHA-256 verified against the manifest (empty without a rom payload)
  std::vector<std::uint8_t> native;   // the provider source bytes, SHA-256 verified against the manifest (empty without a native payload)
};


// Validates the ZIP structure (entries, names, sizes, CRC), the manifest schema and the payload SHA-256.
[[nodiscard]] Package read_package(std::span<const std::uint8_t> zip);

struct Applied {
  bool ok = false;
  std::string error;
  std::vector<std::uint8_t> image;  // the patched image, SHA-256 verified against the manifest
};

// `rom` must have exactly the manifest's source SHA-256; BPS source/target/patch checksums and the target SHA-256 are verified. A package without a rom payload
// (native-only) returns the verified original bytes unchanged.
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
