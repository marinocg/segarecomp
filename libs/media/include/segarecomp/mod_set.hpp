#pragma once

// Ordered mod sets (SEG-141, ADR 0326): the persisted set definition, the direct compatibility check (dependencies, incompatibilities, version ranges: nothing is resolved, selected or downloaded),
// the verified composition of the members' guest edits on the exact original image, and the composite variant identity. Pure functions over verified packages (no file access, no compiler, no
// clock). There is ONE engine for every platform: what differs per platform comes from the adapter table and from the conflict claims' address spaces. This engine does not run mods: callback
// dispatch and hook ownership between members are not judged here.

#include "segarecomp/mod.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp::mod {

inline constexpr std::size_t kMaxSetMembers = 16;
inline constexpr std::size_t kMaxSetDefinitionBytes = 4096;
inline constexpr unsigned kSetDefinitionFormat = 1;   // the persisted definition
inline constexpr unsigned kSetIdentityFormat = 1;     // the composite identity derivation (a component, so a change of the rule changes every composite key)

// ---- persisted definition ----
// Canonical text, LF-terminated lines in exactly this order: `segarecomp-modset=1`, `platform=<platform>`, then 1..16 `member=<package identity>` lines IN SET ORDER. A definition names packages by
// their exact identity only (never by id, version or path). Anything else is refused: E_SET_VERSION (another format number), E_SET_SYNTAX, E_SET_SIZE, E_SET_DUPLICATE_MOD, E_PLATFORM_UNKNOWN.
struct SetDefinition {
  std::string platform;
  std::vector<std::string> members;
  bool operator==(const SetDefinition &) const = default;
};
struct SetDefinitionResult {
  bool ok = false;
  Rejection rejection;
  SetDefinition definition;
};
[[nodiscard]] std::string serialize_set_definition(const SetDefinition &definition);
[[nodiscard]] SetDefinitionResult parse_set_definition(std::string_view text);
[[nodiscard]] bool set_name_ok(std::string_view name);   // the name of a stored definition: [a-z0-9]+(-[a-z0-9]+)*, at most 64

// ---- installed packages ----
// The verification boundary of a stored package: the archive `bytes` are parsed and must be the package of exactly `identity` (E_PACKAGE_CHANGED otherwise, or the reader's own class). The bytes that
// come back are the bytes that were verified; a caller compiles and publishes THESE and never reads the file again, so nothing else can be recorded under the identity. A refusal returns no bytes.
struct VerifiedPackage {
  bool ok = false;
  Rejection rejection;
  Package package;
  std::vector<std::uint8_t> bytes;
};
[[nodiscard]] VerifiedPackage verify_package_bytes(std::vector<std::uint8_t> bytes, std::string_view identity);

// ---- direct compatibility check (SEG-141-T004) ----
// One violated rule. `mods` names the package ids concerned (the declaring or offending mod first); `message` is readable, `code` is stable.
struct SetViolation {
  std::string code;
  std::vector<std::string> mods;
  std::string message;
};
// Every violated rule of an ordered set, in a deterministic order: E_SET_SIZE, then in member order E_SET_DUPLICATE_MOD and E_SET_PLATFORM, then per member (declaration order) E_SET_MISSING_DEPENDENCY,
// E_SET_VERSION_MISMATCH and E_SET_INCOMPATIBLE. A dependency is satisfied by any member of that id in the range, wherever it stands: the set order never installs or reorders anything.
[[nodiscard]] std::vector<SetViolation> check_set_relations(std::span<const Manifest *const> members);
// A mod used ALONE is a one-member set: the first violated rule (a single-mod build never silently ignores a declared requirement), or nothing.
[[nodiscard]] std::optional<Rejection> check_standalone(const Manifest &manifest);

// ---- source identity ----
// What every member of a set must share. Cartridge platforms: the exact original image plus the declared mapper configuration. Disc platforms: the disc identity plus the BIOS requirement (the disc
// source kind is reserved in schema 3, so no disc PACKAGE can be read yet; the identity text is defined here so that the one engine already keys it).
struct SetSource {
  std::string kind;                                // "cartridge-image" | "disc-image"
  std::string image_sha256, mapper;                // cartridge
  std::string disc_identity, bios_requirement;     // disc
};
[[nodiscard]] std::string source_identity_text(const SetSource &source);
// SHA-256 over the adapter's `capability=support-level` lines of the whole catalogue: a product whose platform adapter offers another capability set keys every composite anew.
[[nodiscard]] std::string adapter_capability_digest(const Adapter &adapter);

// ---- conflict claims (SEG-141-T002) ----
// A range one member owns in one address space. Different spaces never overlap each other: `cartridge-image` (offsets of the cartridge image, bank-qualified on Master System by image_location),
// `z80.sound_ram` (the Genesis sound-CPU addresses injected Z80 code is assembled for) and, once disc payload kinds exist, one space per file/extent (`disc-file:<path>`). `cpu` and `kind` are diagnostic
// and rule selection.
struct SetClaim {
  std::size_t member = 0;
  std::string space, cpu, kind;   // kind: "write" (edit or injected block) | "hook-site"
  std::uint64_t start = 0, end = 0;
};
struct SetOverlap {
  SetClaim first, second;         // first.member < second.member: the claims of the lowest overlapping range of this pair
  std::uint64_t start = 0, end = 0;
  std::size_t ranges = 1;         // how many overlapping claim pairs this entry stands for
};
// Every overlap between claims of DIFFERENT members in the same space where at least one side writes: exactly one entry per (pair of members, space, which side writes), so no colliding pair is ever
// left out and the report stays bounded by the member count. Ordered by (members, space, start).
[[nodiscard]] std::vector<SetOverlap> find_overlaps(std::span<const SetClaim> claims);

// ---- composition ----
struct SetComposition {
  bool ok = false;
  std::vector<SetViolation> violations;            // every violation found (byte overlaps: one per colliding pair of members and space); empty iff ok
  SetSource source;
  std::vector<std::string> member_effective;       // each member's own effective image digest against the original (set order)
  std::string original_sha256, effective_sha256;
  std::vector<std::uint8_t> image;                 // the composed image; empty unless ok
  std::vector<SetClaim> claims;
  std::vector<std::string> warnings;
  bool header_repaired = false;
};
// Verifies the ordered set against one exact original image and composes the members' image changes. Every member is first built ALONE against the unmodified original (so every expected-value
// assertion is evaluated against the pre-composition bytes); the writes must then be disjoint; the platform's header repair runs once over the composed image. Nothing is merged or reordered:
// E_SET_SOURCE (a member does not declare this image, or declares another mapper), E_SET_EDIT_OVERLAP, E_SET_BPS_CONFLICT (a bps-image declares no write ranges, so it composes only with members
// that neither change the image nor declare hook sites), E_SET_HOOK_SITE (a write covers another member's hooked entry), E_SET_HOOK_OWNER (two members hook one function of one CPU and one of them
// replaces it), E_SET_GUEST_ADMISSION (Master System: the injected code of two members is
// not admitted together), E_SET_HEADER_POLICY, E_HEADER_FIELD_COLLISION, plus every relation rule above and each member's own refusal class. `members` come from read_package.
[[nodiscard]] SetComposition compose_set(std::span<const Package *const> members, std::span<const std::uint8_t> original, GuestAdmitter admit = nullptr);

// ---- composite variant identity (SEG-141-T001) ----
// `member_variants[i]` is variant_identity() of members[i] ALONE against the same original (with its Luau bytecode digest). A ONE-member set returns it unchanged, component for component, so a
// single mod keeps exactly its SEG-135-T005 identity and cache entry. Otherwise the components are, in order: set_format, platform, source_kind, source_identity, adapter_capabilities,
// membership (order-independent), effective_image, then `member.<i>` = `<package identity>:<single-mod key>` IN SET ORDER. The key is SHA-256 over the "name=value" lines.
// `composition` must be ok. An empty result (no key) means the arguments do not belong together.
[[nodiscard]] VariantIdentity set_identity(std::span<const Package *const> members, std::span<const VariantIdentity> member_variants, const SetComposition &composition);

}  // namespace segarecomp::mod
