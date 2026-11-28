#pragma once

// SEG-122 (ADR 0266): installing and removing widescreen patch packages of one library game. Orchestration only: every verification is the
// shared libs/media implementation (`segarecomp patch ...` runs the same code). A package is data; installing it never changes the original
// game file, never selects the Patched variant and never starts a build.

#include "library_catalog.hpp"
#include "segarecomp/mod_set.hpp"
#include "segarecomp/patch.hpp"

#include <filesystem>
#include <string>

namespace launcher {

struct PatchPreview {
  bool ok = false;               // the package verifies completely against THIS game's original image
  std::string error;             // plain-language refusal when !ok
  segarecomp::patch::PackageInfo info;   // valid when the package itself parsed (even if it does not fit this game)
  bool package_valid = false;
  bool fits_game = false;
  bool already_installed = false;
  // SEG-143-T008: set for a schema 3 mod package ("luau" | "native"); `info` then carries what the approval sheet shows (name, author, license, ABI) and `effective_sha256` is the image the build compiles.
  bool schema3 = false;
  std::string mod_trust, effective_sha256;
};

// Full verification without installing anything: ZIP structure, manifest, payload digest, the exact original bytes, BPS application, patched digest.
[[nodiscard]] PatchPreview preview_patch(const LibraryEntry &entry, const std::filesystem::path &package, const std::filesystem::path &original_rom);

struct PatchInstall {
  bool ok = false;
  bool added = false;            // false: the identical package was already installed
  std::string error;
  std::string identity;
};

// Verifies exactly like preview_patch, then publishes a copy of the very bytes that were verified to `<library_root>/patches/<identity>.segapatch`
// (temporary file + rename) and records it on the entry. A failure leaves neither a file nor a record behind.
// A package that carries executable source (a trusted native-source patch) is installed only with `trust_native` (the user's explicit approval of that exact package identity, given by the
// code-bearing install sheet); installing the same package again with `trust_native` approves an unapproved record. The approval never transfers to another identity.
// The launch gate of the selected variant: `launch_refusal` plus, for a schema 3 mod, the reconciliation of its presentation defaults with the freshly VERIFIED stored package (a record written by an
// earlier release has none or outdated ones). The defaults reach `view` before the launch and the entry's record is corrected in place; `*record_changed` tells the caller to save the catalog (a
// verified, unchanged record is never rewritten). Returns "" or the plain-language refusal (nothing is modified on a refusal).
[[nodiscard]] std::string prepare_variant_launch(LibraryEntry &entry, RomView &view, bool *record_changed);

PatchInstall install_patch(LibraryEntry &entry, const std::filesystem::path &package, const std::filesystem::path &original_rom,
                           const std::filesystem::path &library_root, long long now, bool trust_native = false);

// Removes the record and the installed package file; selects Vanilla if this patch was selected. The original ROM, the Vanilla build, the
// user's settings and recordings are untouched. Its published build stays in the cache until storage cleanup removes it (it is no longer current).
bool remove_patch(LibraryEntry &entry, const std::string &identity, const std::filesystem::path &library_root, std::string *error = nullptr);

// SEG-141-T001 (ADR 0326): ordered mod sets of one library game. The definition is the SHARED persisted format of segarecomp/mod_set.hpp (the one `segarecomp mod set` reads and writes), stored as
// `<library_root>/mod-sets/<game sha256>/<name>.modset`; every check is the shared engine. A set is stored only when each member is an installed schema 3 mod of this game whose stored package still
// verifies, and the whole set composes on the exact original image. The launcher does not SELECT, build or show a set (`segarecomp mod set build` builds one).
struct ModSet {
  bool ok = false;
  std::string error;                                   // plain-language refusal naming EVERY violated rule
  segarecomp::mod::SetDefinition definition;           // set order
  std::vector<segarecomp::mod::SetViolation> violations;
  std::string effective_sha256;                        // the composed image
};
// Replaces an existing definition of the same name; a refusal writes nothing.
[[nodiscard]] ModSet store_mod_set(const LibraryEntry &entry, const std::string &name, const std::vector<std::string> &identities, const std::filesystem::path &original_rom,
                                   const std::filesystem::path &library_root);
// Reads the stored definition and verifies it again from scratch (a removed or changed member is a refusal, never a smaller set).
[[nodiscard]] ModSet load_mod_set(const LibraryEntry &entry, const std::string &name, const std::filesystem::path &original_rom, const std::filesystem::path &library_root);

// SEG-141-T006: the launcher's plain-language words for one composition failure class: what went wrong and what the player can do. Every class the engine and the set build can report has its own
// text; an unknown class gets the generic one. The launcher has no mod-set screen: these texts are what ModSet::error carries.
struct ModSetClassText { std::string title, next; };
[[nodiscard]] ModSetClassText mod_set_class_text(const std::string &code);

// "<name> <version>" label of an installed patch.
[[nodiscard]] std::string patch_label(const InstalledPatch &patch);

// SEG-150-T006: the vocabulary of the per-game Mods tab. Pure functions of recorded facts (nothing is guessed: an unknown author or capability list is reported as unknown).
// Mods are admitted for Genesis games only (verify_mod / install_patch refuse every other platform); "" = available, otherwise the plain-language reason.
[[nodiscard]] std::string mods_unavailable_reason(const std::string &platform_id);
// What the package carries, from the record: "Native C source (runs with host-process permissions)", "Luau script", "ROM data edits" or "Patched game image".
[[nodiscard]] std::string mod_kind_label(const InstalledPatch &patch);
// The capabilities the record names ("work RAM snapshot, VDP state, ..."); "" when the record does not hold them (a schema 3 mod names its capabilities only on the install sheet).
[[nodiscard]] std::string mod_capabilities_label(const InstalledPatch &patch);

// The action that follows a refusal (the refusal text itself stays the product's typed message): keyed on what the refusal says, never invented for an unknown one.
[[nodiscard]] std::string mod_refusal_next_step(const std::string &refusal);

enum class ModTone { neutral, ok, warn, err };
struct ModStatusInput {
  bool selected = false;
  bool built = false;             // a published build of THIS mod exists
  bool package_present = true;    // the installed package file is a regular file in the library
  bool failed = false;            // the last preparation of the selected variant failed
  std::string failure;            // its plain-language reason
};
struct ModStatus {
  std::string code;               // stable: installed | installed_prepared | selected_needs_preparation | selected_prepared | needs_approval | incompatible_package_missing | preparation_failed
  std::string label;              // what the row says
  std::string reason;             // typed reason for a blocked state ("" otherwise)
  std::string next;               // the action that resolves it ("" when nothing is needed)
  ModTone tone = ModTone::neutral;
};
[[nodiscard]] ModStatus mod_status(const InstalledPatch &patch, const ModStatusInput &in);

}  // namespace launcher
