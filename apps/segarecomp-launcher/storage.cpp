#include "storage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace launcher {
namespace {

std::string utf8_of(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

constexpr std::size_t max_entries_per_tree = 200000;
constexpr int max_depth = 16;

struct Measure {
  std::uintmax_t bytes = 0;
  std::size_t visited = 0;
  bool has_symlink = false, truncated = false;
};

// Recursive size of a directory tree without following links. Bounded in entries and depth; a bound hit marks the result truncated.
void measure_tree(const fs::path &dir, Measure &m, int depth, const std::atomic<bool> *cancel) {
  if (depth > max_depth) { m.truncated = true; return; }
  std::error_code ec;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    if (cancel != nullptr && cancel->load()) { m.truncated = true; return; }
    if (++m.visited > max_entries_per_tree) { m.truncated = true; return; }
    std::error_code sec;
    const auto status = it->symlink_status(sec);
    if (sec) continue;
    if (status.type() == fs::file_type::symlink) { m.has_symlink = true; continue; }
    if (status.type() == fs::file_type::regular) {
      std::error_code zec;
      const auto size = it->file_size(zec);
      if (!zec) m.bytes += size;
    } else if (status.type() == fs::file_type::directory) {
      measure_tree(it->path(), m, depth + 1, cancel);
    }
  }
}

enum class Category { executable, generated_c, objects, logs, metadata, other };

// Debug-symbol files (game.pdb and the like) beside the executable are build intermediates too: nothing at run time needs them.
bool is_pdb(const std::string &name) {
  if (name.size() < 5) return false;
  std::string ext = name.substr(name.size() - 4);
  for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".pdb";
}

Category categorize(const std::string &name) {
  if (name == "game" || name == "game.exe") return Category::executable;
  if (name == "generated.c" || name == "generated" || name == "generated-z80") return Category::generated_c;
  if (name == "obj" || is_pdb(name)) return Category::objects;
  if (name == "build.log" || name == "run.log") return Category::logs;
  if (name == "status.json" || name == "metadata.json") return Category::metadata;
  return Category::other;
}

void add_to(UsageBreakdown &use, Category c, std::uintmax_t bytes) {
  switch (c) {
    case Category::executable: use.executable += bytes; break;
    case Category::generated_c: use.generated_c += bytes; break;
    case Category::objects: use.objects += bytes; break;
    case Category::logs: use.logs += bytes; break;
    case Category::metadata: use.metadata += bytes; break;
    case Category::other: use.other += bytes; break;
  }
}

void measure_build_dir(BuildDirUsage &usage, const std::atomic<bool> *cancel) {
  std::error_code ec;
  for (fs::directory_iterator it(usage.path, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    if (cancel != nullptr && cancel->load()) { usage.truncated = true; return; }
    std::error_code sec;
    const auto status = it->symlink_status(sec);
    if (sec) continue;
    const Category category = categorize(utf8_of(it->path().filename()));
    if (status.type() == fs::file_type::symlink) { usage.has_symlink = true; continue; }
    if (status.type() == fs::file_type::regular) {
      std::error_code zec;
      const auto size = it->file_size(zec);
      if (!zec) add_to(usage.use, category, size);
    } else if (status.type() == fs::file_type::directory) {
      Measure m;
      measure_tree(it->path(), m, 1, cancel);
      add_to(usage.use, category, m.bytes);
      usage.has_symlink = usage.has_symlink || m.has_symlink;
      usage.truncated = usage.truncated || m.truncated;
    }
  }
}

std::uintmax_t measure_path(const fs::path &path) {
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec) return 0;
  if (status.type() == fs::file_type::regular) { const auto size = fs::file_size(path, ec); return ec ? 0 : size; }
  if (status.type() == fs::file_type::directory) { Measure m; measure_tree(path, m, 0, nullptr); return m.bytes; }
  return 0;
}

bool is_dir_real(const fs::path &p) {
  std::error_code ec;
  const auto status = fs::symlink_status(p, ec);
  return !ec && status.type() == fs::file_type::directory;
}

} // namespace

// ---- BusyGuard ----
bool BusyGuard::try_begin_build(const std::string &name) { std::lock_guard<std::mutex> l(mutex_); if (maintenance_) return false; builds_.insert(name); return true; }
void BusyGuard::end_build(const std::string &name) { std::lock_guard<std::mutex> l(mutex_); if (const auto it = builds_.find(name); it != builds_.end()) builds_.erase(it); }
bool BusyGuard::try_begin_run(const std::string &name) { std::lock_guard<std::mutex> l(mutex_); if (maintenance_) return false; runs_.insert(name); return true; }
void BusyGuard::end_run(const std::string &name) { std::lock_guard<std::mutex> l(mutex_); if (const auto it = runs_.find(name); it != runs_.end()) runs_.erase(it); }
bool BusyGuard::try_begin_maintenance() { std::lock_guard<std::mutex> l(mutex_); if (maintenance_ || !builds_.empty() || !runs_.empty()) return false; maintenance_ = true; return true; }
void BusyGuard::end_maintenance() { std::lock_guard<std::mutex> l(mutex_); maintenance_ = false; }
bool BusyGuard::entry_busy(const std::string &name) const {
  std::lock_guard<std::mutex> l(mutex_);
  return builds_.count(name) != 0 || runs_.count(name) != 0;
}
bool BusyGuard::any_build() const { std::lock_guard<std::mutex> l(mutex_); return !builds_.empty(); }
bool BusyGuard::maintenance() const { std::lock_guard<std::mutex> l(mutex_); return maintenance_; }
bool BusyGuard::any_activity() const { std::lock_guard<std::mutex> l(mutex_); return !builds_.empty() || !runs_.empty() || maintenance_; }

// ---- classification ----
DirName parse_dir_name(const std::string &name) {
  DirName result;
  std::string key = name;
  result.kind = DirKind::current;
  for (const char *suffix : {".partial", ".failed"}) {
    const std::string s = suffix;
    if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
      key = name.substr(0, name.size() - s.size());
      result.kind = s == ".partial" ? DirKind::partial : DirKind::failed;
    }
  }
  result.valid = is_hex_digest(key, 32);
  result.key = result.valid ? key : std::string();
  return result;
}

bool is_within(const fs::path &root, const fs::path &path) {
  const fs::path r = root.lexically_normal(), p = path.lexically_normal();
  const fs::path rel = p.lexically_relative(r);
  if (rel.empty() || rel == ".") return false;
  return *rel.begin() != "..";
}

std::string format_bytes(std::uintmax_t bytes) {
  char text[48];
  const double v = static_cast<double>(bytes);
  if (bytes < 1024U) std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
  else if (bytes < 1024U * 1024U) std::snprintf(text, sizeof text, "%.1f KiB", v / 1024.0);
  else if (bytes < 1024ULL * 1024U * 1024U) std::snprintf(text, sizeof text, "%.1f MiB", v / (1024.0 * 1024.0));
  else std::snprintf(text, sizeof text, "%.2f GiB", v / (1024.0 * 1024.0 * 1024.0));
  return text;
}

std::uintmax_t StorageReport::builds_total() const {
  std::uintmax_t total = 0;
  for (const auto &e : entries) total += e.total.total();
  for (const auto &u : unlinked) total += u.use.total();
  return total;
}

const EntryUsage *StorageReport::find(const std::string &id) const {
  for (const auto &e : entries)
    if (e.id == id) return &e;
  return nullptr;
}

StorageReport scan_storage(const Catalog &catalog, const fs::path &cache_root, const fs::path &library_root, const std::atomic<bool> *cancel) {
  StorageReport report;
  std::map<std::string, std::size_t> owner_of_key;  // build key -> entry index
  for (std::size_t i = 0; i < catalog.entries().size(); ++i) {
    const LibraryEntry &e = catalog.entries()[i];
    EntryUsage usage;
    usage.id = e.id();
    report.entries.push_back(std::move(usage));
    if (!e.current_build_key.empty()) owner_of_key[e.current_build_key] = i;
    for (const auto &b : e.builds) owner_of_key.emplace(b.key, i);
  }
  std::error_code ec;
  const fs::path games = cache_root / "games";
  if (is_dir_real(games)) {
    std::size_t visited = 0;
    for (fs::directory_iterator it(games, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && visited < 100000; it.increment(ec), ++visited) {
      if (cancel != nullptr && cancel->load()) return report;  // partial report, complete == false
      if (!is_dir_real(it->path())) continue;                  // symlinks and stray files are neither measured nor touched
      BuildDirUsage dir;
      dir.name = utf8_of(it->path().filename());
      dir.path = it->path();
      const DirName parsed = parse_dir_name(dir.name);
      std::size_t owner = report.entries.size();
      if (parsed.valid) {
        dir.kind = parsed.kind;
        if (const auto found = owner_of_key.find(parsed.key); found != owner_of_key.end()) owner = found->second;
        if (parsed.kind == DirKind::current) {
          if (owner < report.entries.size()) dir.kind = catalog.entries()[owner].current_build_key == parsed.key ? DirKind::current : DirKind::superseded;
          else dir.kind = DirKind::unlinked;
        }
        dir.valid_build = parsed.kind == DirKind::current && entry_ready(dir.path);
      } else {
        dir.kind = DirKind::unlinked;  // unrecognized name: reported, never deleted
      }
      measure_build_dir(dir, cancel);
      if (dir.kind == DirKind::partial || dir.kind == DirKind::failed) report.failed_partial += dir.use.total();
      if (owner < report.entries.size() && dir.kind != DirKind::unlinked) {
        dir.owner_id = report.entries[owner].id;
        EntryUsage &usage = report.entries[owner];
        usage.total.add(dir.use);
        if (dir.kind == DirKind::current || dir.kind == DirKind::superseded) usage.intermediates += dir.use.generated_c + dir.use.objects;
        usage.dirs.push_back(std::move(dir));
      } else if (owner < report.entries.size()) {  // a failed/partial sibling of an owned key keeps its owner for display
        dir.owner_id = report.entries[owner].id;
        report.entries[owner].total.add(dir.use);
        report.entries[owner].dirs.push_back(std::move(dir));
      } else {
        report.unlinked.push_back(std::move(dir));
      }
    }
  }
  const fs::path zig = cache_root / "zig-cache";
  if (is_dir_real(zig)) {
    Measure m;
    measure_tree(zig, m, 0, cancel);
    report.zig_cache = m.bytes;
    report.zig_truncated = m.truncated;
  }
  report.catalog_and_covers = measure_path(library_root / "covers");
  for (const char *name : {"library.json", "library.json.bak"}) report.catalog_and_covers += measure_path(library_root / name);
  const auto space = fs::space(fs::exists(cache_root, ec) ? cache_root : cache_root.parent_path(), ec);
  report.free_bytes = ec ? 0 : space.available;
  report.complete = cancel == nullptr || !cancel->load();
  return report;
}

namespace {

void add_item(Plan &plan, const fs::path &path, bool directory, std::uintmax_t bytes) {
  plan.items.push_back({path, bytes, directory});
  plan.bytes += bytes;
}

const BuildDirUsage *busy_dir(const EntryUsage &entry, const BusyGuard &busy) {
  for (const auto &d : entry.dirs)
    if (busy.entry_busy(parse_dir_name(d.name).key)) return &d;
  return nullptr;
}

} // namespace

Plan plan_intermediates(const StorageReport &report, const std::vector<std::string> &ids, const BusyGuard &busy) {
  Plan plan;
  plan.kind = PlanKind::intermediates;
  for (const auto &id : ids) {
    const EntryUsage *entry = report.find(id);
    if (entry == nullptr) continue;
    if (busy_dir(*entry, busy) != nullptr) { plan.excluded.push_back(id + " (a build or game is running)"); continue; }
    plan.entry_ids.push_back(id);
    for (const auto &d : entry->dirs) {
      if (d.kind != DirKind::current && d.kind != DirKind::superseded) continue;
      for (const char *name : {"generated.c", "generated", "generated-z80", "obj"}) {
        const fs::path path = d.path / name;
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (ec || (status.type() != fs::file_type::regular && status.type() != fs::file_type::directory)) continue;
        add_item(plan, path, status.type() == fs::file_type::directory, measure_path(path));
      }
      std::error_code lec;  // debug-symbol (.pdb) files next to the executable
      for (fs::directory_iterator it(d.path, lec), end; !lec && it != end; it.increment(lec)) {
        std::error_code sec;
        const auto status = it->symlink_status(sec);
        if (sec || status.type() != fs::file_type::regular || !is_pdb(utf8_of(it->path().filename()))) continue;
        add_item(plan, it->path(), false, measure_path(it->path()));
      }
    }
  }
  plan.remains = {"Native executables (games stay playable)", "Library entries, covers and settings", "Build status and logs"};
  plan.notes = {"Generated C sources, object files and debug-symbol (.pdb) files are deleted. They are recreated by the next Recompile."};
  return plan;
}

Plan merge_plans_non_overlapping(Plan primary, const Plan &extra) {
  const auto inside = [](const fs::path &path, const fs::path &dir) {
    const fs::path rel = path.lexically_relative(dir);
    return !rel.empty() && *rel.begin() != "..";  // "." (the same path) is inside too
  };
  const std::size_t primary_count = primary.items.size();
  for (const auto &item : extra.items) {
    bool covered = false;
    for (std::size_t i = 0; i < primary_count && !covered; ++i)
      if (primary.items[i].directory && inside(item.path, primary.items[i].path)) covered = true;
    if (covered) continue;
    primary.items.push_back(item);
    primary.bytes += item.bytes;
  }
  for (const auto &id : extra.entry_ids)
    if (std::find(primary.entry_ids.begin(), primary.entry_ids.end(), id) == primary.entry_ids.end()) primary.entry_ids.push_back(id);
  for (const auto &x : extra.excluded) primary.excluded.push_back(x);
  for (const auto &n : extra.notes) if (std::find(primary.notes.begin(), primary.notes.end(), n) == primary.notes.end()) primary.notes.push_back(n);
  return primary;
}

Plan plan_archive(const StorageReport &report, const std::vector<std::string> &ids, const BusyGuard &busy) {
  Plan plan;
  plan.kind = PlanKind::archive;
  for (const auto &id : ids) {
    const EntryUsage *entry = report.find(id);
    if (entry == nullptr) continue;
    if (busy_dir(*entry, busy) != nullptr) { plan.excluded.push_back(id + " (a build or game is running)"); continue; }
    plan.entry_ids.push_back(id);
    for (const auto &d : entry->dirs) add_item(plan, d.path, true, d.use.total());
  }
  plan.remains = {"Library entries with title, ROM reference, build history, last-played times", "Covers and settings", "Original ROM files (never touched)"};
  plan.notes = {"Native executables and all generated and build files of these games are deleted.", "Each game shows 'Rebuild to play' until it is recompiled."};
  return plan;
}

Plan plan_superseded(const StorageReport &report, const std::vector<std::string> &ids, const BusyGuard &busy) {
  Plan plan;
  plan.kind = PlanKind::superseded;
  for (const auto &id : ids) {
    const EntryUsage *entry = report.find(id);
    if (entry == nullptr) continue;
    if (busy_dir(*entry, busy) != nullptr) { plan.excluded.push_back(id + " (a build or game is running)"); continue; }
    bool any = false;
    for (const auto &d : entry->dirs)
      if (d.kind == DirKind::superseded) { add_item(plan, d.path, true, d.use.total()); any = true; }
    if (any) plan.entry_ids.push_back(id);
  }
  plan.remains = {"The current build of each game", "Library entries, covers and settings"};
  plan.notes = {"Builds produced by an earlier release or option set are deleted; they are no longer used."};
  return plan;
}

Plan plan_failed_partial(const StorageReport &report, bool keep_diagnostics, const BusyGuard &busy) {
  Plan plan;
  plan.kind = PlanKind::failed_partial;
  plan.keep_diagnostics = keep_diagnostics;
  const auto consider = [&](const BuildDirUsage &d) {
    if (d.kind != DirKind::partial && d.kind != DirKind::failed) return;
    if (!parse_dir_name(d.name).valid) return;
    if (busy.entry_busy(parse_dir_name(d.name).key)) { plan.excluded.push_back(d.name + " (in progress)"); return; }
    if (!keep_diagnostics || d.kind == DirKind::partial) {
      add_item(plan, d.path, true, d.use.total());
      return;
    }
    std::error_code ec;  // keep build.log and status.json of a failed build, remove the rest
    for (fs::directory_iterator it(d.path, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string name = utf8_of(it->path().filename());
      if (name == "build.log" || name == "status.json") continue;
      std::error_code sec;
      const auto status = it->symlink_status(sec);
      if (sec || (status.type() != fs::file_type::regular && status.type() != fs::file_type::directory)) continue;
      add_item(plan, it->path(), status.type() == fs::file_type::directory, measure_path(it->path()));
    }
  };
  for (const auto &e : report.entries)
    for (const auto &d : e.dirs) consider(d);
  for (const auto &d : report.unlinked) consider(d);
  plan.remains = {"Every finished build and its executable", keep_diagnostics ? "build.log and status.json of failed builds" : "Nothing from the deleted failed builds", "Library entries and covers"};
  plan.notes = {"Only folders named *.failed and *.partial are deleted; unknown folders are never touched."};
  return plan;
}

Plan plan_zig_cache(const StorageReport &report, const fs::path &cache_root, const BusyGuard &busy) {
  Plan plan;
  plan.kind = PlanKind::zig_cache;
  if (busy.any_build()) { plan.excluded.push_back("compiler cache (a build is running)"); return plan; }
  const fs::path zig = cache_root / "zig-cache";
  if (is_dir_real(zig)) {
    std::error_code ec;
    for (fs::directory_iterator it(zig, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code sec;
      const auto status = it->symlink_status(sec);
      if (sec || (status.type() != fs::file_type::regular && status.type() != fs::file_type::directory)) continue;
      add_item(plan, it->path(), status.type() == fs::file_type::directory, measure_path(it->path()));
    }
  }
  (void)report;
  plan.remains = {"All built games and their executables", "Library entries, covers and settings"};
  plan.notes = {"Shared compiler cache is deleted; it is only an accelerator.", "The next Recompile of any game is slower while the cache is rebuilt."};
  return plan;
}

Plan plan_remove_from_library(const StorageReport &report, const std::vector<std::string> &ids, bool delete_cache, const BusyGuard &busy) {
  Plan plan;
  plan.kind = PlanKind::remove_from_library;
  plan.delete_cache_with_entry = delete_cache;
  for (const auto &id : ids) {
    const EntryUsage *entry = report.find(id);
    if (entry != nullptr && busy_dir(*entry, busy) != nullptr) { plan.excluded.push_back(id + " (a build or game is running)"); continue; }
    plan.entry_ids.push_back(id);
    if (delete_cache && entry != nullptr)
      for (const auto &d : entry->dirs) add_item(plan, d.path, true, d.use.total());
  }
  plan.remains = {"Original ROM files (never touched)", "Cover images in the covers folder", delete_cache ? "Nothing else of these games" : "Their build folders in the cache (unlinked until removed)"};
  plan.notes = {"The library entries, titles, history and settings of these games are deleted."};
  return plan;
}

// ---- execution ----
namespace {

#include "storage_remover.hpp"

// The only shapes a plan item may take, relative to the cache root.
bool item_shape_allowed(const fs::path &relative) {
  std::vector<std::string> parts;
  for (const auto &p : relative) parts.push_back(utf8_of(p));
  if (parts.empty() || parts.size() > 3) return false;
  for (const auto &p : parts)
    if (p.empty() || p == "." || p == "..") return false;
  if (parts[0] == "zig-cache") return parts.size() <= 2;
  if (parts[0] == "games") {
    if (parts.size() < 2 || !parse_dir_name(parts[1]).valid) return false;
    return parts.size() <= 3;
  }
  return false;
}

} // namespace

ExecResult execute_plan(const Plan &plan, const fs::path &cache_root, const BusyGuard &busy) {
  ExecResult result;
  std::error_code ec;
  // Resolve only the trusted prefix OUTSIDE the cache (e.g. macOS /tmp).
  // The cache itself and every descendant must be opened without following links.
  const fs::path absolute_root = fs::absolute(cache_root, ec).lexically_normal();
  const fs::path parent_root = fs::canonical(absolute_root.parent_path(), ec);
  if (ec || parent_root.empty()) { result.problems.push_back("the cache folder cannot be resolved"); return result; }
  const fs::path anchored_root = parent_root / absolute_root.filename();
  if (!busy.maintenance()) { result.problems.push_back("internal: cleanup must hold the maintenance lock"); return result; }
  for (const auto &item : plan.items) {
    const std::string label = utf8_of(item.path.filename());
    const fs::path normal = item.path.lexically_normal();
    if (!is_within(cache_root.lexically_normal(), normal)) { result.problems.push_back("refused (outside the cache): " + label); continue; }
    const fs::path relative = normal.lexically_relative(cache_root.lexically_normal());
    if (!item_shape_allowed(relative)) { result.problems.push_back("refused (not a managed item): " + label); continue; }
    const auto first = relative.begin();
    if (*first == "games") {
      auto it = relative.begin();
      ++it;
      const DirName name = parse_dir_name(utf8_of(*it));
      if (busy.entry_busy(name.key)) { result.problems.push_back("busy: " + utf8_of(*it)); continue; }
    } else if (busy.any_build()) {
      result.problems.push_back("busy: the compiler cache is in use");
      continue;
    }
    Remover remover{result};
    remover.remove(anchored_root, relative, item.directory);
  }
  return result;
}

void apply_plan_to_catalog(const Plan &plan, Catalog &catalog) {
  if (plan.kind == PlanKind::archive) {
    for (const auto &id : plan.entry_ids)
      if (LibraryEntry *entry = catalog.find_id(id)) entry->archived = true;
  } else if (plan.kind == PlanKind::remove_from_library) {
    for (const auto &id : plan.entry_ids) catalog.remove(id);
  }
}

std::string describe_policy(const CleanupPolicy &policy) {
  if (policy.mode == "intermediates") return "After every successful build, generated C and object files of that build are deleted. Executables are kept.";
  if (policy.mode == "keep_last_n")
    return "After every successful build, games beyond the newest " + std::to_string(policy.keep_last_n) +
           " playable ones are reduced to metadata only (build deleted, entry kept). Favorites, running games and games being built are never reduced.";
  if (policy.mode == "soft_limit")
    return "When builds use more than " + std::to_string(policy.soft_limit_mb) + " MiB, the launcher shows a reminder and opens Storage. Nothing is deleted without confirmation.";
  return "Nothing is cleaned automatically. Use Manage Storage to reclaim space.";
}

std::vector<std::string> policy_eviction_candidates(const Catalog &catalog, const StorageReport &report, const CleanupPolicy &policy, const std::set<std::string> &active_ids) {
  std::vector<std::string> result;
  if (policy.mode != "keep_last_n" || policy.keep_last_n < 1) return result;
  struct Row { std::string id; long long recency; bool favorite; };
  std::vector<Row> rows;
  for (const auto &entry : catalog.entries()) {
    const EntryUsage *usage = report.find(entry.id());
    if (usage == nullptr) continue;
    const bool playable = std::any_of(usage->dirs.begin(), usage->dirs.end(), [](const BuildDirUsage &d) { return d.kind == DirKind::current && d.valid_build; });
    if (!playable) continue;
    rows.push_back({entry.id(), std::max({entry.last_played_at, entry.last_built_at, entry.added_at}), entry.favorite});
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.recency != b.recency ? a.recency > b.recency : a.id < b.id; });
  for (std::size_t i = static_cast<std::size_t>(policy.keep_last_n); i < rows.size(); ++i)
    if (!rows[i].favorite && active_ids.count(rows[i].id) == 0) result.push_back(rows[i].id);
  return result;
}

} // namespace launcher
