#include "library_catalog.hpp"

#include "json_lite.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace launcher {
namespace {

std::string utf8_of(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

bool read_file(const fs::path &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

std::string clamp_text(std::string text, std::size_t max) {
  if (text.size() > max) text.resize(max);
  return text;
}

json::Value to_json(const LibraryEntry &e) {
  json::Value v = json::Value::make_object();
  v.set("platform", json::Value::make_string(e.platform_id));
  v.set("sha256", json::Value::make_string(e.sha256));
  v.set("detected_title", json::Value::make_string(e.detected_title));
  v.set("display_title", json::Value::make_string(e.display_title));
  json::Value paths = json::Value::make_array();
  for (const auto &p : e.rom_paths) paths.push(json::Value::make_string(p));
  v.set("rom_paths", std::move(paths));
  v.set("rom_path_unknown", json::Value::make_bool(e.rom_path_unknown));
  v.set("mapper", json::Value::make_string(e.mapper));
  v.set("mapper_manifest_sha256", json::Value::make_string(e.mapper_manifest_sha256));
  v.set("aot_policy", json::Value::make_string(e.aot_policy));
  v.set("aot_model", json::Value::make_string(e.aot_model));
  v.set("hint_alias_discovery", json::Value::make_bool(e.hint_alias_discovery));
  v.set("region", json::Value::make_string(e.region));
  v.set("cover", json::Value::make_string(e.cover));
  v.set("favorite", json::Value::make_bool(e.favorite));
  v.set("archived", json::Value::make_bool(e.archived));
  v.set("added_at", json::Value::make_int(e.added_at));
  v.set("last_built_at", json::Value::make_int(e.last_built_at));
  v.set("last_played_at", json::Value::make_int(e.last_played_at));
  v.set("last_run", json::Value::make_string(e.last_run));
  v.set("last_exit_code", json::Value::make_int(e.last_exit_code));
  v.set("current_build_key", json::Value::make_string(e.current_build_key));
  json::Value builds = json::Value::make_array();
  for (const auto &b : e.builds) {
    json::Value o = json::Value::make_object();
    o.set("key", json::Value::make_string(b.key));
    o.set("built_at", json::Value::make_int(b.built_at));
    o.set("mode", json::Value::make_string(b.mode));
    o.set("version", json::Value::make_string(b.version));
    o.set("toolchain", json::Value::make_string(b.toolchain));
    builds.push(std::move(o));
  }
  v.set("builds", std::move(builds));
  return v;
}

// Validates and sanitizes one entry; nullopt when it cannot be an entry at all (bad platform or digest).
bool entry_from_json(const json::Value &v, LibraryEntry &e) {
  if (!v.is_object()) return false;
  e = LibraryEntry{};
  e.platform_id = v.str("platform");
  e.sha256 = v.str("sha256");
  if ((e.platform_id != "genesis" && e.platform_id != "master-system") || !is_hex_digest(e.sha256, 64)) return false;
  e.detected_title = clamp_text(v.str("detected_title"), 200);
  e.display_title = clamp_text(v.str("display_title"), 200);
  if (const json::Value *paths = v.get("rom_paths"); paths != nullptr && paths->is_array())
    for (const auto &p : paths->items)
      if (p.kind == json::Value::Kind::string && !p.string.empty() && p.string.size() < 4096 && e.rom_paths.size() < Catalog::max_paths &&
          std::find(e.rom_paths.begin(), e.rom_paths.end(), p.string) == e.rom_paths.end())
        e.rom_paths.push_back(p.string);
  e.rom_path_unknown = v.flag("rom_path_unknown") && e.rom_paths.empty();
  e.mapper = v.str("mapper");
  if (e.mapper != "sega" && e.mapper != "rom_only") e.mapper.clear();
  e.mapper_manifest_sha256 = v.str("mapper_manifest_sha256");
  if (!is_hex_digest(e.mapper_manifest_sha256, 64)) e.mapper_manifest_sha256.clear();
  e.aot_policy = v.str("aot_policy") == "optimized" ? "optimized" : "compatibility";
  e.aot_model = v.str("aot_model") == "gen2" ? "gen2" : "v1";
  e.hint_alias_discovery = v.flag("hint_alias_discovery");
  const std::string region = v.str("region");
  e.region = region == "japan" || region == "europe" ? region : "usa";
  e.cover = v.str("cover");
  // A cover is a bare file name inside the covers directory: no separators, no traversal.
  if (e.cover.find_first_of("/\\") != std::string::npos || e.cover == "." || e.cover == ".." || e.cover.size() > 128) e.cover.clear();
  e.favorite = v.flag("favorite");
  e.archived = v.flag("archived");
  e.added_at = std::max(0LL, v.num("added_at"));
  e.last_built_at = std::max(0LL, v.num("last_built_at"));
  e.last_played_at = std::max(0LL, v.num("last_played_at"));
  const std::string run = v.str("last_run");
  e.last_run = run == "clean" || run == "failed" || run == "unadmitted_pc" ? run : std::string();
  e.last_exit_code = static_cast<int>(std::clamp(v.num("last_exit_code"), -1000000LL, 1000000LL));
  e.current_build_key = v.str("current_build_key");
  if (!is_hex_digest(e.current_build_key, 32)) e.current_build_key.clear();
  if (const json::Value *builds = v.get("builds"); builds != nullptr && builds->is_array())
    for (const auto &b : builds->items) {
      if (!b.is_object()) continue;
      BuildRecord r;
      r.key = b.str("key");
      if (!is_hex_digest(r.key, 32)) continue;
      r.built_at = std::max(0LL, b.num("built_at"));
      r.mode = b.str("mode") == "optimized" ? "optimized" : "compatibility";
      r.version = clamp_text(b.str("version"), 64);
      r.toolchain = clamp_text(b.str("toolchain"), 64);
      e.builds.push_back(r);
    }
  if (e.builds.size() > Catalog::max_builds) e.builds.erase(e.builds.begin(), e.builds.end() - static_cast<std::ptrdiff_t>(Catalog::max_builds));
  return true;
}

} // namespace

bool is_hex_digest(const std::string &text, std::size_t length) {
  if (text.size() != length) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

Catalog::Catalog(fs::path file) : file_(std::move(file)) {}

bool Catalog::parse_text(const std::string &text, std::string *error) {
  std::string message;
  const auto root = json::parse(text, &message);
  if (!root || !root->is_object()) { if (error) *error = root ? "not an object" : message; return false; }
  const long long version = root->num("schema", -1);
  if (version < 1) { if (error) *error = "missing schema version"; return false; }
  const json::Value *games = root->get("games");
  if (games == nullptr || !games->is_array()) { if (error) *error = "missing games list"; return false; }
  entries_.clear();
  for (const auto &item : games->items) {
    LibraryEntry entry;
    if (!entry_from_json(item, entry)) continue;  // a damaged record is dropped, the rest of the library survives
    if (LibraryEntry *existing = find(entry.platform_id, entry.sha256)) {  // duplicate record: merge the tracked locations
      for (const auto &p : entry.rom_paths)
        if (existing->rom_paths.size() < max_paths && std::find(existing->rom_paths.begin(), existing->rom_paths.end(), p) == existing->rom_paths.end())
          existing->rom_paths.push_back(p);
      continue;
    }
    if (entries_.size() >= max_entries) break;
    entries_.push_back(std::move(entry));
  }
  read_only_ = version > schema_version;
  return true;
}

CatalogLoad Catalog::load() {
  entries_.clear();
  read_only_ = false;
  notice_.clear();
  std::error_code ec;
  const fs::path backup = fs::path(file_).concat(".bak");
  std::string text, error;
  const bool exists = fs::is_regular_file(file_, ec);
  if (exists && read_file(file_, text) && parse_text(text, &error)) {
    if (read_only_) { notice_ = "The library was written by a newer Segarecomp and is opened read-only."; return CatalogLoad::read_only_newer; }
    return CatalogLoad::loaded;
  }
  if (!exists && !fs::is_regular_file(backup, ec)) return CatalogLoad::created;
  // The main file is missing or damaged: try the previous good copy before giving up.
  if (fs::is_regular_file(backup, ec) && read_file(backup, text) && parse_text(text, &error)) {
    notice_ = exists ? "The library file was damaged; the previous copy was restored." : "The library file was missing; the previous copy was restored.";
    if (exists) {  // keep the damaged file for inspection, never delete it
      fs::path aside = fs::path(file_).concat(".corrupt");
      fs::rename(file_, aside, ec);
    }
    return CatalogLoad::recovered_from_backup;
  }
  entries_.clear();
  read_only_ = false;
  if (exists) {
    fs::path aside = fs::path(file_).concat(".corrupt");
    for (int n = 1; fs::exists(aside, ec) && n < 10; ++n) aside = fs::path(file_).concat(".corrupt" + std::to_string(n));
    fs::rename(file_, aside, ec);
    notice_ = "The library file could not be read (" + error + "); it was kept as " + utf8_of(aside.filename()) + " and a new library was started.";
  }
  return CatalogLoad::recovered_empty;
}

bool Catalog::save(std::string *error) {
  if (read_only_) { if (error) *error = "the library is read-only"; return false; }
  json::Value root = json::Value::make_object();
  root.set("schema", json::Value::make_int(schema_version));
  json::Value games = json::Value::make_array();
  for (const auto &e : entries_) games.push(to_json(e));
  root.set("games", std::move(games));
  const std::string text = json::dump(root);
  std::error_code ec;
  fs::create_directories(file_.parent_path(), ec);
  const fs::path tmp = fs::path(file_).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) { if (error) *error = "cannot write " + utf8_of(tmp); fs::remove(tmp, ec); return false; }
  }
  if (fs::is_regular_file(file_, ec)) fs::copy_file(file_, fs::path(file_).concat(".bak"), fs::copy_options::overwrite_existing, ec);
  fs::rename(tmp, file_, ec);
  if (ec) { if (error) *error = "cannot replace " + utf8_of(file_) + ": " + ec.message(); fs::remove(tmp, ec); return false; }
  return true;
}

LibraryEntry *Catalog::find(const std::string &platform_id, const std::string &sha256) {
  for (auto &e : entries_)
    if (e.platform_id == platform_id && e.sha256 == sha256) return &e;
  return nullptr;
}

LibraryEntry *Catalog::find_id(const std::string &id) {
  for (auto &e : entries_)
    if (e.id() == id) return &e;
  return nullptr;
}

Catalog::ImportResult Catalog::import_rom(const RomView &rom, long long now) {
  ImportResult result;
  if (read_only_) { result.error = "The library is read-only."; return result; }
  if (!rom.error.empty()) { result.error = rom.error; return result; }
  if (!rom.supported_platform || rom.platform_id.empty() || !is_hex_digest(rom.sha256, 64)) { result.error = "Not a supported Genesis or Master System ROM."; return result; }
  const std::string path = utf8_of(rom.path);
  LibraryEntry *entry = find(rom.platform_id, rom.sha256);
  if (entry == nullptr) {
    if (entries_.size() >= max_entries) { result.error = "The library is full."; return result; }
    LibraryEntry fresh;
    fresh.platform_id = rom.platform_id;
    fresh.sha256 = rom.sha256;
    fresh.detected_title = clamp_text(rom.title, 200);
    fresh.added_at = now;
    fresh.region = rom.region;
    fresh.mapper = rom.mapper;
    fresh.mapper_manifest_sha256 = rom.mapper_manifest_sha256;
    entries_.push_back(std::move(fresh));
    entry = &entries_.back();
    result.added = true;
  }
  if (!path.empty() && std::find(entry->rom_paths.begin(), entry->rom_paths.end(), path) == entry->rom_paths.end() && entry->rom_paths.size() < max_paths) {
    entry->rom_paths.push_back(path);
    entry->rom_path_unknown = false;
    result.path_added = !result.added;
  }
  if (!rom.mapper.empty()) entry->mapper = rom.mapper;
  if (!rom.mapper_manifest_sha256.empty()) entry->mapper_manifest_sha256 = rom.mapper_manifest_sha256;
  result.entry = entry;
  return result;
}

bool Catalog::relocate(LibraryEntry &entry, const fs::path &path, const std::string &sha256_of_file) {
  if (sha256_of_file != entry.sha256) return false;  // identity is the content, never the name
  const std::string text = utf8_of(path);
  entry.rom_paths.erase(std::remove(entry.rom_paths.begin(), entry.rom_paths.end(), text), entry.rom_paths.end());
  entry.rom_paths.insert(entry.rom_paths.begin(), text);
  if (entry.rom_paths.size() > max_paths) entry.rom_paths.resize(max_paths);
  entry.rom_path_unknown = false;
  return true;
}

bool Catalog::remove(const std::string &id) {
  const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const LibraryEntry &e) { return e.id() == id; });
  if (it == entries_.end()) return false;
  entries_.erase(it);
  return true;
}

void Catalog::record_build(LibraryEntry &entry, const BuildRecord &record) {
  entry.builds.erase(std::remove_if(entry.builds.begin(), entry.builds.end(), [&](const BuildRecord &b) { return b.key == record.key; }), entry.builds.end());
  entry.builds.push_back(record);
  if (entry.builds.size() > max_builds) entry.builds.erase(entry.builds.begin(), entry.builds.end() - static_cast<std::ptrdiff_t>(max_builds));
  entry.current_build_key = record.key;
  entry.last_built_at = record.built_at;
  entry.archived = false;
}

void Catalog::record_run(LibraryEntry &entry, int exit_code, const std::string &verdict, long long now) {
  entry.last_played_at = now;
  entry.last_exit_code = exit_code;
  entry.last_run = verdict;
}

std::size_t Catalog::migrate_cache(const fs::path &cache_root, long long now) {
  std::size_t added = 0;
  std::error_code ec;
  const fs::path games = cache_root / "games";
  if (read_only_ || !fs::is_directory(games, ec) || fs::is_symlink(games, ec)) return 0;
  std::size_t visited = 0;
  for (fs::directory_iterator it(games, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && visited < 20000; it.increment(ec), ++visited) {
    const fs::path dir = it->path();
    const std::string name = utf8_of(dir.filename());
    std::error_code sec;
    const auto status = fs::symlink_status(dir, sec);
    if (sec || status.type() != fs::file_type::directory) continue;  // symlinks and stray files are never followed
    if (!is_hex_digest(name, 32)) continue;                          // .partial / .failed and anything unknown
    if (!entry_ready(dir)) continue;                                 // only a verified finished build is adopted
    const fs::path meta_path = dir / "metadata.json";
    std::error_code fec;
    if (!fs::is_regular_file(meta_path, fec) || fs::file_size(meta_path, fec) > 8192U) continue;
    std::string text;
    if (!read_file(meta_path, text)) continue;
    const auto meta = json::parse(text);
    if (!meta || !meta->is_object()) continue;
    const std::string platform = meta->str("platform", "genesis");
    const std::string sha = meta->str("rom_sha256");
    if ((platform != "genesis" && platform != "master-system") || !is_hex_digest(sha, 64)) continue;
    LibraryEntry *entry = find(platform, sha);
    if (entry == nullptr) {
      if (entries_.size() >= max_entries) break;
      LibraryEntry fresh;
      fresh.platform_id = platform;
      fresh.sha256 = sha;
      fresh.detected_title = clamp_text(meta->str("title", "Unknown game"), 200);
      fresh.rom_path_unknown = true;  // older metadata only says "referenced, not copied": a path is never invented
      fresh.added_at = now;
      fresh.mapper = meta->str("mapper") == "sega" || meta->str("mapper") == "rom_only" ? meta->str("mapper") : std::string();
      entries_.push_back(std::move(fresh));
      entry = &entries_.back();
      ++added;
    }
    if (std::any_of(entry->builds.begin(), entry->builds.end(), [&](const BuildRecord &b) { return b.key == name; })) continue;
    BuildRecord record;
    record.key = name;
    const PolicyReport policy = read_policy_report(dir);
    record.mode = policy.present && policy.requested == "optimized" ? "optimized" : "compatibility";
    record.version = clamp_text(meta->str("segarecomp_version"), 64);
    record.toolchain = clamp_text(meta->str("toolchain"), 64);
    record.built_at = 0;  // the legacy record carries no build time and one is never invented
    if (entry->current_build_key.empty()) { entry->aot_policy = record.mode; }
    record_build(*entry, record);
  }
  return added;
}

RomView view_from_entry(const LibraryEntry &entry) {
  RomView view;
  if (!entry.rom_paths.empty()) view.path = fs::path(std::u8string(reinterpret_cast<const char8_t *>(entry.rom_paths.front().c_str())));
  view.sha256 = entry.sha256;
  view.title = entry.title();
  view.platform_id = entry.platform_id;
  view.supported_platform = true;
  view.platform = entry.platform_id == "master-system" ? "Master System" : "Genesis / Mega Drive";
  view.mapper = entry.mapper;
  view.mapper_manifest_sha256 = entry.mapper_manifest_sha256;
  view.aot_policy = entry.aot_policy;
  view.aot_model = entry.aot_model;
  view.hint_alias_discovery = entry.hint_alias_discovery && entry.platform_id == "genesis";
  view.region = entry.region;
  // The experimental SMS Optimized policy is only valid when the product states it; a stored request is honoured by the caller.
  view.sms_optimized_available = entry.platform_id == "master-system" && entry.aot_policy == "optimized";
  return view;
}

} // namespace launcher
