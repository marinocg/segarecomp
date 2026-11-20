#pragma once

// SEG-150-T008 (ADR 0317): where the launcher's data lives, and the crash-recoverable move of it. Core only: no window, no SDL, no UI. A worker thread
// drives it (plan -> run_migration with a cancel token and a progress callback); the next start calls recover_pending_migration() before any root is used.
//
// "Data root" = the directory that is today's default cache root (`<root>/library` is the durable library, `<root>/games` and friends are regenerable).
// A tiny bootstrap file OUTSIDE every movable root names the active data root; an absent bootstrap means today's defaults. The environment overrides
// SEGARECOMP_CACHE_DIR / SEGARECOMP_LIBRARY_DIR always win, and a path they govern is never migrated (the plan refuses and says which path is effective).

#include "storage.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

// ---- environment and bootstrap ----
struct DataEnvironment {
  std::optional<std::string> cache_dir;    // SEGARECOMP_CACHE_DIR (set, even empty, means overridden)
  std::optional<std::string> library_dir;  // SEGARECOMP_LIBRARY_DIR
};

enum class BootstrapState { absent, valid, damaged, newer_schema };
struct Bootstrap {
  BootstrapState state = BootstrapState::absent;
  std::filesystem::path root;   // valid only
  std::string detail;
};
[[nodiscard]] Bootstrap read_bootstrap(const std::filesystem::path &file);
// Atomic (temporary file + rename in the same directory). False with `error` when it cannot be written; the previous bootstrap is then unchanged.
[[nodiscard]] bool write_bootstrap(const std::filesystem::path &file, const std::filesystem::path &root, std::string *error = nullptr);

enum class RootSource { default_location, bootstrap, environment };
struct ResolvedRoots {
  std::filesystem::path cache, library;       // the effective locations
  RootSource cache_source = RootSource::default_location, library_source = RootSource::default_location;
  Bootstrap bootstrap;
  bool bootstrap_ignored = false;             // a bootstrap exists but the environment (or an unavailable root) took precedence
  std::string notice;                         // "" when nothing needs saying
};
// Pure apart from reading the bootstrap file and testing that a bootstrapped root is a directory. A bootstrapped root that is not available (unplugged drive)
// falls back to the default location with `bootstrap_ignored` and a notice: nothing is created at the missing location and the old data stays reachable.
[[nodiscard]] ResolvedRoots resolve_roots(const DataEnvironment &env, const std::filesystem::path &default_cache, const std::filesystem::path &bootstrap_file);

// ---- plan ----
enum class MigrationRefusal {
  env_override_cache, env_override_library, bootstrap_unusable, journal_present, previous_migration_pending_cleanup, source_missing, source_symlink,
  source_unsupported_entry, source_unsafe_name, source_too_large, destination_not_absolute, destination_unsafe, destination_same, destination_inside_source,
  source_inside_destination, destination_symlink, destination_not_directory, destination_not_empty, destination_not_writable, insufficient_space
};
[[nodiscard]] const char *refusal_name(MigrationRefusal refusal);   // the stable lower-case code

struct Refused { MigrationRefusal code; std::string detail; };

enum class ItemClass { durable, regenerable, left_behind };
// The data-class areas (one stable name each, the table in the ADR): catalog, settings, bios_profiles, covers, managed_roms, mods, saves, recordings, sega_cd_data,
// builds, compiler_cache, downloads, catalog_backup, other.
struct MigrationItem {
  std::filesystem::path rel;     // relative to the data root, e.g. library/covers/a.png (files only)
  std::uintmax_t bytes = 0;
  std::string area;
};
struct AreaTotal { std::string area; ItemClass cls = ItemClass::durable; std::size_t entries = 0; std::uintmax_t bytes = 0; };

struct MigrationOptions {
  std::filesystem::path source;                 // the effective data root (ResolvedRoots::cache)
  std::filesystem::path destination;
  DataEnvironment env;
  std::filesystem::path bootstrap_file;
  std::vector<std::filesystem::path> protected_paths;   // a destination equal to or inside one of these is unsafe (e.g. the installation directory)
  std::function<std::uintmax_t(const std::filesystem::path &)> free_space;   // test seam; default: the filesystem's available space at the nearest existing ancestor
  std::size_t max_files = 100000;
};

struct MigrationPlan {
  std::filesystem::path source, destination;
  std::vector<MigrationItem> items;             // durable files, sorted by relative path (what will be copied, verified and rebased)
  std::vector<AreaTotal> areas;                 // durable, regenerable and left-behind totals, for display
  std::uintmax_t durable_bytes = 0, regenerable_bytes = 0, required_bytes = 0, available_bytes = 0;
  std::size_t regenerable_entries = 0;          // build folders / caches that stay in the old root: those games need a rebuild at the new location
  std::vector<std::string> left_behind;         // relative names that are neither migrated nor regenerable (never moved, never removed)
  std::vector<Refused> refusals;
  std::vector<std::string> notes;
  [[nodiscard]] bool ok() const { return refusals.empty(); }
};
[[nodiscard]] MigrationPlan build_migration_plan(const MigrationOptions &options, const std::atomic<bool> *cancel = nullptr);

// ---- execution ----
struct MigrationProgress {
  std::string phase;                            // plan | copy | rebase | verify | commit | done
  std::size_t files_done = 0, files_total = 0;
  std::uintmax_t bytes_done = 0, bytes_total = 0;
  std::string current;                          // the relative file being processed ("" between files)
};
using MigrationProgressFn = std::function<void(const MigrationProgress &)>;

// Test seams only. `crash_at` simulates the process dying: it returns true to stop at this point, leaving the journal and the half-built destination exactly as a
// crash would. Points: "copy" (index = files copied so far), "rebase", "verify", "commit_before", "commit_after". `after_copy` runs after each file was copied.
struct MigrationHooks {
  std::function<bool(const char *point, std::size_t index)> crash_at;
  std::function<void(const std::filesystem::path &copied)> after_copy;
};

enum class MigrationOutcome { completed, refused, busy, cancelled, failed, interrupted };
struct MigrationResult {
  MigrationOutcome outcome = MigrationOutcome::failed;
  std::string reason;                           // stable code for failed/refused: a refusal name, source_changed, copy_failed, rebase_failed, verify_failed, journal_write_failed, bootstrap_write_failed, ...
  std::string detail;                           // one human line
  std::vector<Refused> refusals;                // outcome == refused
  std::string migration_id;
  std::size_t files = 0;
  std::uintmax_t bytes = 0;
  std::size_t rebased_paths = 0;                // catalog references that pointed at a managed copy under the old root and now point at the new one
  std::size_t cleared_programs = 0;             // Sega CD imported programs that lived in a regenerable build folder: reference cleared, the game needs a rebuild
  std::size_t entries_needing_rebuild = 0;      // library entries with a recorded build; builds are not migrated
  std::vector<std::string> notes;
  [[nodiscard]] bool ok() const { return outcome == MigrationOutcome::completed; }
};
// Blocking: run it on a worker. Takes BusyGuard maintenance for its whole duration (busy while any build, game, import or maintenance runs). Plans again itself.
// Nothing at the old root is changed. Cancel and every failure before the commit roll the destination back (only files this journal created) and leave the
// bootstrap alone; the commit is exactly the atomic bootstrap write. The new root takes effect at the next start.
[[nodiscard]] MigrationResult run_migration(const MigrationOptions &options, BusyGuard &busy, const std::atomic<bool> *cancel = nullptr,
                                            const MigrationProgressFn &progress = {}, const MigrationHooks *hooks = nullptr);

// ---- recovery at startup ----
enum class RecoveryAction { none, rolled_back, completed, kept_completed, owner_alive, journal_invalid, rollback_incomplete };
struct RecoveryResult {
  RecoveryAction action = RecoveryAction::none;
  std::string detail;
  std::string source, destination;
};
// Call once at startup before any root is used. Deterministic: before the commit the old root stays authoritative and the destination is cleaned only through the
// journal's own manifest and ownership marker; after the commit (bootstrap names the destination) the new root is authoritative. Never throws, never touches the source.
[[nodiscard]] RecoveryResult recover_pending_migration(const std::filesystem::path &bootstrap_file);

// ---- completed migration bookkeeping and the old-root cleanup (a separate, explicit operation) ----
struct CompletedMigration {
  bool present = false;
  std::filesystem::path source, destination;
  std::string id;
};
[[nodiscard]] CompletedMigration read_completed_migration(const std::filesystem::path &bootstrap_file);
// Forgets a completed migration (or a damaged journal) without touching either root: the old root is then never offered for cleanup again.
[[nodiscard]] bool dismiss_completed_migration(const std::filesystem::path &bootstrap_file);

enum class CleanupKind { regenerable, migrated_copy };
struct CleanupItem {
  std::filesystem::path path;
  std::uintmax_t bytes = 0;
  bool directory = false;
  CleanupKind kind = CleanupKind::regenerable;
  std::string area;
};
struct CleanupPlan {
  bool eligible = false;
  std::string reason;                           // when not eligible: not_completed | bootstrap_mismatch | old_root_active | old_root_missing
  std::vector<CleanupItem> items;
  std::uintmax_t bytes = 0;
  std::vector<std::string> kept;                // what stays and why (changed since migration, symlink inside, unknown, ...)
};
// Lists what removing the old root's managed items would delete. Regenerable items (build folders, zig-cache, Sega CD object cache, cover downloads) are always
// listed; `include_migrated_copies` adds the durable files whose verified copy exists at the new root unchanged. Saves, recordings, ROMs and approvals are never listed
// otherwise. Anything else in the old root (unknown files, the catalog backup) is kept.
[[nodiscard]] CleanupPlan plan_old_root_cleanup(const std::filesystem::path &bootstrap_file, const DataEnvironment &env, bool include_migrated_copies);
struct CleanupResult {
  std::size_t removed = 0;
  std::uintmax_t bytes_freed = 0;
  std::vector<std::string> problems;
  bool refused = false;
  std::string reason;
  bool ok() const { return !refused && problems.empty(); }
};
// Re-plans, re-validates every item (lexically and physically inside the old root, never a symlink) and removes only those. Takes BusyGuard maintenance.
[[nodiscard]] CleanupResult run_old_root_cleanup(const std::filesystem::path &bootstrap_file, const DataEnvironment &env, bool include_migrated_copies, BusyGuard &busy,
                                                 const std::atomic<bool> *cancel = nullptr);

} // namespace launcher
