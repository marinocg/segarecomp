#include "data_location.hpp"

#include "json_lite.hpp"
#include "library_catalog.hpp"
#include "sega_cd.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <tuple>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace launcher {

namespace {

constexpr std::uintmax_t hash_limit = std::uintmax_t(1) << 40U;
constexpr std::size_t max_scan_entries = 1000000;
constexpr long long stale_after_seconds = 24LL * 3600LL;

std::string u8(const fs::path &p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
std::string gen(const fs::path &p) { const std::u8string s = p.generic_u8string(); return std::string(s.begin(), s.end()); }
fs::path from_u8(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool read_text(const fs::path &file, std::string &out, std::size_t limit = 8U << 20U) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return out.size() <= limit;
}

bool write_text_atomic(const fs::path &file, const std::string &text, std::string *error) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = fs::path(file).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) { fs::remove(tmp, ec); if (error) *error = "cannot write " + u8(tmp); return false; }
  }
  fs::rename(tmp, file, ec);
  if (ec) { fs::remove(tmp, ec); if (error) *error = "cannot replace " + u8(file) + ": " + ec.message(); return false; }
  return true;
}

long long now_seconds() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

long long own_pid() {
#if defined(_WIN32)
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(getpid());
#endif
}
bool process_alive(long long pid) {
  if (pid <= 0) return false;
#if defined(_WIN32)
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(h, &code) != 0 && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
#else
  return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

bool path_is_link(const fs::path &p) {
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(p, ec);
  return !ec && fs::is_symlink(st);
}

bool same_place(const fs::path &a, const fs::path &b) {
  std::error_code ec;
  return fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec) && !ec;
}

fs::path nearest_existing(fs::path p) {
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) {
    const fs::path parent = p.parent_path();
    if (parent == p) break;
    p = parent;
  }
  return p;
}

// ---- the journal files live beside the bootstrap, outside every movable root ----
fs::path journal_path(const fs::path &bootstrap) { return fs::path(bootstrap).concat(".journal"); }
fs::path manifest_path(const fs::path &bootstrap) { return fs::path(bootstrap).concat(".manifest"); }

struct Journal {
  std::string id, phase;      // phase: copy | verify | commit | done
  fs::path source, dest;
  bool dest_created = false;
  long long pid = 0, started = 0;
  std::size_t files = 0;
  std::uintmax_t bytes = 0;
};
enum class JournalState { absent, ok, invalid, newer };

JournalState read_journal(const fs::path &bootstrap, Journal &j) {
  std::string text;
  std::error_code ec;
  if (!fs::exists(journal_path(bootstrap), ec)) return JournalState::absent;
  if (!read_text(journal_path(bootstrap), text, 1U << 20U)) return JournalState::invalid;
  const auto root = json::parse(text);
  if (!root || !root->is_object()) return JournalState::invalid;
  if (root->num("schema", 0) > 1) return JournalState::newer;
  if (root->num("schema", 0) != 1) return JournalState::invalid;
  j.id = root->str("id");
  j.phase = root->str("phase");
  j.source = from_u8(root->str("source"));
  j.dest = from_u8(root->str("destination"));
  j.dest_created = root->flag("destination_created");
  j.pid = root->num("pid");
  j.started = root->num("started");
  j.files = static_cast<std::size_t>(root->num("files"));
  j.bytes = static_cast<std::uintmax_t>(root->num("bytes"));
  const bool id_ok = j.id.size() == 16 && std::all_of(j.id.begin(), j.id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  const bool phase_ok = j.phase == "copy" || j.phase == "verify" || j.phase == "commit" || j.phase == "done";
  if (!id_ok || !phase_ok || !j.source.is_absolute() || !j.dest.is_absolute()) return JournalState::invalid;
  return JournalState::ok;
}

bool write_journal(const fs::path &bootstrap, const Journal &j, std::string *error) {
  json::Value root = json::Value::make_object();
  root.set("schema", json::Value::make_int(1));
  root.set("id", json::Value::make_string(j.id));
  root.set("phase", json::Value::make_string(j.phase));
  root.set("source", json::Value::make_string(u8(j.source)));
  root.set("destination", json::Value::make_string(u8(j.dest)));
  root.set("destination_created", json::Value::make_bool(j.dest_created));
  root.set("pid", json::Value::make_int(j.pid));
  root.set("started", json::Value::make_int(j.started));
  root.set("files", json::Value::make_int(static_cast<long long>(j.files)));
  root.set("bytes", json::Value::make_int(static_cast<long long>(j.bytes)));
  return write_text_atomic(journal_path(bootstrap), json::dump(root), error);
}

void remove_journal(const fs::path &bootstrap) {
  std::error_code ec;
  fs::remove(journal_path(bootstrap), ec);
  fs::remove(manifest_path(bootstrap), ec);
}

struct ManifestEntry { std::uintmax_t size = 0; std::string sha; fs::path rel; };   // sha "-" until verified

bool safe_rel(const fs::path &rel) {
  if (rel.empty() || rel.is_absolute() || rel.has_root_name()) return false;
  for (const auto &part : rel) if (part == ".." || part == "." || part.empty()) return false;
  return true;
}

bool write_manifest(const fs::path &bootstrap, const std::string &id, const std::vector<ManifestEntry> &entries, std::string *error) {
  std::string text = "segarecomp-migration-manifest 1 " + id + "\n";
  for (const auto &e : entries) text += std::to_string(e.size) + "\t" + e.sha + "\t" + gen(e.rel) + "\n";
  return write_text_atomic(manifest_path(bootstrap), text, error);
}

bool read_manifest(const fs::path &bootstrap, const std::string &id, std::vector<ManifestEntry> &out) {
  std::string text;
  if (!read_text(manifest_path(bootstrap), text, 256U << 20U)) return false;
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line) || line != "segarecomp-migration-manifest 1 " + id) return false;
  while (std::getline(in, line)) {
    const std::size_t a = line.find('\t');
    const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) return false;
    ManifestEntry e;
    try { e.size = static_cast<std::uintmax_t>(std::stoull(line.substr(0, a))); } catch (...) { return false; }
    e.sha = line.substr(a + 1, b - a - 1);
    e.rel = from_u8(line.substr(b + 1));
    if (!safe_rel(e.rel) || !(e.sha == "-" || is_hex_digest(e.sha, 64))) return false;
    out.push_back(std::move(e));
  }
  return true;
}

std::string marker_name(const std::string &id) { return ".segarecomp-migration-" + id; }

// The only files besides the manifest entries that a migration creates in the destination: the catalog backup that Catalog::save() writes when the catalog is rebased.
std::vector<std::string> derived_names() {
  std::vector<std::string> names = {"library/library.json.bak", "library/library.json.tmp", "library/library.json.corrupt"};   // Catalog::save()/load() side files
  for (int n = 1; n < 10; ++n) names.push_back("library/library.json.corrupt" + std::to_string(n));
  return names;
}

// Removes what this journal created at the destination. Proof of ownership: the marker file carrying the journal id (written only after the destination was verified
// empty or created by us). Without the marker nothing is touched. Only manifest entries and the fixed derived names are removed, and only empty directories after them.
bool rollback_destination(const Journal &j, const std::vector<ManifestEntry> &entries) {
  std::error_code ec;
  const fs::path marker = j.dest / marker_name(j.id);
  if (!fs::exists(marker, ec)) return true;
  bool clean = true;
  std::set<fs::path> dirs;
  const auto drop = [&](const fs::path &rel) {
    const fs::path p = j.dest / rel;
    for (fs::path d = rel.parent_path(); !d.empty(); d = d.parent_path())   // never follow a link that replaced an intermediate directory
      if (fs::is_symlink(fs::symlink_status(j.dest / d, ec))) { clean = false; return; }
    const fs::file_status st = fs::symlink_status(p, ec);
    if (fs::is_regular_file(st) || fs::is_symlink(st)) { if (!fs::remove(p, ec) || ec) clean = false; }
    else if (fs::exists(st)) clean = false;   // a directory or special file where a file was recorded: not ours to remove
    for (fs::path d = rel.parent_path(); !d.empty(); d = d.parent_path()) dirs.insert(d);
  };
  for (const auto &e : entries) drop(e.rel);
  for (const std::string &name : derived_names()) drop(fs::path(name));
  std::vector<fs::path> ordered(dirs.begin(), dirs.end());
  std::sort(ordered.begin(), ordered.end(), [](const fs::path &a, const fs::path &b) { return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end()); });
  for (const auto &d : ordered) fs::remove(j.dest / d, ec);   // empty directories only: remove() fails on a non-empty one
  fs::remove(marker, ec);
  if (j.dest_created) fs::remove(j.dest, ec);
  return clean;
}

// ---- cooperative instance markers (beside the bootstrap, outside every movable root) ----
fs::path instances_dir(const fs::path &bootstrap) { return fs::path(bootstrap).concat(".instances"); }

bool all_digits(const std::string &t) { return !t.empty() && t.size() < 19 && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }); }

struct TreeMeasure { std::uintmax_t bytes = 0; std::size_t entries = 0; bool has_symlink = false, truncated = false; };

TreeMeasure measure_tree(const fs::path &root, const std::atomic<bool> *cancel = nullptr) {
  TreeMeasure m;
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (cancel != nullptr && cancel->load()) { m.truncated = true; break; }
    if (++m.entries > max_scan_entries) { m.truncated = true; break; }
    const fs::file_status st = it->symlink_status(ec);
    if (fs::is_symlink(st)) { m.has_symlink = true; continue; }
    if (fs::is_regular_file(st)) { const auto sz = it->file_size(ec); if (!ec) m.bytes += sz; }
  }
  return m;
}

bool unsafe_name(const std::string &name) {
  return std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

const char *durable_file_area(const std::string &name) {
  if (name == "library.json") return "catalog";
  if (name == "launcher-settings.json" || name == "viewer-settings.cfg" || name == "gamecontrollerdb.txt") return "settings";
  if (name == "sega-cd-bios.json") return "bios_profiles";
  return nullptr;
}
const char *durable_dir_area(const std::string &name) {
  if (name == "covers") return "covers";
  if (name == "roms") return "managed_roms";
  if (name == "patches") return "mods";
  if (name == "saves") return "saves";
  if (name == "sessions") return "recordings";
  return nullptr;
}
bool scd_build_name(const std::string &n) { return n == "build" || n == "build.partial" || n == "build.failed"; }

void add_area(MigrationPlan &plan, const std::string &area, ItemClass cls, std::uintmax_t bytes, std::size_t entries) {
  for (auto &a : plan.areas) if (a.area == area && a.cls == cls) { a.bytes += bytes; a.entries += entries; return; }
  plan.areas.push_back({area, cls, entries, bytes});
}

// Durable tree: every regular file is an item; a symlink or special file refuses the whole migration.
void scan_durable_tree(MigrationPlan &plan, const fs::path &root, const fs::path &rel_root, const std::string &area, std::size_t max_files, const std::atomic<bool> *cancel) {
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (cancel != nullptr && cancel->load()) return;
    const fs::file_status st = it->symlink_status(ec);
    const fs::path rel = rel_root / fs::relative(it->path(), root, ec);
    if (fs::is_symlink(st)) { plan.refusals.push_back({MigrationRefusal::source_symlink, "a link inside the data root is never followed: " + u8(rel)}); it.disable_recursion_pending(); continue; }
    if (fs::is_directory(st)) continue;
    if (!fs::is_regular_file(st)) { plan.refusals.push_back({MigrationRefusal::source_unsupported_entry, "not a regular file: " + u8(rel)}); continue; }
    if (unsafe_name(u8(rel))) { plan.refusals.push_back({MigrationRefusal::source_unsafe_name, "control characters in a name under " + u8(rel_root)}); continue; }
    if (plan.items.size() >= max_files) { plan.refusals.push_back({MigrationRefusal::source_too_large, "more than " + std::to_string(max_files) + " files"}); return; }
    const auto sz = fs::file_size(it->path(), ec);
    plan.items.push_back({rel, ec ? 0 : sz, area});
    add_area(plan, area, ItemClass::durable, ec ? 0 : sz, 1);
  }
  if (ec) plan.refusals.push_back({MigrationRefusal::source_unsupported_entry, "cannot read " + u8(root) + ": " + ec.message()});
}

void note_regenerable(MigrationPlan &plan, const fs::path &dir, const std::string &area, const std::atomic<bool> *cancel) {
  const TreeMeasure m = measure_tree(dir, cancel);
  plan.regenerable_bytes += m.bytes;
  ++plan.regenerable_entries;
  add_area(plan, area, ItemClass::regenerable, m.bytes, 1);
}

void scan_source(MigrationPlan &plan, std::size_t max_files, const std::atomic<bool> *cancel) {
  std::error_code ec;
  const fs::path &S = plan.source;
  for (const auto &top : fs::directory_iterator(S, ec)) {
    const std::string name = u8(top.path().filename());
    const fs::file_status st = top.symlink_status(ec);
    if (name == "library") {
      if (fs::is_symlink(st)) { plan.refusals.push_back({MigrationRefusal::source_symlink, "the library folder is a link: " + u8(top.path())}); continue; }
      if (!fs::is_directory(st)) continue;
      for (const auto &lib : fs::directory_iterator(top.path(), ec)) {
        const std::string lname = u8(lib.path().filename());
        const fs::file_status lst = lib.symlink_status(ec);
        const fs::path rel = fs::path("library") / lname;
        if (fs::is_symlink(lst)) { plan.refusals.push_back({MigrationRefusal::source_symlink, "a link inside the data root is never followed: " + u8(rel)}); continue; }
        if (fs::is_regular_file(lst)) {
          if (const char *area = durable_file_area(lname)) {
            if (unsafe_name(lname)) { plan.refusals.push_back({MigrationRefusal::source_unsafe_name, "control characters in a library file name"}); continue; }
            const auto sz = fs::file_size(lib.path(), ec);
            plan.items.push_back({rel, ec ? 0 : sz, area});
            add_area(plan, area, ItemClass::durable, ec ? 0 : sz, 1);
          } else if (lname == "library.json.bak") {
            const auto sz = fs::file_size(lib.path(), ec);
            add_area(plan, "catalog_backup", ItemClass::regenerable, ec ? 0 : sz, 1);   // re-created by the next catalog save at the new root
          } else {
            plan.left_behind.push_back(u8(rel));
          }
        } else if (fs::is_directory(lst)) {
          if (const char *area = durable_dir_area(lname)) scan_durable_tree(plan, lib.path(), rel, area, max_files, cancel);
          else if (lname == "sega-cd") {
            for (const auto &game : fs::directory_iterator(lib.path(), ec)) {
              const fs::file_status gst = game.symlink_status(ec);
              const fs::path grel = rel / game.path().filename();
              if (fs::is_symlink(gst)) { plan.refusals.push_back({MigrationRefusal::source_symlink, "a link inside the data root is never followed: " + u8(grel)}); continue; }
              if (!fs::is_directory(gst)) { plan.left_behind.push_back(u8(grel)); continue; }
              for (const auto &part : fs::directory_iterator(game.path(), ec)) {
                const std::string pname = u8(part.path().filename());
                const fs::file_status pst = part.symlink_status(ec);
                const fs::path prel = grel / pname;
                if (fs::is_symlink(pst)) { plan.refusals.push_back({MigrationRefusal::source_symlink, "a link inside the data root is never followed: " + u8(prel)}); continue; }
                if (fs::is_directory(pst) && scd_build_name(pname)) { note_regenerable(plan, part.path(), "builds", cancel); continue; }
                if (fs::is_directory(pst)) { scan_durable_tree(plan, part.path(), prel, "sega_cd_data", max_files, cancel); continue; }
                if (!fs::is_regular_file(pst)) { plan.refusals.push_back({MigrationRefusal::source_unsupported_entry, "not a regular file: " + u8(prel)}); continue; }
                const auto sz = fs::file_size(part.path(), ec);
                plan.items.push_back({prel, ec ? 0 : sz, "sega_cd_data"});
                add_area(plan, "sega_cd_data", ItemClass::durable, ec ? 0 : sz, 1);
              }
            }
          } else plan.left_behind.push_back(u8(rel));
        }
      }
    } else if (name == "games" || name == "zig-cache" || name == "sega-cd-objects" || name == "cover-downloads") {
      if (fs::is_symlink(st) || !fs::is_directory(st)) { plan.left_behind.push_back(name); continue; }
      if (name == "games") {
        for (const auto &g : fs::directory_iterator(top.path(), ec)) {
          const fs::file_status gst = g.symlink_status(ec);
          if (parse_dir_name(u8(g.path().filename())).valid && fs::is_directory(gst) && !fs::is_symlink(gst)) note_regenerable(plan, g.path(), "builds", cancel);
          else plan.left_behind.push_back("games/" + u8(g.path().filename()));
        }
      } else note_regenerable(plan, top.path(), name == "cover-downloads" ? "downloads" : "compiler_cache", cancel);
    } else {
      plan.left_behind.push_back(name);
    }
  }
  std::sort(plan.items.begin(), plan.items.end(), [](const MigrationItem &a, const MigrationItem &b) { return a.rel < b.rel; });
  std::sort(plan.left_behind.begin(), plan.left_behind.end());
  std::sort(plan.areas.begin(), plan.areas.end(), [](const AreaTotal &a, const AreaTotal &b) { return std::tie(a.cls, a.area) < std::tie(b.cls, b.area); });
  for (const auto &item : plan.items) plan.durable_bytes += item.bytes;
}

std::uintmax_t default_free_space(const fs::path &p) {
  std::error_code ec;
  const auto info = fs::space(p, ec);
  return ec ? 0 : info.available;
}

void check_destination(MigrationPlan &plan, const MigrationOptions &o) {
  const fs::path &D = plan.destination;
  std::error_code ec;
  const auto refuse = [&](MigrationRefusal code, std::string detail) { plan.refusals.push_back({code, std::move(detail)}); };
  if (D.empty() || !D.is_absolute()) { refuse(MigrationRefusal::destination_not_absolute, "choose a full folder path"); return; }
  bool dots = false;
  for (const auto &part : D) if (part == ".." || part == ".") dots = true;
  if (dots) { refuse(MigrationRefusal::destination_unsafe, "the path contains . or .. components"); return; }
  if (D == D.root_path() || D.parent_path() == D) { refuse(MigrationRefusal::destination_unsafe, "a drive or filesystem root cannot be a data folder"); return; }
  for (const auto &prot : o.protected_paths) {
    if (D == prot || is_within(prot, D) || same_place(D, prot)) { refuse(MigrationRefusal::destination_unsafe, "inside the installation: " + u8(prot)); return; }
  }
  // Every existing component must be a real directory: a link could send the data anywhere.
  fs::path walk;
  for (const auto &part : D) {
    walk /= part;
    if (walk == D.root_path()) continue;
    if (fs::exists(walk, ec) && path_is_link(walk)) { refuse(MigrationRefusal::destination_symlink, "a link on the path: " + u8(walk)); return; }
  }
  const bool exists = fs::exists(D, ec);
  if (exists && !fs::is_directory(D, ec)) { refuse(MigrationRefusal::destination_not_directory, "not a folder: " + u8(D)); return; }
  if (exists && same_place(D, plan.source)) { refuse(MigrationRefusal::destination_same, "that is the current data folder (possibly under another spelling)"); return; }
  for (fs::path a = D.parent_path(); !a.empty(); a = a.parent_path()) {
    if (same_place(a, plan.source)) { refuse(MigrationRefusal::destination_inside_source, "the new folder would be inside the current one"); return; }
    if (a.parent_path() == a) break;
  }
  if (exists) {
    for (fs::path a = plan.source; !a.empty(); a = a.parent_path()) {
      if (same_place(a, D)) { refuse(MigrationRefusal::source_inside_destination, "the current data folder is inside the new one"); return; }
      if (a.parent_path() == a) break;
    }
    if (fs::directory_iterator(D, ec) != fs::directory_iterator{}) { refuse(MigrationRefusal::destination_not_empty, "the folder is not empty and was not created by Segarecomp: " + u8(D)); return; }
  }
  const fs::path anchor = nearest_existing(D);
  const fs::perms perms = fs::status(anchor, ec).permissions();
  const bool any_write = (perms & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) != fs::perms::none;
  if (ec || !fs::is_directory(anchor, ec) || !any_write) { refuse(MigrationRefusal::destination_not_writable, "cannot write to " + u8(anchor)); return; }
  plan.required_bytes = plan.durable_bytes + plan.durable_bytes / 10 + (8U << 20U);
  plan.available_bytes = o.free_space ? o.free_space(anchor) : default_free_space(anchor);
  if (plan.available_bytes < plan.required_bytes)
    refuse(MigrationRefusal::insufficient_space, "needs " + format_bytes(plan.required_bytes) + " free (data plus a safety margin), " + format_bytes(plan.available_bytes) + " available at " + u8(anchor));
}

std::string random_id() {
  std::random_device rd;
  std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32U) ^ rd() ^ static_cast<std::uint64_t>(now_seconds()));
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(gen()));
  return buf;
}

} // namespace


// ------------------------------------------------------------------------------------------------ instance markers

InstanceMarker::InstanceMarker(const fs::path &bootstrap_file) {
  std::error_code ec;
  const fs::path dir = instances_dir(bootstrap_file);
  if (!fs::exists(bootstrap_file.parent_path(), ec)) created_parent_ = bootstrap_file.parent_path();   // leave nothing behind that did not exist before
  fs::create_directories(dir, ec);
  if (ec) return;   // best effort: a launcher that cannot register simply is not seen by a move (the final digest re-check still applies)
  pid_ = own_pid();
  const fs::path file = dir / std::to_string(pid_);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << "segarecomp-instance 1 " << pid_ << " " << now_seconds() << "\n";
  out.flush();
  if (out) file_ = file;
}

InstanceMarker::~InstanceMarker() {
  if (file_.empty() || own_pid() != pid_) return;   // a forked child that inherited this object never removes its parent's marker
  std::error_code ec;
  fs::remove(file_, ec);
  fs::remove(file_.parent_path(), ec);   // only when it is empty
  if (!created_parent_.empty()) fs::remove(created_parent_, ec);
}

// Non-throwing, bounded: 1..2147483647 in at most 10 plain digits, otherwise 0 (ignored).
static long long parse_marker_pid(const std::string &name) {
  if (name.empty() || name.size() > 10 || !all_digits(name)) return 0;
  std::uint32_t v = 0;
  const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), v);
  if (ec != std::errc() || ptr != name.data() + name.size() || v == 0 || v > 2147483647U) return 0;
  return static_cast<long long>(v);
}

bool foreign_instance_running(const fs::path &bootstrap_file, std::string *who) {
  std::error_code ec;
  const fs::path dir = instances_dir(bootstrap_file);
  if (!fs::is_directory(dir, ec)) return false;
  bool found = false;
  std::vector<fs::path> dead;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = u8(it->path().filename());
    if (!all_digits(name) || !it->is_regular_file(ec)) continue;
    const long long pid = parse_marker_pid(name);
    if (pid <= 0) continue;   // a name that cannot be read safely as a process id is neither trusted nor deleted
    if (pid == own_pid()) continue;
    if (process_alive(pid)) {
      if (!found && who) *who = "another Segarecomp process (id " + name + ") is running; its marker is " + u8(it->path());
      found = true;
    } else dead.push_back(it->path());
  }
  for (const auto &d : dead) fs::remove(d, ec);
  return found;
}

// ------------------------------------------------------------------------------------------------ bootstrap

Bootstrap read_bootstrap(const fs::path &file) {
  Bootstrap b;
  std::error_code ec;
  if (!fs::exists(file, ec)) return b;
  std::string text;
  b.state = BootstrapState::damaged;
  if (!read_text(file, text, 64U << 10U)) { b.detail = "unreadable"; return b; }
  const auto root = json::parse(text);
  if (!root || !root->is_object()) { b.detail = "not a JSON object"; return b; }
  if (root->num("schema", 0) > 1) { b.state = BootstrapState::newer_schema; b.detail = "written by a newer release"; return b; }
  const fs::path path = from_u8(root->str("root"));
  if (root->num("schema", 0) != 1 || path.empty() || !path.is_absolute()) { b.detail = "missing or relative root"; return b; }
  b.state = BootstrapState::valid;
  b.root = path;
  return b;
}

bool write_bootstrap(const fs::path &file, const fs::path &root, std::string *error) {
  if (!root.is_absolute()) { if (error) *error = "the data root must be an absolute path"; return false; }
  json::Value v = json::Value::make_object();
  v.set("schema", json::Value::make_int(1));
  v.set("root", json::Value::make_string(u8(root)));
  return write_text_atomic(file, json::dump(v), error);
}

ResolvedRoots resolve_roots(const DataEnvironment &env, const fs::path &default_cache, const fs::path &bootstrap_file) {
  ResolvedRoots r;
  r.bootstrap = read_bootstrap(bootstrap_file);
  std::error_code ec;
  if (env.cache_dir) {
    r.cache = from_u8(*env.cache_dir);
    r.cache_source = RootSource::environment;
    if (r.bootstrap.state == BootstrapState::valid) { r.bootstrap_ignored = true; r.notice = "SEGARECOMP_CACHE_DIR is set, so the saved data location is not used."; }
  } else if (r.bootstrap.state == BootstrapState::valid && fs::is_directory(r.bootstrap.root, ec)) {
    r.cache = r.bootstrap.root;
    r.cache_source = RootSource::bootstrap;
  } else {
    r.cache = default_cache;
    if (r.bootstrap.state == BootstrapState::valid) { r.bootstrap_ignored = true; r.notice = "The saved data location " + u8(r.bootstrap.root) + " is not available; the default location is used until it is."; }
    else if (r.bootstrap.state == BootstrapState::damaged) { r.notice = "The saved data location file is damaged (" + r.bootstrap.detail + "); the default location is used."; }
    else if (r.bootstrap.state == BootstrapState::newer_schema) { r.notice = "The saved data location file was written by a newer release; the default location is used."; }
  }
  if (env.library_dir) { r.library = from_u8(*env.library_dir); r.library_source = RootSource::environment; }
  else { r.library = r.cache / "library"; r.library_source = r.cache_source; }
  return r;
}

const char *refusal_name(MigrationRefusal r) {
  switch (r) {
    case MigrationRefusal::env_override_cache: return "env_override_cache";
    case MigrationRefusal::env_override_library: return "env_override_library";
    case MigrationRefusal::bootstrap_unusable: return "bootstrap_unusable";
    case MigrationRefusal::saved_location_unavailable: return "saved_location_unavailable";
    case MigrationRefusal::journal_present: return "journal_present";
    case MigrationRefusal::previous_migration_pending_cleanup: return "previous_migration_pending_cleanup";
    case MigrationRefusal::source_missing: return "source_missing";
    case MigrationRefusal::source_symlink: return "source_symlink";
    case MigrationRefusal::source_unsupported_entry: return "source_unsupported_entry";
    case MigrationRefusal::source_unsafe_name: return "source_unsafe_name";
    case MigrationRefusal::source_too_large: return "source_too_large";
    case MigrationRefusal::destination_not_absolute: return "destination_not_absolute";
    case MigrationRefusal::destination_unsafe: return "destination_unsafe";
    case MigrationRefusal::destination_same: return "destination_same";
    case MigrationRefusal::destination_inside_source: return "destination_inside_source";
    case MigrationRefusal::source_inside_destination: return "source_inside_destination";
    case MigrationRefusal::destination_symlink: return "destination_symlink";
    case MigrationRefusal::destination_not_directory: return "destination_not_directory";
    case MigrationRefusal::destination_not_empty: return "destination_not_empty";
    case MigrationRefusal::destination_not_writable: return "destination_not_writable";
    case MigrationRefusal::insufficient_space: return "insufficient_space";
    case MigrationRefusal::other_instance_running: return "other_instance_running";
  }
  return "unknown";
}

const char *recovery_action_name(RecoveryAction a) {
  switch (a) {
    case RecoveryAction::none: return "none";
    case RecoveryAction::rolled_back: return "rolled_back";
    case RecoveryAction::completed: return "completed";
    case RecoveryAction::kept_completed: return "kept_completed";
    case RecoveryAction::owner_alive: return "owner_alive";
    case RecoveryAction::journal_invalid: return "journal_invalid";
    case RecoveryAction::rollback_incomplete: return "rollback_incomplete";
  }
  return "unknown";
}

PlainText refusal_text(MigrationRefusal r) {
  switch (r) {
    case MigrationRefusal::env_override_cache: return {"The data folder is set by the SEGARECOMP_CACHE_DIR environment variable, so it cannot be moved from here.", "Remove that variable from the environment that starts Segarecomp, restart, and choose a location here."};
    case MigrationRefusal::env_override_library: return {"The library folder is set by the SEGARECOMP_LIBRARY_DIR environment variable, so it cannot be moved from here.", "Remove that variable from the environment that starts Segarecomp, restart, and choose a location here."};
    case MigrationRefusal::bootstrap_unusable: return {"The saved data location file cannot be used (it is damaged or was written by a newer version).", "Keep using the current location. To choose another one, fix or remove the file named in the details, then restart."};
    case MigrationRefusal::saved_location_unavailable: return {"Your saved data location is not available (is its drive connected?). Segarecomp is using the default location until it is, so a move from here would leave your real library behind.", "Reconnect the drive that holds your data and restart Segarecomp, then try again. Nothing was changed."};
    case MigrationRefusal::journal_present: return {"An earlier move did not finish.", "Restart Segarecomp: the unfinished move is rolled back or completed when it starts. Then try again."};
    case MigrationRefusal::previous_migration_pending_cleanup: return {"A previous move is complete and its old folder has not been dealt with yet.", "Under Storage & Data, clean up the previous location or choose Keep it, then try again."};
    case MigrationRefusal::source_missing: return {"The current data folder was not found.", "Check that its drive is connected, then try again."};
    case MigrationRefusal::source_symlink: return {"The current data folder is a link, which cannot be moved safely.", "Point Segarecomp at the real folder, or keep using this location."};
    case MigrationRefusal::source_unsupported_entry: return {"The data folder holds something that is not an ordinary file or folder (a link or a device).", "Remove or move that item out of the data folder, then try again."};
    case MigrationRefusal::source_unsafe_name: return {"A file name in the data folder cannot be copied safely.", "Rename or remove the file named in the details, then try again."};
    case MigrationRefusal::source_too_large: return {"The data folder holds too many files to move in one step (more than 100,000).", "Remove files you do not need (see Review usage and clean up), then try again."};
    case MigrationRefusal::destination_not_absolute: return {"The new location is not a complete folder path.", "Choose the folder again."};
    case MigrationRefusal::destination_unsafe: return {"That folder is not allowed as a data location (a drive root, a system folder or the folder Segarecomp is installed in).", "Choose another folder."};
    case MigrationRefusal::destination_same: return {"That is the folder Segarecomp already uses.", "Choose a different folder."};
    case MigrationRefusal::destination_inside_source: return {"The new folder is inside the current data folder.", "Choose a folder outside it."};
    case MigrationRefusal::source_inside_destination: return {"The current data folder is inside the folder you chose.", "Choose a folder that does not contain it."};
    case MigrationRefusal::destination_symlink: return {"The chosen folder, or a folder above it, is a link.", "Choose a real folder."};
    case MigrationRefusal::destination_not_directory: return {"The chosen location is a file, not a folder.", "Choose a folder."};
    case MigrationRefusal::destination_not_empty: return {"The chosen folder already contains files, and Segarecomp never mixes its data into other files.", "Choose an empty folder, or a new folder inside it."};
    case MigrationRefusal::destination_not_writable: return {"Segarecomp cannot write to the chosen folder.", "Choose a folder you can write to."};
    case MigrationRefusal::insufficient_space: return {"There is not enough free space at the chosen location.", "Free up space there or choose another drive."};
    case MigrationRefusal::other_instance_running: return {"Another Segarecomp window or command is running and could be using your data.", "Close the other Segarecomp window (or wait for its command to finish), then try again. Nothing was changed."};
  }
  return {"The move was refused.", "Nothing was changed. Choose another location."};
}

PlainText failure_text(const std::string &reason) {
  for (int i = static_cast<int>(MigrationRefusal::env_override_cache); i <= static_cast<int>(MigrationRefusal::other_instance_running); ++i)
    if (reason == refusal_name(static_cast<MigrationRefusal>(i))) return refusal_text(static_cast<MigrationRefusal>(i));
  if (reason == "busy") return {"A game, a preparation, an import or a cleanup is running.", "Wait for it to finish (or cancel it), then try again."};
  if (reason == "cancelled") return {"The move was cancelled.", "Nothing was changed; the current location stays in use. Start again whenever you like."};
  if (reason == "interrupted") return {"The move was interrupted.", "Restart Segarecomp: the unfinished move is rolled back at the next start. Nothing was switched."};
  if (reason == "copy_failed") return {"A file could not be copied to the new location.", "Check that the destination has room and is writable, then try again. The current location is unchanged."};
  if (reason == "source_changed") return {"A file in your data changed while it was being moved (checked again by content just before switching).", "Close any other Segarecomp window, then try again when nothing else is using your data. The current location is unchanged."};
  if (reason == "rebase_failed") return {"The copied library could not be updated for the new location.", "Try again. The current location is unchanged."};
  if (reason == "verify_failed") return {"The copy did not match the original, so it was not used.", "Try again, or choose another drive. The current location is unchanged."};
  if (reason == "journal_write_failed") return {"Segarecomp could not write its move record next to the data location file.", "Check that the folder holding that file is writable, then try again. The current location is unchanged."};
  if (reason == "bootstrap_write_failed") return {"Segarecomp could not save the new location, so it did not switch.", "Check that the folder holding the data location file is writable, then try again. The current location is unchanged."};
  return {"The move did not complete.", "The current location is unchanged. Try again."};
}

PlainText cleanup_refusal_text(const std::string &reason) {
  if (reason == "not_completed") return {"There is no completed move to clean up.", "Nothing to do."};
  if (reason == "bootstrap_mismatch") return {"The data location has changed since the move, so the old folder is no longer offered for cleanup.", "Nothing was removed. Choose Keep it to forget the previous location."};
  if (reason == "old_root_active") return {"The previous folder is still the one in use (SEGARECOMP_CACHE_DIR names it or a folder inside or around it).", "Remove SEGARECOMP_CACHE_DIR, restart, and try again."};
  if (reason == "old_root_missing") return {"The previous folder is no longer there (or is a link).", "Nothing to remove. Choose Keep it to forget it."};
  if (reason == "busy") return {"A game, a preparation, an import or another cleanup is running.", "Wait for it to finish, then try again."};
  if (reason == "library_active") return {"The library folder in use is set by the SEGARECOMP_LIBRARY_DIR environment variable and lies inside the previous folder, so removing anything there could delete your active library.", "Remove or change SEGARECOMP_LIBRARY_DIR, restart, and try again. Nothing was removed."};
  if (reason == "home_active") return {"The Segarecomp home folder (SEGARECOMP_HOME) lies inside the previous folder, so it cannot be cleaned up.", "Remove or change SEGARECOMP_HOME, restart, and try again. Nothing was removed."};
  if (reason == "other_instance_running") return {"Another Segarecomp window or command is running and could be using the previous folder.", "Close the other Segarecomp window, then try again. Nothing was removed."};
  if (reason == "restart_needed") return {"Segarecomp is still using the previous location until it is restarted.", "Restart Segarecomp first; the previous location can be cleaned up afterwards."};
  return {"The cleanup was refused.", "Nothing was removed."};
}

// ------------------------------------------------------------------------------------------------ plan

MigrationPlan build_migration_plan(const MigrationOptions &o, const std::atomic<bool> *cancel) {
  MigrationPlan plan;
  plan.source = o.source;
  plan.destination = o.destination;
  std::error_code ec;
  if (o.env.cache_dir)
    plan.refusals.push_back({MigrationRefusal::env_override_cache, "SEGARECOMP_CACHE_DIR governs the cache folder; the effective path is " + *o.env.cache_dir + ". Unset it to move the data."});
  if (o.env.library_dir)
    plan.refusals.push_back({MigrationRefusal::env_override_library, "SEGARECOMP_LIBRARY_DIR governs the library folder; the effective path is " + *o.env.library_dir + ". Unset it to move the data."});
  if (!plan.refusals.empty()) return plan;   // an override is never silently ignored or replaced: nothing else is examined
  const Bootstrap boot = read_bootstrap(o.bootstrap_file);
  if (boot.state == BootstrapState::damaged || boot.state == BootstrapState::newer_schema)
    plan.refusals.push_back({MigrationRefusal::bootstrap_unusable, "the saved data location file cannot be used (" + boot.detail + ")"});
  else if (boot.state == BootstrapState::valid && !fs::is_directory(boot.root, ec))
    plan.refusals.push_back({MigrationRefusal::saved_location_unavailable, "the saved data location " + u8(boot.root) + " is not available; the pointer is never re-pointed while it is unavailable"});
  Journal j;
  switch (read_journal(o.bootstrap_file, j)) {
    case JournalState::absent: break;
    case JournalState::ok:
      if (j.phase == "done") plan.refusals.push_back({MigrationRefusal::previous_migration_pending_cleanup, "the previous move from " + u8(j.source) + " is complete; clean up or keep that old folder first"});
      else plan.refusals.push_back({MigrationRefusal::journal_present, "an earlier move is unfinished and is recovered at the next start"});
      break;
    default: plan.refusals.push_back({MigrationRefusal::journal_present, "a migration journal exists that this release cannot read"});
  }
  if (std::string who; foreign_instance_running(o.bootstrap_file, &who)) plan.refusals.push_back({MigrationRefusal::other_instance_running, who});
  if (!fs::is_directory(o.source, ec)) { plan.refusals.push_back({MigrationRefusal::source_missing, "no data folder at " + u8(o.source)}); return plan; }
  if (path_is_link(o.source)) { plan.refusals.push_back({MigrationRefusal::source_symlink, "the data folder is a link: " + u8(o.source)}); return plan; }
  scan_source(plan, o.max_files, cancel);
  check_destination(plan, o);
  plan.notes.push_back("Builds, the compiler cache and downloads stay in the old folder; games with a build are rebuilt at the new location.");
  plan.notes.push_back("ROM, BIOS and disc files you added by reference are not moved or changed.");
  return plan;
}

// ------------------------------------------------------------------------------------------------ execution

namespace {

struct RebaseOutcome { bool ok = true; std::string error; std::size_t rebased = 0, cleared = 0, needing_rebuild = 0; std::vector<std::string> ids; std::vector<fs::path> rebased_files; };

// Rewrites, in the COPY of the catalog only, the references that point at a managed file now also present at the new root. External references stay byte-identical.
RebaseOutcome rebase_catalog(const fs::path &source, const fs::path &dest, const std::set<std::string> &migrated) {
  RebaseOutcome r;
  std::error_code ec;
  const fs::path file = dest / "library" / "library.json";
  Catalog catalog(file);
  if (catalog.load() != CatalogLoad::loaded) { r.ok = false; r.error = "the copied catalog could not be read back as it was"; return r; }
  const fs::path canon_source = fs::weakly_canonical(source, ec);
  const auto managed_rel = [&](const std::string &text, bool &inside) -> std::optional<std::string> {
    inside = false;
    const fs::path p = from_u8(text);
    if (p.empty() || !p.is_absolute()) return std::nullopt;
    for (const fs::path &candidate : {p.lexically_normal(), fs::weakly_canonical(p, ec)}) {
      for (const fs::path &root : {source, canon_source}) {
        if (!is_within(root, candidate)) continue;
        inside = true;
        const std::string rel = gen(candidate.lexically_relative(root));
        if (migrated.count(rel) != 0) return rel;
      }
    }
    return std::nullopt;
  };
  std::size_t changes = 0;
  for (auto &entry : catalog.entries()) {
    r.ids.push_back(entry.id());
    if (!entry.current_build_key.empty() || !entry.builds.empty()) ++r.needing_rebuild;
    for (auto &path : entry.rom_paths) {
      bool inside = false;
      if (const auto rel = managed_rel(path, inside)) { path = u8(dest / from_u8(*rel)); ++r.rebased; ++changes; r.rebased_files.push_back(from_u8(path)); }
    }
    if (entry.platform_id == "sega-cd") {
      for (std::string *path : {&entry.sega_cd.bios_path, &entry.sega_cd.cue_path}) {
        bool inside = false;
        if (const auto rel = managed_rel(*path, inside)) { *path = u8(dest / from_u8(*rel)); ++r.rebased; ++changes; r.rebased_files.push_back(from_u8(*path)); }
      }
      bool inside = false;
      std::string &program = entry.sega_cd.program.path;
      if (!program.empty()) {
        if (const auto rel = managed_rel(program, inside)) { program = u8(dest / from_u8(*rel)); ++r.rebased; ++changes; r.rebased_files.push_back(from_u8(program)); }
        else if (inside) { entry.sega_cd.program = scd::ProgramRecord{}; ++r.cleared; ++changes; }   // lived in a regenerable build folder: rebuild
      }
    }
  }
  if (changes == 0) return r;
  if (catalog.read_only()) { r.ok = false; r.error = "the catalog was written by a newer release and cannot be rewritten"; return r; }
  std::string error;
  if (!catalog.save(&error)) { r.ok = false; r.error = "the rebased catalog could not be saved: " + error; }
  return r;
}

struct Fail { std::string reason, detail; };

} // namespace

MigrationResult run_migration(const MigrationOptions &o, BusyGuard &busy, const std::atomic<bool> *cancel, const MigrationProgressFn &progress, const MigrationHooks *hooks) {
  MigrationResult res;
  if (!busy.try_begin_maintenance()) {
    res.outcome = MigrationOutcome::busy;
    res.reason = "busy";
    res.detail = "a build, game, import or maintenance is running; finish it first";
    return res;
  }
  struct Release { BusyGuard &b; ~Release() { b.end_maintenance(); } } release{busy};

  const auto report = [&](const char *phase, std::size_t done, std::size_t total, std::uintmax_t bdone, std::uintmax_t btotal, const std::string &current) {
    if (progress) progress({phase, done, total, bdone, btotal, current});
  };
  const auto crash = [&](const char *point, std::size_t index) { return hooks != nullptr && hooks->crash_at && hooks->crash_at(point, index); };
  const auto cancelled = [&] { return cancel != nullptr && cancel->load(); };

  report("plan", 0, 0, 0, 0, "");
  const MigrationPlan plan = build_migration_plan(o, cancel);
  if (!plan.ok()) {
    res.outcome = MigrationOutcome::refused;
    res.refusals = plan.refusals;
    res.reason = refusal_name(plan.refusals.front().code);
    res.detail = plan.refusals.front().detail;
    return res;
  }
  if (cancelled()) { res.outcome = MigrationOutcome::cancelled; res.reason = "cancelled"; res.detail = "cancelled before anything was written"; return res; }

  Journal j;
  j.id = random_id();
  j.phase = "copy";
  j.source = plan.source;
  j.dest = plan.destination;
  std::error_code ec;
  j.dest_created = !fs::exists(j.dest, ec);
  j.pid = own_pid();
  j.started = now_seconds();
  j.files = plan.items.size();
  j.bytes = plan.durable_bytes;
  res.migration_id = j.id;
  res.files = plan.items.size();
  res.bytes = plan.durable_bytes;

  std::vector<ManifestEntry> manifest;
  for (const auto &item : plan.items) manifest.push_back({item.bytes, "-", item.rel});

  const auto abort_with = [&](MigrationOutcome outcome, std::string reason, std::string detail) {
    res.outcome = outcome;
    res.reason = std::move(reason);
    res.detail = std::move(detail);
    if (rollback_destination(j, manifest)) remove_journal(o.bootstrap_file);
    else res.notes.push_back("the unfinished copy could not be removed completely; it is retried at the next start");
    return res;
  };

  std::string error;
  if (!write_manifest(o.bootstrap_file, j.id, manifest, &error)) { res.outcome = MigrationOutcome::failed; res.reason = "journal_write_failed"; res.detail = error; remove_journal(o.bootstrap_file); return res; }
  if (!write_journal(o.bootstrap_file, j, &error)) { res.outcome = MigrationOutcome::failed; res.reason = "journal_write_failed"; res.detail = error; remove_journal(o.bootstrap_file); return res; }

  fs::create_directories(j.dest, ec);
  if (ec || !fs::is_directory(j.dest)) return abort_with(MigrationOutcome::failed, "destination_not_writable", "cannot create " + u8(j.dest));
  {
    std::ofstream marker(j.dest / marker_name(j.id), std::ios::binary);
    marker << "segarecomp migration " << j.id << "\n";
    marker.flush();
    if (!marker) { marker.close(); fs::remove(j.dest / marker_name(j.id), ec); if (j.dest_created) fs::remove(j.dest, ec); remove_journal(o.bootstrap_file); res.outcome = MigrationOutcome::failed; res.reason = "destination_not_writable"; res.detail = "cannot write into " + u8(j.dest); return res; }
  }

  // ---- copy ----
  std::vector<std::string> digests(plan.items.size());
  std::uintmax_t bytes_done = 0;
  for (std::size_t i = 0; i < plan.items.size(); ++i) {
    if (crash("copy", i)) { res.outcome = MigrationOutcome::interrupted; res.reason = "interrupted"; res.detail = "simulated process death during the copy"; return res; }
    if (cancelled()) return abort_with(MigrationOutcome::cancelled, "cancelled", "cancelled during the copy; the old data folder is unchanged");
    const MigrationItem &item = plan.items[i];
    const fs::path from = plan.source / item.rel, to = j.dest / item.rel;
    report("copy", i, plan.items.size(), bytes_done, plan.durable_bytes, u8(item.rel));
    const scd::Digest before = scd::hash_file(from, hash_limit, nullptr, cancel);
    if (!before.ok) return abort_with(before.error == "cancelled" ? MigrationOutcome::cancelled : MigrationOutcome::failed, before.error == "cancelled" ? "cancelled" : "copy_failed", "cannot read " + u8(item.rel) + " (" + before.error + ")");
    if (before.size != item.bytes) return abort_with(MigrationOutcome::failed, "source_changed", u8(item.rel) + " changed while the move was planned");
    digests[i] = before.sha256;
    fs::create_directories(to.parent_path(), ec);
    if (ec) return abort_with(MigrationOutcome::failed, "copy_failed", "cannot create " + u8(to.parent_path()) + ": " + ec.message());
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) return abort_with(MigrationOutcome::failed, "copy_failed", "cannot copy " + u8(item.rel) + ": " + ec.message());
    fs::last_write_time(to, fs::last_write_time(from, ec), ec);
    if (hooks != nullptr && hooks->after_copy) hooks->after_copy(to);
    bytes_done += item.bytes;
  }
  report("copy", plan.items.size(), plan.items.size(), bytes_done, plan.durable_bytes, "");

  // ---- rebase (copy of the catalog only) ----
  if (crash("rebase", 0)) { res.outcome = MigrationOutcome::interrupted; res.reason = "interrupted"; res.detail = "simulated process death before the rebase"; return res; }
  report("rebase", 0, 0, 0, 0, "");
  RebaseOutcome rebase;
  std::size_t catalog_index = plan.items.size();
  std::set<std::string> migrated;
  for (std::size_t i = 0; i < plan.items.size(); ++i) {
    migrated.insert(gen(plan.items[i].rel));
    if (plan.items[i].rel == fs::path("library") / "library.json") catalog_index = i;
  }
  if (catalog_index < plan.items.size()) {
    // The copy must still be exactly what was hashed before it was copied (the source catalog is only digested once; this closes the copy-time window for the file the rebase rewrites).
    const scd::Digest copied = scd::hash_file(j.dest / plan.items[catalog_index].rel, hash_limit, nullptr, cancel);
    if (!copied.ok) return abort_with(copied.error == "cancelled" ? MigrationOutcome::cancelled : MigrationOutcome::failed, copied.error == "cancelled" ? "cancelled" : "verify_failed", "cannot read back the copied catalog");
    if (copied.size != plan.items[catalog_index].bytes || copied.sha256 != digests[catalog_index]) return abort_with(MigrationOutcome::failed, "source_changed", "the catalog changed while it was being copied");
    rebase = rebase_catalog(plan.source, j.dest, migrated);
    if (!rebase.ok) return abort_with(MigrationOutcome::failed, "rebase_failed", rebase.error);
  }
  res.rebased_paths = rebase.rebased;
  res.cleared_programs = rebase.cleared;
  res.entries_needing_rebuild = rebase.needing_rebuild;
  const bool catalog_rewritten = rebase.rebased + rebase.cleared != 0;

  // ---- verify ----
  j.phase = "verify";
  if (!write_journal(o.bootstrap_file, j, &error)) return abort_with(MigrationOutcome::failed, "journal_write_failed", error);
  std::set<fs::path> expected_files;
  for (std::size_t i = 0; i < plan.items.size(); ++i) {
    if (crash("verify", i)) { res.outcome = MigrationOutcome::interrupted; res.reason = "interrupted"; res.detail = "simulated process death during verification"; return res; }
    if (cancelled()) return abort_with(MigrationOutcome::cancelled, "cancelled", "cancelled during verification; the old data folder is unchanged");
    const MigrationItem &item = plan.items[i];
    report("verify", i, plan.items.size(), 0, 0, u8(item.rel));
    const fs::path from = plan.source / item.rel, to = j.dest / item.rel;
    expected_files.insert(to);
    const fs::file_status st = fs::symlink_status(to, ec);
    if (!fs::is_regular_file(st)) return abort_with(MigrationOutcome::failed, "verify_failed", "missing at the new location: " + u8(item.rel));
    if (fs::file_size(from, ec) != item.bytes || ec) return abort_with(MigrationOutcome::failed, "source_changed", u8(item.rel) + " changed during the move");
    if (i == catalog_index && catalog_rewritten) continue;   // verified below by reading it back as a catalog
    const scd::Digest after = scd::hash_file(to, hash_limit, nullptr, cancel);
    if (!after.ok) return abort_with(after.error == "cancelled" ? MigrationOutcome::cancelled : MigrationOutcome::failed, after.error == "cancelled" ? "cancelled" : "verify_failed", "cannot read back " + u8(item.rel));
    if (after.size != item.bytes || after.sha256 != digests[i]) return abort_with(MigrationOutcome::failed, "verify_failed", "content differs at the new location: " + u8(item.rel));
  }
  if (catalog_rewritten) {
    expected_files.insert(j.dest / "library" / "library.json.bak");
    Catalog check(j.dest / "library" / "library.json");
    if (check.load() != CatalogLoad::loaded || check.entries().size() != rebase.ids.size()) return abort_with(MigrationOutcome::failed, "verify_failed", "the rebased catalog does not read back");
    for (std::size_t i = 0; i < rebase.ids.size(); ++i)
      if (check.entries()[i].id() != rebase.ids[i]) return abort_with(MigrationOutcome::failed, "verify_failed", "the rebased catalog lists different games");
    for (const auto &f : rebase.rebased_files)
      if (!fs::is_regular_file(f, ec)) return abort_with(MigrationOutcome::failed, "verify_failed", "a rebased reference has no file: " + u8(f));
  }
  {
    // Exactly what was planned exists at the new root, and nothing else (counts, not only names).
    std::size_t found = 0;
    for (fs::recursive_directory_iterator it(j.dest, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (it->path().filename() == marker_name(j.id)) continue;
      ++found;
      if (expected_files.count(it->path()) == 0) return abort_with(MigrationOutcome::failed, "verify_failed", "unexpected file at the new location: " + u8(fs::relative(it->path(), j.dest, ec)));
    }
    if (found != expected_files.size()) return abort_with(MigrationOutcome::failed, "verify_failed", "file count differs at the new location");
  }
  for (std::size_t i = 0; i < manifest.size(); ++i) manifest[i].sha = digests[i];
  if (!write_manifest(o.bootstrap_file, j.id, manifest, &error)) return abort_with(MigrationOutcome::failed, "journal_write_failed", error);

  // ---- final source re-verification (SEG-150 round A): every planned source file is hashed again and must equal the digest taken before it was copied (size alone cannot see a
  // same-length rewrite); the durable file set must still be exactly the planned one. The catalog's destination copy is rebased by design, so its SOURCE is compared with the digest
  // of what was copied and rebased, never with the rebased bytes. A writer that ignores the protocol and changes the source after this point is not detectable.
  for (std::size_t i = 0; i < plan.items.size(); ++i) {
    if (cancelled()) return abort_with(MigrationOutcome::cancelled, "cancelled", "cancelled during the final source check; the old data folder is unchanged");
    report("verify", i, plan.items.size(), 0, 0, u8(plan.items[i].rel));
    const scd::Digest again = scd::hash_file(plan.source / plan.items[i].rel, hash_limit, nullptr, cancel);
    if (!again.ok && again.error == "cancelled") return abort_with(MigrationOutcome::cancelled, "cancelled", "cancelled during the final source check; the old data folder is unchanged");
    if (!again.ok || again.size != plan.items[i].bytes || again.sha256 != digests[i]) return abort_with(MigrationOutcome::failed, "source_changed", u8(plan.items[i].rel) + " changed during the move");
  }
  {
    MigrationPlan now;
    now.source = plan.source;
    scan_source(now, o.max_files, cancel);
    bool same = now.refusals.empty() && now.items.size() == plan.items.size();
    for (std::size_t i = 0; same && i < plan.items.size(); ++i) same = now.items[i].rel == plan.items[i].rel && now.items[i].bytes == plan.items[i].bytes;
    if (cancelled()) return abort_with(MigrationOutcome::cancelled, "cancelled", "cancelled during the final source check; the old data folder is unchanged");
    if (!same) return abort_with(MigrationOutcome::failed, "source_changed", "files were added, removed or replaced in the old data folder during the move");
  }
  if (std::string who; foreign_instance_running(o.bootstrap_file, &who)) return abort_with(MigrationOutcome::failed, "other_instance_running", who);

  // ---- commit: the atomic bootstrap write is the one irreversible step; cancel is honoured only before it ----
  if (cancelled()) return abort_with(MigrationOutcome::cancelled, "cancelled", "cancelled before the switch; the old data folder is unchanged");
  if (crash("commit_before", 0)) { res.outcome = MigrationOutcome::interrupted; res.reason = "interrupted"; res.detail = "simulated process death before the switch"; return res; }
  report("commit", 0, 0, 0, 0, "");
  j.phase = "commit";
  if (!write_journal(o.bootstrap_file, j, &error)) return abort_with(MigrationOutcome::failed, "journal_write_failed", error);
  if (!write_bootstrap(o.bootstrap_file, j.dest, &error)) return abort_with(MigrationOutcome::failed, "bootstrap_write_failed", error);
  if (crash("commit_after", 0)) { res.outcome = MigrationOutcome::interrupted; res.reason = "interrupted"; res.detail = "simulated process death after the switch"; return res; }
  j.phase = "done";
  if (!write_journal(o.bootstrap_file, j, &error)) res.notes.push_back("the completion record could not be written (" + error + "); the next start finishes it");
  fs::remove(j.dest / marker_name(j.id), ec);
  report("done", plan.items.size(), plan.items.size(), plan.durable_bytes, plan.durable_bytes, "");
  res.outcome = MigrationOutcome::completed;
  res.detail = "The data now lives at " + u8(j.dest) + ". It is used the next time Segarecomp starts; the old folder was left as it was.";
  return res;
}

// ------------------------------------------------------------------------------------------------ recovery

RecoveryResult recover_pending_migration(const fs::path &bootstrap_file) {
  RecoveryResult out;
  Journal j;
  const JournalState state = read_journal(bootstrap_file, j);
  if (state == JournalState::absent) return out;
  if (state != JournalState::ok) {
    out.action = RecoveryAction::journal_invalid;
    out.detail = state == JournalState::newer ? "the migration journal was written by a newer release and is left alone" : "the migration journal is damaged and is left alone";
    return out;
  }
  out.source = u8(j.source);
  out.destination = u8(j.dest);
  const Bootstrap boot = read_bootstrap(bootstrap_file);
  const bool names_dest = boot.state == BootstrapState::valid && (boot.root == j.dest || same_place(boot.root, j.dest));
  std::error_code ec;
  if (j.phase == "done") {
    out.action = RecoveryAction::kept_completed;
    out.detail = names_dest ? "a completed move is on record; the old folder can be cleaned up" : "a completed move is on record but the data location has since changed";
    return out;
  }
  if (j.phase == "commit" && names_dest) {   // the flip happened, the journal did not reach `done`
    j.phase = "done";
    std::string error;
    if (!write_journal(bootstrap_file, j, &error)) { out.action = RecoveryAction::rollback_incomplete; out.detail = "the switch is complete but its record could not be written: " + error; return out; }
    fs::remove(j.dest / marker_name(j.id), ec);
    out.action = RecoveryAction::completed;
    out.detail = "the move had switched to the new folder; it is now recorded as complete";
    return out;
  }
  if (j.pid != own_pid() && process_alive(j.pid) && now_seconds() - j.started < stale_after_seconds) {
    out.action = RecoveryAction::owner_alive;
    out.detail = "another Segarecomp process is still moving the data";
    return out;
  }
  std::vector<ManifestEntry> manifest;
  if (!read_manifest(bootstrap_file, j.id, manifest)) {
    out.action = RecoveryAction::journal_invalid;
    out.detail = "the migration manifest is missing or damaged; nothing was removed";
    return out;
  }
  if (rollback_destination(j, manifest)) {
    remove_journal(bootstrap_file);
    out.action = RecoveryAction::rolled_back;
    out.detail = "an unfinished move was undone; the old data folder remains in use";
  } else {
    out.action = RecoveryAction::rollback_incomplete;
    out.detail = "an unfinished move could not be undone completely; it is retried at the next start";
  }
  return out;
}

// ------------------------------------------------------------------------------------------------ completed migration and old-root cleanup

CompletedMigration read_completed_migration(const fs::path &bootstrap_file) {
  CompletedMigration c;
  Journal j;
  if (read_journal(bootstrap_file, j) != JournalState::ok || j.phase != "done") return c;
  const Bootstrap boot = read_bootstrap(bootstrap_file);
  if (boot.state != BootstrapState::valid || !(boot.root == j.dest || same_place(boot.root, j.dest))) return c;
  c.present = true;
  c.source = j.source;
  c.destination = j.dest;
  c.id = j.id;
  return c;
}

bool dismiss_completed_migration(const fs::path &bootstrap_file) {
  Journal j;
  const JournalState state = read_journal(bootstrap_file, j);
  if (state == JournalState::absent) return false;
  if (state == JournalState::ok && j.phase != "done") return false;
  remove_journal(bootstrap_file);
  return true;
}

namespace {

bool physically_inside(const fs::path &root, const fs::path &p) {
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(root, ec), b = fs::weakly_canonical(p, ec);
  return is_within(a, b);
}

// Is `p` the same place as `root` or lies inside it, after symlink resolution, ".." and trailing separators are normalized?
bool place_in_or_same(const fs::path &p, const fs::path &root) {
  if (p.empty() || root.empty()) return false;
  std::error_code ec;
  const auto canon = [&](const fs::path &p) { fs::path c = fs::weakly_canonical(p, ec).lexically_normal(); if (!c.has_filename() && c != c.root_path()) c = c.parent_path(); return c; };
  const fs::path cp = canon(p), cr = canon(root);
  return cp == cr || is_within(cr, cp) || same_place(p, root);
}

// The destination catalog must be a catalog the regular loader reads unchanged. Loading happens on a temporary COPY (Catalog::load() renames a damaged file aside and may restore the
// backup; the destination must not be touched by a check). Anything but CatalogLoad::loaded, a record the loader would drop, or a managed reference without its file is a "no".
struct CatalogCheck { bool ok = false; bool parsed = false; std::string why; std::set<fs::path> old_refs; };   // parsed: old_refs is complete even when ok is false

CatalogCheck check_destination_catalog(const fs::path &dest_root, const fs::path &old_root) {
  CatalogCheck r;
  std::error_code ec;
  const fs::path file = dest_root / "library" / "library.json";
  if (path_is_link(file) || !fs::is_regular_file(file, ec)) { r.why = "the new catalog is missing or not a regular file"; return r; }
  std::string text;
  if (!read_text(file, text, 64U << 20U) || text.empty()) { r.why = "the new catalog is empty or unreadable"; return r; }
  const fs::path tmp = fs::temp_directory_path(ec) / ("segarecomp-catalog-check-" + random_id());
  fs::create_directories(tmp, ec);
  struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{tmp};
  if (ec) { r.why = "cannot examine the new catalog"; return r; }
  {
    std::ofstream out(tmp / "library.json", std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.flush()) { r.why = "cannot examine the new catalog"; return r; }
  }
  Catalog catalog(tmp / "library.json");
  if (catalog.load() != CatalogLoad::loaded) { r.why = "the new catalog is not a readable library catalog"; return r; }
  const auto raw = json::parse(text);
  const json::Value *games = raw ? raw->get("games") : nullptr;
  if (games == nullptr || !games->is_array() || games->items.size() != catalog.entries().size()) { r.why = "the new catalog has records that cannot be read"; return r; }
  r.parsed = true;
  const fs::path lib = dest_root / "library";
  const auto classify = [&](const std::string &text_path, std::string &problem) {
    const fs::path p = from_u8(text_path);
    if (p.empty() || !p.is_absolute()) return;
    const fs::path canon_p = fs::weakly_canonical(p, ec);
    if (place_in_or_same(p, old_root)) r.old_refs.insert(canon_p);
    if (place_in_or_same(p, lib) && !fs::is_regular_file(p, ec) && problem.empty()) problem = "a library file it names is missing: " + u8(p.filename());
  };
  std::string problem;
  for (const LibraryEntry &e : catalog.entries()) {
    for (const auto &path : e.rom_paths) classify(path, problem);
    if (e.platform_id == "sega-cd") { classify(e.sega_cd.bios_path, problem); classify(e.sega_cd.cue_path, problem); classify(e.sega_cd.program.path, problem); }
    if (!e.cover.empty() && !fs::is_regular_file(lib / "covers" / from_u8(e.cover), ec) && problem.empty()) problem = "a cover it names is missing: " + e.cover;
    for (const auto &patch : e.patches)
      if (!fs::is_regular_file(lib / "patches" / from_u8(patch.identity + ".segapatch"), ec) && problem.empty()) problem = "an installed patch package it names is missing";
  }
  if (!problem.empty()) { r.why = problem; return r; }
  r.ok = true;
  return r;
}

// "" when no environment override touches the old root; otherwise the typed cleanup reason.
std::string active_environment_reason(const DataEnvironment &env, const fs::path &old_root) {
  if (env.cache_dir && place_in_or_same(from_u8(*env.cache_dir), old_root)) return "old_root_active";
  if (env.library_dir && place_in_or_same(from_u8(*env.library_dir), old_root)) return "library_active";
  if (env.home_dir && place_in_or_same(from_u8(*env.home_dir), old_root)) return "home_active";
  return "";
}

// The verified new copy is only a safe stand-in for the old file when it is a separate file in the new root: no directory of its path under the new root is a link (a link
// into the old root would make "old" and "new" the same inode, and removing the old path would delete the only copy), it resolves outside the old root and it is
// not the same file. Empty when trusted, otherwise the note for the kept item.
std::string new_copy_doubt(const fs::path &old_root, const fs::path &new_root, const fs::path &rel, const fs::path &old_file) {
  std::error_code ec;
  const fs::path new_file = new_root / rel;
  fs::path walk = new_root;
  const fs::path parent = rel.parent_path();
  for (const auto &part : parent) {
    if (part.empty() || part == ".") continue;
    walk /= part;
    if (path_is_link(walk)) return "a folder of the new copy is a link; kept";
  }
  if (path_is_link(new_file)) return "the new copy is a link; kept";
  const fs::path canon_new = fs::weakly_canonical(new_file, ec);
  if (ec || physically_inside(old_root, canon_new)) return "the new copy lies inside the old folder; kept";
  const bool same = fs::equivalent(old_file, new_file, ec);
  if (ec || same) return "the new copy is the same file as the original; kept";
  return {};
}

void add_cleanup_dir(CleanupPlan &plan, const fs::path &dir, const std::string &area) {
  const TreeMeasure m = measure_tree(dir);
  if (m.has_symlink || m.truncated) { plan.kept.push_back(u8(dir.filename()) + ": contains a link or is too large to examine; kept"); return; }
  plan.items.push_back({dir, m.bytes, true, CleanupKind::regenerable, area});
  plan.bytes += m.bytes;
}

} // namespace

CleanupPlan plan_old_root_cleanup(const fs::path &bootstrap_file, const DataEnvironment &env, bool include_copies) {
  CleanupPlan plan;
  Journal j;
  if (read_journal(bootstrap_file, j) != JournalState::ok || j.phase != "done") { plan.reason = "not_completed"; return plan; }
  const Bootstrap boot = read_bootstrap(bootstrap_file);
  if (boot.state != BootstrapState::valid || !(boot.root == j.dest || same_place(boot.root, j.dest))) { plan.reason = "bootstrap_mismatch"; return plan; }
  std::error_code ec;
  if (const std::string active = active_environment_reason(env, j.source); !active.empty()) { plan.reason = active; return plan; }
  if (same_place(j.source, j.dest) || !fs::is_directory(j.source, ec) || path_is_link(j.source)) { plan.reason = "old_root_missing"; return plan; }
  if (foreign_instance_running(bootstrap_file)) { plan.reason = "other_instance_running"; return plan; }
  const fs::path &S = j.source;
  // Regenerable items: the same shapes storage.cpp recognizes. Nothing else is ever a candidate.
  if (fs::is_directory(S / "games", ec) && !path_is_link(S / "games"))
    for (const auto &g : fs::directory_iterator(S / "games", ec))
      if (parse_dir_name(u8(g.path().filename())).valid && fs::is_directory(g.symlink_status(ec)) && !path_is_link(g.path())) add_cleanup_dir(plan, g.path(), "builds");
  for (const char *name : {"zig-cache", "sega-cd-objects", "cover-downloads"})
    if (fs::is_directory(S / name, ec) && !path_is_link(S / name)) add_cleanup_dir(plan, S / name, std::string(name) == "cover-downloads" ? "downloads" : "compiler_cache");
  if (fs::is_directory(S / "library" / "sega-cd", ec) && !path_is_link(S / "library") && !path_is_link(S / "library" / "sega-cd"))
    for (const auto &g : fs::directory_iterator(S / "library" / "sega-cd", ec))
      if (fs::is_directory(g.symlink_status(ec)) && !path_is_link(g.path()))
        for (const char *build : {"build", "build.partial", "build.failed"})
          if (fs::is_directory(g.path() / build, ec) && !path_is_link(g.path() / build)) add_cleanup_dir(plan, g.path() / build, "builds");
  if (include_copies) {
    std::vector<ManifestEntry> manifest;
    if (!read_manifest(bootstrap_file, j.id, manifest)) { plan.kept.push_back("the manifest is unreadable; no migrated copy is offered"); }
    else {
      const CatalogCheck catalog_check = check_destination_catalog(j.dest, S);
      for (const auto &e : manifest) {
        const fs::path old_file = S / e.rel, new_file = j.dest / e.rel;
        const auto note = [&](const char *why) { plan.kept.push_back(u8(e.rel) + ": " + why); };
        if (e.sha == "-") { note("not verified at migration time; kept"); continue; }
        if (e.rel == fs::path("library") / "library.json" && !catalog_check.ok) { plan.kept.push_back(u8(e.rel) + ": " + catalog_check.why + "; the original catalog is kept"); continue; }
        if (catalog_check.old_refs.count(fs::weakly_canonical(old_file, ec)) != 0) { note("the new catalog still refers to this file; kept"); continue; }
        if (path_is_link(old_file) || !fs::is_regular_file(old_file, ec)) { note("no longer a regular file; kept"); continue; }
        if (!fs::is_regular_file(new_file, ec) || path_is_link(new_file)) { note("no copy at the new location; kept"); continue; }
        if (const std::string doubt = new_copy_doubt(S, j.dest, e.rel, old_file); !doubt.empty()) { plan.kept.push_back(u8(e.rel) + ": " + doubt); continue; }
        const scd::Digest d = scd::hash_file(old_file, hash_limit);
        if (!d.ok || d.size != e.size || d.sha256 != e.sha) { note("changed since the move; kept"); continue; }
        // The new copy must still match what was verified at the move. The catalog is the one file the move rewrites by design (and the user may have updated it since): it was
        // validated above as a loadable catalog with its managed references instead of being compared byte for byte.
        if (e.rel != fs::path("library") / "library.json") { const scd::Digest nd = scd::hash_file(new_file, hash_limit); if (!nd.ok || nd.size != e.size || nd.sha256 != e.sha) { note("the new copy differs from what was verified at the move; kept"); continue; } }
        plan.items.push_back({old_file, e.size, false, CleanupKind::migrated_copy, "migrated_copy"});
        plan.bytes += e.size;
      }
    }
  }
  plan.eligible = true;
  return plan;
}

CleanupResult run_old_root_cleanup(const fs::path &bootstrap_file, const DataEnvironment &env, bool include_copies, BusyGuard &busy, const std::atomic<bool> *cancel, const CleanupHooks *hooks) {
  CleanupResult res;
  if (!busy.try_begin_maintenance()) { res.refused = true; res.reason = "busy"; return res; }
  struct Release { BusyGuard &b; ~Release() { b.end_maintenance(); } } release{busy};
  const CleanupPlan plan = plan_old_root_cleanup(bootstrap_file, env, include_copies);
  if (!plan.eligible) { res.refused = true; res.reason = plan.reason; return res; }
  Journal j;
  if (read_journal(bootstrap_file, j) != JournalState::ok) { res.refused = true; res.reason = "not_completed"; return res; }
  const fs::path &S = j.source;
  std::error_code ec;
  std::set<fs::path> parents;
  const fs::path catalog_file = S / "library" / "library.json";
  std::map<fs::path, ManifestEntry> manifest;   // read once; the plan is a preview, every file is verified again against it right before its removal
  if (include_copies) {
    std::vector<ManifestEntry> entries;
    if (read_manifest(bootstrap_file, j.id, entries)) for (const auto &e : entries) manifest[e.rel] = e;
  }
  for (const auto &item : plan.items) {
    if (cancel != nullptr && cancel->load()) { res.problems.push_back("cancelled; the remaining items were not removed"); break; }
    const std::string shown = u8(item.path);
    if (hooks != nullptr && hooks->before_remove) hooks->before_remove(item.path);
    if (const std::string active = active_environment_reason(env, S); !active.empty()) { res.refused = true; res.reason = active; break; }   // re-checked at the moment of deletion
    if (!is_within(S, item.path) || !physically_inside(S, item.path) || path_is_link(item.path)) { res.problems.push_back("outside the old folder or a link, skipped: " + shown); continue; }
    if (item.directory) {
      if (!fs::is_directory(item.path, ec) || measure_tree(item.path).has_symlink) { res.problems.push_back("changed or contains a link, skipped: " + shown); continue; }
      const auto removed = fs::remove_all(item.path, ec);
      if (ec) { res.problems.push_back("could not remove " + shown + ": " + ec.message()); continue; }
      res.removed += static_cast<std::size_t>(removed);
    } else {
      if (!fs::is_regular_file(item.path, ec)) { res.problems.push_back("changed, skipped: " + shown); continue; }
      if (item.kind == CleanupKind::migrated_copy) {   // re-checked at the moment of deletion: the original is removed only while a separate verified copy stands
        const fs::path rel = item.path.lexically_relative(S);
        const bool is_catalog = item.path == catalog_file;
        if (const std::string doubt = new_copy_doubt(S, j.dest, rel, item.path); !doubt.empty()) { res.problems.push_back("kept " + shown + ": " + doubt); continue; }
        const auto entry = manifest.find(rel);
        if (entry == manifest.end() || entry->second.sha == "-") { res.problems.push_back("kept " + shown + ": it is not in the verified migration record"); continue; }
        const scd::Digest od = scd::hash_file(item.path, hash_limit);
        if (!od.ok || od.size != entry->second.size || od.sha256 != entry->second.sha) { res.problems.push_back("kept " + shown + ": the original changed since it was verified"); continue; }
        const fs::path new_file = j.dest / rel;
        if (!is_catalog) {   // the catalog is the one file the move rewrites by design: structural validation below instead of byte identity
          const scd::Digest nd = scd::hash_file(new_file, hash_limit);
          if (!nd.ok || nd.size != entry->second.size || nd.sha256 != entry->second.sha) { res.problems.push_back("kept " + shown + ": the new copy differs from what was verified at the move"); continue; }
        }
        // The destination catalog as it is NOW: it must still be readable (otherwise nothing managed is deleted) and must not refer to this file.
        const CatalogCheck now = check_destination_catalog(j.dest, S);
        if (is_catalog && !now.ok) { res.problems.push_back("kept the original catalog: " + now.why); continue; }
        if (!now.parsed) { res.problems.push_back("kept " + shown + ": the new catalog cannot be read (" + now.why + ")"); continue; }
        std::error_code rec;
        if (now.old_refs.count(fs::weakly_canonical(item.path, rec)) != 0 || rec) { res.problems.push_back("kept " + shown + ": the new catalog now refers to it"); continue; }
      }
      if (!fs::remove(item.path, ec) || ec) { res.problems.push_back("could not remove " + shown); continue; }
      ++res.removed;
      for (fs::path d = item.path.parent_path(); is_within(S, d); d = d.parent_path()) parents.insert(d);
    }
    res.bytes_freed += item.bytes;
  }
  std::vector<fs::path> dirs(parents.begin(), parents.end());
  std::sort(dirs.begin(), dirs.end(), [](const fs::path &a, const fs::path &b) { return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end()); });
  for (const auto &d : dirs) fs::remove(d, ec);   // empty directories only
  if (include_copies && !res.refused && res.problems.empty() && plan.kept.empty()) {
    fs::remove(S / "library", ec);
    fs::remove(S, ec);
    remove_journal(bootstrap_file);
  }
  return res;
}

} // namespace launcher
