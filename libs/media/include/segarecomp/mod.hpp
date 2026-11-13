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
inline constexpr std::size_t kMaxSymbols = 255;   // manifest `symbols` entries (ADR 0290)
inline constexpr std::size_t kMaxHooks = 64;      // manifest `hooks` entries (ADR 0298)
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
  bool rom_big_endian;              // default byte order of a typed `u16`/`u32` ROM edit: the main CPU's data order (genesis 68000 = big, master-system Z80 = little)
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

// A per-revision guest symbol (ADR 0290): a NAME (a token, never disassembly) mapped to an address in one declared revision, with how that address was established. Only `verified`
// entries may be a patch site; `manual` (author-entered, unchecked) and `imported-hint` (tool output) entries are recorded and shown but never resolved for an edit.
struct Symbol {
  std::string name, revision, provenance;   // provenance in kSymbolProvenance
  std::uint32_t address = 0;
  bool operator==(const Symbol &) const = default;
};
inline constexpr std::string_view kSymbolProvenance[] = {"manual", "verified", "imported-hint"};

// A statically resolved guest-function hook declaration (ADR 0298, mod-execution-phases.md section 5). `symbol` names a VERIFIED symbol of the revision (`revision` empty = every declared revision);
// `mode` is before | after | before-after | replace. `reads` are register-view names the callback may read (d0..d7, a0..a7, ccr, cycles, stack0..stack7); a replace hook additionally declares
// `writes` (d0..d7, a0..a6, ccr) and the guest `cost` in cycles charged for the replaced function. Names are sorted ascending without duplicates (a canonical manifest).
struct Hook {
  std::string id, symbol, revision, cpu, mode;
  std::vector<std::string> reads, writes;
  bool has_replace = false;
  std::uint32_t cost = 0;
  bool operator==(const Hook &) const = default;
};
inline constexpr std::string_view kHookModes[] = {"before", "after", "before-after", "replace"};

// A declared guest-memory range a mod's scheduled callbacks may touch (SEG-138-T009, ADR 0300). `revision` empty = every declared revision. `space` is one of the platform's writable/readable spaces
// (Genesis: `m68k.work_ram`); `access` is `read`, `write` or `read-write` and must be backed by the matching declared capability. The scheduler denies every access outside a declared range.
struct MemoryRange {
  std::string revision, space, access;
  std::uint32_t offset = 0, length = 0;
  bool operator==(const MemoryRange &) const = default;
};
inline constexpr std::string_view kMemoryAccess[] = {"read", "write", "read-write"};
inline constexpr std::size_t kMaxMemoryRanges = 16;   // per manifest (the scheduler holds at most 16 per mod)

// Geometry declaration (SEG-139-T004, ADR 0302): the display-aspect range a package supports and, per named scene, the components it covers. A declaration is a CLAIM the host checks against what a
// frame actually delivered; it never grants anything. `aspect_min` (default 4:3 when empty) and `aspect_max` are "N:M" ratios inside 4:3..32:9; scenes require `aspect_max`.
struct GeometryScene {
  std::string id;                        // a token [a-z0-9_-], at most 32
  std::vector<std::string> components;   // kGeometryComponents, strictly ascending, at least one
  bool operator==(const GeometryScene &) const = default;
};
inline constexpr std::string_view kGeometryComponents[] = {"actors", "hud", "scenery"};
inline constexpr std::size_t kMaxGeometryScenes = 32;

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
  bool has_geometry = false, has_hooks = false;  // `geometry` is a reserved key (present but empty); `hooks` holds the declarations below
  std::string aspect_min, aspect_max;           // geometry declaration ("" = not declared); see GeometryScene
  std::vector<GeometryScene> geometry_scenes;    // strictly sorted by id in a canonical manifest
  std::string default_widescreen, default_side_style;
  std::vector<Payload> payloads;
  std::vector<Mapping> mappings;
  std::vector<Symbol> symbols;                   // sorted by (name, revision) in a canonical manifest
  std::vector<Hook> hooks;                       // sorted by id in a canonical manifest (ADR 0298)
  std::vector<MemoryRange> memory;               // sorted by (revision, space, offset, length, access) in a canonical manifest (ADR 0300)
  std::string header_repair;                     // "" (default), "genesis-checksum" or "none" (ADR 0293)
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

// ---- typed ROM edits (SEG-136-T001) and symbol resolution (SEG-136-T002) ----
// A `rom-edit-set` payload is ASCII text, LF-separated, one edit per line (blank lines and `#` comment lines allowed):
//   <revision|*> <type> <site> <expected> <value>
// type: u8 | u16 | u32 (the platform's default byte order, Adapter::rom_big_endian) | u16be u16le u32be u32le | bytes.
// site: `0x` + 1..8 lower-case hex digits (a literal image offset), or `@name` / `@name+0xN` (a verified symbol of the revision plus an addend).
// expected, value: integer types `0x` + exactly 2*width lower-case hex digits; `bytes` plain hex of equal even length. `expected` is asserted against the exact original image before `value` replaces it.
// Stable classes: E_EDIT_SYNTAX, E_EDIT_REVISION, E_EDIT_COUNT, E_EDIT_OVERLAP, E_EDIT_RANGE, E_EDIT_EXPECTED_MISMATCH, E_SYMBOL_UNKNOWN, E_SYMBOL_NOT_VERIFIED, E_SYMBOL_DUPLICATE, E_SYMBOL_REVISION.
struct RomEdit {
  std::string revision;             // "" = every declared revision (`*`)
  std::string symbol;               // "" = literal site
  std::uint32_t site = 0;           // literal image offset, or the addend of `symbol`
  std::vector<std::uint8_t> expected, value;   // already in image byte order; equal length >= 1
  std::size_t line = 0;             // 1-based source line (diagnostics only)
};
struct RomEditSet {
  bool ok = false;
  Rejection rejection;
  std::vector<RomEdit> edits;
};
// Syntax and revision labels only (no symbols, no image).
[[nodiscard]] RomEditSet parse_rom_edit_set(std::span<const std::uint8_t> text, const Manifest &manifest);
// The package-level check run by the reader and `mod check`: syntax, the informational `edits` count, and for EVERY declared revision independently: symbol resolution (verified only) and overlap.
[[nodiscard]] std::optional<Rejection> check_rom_edit_set(const Manifest &manifest, std::span<const std::uint8_t> text, unsigned declared_edits);
struct EditedImage {
  bool ok = false;
  Rejection rejection;
  std::vector<std::uint8_t> image;
  unsigned applied = 0;
};
// Applies the edits of one declared revision to `original` (which must be that revision's exact image: the caller selected `revision` by digest). Every `expected` is asserted first; any mismatch or range
// error fails closed and yields no image. Edits of other revisions are not looked at.
[[nodiscard]] EditedImage apply_rom_edits(const Manifest &manifest, std::span<const std::uint8_t> text, std::string_view revision, std::span<const std::uint8_t> original);
// Parses a symbol-hint list (`name address` or CSV `name,address[,...]`, `0x` hex or decimal, `#` comments, quoted CSV fields) into imported-hint symbols for `revision`. Names are lower-cased and
// folded to the token alphabet; an unparsable line rejects the whole input. Never produces `verified`.
struct SymbolHints {
  bool ok = false;
  Rejection rejection;
  std::vector<Symbol> symbols;
};
[[nodiscard]] SymbolHints parse_symbol_hints(std::string_view text, std::string_view revision);
// Token alphabet and size of a symbol name (lower-case a-z 0-9 `_` `-` `.`, 1..64).
[[nodiscard]] bool symbol_name_ok(std::string_view name);

// Resolved byte ranges ([start, end)) of the edits that apply to one declared revision, already checked for overlap; and the lookup of a VERIFIED symbol (others never resolve).
struct Range {
  std::uint64_t start = 0, end = 0;
};
struct EditRanges {
  bool ok = false;
  Rejection rejection;
  std::vector<Range> ranges;
};
[[nodiscard]] EditRanges rom_edit_ranges(const Manifest &manifest, std::span<const std::uint8_t> text, std::string_view revision);
[[nodiscard]] std::optional<std::uint32_t> verified_symbol(const Manifest &manifest, const std::string &name, const std::string &revision, Rejection &why);

// Declaration check run by the reader and `mod check`: known space of the platform, non-empty and inside the space, 16/32-bit friendly (no alignment is imposed on the range itself), a declared revision label,
// no exact duplicate, and an access backed by the declared `memory.read.<space>` / `memory.write.<space>` capability. Stable classes: E_MEMORY_RANGE, E_MEMORY_SPACE, E_MEMORY_ACCESS, E_MEMORY_REVISION,
// E_MEMORY_DUPLICATE, E_MEMORY_COUNT, E_CAPABILITY_INCONSISTENT.
[[nodiscard]] std::optional<Rejection> check_memory(const Manifest &manifest);
// Declaration check run by the reader and `mod check` (SEG-139-T004): ratios are canonical decimal "N:M" (no leading zeros, terms 1..4096) inside 4:3..32:9 with min <= max; scenes require
// `aspect_max`, are at most 32, have token ids in strictly ascending order and at least one component each, components from kGeometryComponents strictly ascending. A geometry object that
// declares nothing (`{"scenes":[]}`) stays legal and means "no claim". Stable classes: E_GEOMETRY_ASPECT, E_GEOMETRY_SCENE.
[[nodiscard]] std::optional<Rejection> check_geometry(const Manifest &manifest);
// The compact text the viewer host reads (`min=4:3;max=16:9;scene=id:comp+comp`, see libs/viewer sv_geomdecl_parse), or "" when no range is declared. Pure.
[[nodiscard]] std::string geometry_declaration_text(const Manifest &manifest);
// The ranges that apply to one revision label (its own plus those declared for every revision), in canonical order.
[[nodiscard]] std::vector<MemoryRange> memory_for_revision(const Manifest &manifest, std::string_view revision);

// ---- function hooks (SEG-138-T004, ADR 0298) ----
// Register-view bit of a name (mod-execution-phases.md 5.3): d0..d7 = 0..7, a0..a7 = 8..15, ccr = 16, cycles = 17, stack0..stack7 = 24..31. Writable names are d0..d7, a0..a6, ccr.
[[nodiscard]] std::optional<std::uint32_t> hook_register_bit(std::string_view name, bool writable);
// One hook resolved for one revision: symbol -> image address through a VERIFIED symbol, masks from the names.
struct ResolvedHook {
  std::string id, cpu, mode;
  std::uint32_t entry = 0, reads = 0, writes = 0, cost = 0;
  bool replace_declared = false, flags_written = false;
};
struct ResolvedHooks {
  bool ok = false;
  Rejection rejection;
  std::string revision;
  std::vector<ResolvedHook> hooks;   // in manifest (id) order
};
// Declaration-level check run by the reader and `mod check` for EVERY declared revision: platform/CPU, ids, modes, register names, replacement contract, revision labels, verified symbols (E_SYMBOL_*)
// and per-revision site overlap. Stable classes: E_HOOK_COUNT, E_HOOK_ID, E_HOOK_MODE, E_HOOK_CPU_UNSUPPORTED, E_HOOK_OVERLAY_UNRESOLVED, E_HOOK_REGISTER, E_HOOK_REPLACE_CONTRACT, E_HOOK_REVISION,
// E_HOOK_MID_INSTRUCTION (odd address), E_HOOK_OVERLAP.
[[nodiscard]] std::optional<Rejection> check_hooks(const Manifest &manifest);
[[nodiscard]] ResolvedHooks resolve_hooks(const Manifest &manifest, std::string_view revision);
// The revision is selected by the image's SHA-256 (as everywhere in schema 3); another image is E_HOOK_DIGEST.
[[nodiscard]] ResolvedHooks resolve_hooks_for_image(const Manifest &manifest, std::span<const std::uint8_t> image);
// Canonical text of the resolved hook table (the hook identity: ids, addresses, modes, masks, costs), "" when there is none.
[[nodiscard]] std::string hook_table_text(const ResolvedHooks &resolved);

// ---- guest assembly, bounded injection, collisions, header repair (SEG-136-T003..T005, ADR 0292/0293) ----
// A `guest-asm` payload (kind_version 1, `cpu` m68k | z80) is ASCII text assembled by the in-tree bounded assembler (no includes, macros, host paths or clock). Statements, one per line, `;` comments:
//   free <revision|*> <start> <length> <fill>   a free range of image offsets (0x lower-case hex); its ORIGINAL bytes must all equal <fill> when an image is built
//   org <revision|*> <offset> [at <address>]    starts a block placed at that image offset (execution address = offset, or `at` where the CPU address differs)
//   <label>:  <mnemonic>[.size] <operands>      instructions of the supported subset; dc.b/dc.w/dc.l (m68k) or db/dw (z80); `even`; `data` / `code` mode switches
// Operands may use `@symbol[+N]` (a verified symbol of the revision). Stable classes: E_GUEST_SYNTAX, E_GUEST_REVISION, E_GUEST_OPERAND, E_GUEST_LABEL_UNKNOWN, E_GUEST_LABEL_DUPLICATE,
// E_GUEST_VALUE_RANGE, E_GUEST_ALIGN, E_GUEST_ADDRESS, E_GUEST_BRANCH_RANGE, E_GUEST_BRANCH_TARGET, E_GUEST_FORBIDDEN_INSTRUCTION, E_GUEST_WRITE_OUTSIDE_RAM, E_GUEST_OUTSIDE_FREE_RANGE,
// E_GUEST_RANGE_NOT_FREE, E_GUEST_COLLISION, E_GUEST_RANGE, E_MAPPER_UNSUPPORTED, E_HEADER_UNSUPPORTED.
inline constexpr std::size_t kMaxGuestBlocks = 64, kMaxGuestFreeRanges = 64, kMaxGuestLines = 8192;
// Image-independent check run by the reader for EVERY declared revision: syntax, assembly, static validation, free-range membership and collisions with the revision's ROM edits.
// `edit_set` is the rom-edit-set payload text (empty when the package has none).
[[nodiscard]] std::optional<Rejection> check_guest_asm(const Manifest &manifest, std::string_view cpu, std::span<const std::uint8_t> text, std::span<const std::uint8_t> edit_set);

// Header repair of a Genesis image (documented header: ROM start 0x1A0, ROM end 0x1A4, checksum 0x18E over the big-endian words from 0x200 to ROM end).
[[nodiscard]] bool genesis_header_verifies(std::span<const std::uint8_t> image);
[[nodiscard]] std::uint16_t genesis_checksum(std::span<const std::uint8_t> image, std::uint32_t end_exclusive);

// The verified target image of one declared revision: typed edits, then injected guest code, then header repair. `original` must be that revision's exact image (selected by digest).
struct TargetImage {
  bool ok = false;
  Rejection rejection;
  std::string revision;
  std::vector<std::uint8_t> image;
  unsigned edits_applied = 0, blocks_injected = 0;
  std::size_t injected_bytes = 0;
  bool header_repaired = false;
  std::vector<std::string> warnings;   // stable warning classes, e.g. W_CHECKSUM_NOT_REPAIRED
};
[[nodiscard]] TargetImage build_target_image(const Manifest &manifest, const std::map<std::string, std::vector<std::uint8_t>> &files, std::span<const std::uint8_t> original);
[[nodiscard]] bool has_target_edits(const Manifest &manifest);   // a rom-edit-set or guest-asm payload exists

// Deterministic BPS of `original` -> that target, verified by applying it back (SEG-136-T006).
struct TargetBps {
  bool ok = false;
  Rejection rejection;
  std::string revision, source_sha256, target_sha256, patch_sha256;
  std::vector<std::uint8_t> patch;
  TargetImage target;
};
[[nodiscard]] TargetBps build_target_bps(const Manifest &manifest, const std::map<std::string, std::vector<std::uint8_t>> &files, std::span<const std::uint8_t> original);

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
// `luau_bytecode_digest` (SEG-137-T003, Luau policy section 2.5): the digest of the trusted compile step's output, appended as the `luau_bytecode` component when the package has Luau (empty = none, no component).
[[nodiscard]] VariantIdentity variant_identity(const Package &package, std::string_view original_sha256, std::string_view effective_sha256, std::string_view luau_bytecode_digest = {});
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
  std::vector<std::uint8_t> image;   // the effective image bytes (SEG-137-T007: what a generated-native build compiles); equals the original when the package changes nothing
};
[[nodiscard]] EffectiveImage effective_image(const Package &package, std::span<const std::uint8_t> original);

}  // namespace segarecomp::mod
