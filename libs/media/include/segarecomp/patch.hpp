#pragma once

// Portable patch package (`.segapatch`, ADR 0266 schema 1, ADR 0267 schema 2): a bounded ZIP holding `manifest.json` and the declared payloads: a standard
// BPS payload (`rom`, schema 1 and 2) and/or a typed native-capability descriptor (`native`, schema 2 only). The package is DATA ONLY. Applying it produces an input
// image (patched, or unchanged for a native-only package) that the normal static pipeline then processes; the descriptor only selects a statically compiled,
// audited product capability. Nothing in a package is ever executed. Every function fails closed with a readable `error`. The byte-span functions touch no file; the
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

// Native compatibility capabilities this release provides (ADR 0267). A native descriptor may only select one of these, for exactly the listed original
// image, with an implementation version at most the one built in and a subset of the listed components. The table is audited product data, not package data.
inline constexpr unsigned kNativeComponentHud = 1U;
inline constexpr unsigned kNativeComponentScenery = 2U;
struct NativeCapability {
  const char *id;
  std::uint32_t implementation_version;   // the compiled-in provider's version (part of every variant identity)
  const char *source_sha256;              // the one exact original image the provider is verified for
  unsigned components;                    // mask of kNativeComponent*
  bool test_fixture;                      // a project-owned artificial image used only by synthetic tests: visible only when SEGARECOMP_ALLOW_FIXTURE_CAPABILITIES=1 and never buildable
};
// The position of an entry is its provider index in the statically linked registry (platforms/genesis/native_compat/native_compat_registry.c); a test checks the agreement.
inline constexpr NativeCapability kNativeCapabilities[] = {
    {"genesis.sonic1.native-widescreen.v1", 1U, "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6", kNativeComponentHud | kNativeComponentScenery, false},
    {"genesis.fixture.native-widescreen.v1", 1U, "3004c4367a68819c54484ef6050f4c6dcd27cb474bc71d0656bad1f0e606b342", kNativeComponentHud | kNativeComponentScenery, true},
};
// The capability of this release for `id` (fixtures only when the test switch is set), or nullptr.
[[nodiscard]] const NativeCapability *find_native_capability(const std::string &id);
// Its provider index (position in kNativeCapabilities), or -1.
[[nodiscard]] int native_capability_index(const std::string &id);
inline constexpr std::size_t kMaxNativeDescriptorBytes = 4U << 10U;

// Payload types this release understands (closed list; a package naming another type, or requiring a capability not provided by this release, is refused with
// a readable error): `rom` (a BPS delta producing a patched input image) and, in schema 2, `native` (a closed JSON descriptor selecting a native capability).
struct PackageInfo {
  int schema_version = 1;
  std::string id, version, name, platform, purpose;
  std::string source_sha256;
  bool has_rom = false;                 // payloads.rom present
  std::string rom_file, rom_payload_sha256, target_sha256;   // target_sha256 = the patched image digest (set when has_rom)
  // Schema 2 `native` payload (typed descriptor; see NativeCapability). A native-only package leaves the image unchanged.
  bool has_native = false;
  std::string native_file, native_payload_sha256;   // the descriptor digest is part of the package identity and of every variant identity
  std::string native_capability;
  std::uint32_t native_min_implementation = 0, native_implementation = 0;   // the descriptor's requirement / the compiled-in version that serves it
  unsigned native_components = 0;                   // mask of kNativeComponent*
  int native_width_max = -1;                        // descriptor width_max, -1 = not stated
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
  std::vector<std::uint8_t> native;   // the descriptor bytes, SHA-256 verified against the manifest (empty without a native payload)
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
