#pragma once

// The persistent game library: one small JSON file per user, outside every disposable build directory. An entry is keyed by
// `platform + ROM SHA-256` (never by a title); build identities (the cache directory names produced by entry_dir()) are only
// recorded against it. Original ROMs are referenced by path and never copied. Orchestration data only: no recompilation logic.

#include "cartridge_recognition.hpp"
#include "launcher_core.hpp"
#include "sega_cd.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

struct BuildRecord {
  std::string key;        // cache directory name (entry_dir(...).filename()), 32 hex characters
  long long built_at = 0; // unix seconds of the successful build (0 when unknown)
  std::string mode;       // compatibility | optimized
  std::string version;    // launcher/product version that produced it
  std::string toolchain;  // e.g. "zig 0.15.2"
  std::string variant;    // SEG-122: package identity of a Patched build (empty = the Vanilla build); recorded so no build can pass for another variant
};

// SEG-122 (ADR 0266): one installed widescreen patch package of a game. The package file itself lives in `<library>/patches/<identity>.segapatch`; this
// record is what was verified at install time (the original game's exact digest matched, the BPS applied, every digest agreed).
struct InstalledPatch {
  std::string identity;       // SHA-256 of the package manifest (names every payload and target digest)
  std::string id, version, name;
  std::string target_sha256;  // verified digest of the patched image (a native-only package leaves the image unchanged: this is the original's digest)
  // SEG-124 (ADR 0267): a package with a `native` payload CARRIES EXECUTABLE SOURCE (a trusted native-source patch). Recorded at install from the verified package; the build re-verifies the
  // package itself. `native_approved` is the user's explicit approval of THIS exact package identity (set only by the code-bearing install sheet; another package never inherits it).
  std::string native_source_sha256;
  unsigned native_abi = 0, native_caps = 0;
  std::string author, license;
  bool native_approved = false;
  [[nodiscard]] bool has_native() const { return !native_source_sha256.empty(); }
  std::string widescreen, side_style;   // the package's typed presentation defaults (closed name sets, "" = none)
  int width_max = -1;         // the package's stated validated Widescreen Width (0..100), -1 = not stated; informational only (never drives capability)
  long long installed_at = 0;
  std::string build_key;      // the published patched build (entry_dir name), "" = not built
};

struct LibraryEntry {
  std::string platform_id;      // genesis | master-system | sega-cd
  std::string sha256;           // the ROM image digest (64 lowercase hex); Sega CD: scd::entry_identity(BIOS SHA-256, CUE SHA-256)
  std::string detected_title;   // from the image/file name when added
  std::string display_title;    // user edited; empty means "use detected_title"
  std::vector<std::string> rom_paths;  // tracked locations (references); the first is the primary
  bool rom_path_unknown = false;       // migrated from a cache record that never stored a path
  // Last-used per-game build options (the identity-relevant ones are re-derived through entry_dir()).
  // Master System: the declared mapper ("" until declared). Genesis (SEG-122-T009, ADR 0268): the effective cartridge declaration, "lock_on" or "" for the standard cartridge.
  std::string mapper;
  // Genesis: the user chose `mapper` in Advanced build options, so "" means an explicit Standard and no recognition may change it. Otherwise `mapper` is the last automatic result.
  bool mapper_explicit = false;
  std::string mapper_manifest_sha256;
  // SEG-069-T002: Optimized is the launcher default for both platforms (the intent is kept even while the Master System capability is still
  // unknown or unsupported: availability is resolved separately and never rewrites this field). Genesis Optimized means the former
  // Optimized + Experimental model (gen2); the model is not a user choice any more. Compatibility is an explicit selection.
  std::string aot_policy = "optimized";
  std::string aot_model = "gen2";
  bool hint_alias_discovery = false;
  std::string region = "usa";   // Genesis run-time region
  std::string cover;            // file name inside the covers directory; "" shows the generic placeholder
  bool favorite = false;
  bool archived = false;        // the user kept metadata only: the build was deleted on purpose
  long long added_at = 0, last_built_at = 0, last_played_at = 0;
  std::string last_run;         // "" | clean | failed | unadmitted_pc
  int last_exit_code = 0;
  std::string current_build_key;       // the build the entry last produced or adopted
  std::vector<BuildRecord> builds;     // history, oldest first, bounded
  // SEG-122: installed patch packages (Genesis only, at most max_patches) and the selected launch variant ("" = Vanilla, else an installed identity).
  // Per-game widescreen preferences stay keyed by this entry's original identity and are shared by every variant.
  std::vector<InstalledPatch> patches;
  std::string selected_variant;
  [[nodiscard]] const InstalledPatch *find_patch(const std::string &identity) const;
  [[nodiscard]] const InstalledPatch *selected_patch() const { return selected_variant.empty() ? nullptr : find_patch(selected_variant); }
  // Is `key` the build the entry currently stands behind (Vanilla's current build or an installed patch's published build)?
  [[nodiscard]] bool is_current_key(const std::string &key) const;
  // SEG-078-T002 (ADR 0204): Sega CD only (platform_id "sega-cd"): the referenced BIOS and disc, the imported program and the last typed stop.
  // Never written for another platform, so a Genesis / Master System record is byte-for-byte what it was before.
  scd::EntryRecord sega_cd;
  // SEG-122-T009: session state, never saved. The ROM was already examined for its cartridge hardware, and why a look-alike image was not recognized.
  bool cartridge_examined = false;
  std::string cartridge_note;

  [[nodiscard]] std::string id() const { return platform_id + ":" + sha256; }
  [[nodiscard]] const std::string &title() const { return display_title.empty() ? detected_title : display_title; }
};

// SEG-122-T009: what the original ROM says about a Genesis entry's cartridge hardware. `verified` only for a file whose SHA-256 is this entry's: a changed or missing file says nothing.
struct CartridgeCheck {
  bool examined = false;   // some recorded location could be read
  bool verified = false;   // `finding` describes this game
  CartridgeFinding finding;
};
// Reads small headers only, unless an image could change the stored declaration. Safe off the main thread; touches nothing.
[[nodiscard]] CartridgeCheck check_entry_cartridge(const LibraryEntry &entry);
// What an inspection of the original file found (inspect_rom_file ran the recognition).
[[nodiscard]] CartridgeFinding finding_of(const RomView &rom);
// Stores a finding unless the user's explicit choice outranks it. True when the declaration changed, which selects a different build identity.
bool apply_cartridge_finding(LibraryEntry &entry, const CartridgeFinding &finding);

// The Advanced "Cartridge hardware" control: 0 = Automatic (the next status refresh examines the ROM again; a missing ROM keeps the stored declaration), 1 = Standard, 2 = Lock-on.
void select_cartridge(LibraryEntry &entry, int choice);
// The one line that names the effective cartridge hardware; "" for an ordinary game whose hardware needs no mention.
[[nodiscard]] std::string cartridge_status_text(const LibraryEntry &entry);

enum class CatalogLoad { created, loaded, recovered_from_backup, recovered_empty, read_only_newer };

class Catalog {
public:
  // The newest schema this release reads and writes. A library without a Sega CD game is still written as schema 1 (byte-compatible with every earlier
  // release); the first Sega CD game makes it schema 2, which an older release opens read-only instead of damaging (ADR 0204).
  static constexpr int schema_version = 3;  // 3: a library with an installed patch (SEG-122); older releases open it read-only
  static constexpr std::size_t max_entries = 5000, max_paths = 8, max_builds = 16, max_patches = 4;

  explicit Catalog(std::filesystem::path file);
  // Reads the catalog. A missing file is a new empty library; a corrupt file is moved aside (`.corrupt`) after trying `.bak`, never
  // deleted; a file written by a newer schema is shown but never overwritten (read_only()).
  CatalogLoad load();
  // Atomic: writes a temporary file beside the catalog, keeps the previous one as `.bak`, then renames over it.
  [[nodiscard]] bool save(std::string *error = nullptr);
  [[nodiscard]] bool read_only() const { return read_only_; }
  [[nodiscard]] const std::string &notice() const { return notice_; }
  [[nodiscard]] const std::filesystem::path &file() const { return file_; }

  [[nodiscard]] std::vector<LibraryEntry> &entries() { return entries_; }
  [[nodiscard]] const std::vector<LibraryEntry> &entries() const { return entries_; }
  [[nodiscard]] LibraryEntry *find(const std::string &platform_id, const std::string &sha256);
  [[nodiscard]] LibraryEntry *find_id(const std::string &id);

  struct ImportResult { LibraryEntry *entry = nullptr; bool added = false; bool path_added = false; std::string error; };
  // Adds the ROM (or only a new tracked location of an existing one). Never builds, never copies the ROM. A duplicate content digest
  // is the same logical game.
  ImportResult import_rom(const RomView &rom, long long now);
  // SEG-078-T002: adds a Sega CD game (or updates the references and program of the existing one with the same identity). Never copies a file.
  ImportResult import_sega_cd(const scd::EntryRecord &record, const std::string &title, long long now);
  // Rebinds an entry to a relocated file after the caller re-hashed it; refuses a different digest.
  [[nodiscard]] bool relocate(LibraryEntry &entry, const std::filesystem::path &path, const std::string &sha256_of_file);
  bool remove(const std::string &id);

  void record_build(LibraryEntry &entry, const BuildRecord &record);
  void record_run(LibraryEntry &entry, int exit_code, const std::string &verdict, long long now);
  // Imports verifiable product-owned cache records (`<cache>/games/<32 hex>/metadata.json` of a finished build) as entries without a ROM
  // path. Anything unverifiable is ignored. Returns the number of entries added.
  std::size_t migrate_cache(const std::filesystem::path &cache_root, long long now);

private:
  std::filesystem::path file_;
  std::vector<LibraryEntry> entries_;
  bool read_only_ = false;
  std::string notice_;
  [[nodiscard]] bool parse_text(const std::string &text, std::string *error);
};

// A RomView for the entry built from stored fields only (no file access). `path` is the first tracked location. SEG-122: when the entry has a
// selected installed patch the view is that Patched variant (`library_root` locates the installed package); otherwise it is Vanilla.
[[nodiscard]] RomView view_from_entry(const LibraryEntry &entry);
// The same view forced to a variant (identity "" = Vanilla; an identity that is not installed yields Vanilla).
[[nodiscard]] RomView view_for_variant(const LibraryEntry &entry, const std::string &identity);
// The installed copy of a package: `<library_root>/patches/<identity>.segapatch`.
[[nodiscard]] std::filesystem::path patch_package_path(const std::string &identity);
[[nodiscard]] bool is_hex_digest(const std::string &text, std::size_t length);

} // namespace launcher
