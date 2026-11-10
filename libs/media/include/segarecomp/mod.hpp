#pragma once

// Modding platform, host side (ADR 0280 adapters, ADR 0281 schema 3, ADR 0283 ABI 2, ADR 0286 audio): the `.segapatch` schema 3 reader/packer, the `mod.toml` project model,
// the platform-adapter registry and the declared-audio-asset checks. Everything here is parsing, validation and deterministic packing: it executes no payload, runs no compiler and
// reads no file except the explicit project loader. Every rejection carries a STABLE class (`E_*`, schema 3 section 9) plus readable text; the text may change, the class may not.
// Schema 1/2 packages are read only by segarecomp::patch::read_package (patch.hpp) and are untouched by this header.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp::mod {

// ---- stable rejection ----
struct Rejection {
  std::string code;     // e.g. "E_PAYLOAD_KIND_RESERVED"
  std::string message;  // readable, not stable
};

// ---- limits (schema 3 sections 2 and 4) ----
inline constexpr std::size_t kMaxArchiveBytes = 32U << 20U;
inline constexpr std::size_t kMaxManifestBytes = 64U << 10U;
inline constexpr std::size_t kMaxArchiveEntries = 256;
inline constexpr std::size_t kMaxPayloads = 255;
inline constexpr std::size_t kMaxImageBytes = 16U << 20U;       // bps-image payload and source/target image
inline constexpr std::size_t kMaxRomEditBytes = 1U << 20U;
inline constexpr std::size_t kMaxRomEdits = 4096;
inline constexpr std::size_t kMaxGuestCodeBytes = 1U << 20U;    // total
inline constexpr std::size_t kMaxNativeFiles = 64, kMaxNativeFileBytes = 256U << 10U, kMaxNativeTotalBytes = 2U << 20U;
inline constexpr std::size_t kMaxLuauFiles = 64, kMaxLuauFileBytes = 128U << 10U, kMaxLuauTotalBytes = 1U << 20U;
inline constexpr std::size_t kMaxAssets = 128, kMaxAssetBytes = 8U << 20U;
inline constexpr std::size_t kMaxCapabilities = 32, kMaxRevisions = 16, kMaxMappings = 64;
inline constexpr std::uint32_t kMaxAudioSeconds = 600;
inline constexpr std::uint64_t kLuauMemoryPolicyMax = 4194304, kLuauBudgetPolicyMax = 100000;
inline constexpr unsigned kNativeAbiMajor = 2;

// ---- platform adapters (ADR 0280): a table of capability sets, never a class hierarchy ----
enum class Support { unsupported, experimental, supported_with_limitations, supported_and_tested };
[[nodiscard]] const char *support_name(Support level);

struct Adapter {
  std::string_view platform;        // "genesis" | "master-system" | "sega-cd"
  std::string_view source_kind;     // "cartridge-image" | "disc-image"
  unsigned platform_id;             // SegaModPlatform
  std::string_view sdk_header;      // per-platform SDK header file name (platforms/<p>/sdk)
  std::string_view writable_space;  // the one mod-writable space (work RAM)
  std::span<const std::string_view> readable_spaces;
  std::span<const std::string_view> audio_conversions;   // deterministic PCM targets this platform's adapter provides (empty = none)
  std::size_t index;                // column of the capability table
};
[[nodiscard]] const Adapter *find_adapter(std::string_view platform);   // nullptr for an unknown platform: never guessed
[[nodiscard]] std::span<const Adapter> adapters();

struct CapabilityInfo {
  std::string_view name;
  unsigned id;                       // ABI 2 catalogue id (native-sdk-abi-2.md section 4)
  Support level[3];                  // genesis, master-system, sega-cd
  bool luau_bound;                   // a Luau binding exists (otherwise native only)
};
[[nodiscard]] const CapabilityInfo *find_capability(std::string_view name);      // nullptr: not in the catalogue (E_CAPABILITY_UNKNOWN)
[[nodiscard]] std::span<const CapabilityInfo> capability_catalogue();
[[nodiscard]] Support capability_support(const Adapter &adapter, const CapabilityInfo &capability);
// Unknown name -> E_CAPABILITY_UNKNOWN; a REQUIRED name the platform does not support -> E_CAPABILITY_UNSUPPORTED (an optional unsupported name is simply not granted).
[[nodiscard]] std::optional<Rejection> check_capabilities(const Adapter &adapter, const std::vector<std::string> &required, const std::vector<std::string> &optional);

// ---- manifest model (schema 3 section 3) ----
struct Revision {
  std::string label, sha256, mapper;
  bool operator==(const Revision &) const = default;
};

struct Payload {
  std::string kind;                 // bps-image | rom-edit-set | guest-code | native-c11 | luau-source | asset
  unsigned kind_version = 1;
  std::string path;
  std::uint64_t size = 0;
  std::string sha256;
  std::string target_sha256;        // bps-image
  unsigned edits = 0;               // rom-edit-set (informational count)
  std::string cpu;                  // guest-code
  std::string role;                 // native-c11 "provider" | luau-source "script"
  std::string asset_class;          // asset: image | data | audio
  std::string format;               // asset data: token; asset image: "rgba8"
  unsigned width = 0, height = 0;   // asset image
  std::string audio_format;         // asset audio: "wav-pcm-s16le"
  unsigned sample_rate = 0, channels = 0, frames = 0, loop_start = 0, loop_end = 0;
  bool has_loop = false;
  bool operator==(const Payload &) const = default;
};

struct Mapping {                    // modding-audio-architecture.md section 5 (+ SEG-145-T002 asset-binding refinements)
  std::string id, kind;             // kind: command | event | asset-binding
  std::string space;                // command / event
  unsigned offset = 0, width = 0;   // command / event
  std::vector<std::pair<std::string, unsigned>> values;   // command "commands" / event "events": (name, value)
  std::string asset, target, use, revision;   // asset-binding; `use` in kAudioUses; `revision` = a declared revision label ("" = every revision)
  bool has_site = false;            // asset-binding patch site/range
  unsigned site_offset = 0, site_length = 0;
  bool has_bank = false, has_track = false;
  unsigned bank = 0, track = 0;
  bool operator==(const Mapping &) const = default;
};

struct Manifest {
  std::string id, version, name, platform, author, license, purpose;
  std::string source_kind;                       // cartridge-image | disc-image (reserved)
  std::vector<Revision> revisions;
  std::string disc_identity;                     // disc-image only
  std::vector<std::string> capabilities, capabilities_optional;
  unsigned abi_native = 0;                       // 0 = none
  unsigned state_schema = 0;                     // 0 = none
  bool has_limits = false;
  std::uint64_t luau_memory_bytes = 0, luau_budget_units = 0;   // 0 = not requested
  bool has_geometry = false, has_hooks = false;  // reserved keys, present but empty
  std::string default_widescreen, default_side_style;
  std::vector<Payload> payloads;
  std::vector<Mapping> mappings;
  bool operator==(const Manifest &) const = default;
};

inline constexpr std::string_view kAudioUses[] = {"music-sequence", "instrument", "sfx", "pcm-sample", "enhanced-soundtrack"};

// "data" < "luau" < "native" (schema 3 section 4).
[[nodiscard]] std::string trust_class(const Manifest &manifest);
// Canonical manifest bytes (schema 3 section 2: fixed key order, no insignificant whitespace, no trailing newline). Package identity = SHA-256 of these bytes.
[[nodiscard]] std::string serialize_manifest(const Manifest &manifest);
// Content digest (schema 3 section 8): independent of manifest metadata.
[[nodiscard]] std::string content_digest(const Manifest &manifest);

struct Package {
  bool ok = false;
  Rejection rejection;
  Manifest manifest;
  std::string identity;                                   // SHA-256 of the exact manifest bytes received
  std::string content_digest;
  std::map<std::string, std::vector<std::uint8_t>> files;   // payload path -> bytes (size and SHA-256 verified)
};

// Validates the archive, the manifest and every payload against schema 3. Reserved kinds/sources are parsed and rejected.
[[nodiscard]] Package read_package(std::span<const std::uint8_t> zip);
[[nodiscard]] Package read_package_file(const std::filesystem::path &path);
// Cross-checks a manifest against its platform adapter: capability support, space legality of mappings, audio conversion availability. (Pure; used by `mod check`.)
[[nodiscard]] std::optional<Rejection> check_adapter(const Manifest &manifest);
// Canonical stored ZIP (schema 3 section 2): path order, manifest first, fixed times and attributes. Deterministic for equal inputs.
[[nodiscard]] std::vector<std::uint8_t> pack_package(const Manifest &manifest, const std::map<std::string, std::vector<std::uint8_t>> &files);

// ---- native C11 source policy (SEG-135-T003) ----
// Include allowlist: the ABI 2 SDK headers, <stdint.h>/<stddef.h>, and package-local files named by their package path. Anything else (`../`, absolute, other system headers, computed
// includes, digraph/trigraph spellings, #include_next/#import/#embed) is E_INCLUDE_FORBIDDEN. Host-owned compile flags: kNativeCompileFlags.
[[nodiscard]] std::optional<Rejection> check_native_sources(const Manifest &manifest, const std::map<std::string, std::vector<std::uint8_t>> &files, const Adapter &adapter);
inline constexpr std::string_view kNativeCompileFlags[] = {"-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror", "-O0", "-fno-ident", "-c"};

// ---- audio (SEG-145-T002) ----
struct Wav {
  bool ok = false;
  std::string error;
  unsigned sample_rate = 0, channels = 0, frames = 0;
  std::span<const std::uint8_t> data;   // little-endian s16 interleaved
};
[[nodiscard]] Wav parse_wav_pcm_s16le(std::span<const std::uint8_t> bytes);
// Deterministic reference conversion `u8-mono`: channel average (floor), then (sample + 32768) >> 8; same rate. Output is the raw unsigned 8-bit stream.
[[nodiscard]] std::vector<std::uint8_t> convert_pcm_u8_mono(const Wav &wav);

// ---- mod.toml project (SEG-135-T002) ----
struct Project {
  bool ok = false;
  Rejection rejection;
  Manifest manifest;
  std::map<std::string, std::vector<std::uint8_t>> files;   // payload path -> bytes
};
// Parses the closed TOML subset in `text`. `read` supplies the bytes of a project-relative path (bounded by the caller); no ambient data (time, absolute paths) enters the manifest.
using FileReader = std::optional<std::vector<std::uint8_t>> (*)(void *context, const std::string &relative_path, std::size_t limit);
[[nodiscard]] Project parse_project(std::string_view toml, FileReader read, void *context);
// Reads <dir>/mod.toml and its sources from the directory (never outside it).
[[nodiscard]] Project load_project(const std::filesystem::path &dir);
// The full pipeline used by `mod check`: load, build the canonical package, re-read it through the schema 3 reader, then the adapter and source policies.
struct Checked {
  bool ok = false;
  Rejection rejection;
  Project project;
  std::vector<std::uint8_t> package;   // canonical .segapatch bytes
  Package reread;
};
[[nodiscard]] Checked check_project(const std::filesystem::path &dir);
[[nodiscard]] std::string init_project_text(std::string_view platform, std::string_view id);

// ---- variant identity and cache decision (SEG-135-T005) ----
// What a built mod variant depends on, as named components in a FIXED order (a change of any one selects a different key). The package identity already covers every payload digest; the
// explicit source, ABI and capability components exist so a rebuild reason can NAME what changed. Single-mod only: composite identities of mod sets are a later contract.
struct VariantComponent {
  std::string name, value;
  bool operator==(const VariantComponent &) const = default;
};
struct VariantIdentity {
  std::vector<VariantComponent> components;   // original_image, effective_image, package_identity, source_digest, abi, capabilities
  std::string key;                             // SHA-256 over "name=value\n" lines of the components
};
// `package` must come from read_package (its `identity` is SHA-256 of the exact manifest bytes received). `original_sha256` is the exact original image; `effective_sha256` the image that is compiled and run (the bps-image target, else the original).
[[nodiscard]] VariantIdentity variant_identity(const Package &package, std::string_view original_sha256, std::string_view effective_sha256);
// Names of the components whose value differs (empty = identical). A component present in only one list counts as changed.
[[nodiscard]] std::vector<std::string> changed_components(const std::vector<VariantComponent> &previous, const std::vector<VariantComponent> &current);
// "name=value\n" text of the components (the stored variant record) and its inverse; the parser fails closed (empty result) on any malformed line.
[[nodiscard]] std::string serialize_components(const std::vector<VariantComponent> &components);
[[nodiscard]] std::vector<VariantComponent> parse_components(std::string_view text);
// The effective image of a package applied to `original`: a bps-image payload is applied and its target digest verified; otherwise the original. On failure `rejection` is set.
struct EffectiveImage {
  bool ok = false;
  Rejection rejection;
  std::string original_sha256, effective_sha256;
};
[[nodiscard]] EffectiveImage effective_image(const Package &package, std::span<const std::uint8_t> original);

}  // namespace segarecomp::mod
