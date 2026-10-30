#pragma once

// Library storage inventory and the fail-closed maintenance matrix. Everything here operates ONLY on enumerated descendants of the
// Segarecomp cache root (`<cache>/games/<32 hex>[.partial|.failed]/...` and `<cache>/zig-cache`). It never follows a symlink, never
// touches an original ROM, a cover, the catalog or a screenshot, and refuses anything a running build or game owns. No window, no SDL.

#include "library_catalog.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace launcher {

// ---- the one explicit busy guard shared by BuildJob / GameRun starters and cleanup ----
class BusyGuard {
public:
  bool try_begin_build(const std::string &entry_name);   // false while maintenance runs
  void end_build(const std::string &entry_name);
  bool try_begin_run(const std::string &entry_name);
  void end_run(const std::string &entry_name);
  bool try_begin_maintenance();                          // false while any build or run is active
  void end_maintenance();
  [[nodiscard]] bool entry_busy(const std::string &name) const;  // `name` is a bare key; its .partial/.failed siblings are included
  [[nodiscard]] bool any_build() const;
  [[nodiscard]] bool maintenance() const;
  [[nodiscard]] bool any_activity() const;
private:
  mutable std::mutex mutex_;
  std::multiset<std::string> builds_, runs_;
  bool maintenance_ = false;
};

struct UsageBreakdown {
  std::uintmax_t executable = 0, generated_c = 0, objects = 0, logs = 0, metadata = 0, other = 0;
  [[nodiscard]] std::uintmax_t total() const { return executable + generated_c + objects + logs + metadata + other; }
  void add(const UsageBreakdown &o) { executable += o.executable; generated_c += o.generated_c; objects += o.objects; logs += o.logs; metadata += o.metadata; other += o.other; }
};

enum class DirKind { current, superseded, partial, failed, unlinked };

struct BuildDirUsage {
  std::string name;               // directory name inside games/
  std::filesystem::path path;
  DirKind kind = DirKind::unlinked;
  std::string owner_id;           // library entry id, empty when unlinked
  UsageBreakdown use;
  bool has_symlink = false;       // contains a link: reported, never followed or removed
  bool truncated = false;         // the bounded traversal stopped early (sizes are lower bounds)
  bool valid_build = false;       // executable + status ok
  std::string busy_key;           // the BusyGuard name that protects this folder ("<32 hex>" for a native game, "scd-<16 hex>" for a Sega CD game)
};

struct EntryUsage {
  std::string id;
  std::vector<BuildDirUsage> dirs;
  UsageBreakdown total;
  std::uintmax_t intermediates = 0;   // what "Remove build intermediates" would reclaim
  // SEG-109-T004 (ADR 0253): the player's own per-game data (a Sega CD game's backup RAM and input recordings). Reported, never part of `total` and never
  // reachable from a Plan: plans are built from `dirs` only, and execute_plan refuses every path that is not a generated build folder.
  std::uintmax_t user_data = 0;
};

struct StorageReport {
  std::vector<EntryUsage> entries;     // parallel to the catalog order at scan time
  std::vector<BuildDirUsage> unlinked; // folders no library entry owns (shown, never deleted except failed/partial ones)
  std::uintmax_t zig_cache = 0;
  std::uintmax_t scd_object_cache = 0;   // SEG-109-T004: the shared Sega CD object cache (<cache>/sega-cd-objects), an accelerator like zig-cache
  bool zig_truncated = false;
  std::uintmax_t failed_partial = 0;
  std::uintmax_t catalog_and_covers = 0;
  std::uintmax_t free_bytes = 0;
  bool complete = false;               // false for a cancelled scan
  [[nodiscard]] std::uintmax_t builds_total() const;
  [[nodiscard]] const EntryUsage *find(const std::string &id) const;
};

// Bounded read-only traversal (blocking: run it on a worker). `cancel` may be null.
[[nodiscard]] StorageReport scan_storage(const Catalog &catalog, const std::filesystem::path &cache_root, const std::filesystem::path &library_root,
                                         const std::atomic<bool> *cancel = nullptr);

enum class PlanKind { intermediates, archive, superseded, failed_partial, zig_cache, remove_from_library };

struct PlanItem {
  std::filesystem::path path;
  std::uintmax_t bytes = 0;
  bool directory = false;
};

struct Plan {
  PlanKind kind = PlanKind::intermediates;
  std::vector<std::string> entry_ids;
  std::vector<PlanItem> items;
  std::uintmax_t bytes = 0;
  std::vector<std::string> remains;   // what will be kept
  std::vector<std::string> notes;     // irreversible effects, exclusions, warnings
  std::vector<std::string> excluded;  // names skipped (busy)
  bool delete_cache_with_entry = false;  // remove_from_library only
  bool keep_diagnostics = false;         // failed_partial only
};

[[nodiscard]] Plan plan_intermediates(const StorageReport &report, const std::vector<std::string> &ids, const BusyGuard &busy);
[[nodiscard]] Plan plan_archive(const StorageReport &report, const std::vector<std::string> &ids, const BusyGuard &busy);
[[nodiscard]] Plan plan_superseded(const StorageReport &report, const std::vector<std::string> &ids, const BusyGuard &busy);
// One plan from two: `primary` plus every item of `extra` that is not the same path as, or inside, a directory `primary` already deletes
// (a parent and its child must never both be scheduled: the child would be gone when its turn comes). Notes/remains/exclusions are merged.
[[nodiscard]] Plan merge_plans_non_overlapping(Plan primary, const Plan &extra);
[[nodiscard]] Plan plan_failed_partial(const StorageReport &report, bool keep_diagnostics, const BusyGuard &busy);
[[nodiscard]] Plan plan_zig_cache(const StorageReport &report, const std::filesystem::path &cache_root, const BusyGuard &busy);
[[nodiscard]] Plan plan_remove_from_library(const StorageReport &report, const std::vector<std::string> &ids, bool delete_cache, const BusyGuard &busy);

struct ExecResult {
  std::uintmax_t bytes_freed = 0;
  std::size_t removed = 0;
  std::vector<std::string> problems;   // skipped / denied / unexpected items, one line each
  bool ok() const { return problems.empty(); }
};

// Re-validates every item against the canonical cache root (or the library root for Sega CD build folders) and removes it. Must run with BusyGuard::try_begin_maintenance() held by the
// caller. Items that are busy, symlinks, outside the managed set or of an unexpected type are skipped and reported.
// `library_root` (optional): where a Sega CD game's generated build folders (<library>/sega-cd/<id16>/{build,build.partial,build.failed}) live; the library
// folder may be outside the cache. Nothing else under it is ever removable (saves, recordings, logs are refused by shape).
[[nodiscard]] ExecResult execute_plan(const Plan &plan, const std::filesystem::path &cache_root, const BusyGuard &busy, const std::filesystem::path &library_root = {});

// Applies the catalog-side consequence of a plan after a successful execution (marks entries archived, removes library records).
void apply_plan_to_catalog(const Plan &plan, Catalog &catalog);

// ---- path classification (public so tests and the UI can reuse the exact rules) ----
// "<32 hex>" -> current key; "<32 hex>.partial" / ".failed" -> those; anything else -> false.
struct DirName { bool valid = false; std::string key; DirKind kind = DirKind::unlinked; };
[[nodiscard]] DirName parse_dir_name(const std::string &name);
// True when `path` is a STRICT descendant of `root` by lexical normalization (a `..` escape, `root` itself or an unrelated path is false).
[[nodiscard]] bool is_within(const std::filesystem::path &root, const std::filesystem::path &path);

// ---- cleanup policy (settings) ----
struct CleanupPolicy {
  std::string mode = "never";   // never | intermediates | keep_last_n | soft_limit
  int keep_last_n = 10;         // keep_last_n: executables kept, newest played/built first
  long long soft_limit_mb = 4096;
};
[[nodiscard]] std::string describe_policy(const CleanupPolicy &policy);
// Entry ids whose build the `keep_last_n` policy would archive (never an `active` id, never one without a valid build). Pure.
[[nodiscard]] std::vector<std::string> policy_eviction_candidates(const Catalog &catalog, const StorageReport &report, const CleanupPolicy &policy,
                                                                  const std::set<std::string> &active_ids);

[[nodiscard]] std::string format_bytes(std::uintmax_t bytes);

} // namespace launcher
