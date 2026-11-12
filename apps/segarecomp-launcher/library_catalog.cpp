#include "library_catalog.hpp"

#include "json_lite.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace launcher {

RomView view_from_entry_vanilla(const LibraryEntry &entry);  // defined below; the variant logic wraps it

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
  if (e.mapper_explicit) v.set("mapper_explicit", json::Value::make_bool(true));  // absent unless the user chose: every other record is byte-for-byte what it was
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
    if (!b.variant.empty()) o.set("variant", json::Value::make_string(b.variant));   // absent for Vanilla: a library without patches is byte-identical
    builds.push(std::move(o));
  }
  v.set("builds", std::move(builds));
  if (!e.patches.empty()) {
    json::Value list = json::Value::make_array();
    for (const auto &p : e.patches) {
      json::Value o = json::Value::make_object();
      o.set("identity", json::Value::make_string(p.identity));
      o.set("id", json::Value::make_string(p.id));
      o.set("version", json::Value::make_string(p.version));
      o.set("name", json::Value::make_string(p.name));
      o.set("target_sha256", json::Value::make_string(p.target_sha256));
      if (p.has_native()) {   // absent for a BPS-only package: such records are byte-identical to the ones older releases wrote
        o.set("native_source_sha256", json::Value::make_string(p.native_source_sha256));
        o.set("native_abi", json::Value::make_int(p.native_abi));
        o.set("native_caps", json::Value::make_int(p.native_caps));
        o.set("author", json::Value::make_string(p.author));
        if (!p.license.empty()) o.set("license", json::Value::make_string(p.license));
        o.set("native_approved", json::Value::make_int(p.native_approved ? 1 : 0));
      }
      o.set("width_max", json::Value::make_int(p.width_max));
      if (!p.widescreen.empty()) o.set("widescreen", json::Value::make_string(p.widescreen));
      if (!p.side_style.empty()) o.set("side_style", json::Value::make_string(p.side_style));
      o.set("installed_at", json::Value::make_int(p.installed_at));
      o.set("build_key", json::Value::make_string(p.build_key));
      list.push(std::move(o));
    }
    v.set("patches", std::move(list));
    if (!e.selected_variant.empty()) v.set("selected_variant", json::Value::make_string(e.selected_variant));
  }
  if (e.platform_id == "sega-cd") {  // ADR 0204: only these records carry the member
    const scd::EntryRecord &r = e.sega_cd;
    json::Value o = json::Value::make_object();
    o.set("identity_version", json::Value::make_int(r.identity_version));
    o.set("bios_path", json::Value::make_string(r.bios_path));
    o.set("bios_size", json::Value::make_int(static_cast<long long>(r.bios_size)));
    o.set("bios_sha256", json::Value::make_string(r.bios_sha256));
    o.set("cue_path", json::Value::make_string(r.cue_path));
    o.set("cue_size", json::Value::make_int(static_cast<long long>(r.cue_size)));
    o.set("cue_sha256", json::Value::make_string(r.cue_sha256));
    json::Value files = json::Value::make_array();
    for (const auto &f : r.disc_files) {
      json::Value file = json::Value::make_object();
      file.set("name", json::Value::make_string(f.name));
      file.set("size", json::Value::make_int(static_cast<long long>(f.size)));
      file.set("sha256", json::Value::make_string(f.sha256));
      files.push(std::move(file));
    }
    o.set("disc_files", std::move(files));
    o.set("disc_verified", json::Value::make_bool(r.disc_verified));
    json::Value program = json::Value::make_object();
    program.set("path", json::Value::make_string(r.program.path));
    program.set("size", json::Value::make_int(static_cast<long long>(r.program.size)));
    program.set("mtime", json::Value::make_int(r.program.mtime));
    program.set("sha256", json::Value::make_string(r.program.sha256));
    program.set("program_identity", json::Value::make_string(r.program.program_identity));
    program.set("disc_identity", json::Value::make_string(r.program.disc_identity));
    program.set("runtime_abi", json::Value::make_int(r.program.runtime_abi));
    program.set("viewer", json::Value::make_bool(r.program.viewer));
    program.set("verified_at", json::Value::make_int(r.program.verified_at));
    o.set("program", std::move(program));
    o.set("last_stop", json::Value::make_string(r.last_stop));
    if (r.experimental_m3) o.set("experimental_m3", json::Value::make_bool(true));          // SEG-097-T003: absent unless chosen, so older records are byte-identical
    if (r.program_experimental) o.set("program_experimental", json::Value::make_bool(true));
    if (r.file_first && !r.experimental_m3) o.set("file_first", json::Value::make_bool(true));                                   // SEG-103-T007: absent unless chosen
    if (!r.program_built_mode.empty()) o.set("program_built_mode", json::Value::make_string(r.program_built_mode));
    if (!r.program_file_first_inventory.empty()) o.set("program_file_first_inventory", json::Value::make_string(r.program_file_first_inventory));
    v.set("sega_cd", std::move(o));
  }
  return v;
}

std::uint64_t size_field(const json::Value &v, std::string_view key) { return static_cast<std::uint64_t>(std::max(0LL, v.num(key))); }

// A Sega CD record is accepted only when its identity is self-consistent: the entry key must be the identity of the stored BIOS and CUE digests
// (a hand-edited or damaged record is dropped, never half-trusted). Paths are references; nothing else about the files is kept.
bool sega_cd_from_json(const json::Value *o, LibraryEntry &e) {
  if (o == nullptr || !o->is_object()) return false;
  scd::EntryRecord r;
  r.identity_version = static_cast<int>(o->num("identity_version", 0));
  if (r.identity_version != scd::identity_version) return false;  // an unknown identity layout is not guessed at
  r.bios_path = clamp_text(o->str("bios_path"), 4096);
  r.bios_size = size_field(*o, "bios_size");
  r.bios_sha256 = o->str("bios_sha256");
  r.cue_path = clamp_text(o->str("cue_path"), 4096);
  r.cue_size = size_field(*o, "cue_size");
  r.cue_sha256 = o->str("cue_sha256");
  if (!is_hex_digest(r.bios_sha256, 64) || !is_hex_digest(r.cue_sha256, 64) || scd::entry_identity(r.bios_sha256, r.cue_sha256) != e.sha256) return false;
  if (const json::Value *files = o->get("disc_files"); files != nullptr && files->is_array())
    for (const auto &f : files->items) {
      if (!f.is_object() || r.disc_files.size() >= scd::max_disc_files) continue;
      scd::FileFact fact;
      fact.name = clamp_text(f.str("name"), 255);
      fact.size = size_field(f, "size");
      fact.sha256 = f.str("sha256");
      if (fact.name.empty() || fact.name.find_first_of("/\\:") != std::string::npos || !is_hex_digest(fact.sha256, 64)) { r.disc_files.clear(); break; }
      r.disc_files.push_back(std::move(fact));
    }
  r.disc_verified = o->flag("disc_verified") && !r.disc_files.empty();
  if (const json::Value *p = o->get("program"); p != nullptr && p->is_object() && !p->str("path").empty()) {
    r.program.path = clamp_text(p->str("path"), 4096);
    r.program.size = size_field(*p, "size");
    r.program.mtime = p->num("mtime");   // a file clock whose epoch is not the Unix epoch yields a negative stamp (libstdc++ before GCC 14): it must round-trip, or every reopened launcher reads a built program as changed
    r.program.sha256 = p->str("sha256");
    r.program.program_identity = p->str("program_identity");
    r.program.disc_identity = p->str("disc_identity");
    r.program.runtime_abi = static_cast<int>(std::clamp(p->num("runtime_abi"), 0LL, 1000LL));
    r.program.viewer = p->flag("viewer");
    r.program.verified_at = std::max(0LL, p->num("verified_at"));
    if (!is_hex_digest(r.program.sha256, 64) || !is_hex_digest(r.program.program_identity, 64) || !is_hex_digest(r.program.disc_identity, 64)) r.program = scd::ProgramRecord{};
  }
  r.last_stop = clamp_text(o->str("last_stop"), 64);
  r.experimental_m3 = o->flag("experimental_m3");
  r.program_experimental = o->flag("program_experimental");
  r.file_first = o->flag("file_first") && !r.experimental_m3;   // never both: the earlier-existing Optimized choice wins a hand-edited conflict
  {
    const std::string built = clamp_text(o->str("program_built_mode"), 16), inventory = clamp_text(o->str("program_file_first_inventory"), 16);
    if (built == "compatibility" || built == "file_first" || built == "optimized") r.program_built_mode = built;   // anything else reads as unknown
    if (r.program_built_mode.empty() && r.program_experimental) r.program_built_mode = "optimized";   // an earlier release recorded only this flag: its meaning is kept
    if (r.program_built_mode == "file_first" && (inventory == "complete" || inventory == "incomplete")) r.program_file_first_inventory = inventory;
  }
  e.sega_cd = std::move(r);
  if (e.rom_paths.empty() && !e.sega_cd.cue_path.empty()) e.rom_paths.push_back(e.sega_cd.cue_path);
  return true;
}

// Validates and sanitizes one entry; nullopt when it cannot be an entry at all (bad platform or digest).
bool entry_from_json(const json::Value &v, LibraryEntry &e) {
  if (!v.is_object()) return false;
  e = LibraryEntry{};
  e.platform_id = v.str("platform");
  e.sha256 = v.str("sha256");
  if ((e.platform_id != "genesis" && e.platform_id != "master-system" && e.platform_id != "sega-cd") || !is_hex_digest(e.sha256, 64)) return false;
  e.detected_title = clamp_text(v.str("detected_title"), 200);
  e.display_title = clamp_text(v.str("display_title"), 200);
  if (const json::Value *paths = v.get("rom_paths"); paths != nullptr && paths->is_array())
    for (const auto &p : paths->items)
      if (p.kind == json::Value::Kind::string && !p.string.empty() && p.string.size() < 4096 && e.rom_paths.size() < Catalog::max_paths &&
          std::find(e.rom_paths.begin(), e.rom_paths.end(), p.string) == e.rom_paths.end())
        e.rom_paths.push_back(p.string);
  e.rom_path_unknown = v.flag("rom_path_unknown") && e.rom_paths.empty();
  e.mapper = v.str("mapper");
  const bool mapper_known = e.platform_id == "genesis" ? e.mapper == "lock_on" : (e.platform_id == "master-system" && (e.mapper == "sega" || e.mapper == "rom_only"));
  if (!mapper_known) e.mapper.clear();
  e.mapper_explicit = e.platform_id == "genesis" && v.flag("mapper_explicit");
  e.mapper_manifest_sha256 = v.str("mapper_manifest_sha256");
  if (!is_hex_digest(e.mapper_manifest_sha256, 64)) e.mapper_manifest_sha256.clear();
  // A stored selection is honoured (an explicit Compatibility stays Compatibility); only a record without the member takes the default.
  if (v.get("aot_policy") != nullptr) e.aot_policy = v.str("aot_policy") == "optimized" ? "optimized" : "compatibility";
  else e.aot_policy = "optimized";
  // SEG-069-T002: the Genesis Optimized model is always gen2 now (it is meaningless for Compatibility and for Master System). A legacy
  // Optimized/v1 selection takes the new meaning: its v1 build has a different cache identity, so it is shown as needing a rebuild instead of being
  // reused, while a legacy Optimized/gen2 build keeps its identity.
  e.aot_model = e.platform_id == "genesis" ? "gen2" : "v1";
  e.hint_alias_discovery = v.flag("hint_alias_discovery");
  // ADR 0291: an earlier build of this feature stored a per-game "native_gems_audio" request. The sound implementation is now chosen automatically by the build, so the key is
  // neither read nor written (an old catalog stays readable; the key disappears on its next save).
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
      r.variant = b.str("variant");
      if (!r.variant.empty() && !is_hex_digest(r.variant, 64)) r.variant.clear();
      e.builds.push_back(r);
    }
  if (e.builds.size() > Catalog::max_builds) e.builds.erase(e.builds.begin(), e.builds.end() - static_cast<std::ptrdiff_t>(Catalog::max_builds));
  if (const json::Value *patches = v.get("patches"); patches != nullptr && patches->is_array() && e.platform_id == "genesis")
    for (const auto &p : patches->items) {
      if (!p.is_object() || e.patches.size() >= Catalog::max_patches) continue;
      InstalledPatch ip;
      ip.identity = p.str("identity");
      ip.target_sha256 = p.str("target_sha256");
      if (!is_hex_digest(ip.identity, 64) || !is_hex_digest(ip.target_sha256, 64) || e.find_patch(ip.identity) != nullptr) continue;  // a damaged record is dropped
      ip.id = clamp_text(p.str("id"), 64);
      ip.version = clamp_text(p.str("version"), 32);
      ip.name = clamp_text(p.str("name"), 64);
      ip.native_source_sha256 = p.str("native_source_sha256");
      if (!ip.native_source_sha256.empty() || p.get("native_abi") != nullptr) {   // a damaged native record is dropped with the patch: a native package never degrades into a BPS-only one
        ip.native_abi = static_cast<unsigned>(std::clamp(p.num("native_abi"), 0LL, 1000LL));
        ip.native_caps = static_cast<unsigned>(std::clamp(p.num("native_caps"), 0LL, 15LL));
        ip.author = clamp_text(p.str("author"), 64);
        ip.license = clamp_text(p.str("license"), 32);
        ip.native_approved = p.num("native_approved") == 1;
        if (ip.native_abi == 0 || ip.native_caps == 0 || ip.author.empty() || !is_hex_digest(ip.native_source_sha256, 64)) continue;
      }
      ip.width_max = static_cast<int>(std::clamp(p.num("width_max", -1), -1LL, 100LL));
      ip.installed_at = std::max(0LL, p.num("installed_at"));
      ip.widescreen = p.str("widescreen");
      if (ip.widescreen != "original" && ip.widescreen != "extended_backgrounds" && ip.widescreen != "enhanced_gameplay") ip.widescreen.clear();   // closed sets: anything else is no default
      ip.side_style = p.str("side_style");
      if (ip.side_style != "comfort" && ip.side_style != "borders" && ip.side_style != "raw") ip.side_style.clear();
      ip.build_key = p.str("build_key");
      if (!is_hex_digest(ip.build_key, 32)) ip.build_key.clear();
      e.patches.push_back(std::move(ip));
    }
  e.selected_variant = v.str("selected_variant");
  if (e.find_patch(e.selected_variant) == nullptr) e.selected_variant.clear();   // a selection must name an installed patch, else Vanilla
  if (e.platform_id == "sega-cd" && !sega_cd_from_json(v.get("sega_cd"), e)) return false;
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
  const bool has_sega_cd = std::any_of(entries_.begin(), entries_.end(), [](const LibraryEntry &e) { return e.platform_id == "sega-cd"; });
  const bool has_patches = std::any_of(entries_.begin(), entries_.end(), [](const LibraryEntry &e) { return !e.patches.empty(); });
  root.set("schema", json::Value::make_int(has_patches ? 3 : (has_sega_cd ? 2 : 1)));  // schema 1 stays byte-compatible for every library without a Sega CD game or patch
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
    fresh.aot_policy = "optimized";  // the launcher default for both platforms; Master System availability is resolved separately
    fresh.aot_model = rom.platform_id == "genesis" ? "gen2" : "v1";
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
  if (rom.platform_id == "genesis") {
    // SEG-122-T009: the digest matched, so this inspection describes this game. It refreshes the automatic result (an older entry gains it); an explicit choice survives any reimport.
    apply_cartridge_finding(*entry, finding_of(rom));
    entry->cartridge_examined = true;
  } else if (!rom.mapper.empty()) entry->mapper = rom.mapper;
  if (!rom.mapper_manifest_sha256.empty()) entry->mapper_manifest_sha256 = rom.mapper_manifest_sha256;
  result.entry = entry;
  return result;
}

Catalog::ImportResult Catalog::import_sega_cd(const scd::EntryRecord &record, const std::string &title, long long now) {
  ImportResult result;
  if (read_only_) { result.error = "The library is read-only."; return result; }
  if (!is_hex_digest(record.bios_sha256, 64) || !is_hex_digest(record.cue_sha256, 64) || record.bios_path.empty() || record.cue_path.empty()) {
    result.error = "A Sega CD game needs a BIOS image and a disc (CUE).";
    return result;
  }
  const std::string id_digest = scd::entry_identity(record.bios_sha256, record.cue_sha256);
  LibraryEntry *entry = find("sega-cd", id_digest);
  if (entry == nullptr) {
    if (entries_.size() >= max_entries) { result.error = "The library is full."; return result; }
    LibraryEntry fresh;
    fresh.platform_id = "sega-cd";
    fresh.sha256 = id_digest;
    fresh.detected_title = clamp_text(title, 200);
    fresh.added_at = now;
    fresh.aot_policy = "compatibility";
    fresh.aot_model = "v1";
    entries_.push_back(std::move(fresh));
    entry = &entries_.back();
    result.added = true;
  }
  const bool location_changed = entry->sega_cd.cue_path != record.cue_path || entry->sega_cd.bios_path != record.bios_path;
  // An existing program is kept when the new record has none (adding the same game again must not discard what was imported).
  scd::ProgramRecord keep_program = entry->sega_cd.program;
  std::vector<scd::FileFact> keep_files = entry->sega_cd.disc_files;
  const bool keep_verified = entry->sega_cd.disc_verified;
  const std::string keep_stop = entry->sega_cd.last_stop;
  const bool keep_program_experimental = entry->sega_cd.program_experimental;
  const bool keep_choice = entry->sega_cd.experimental_m3;  // the build option is the user's choice for this game; adding it again keeps it
  const bool keep_file_first = entry->sega_cd.file_first;
  const std::string keep_built_mode = entry->sega_cd.program_built_mode, keep_inventory = entry->sega_cd.program_file_first_inventory;
  entry->sega_cd = record;
  entry->sega_cd.experimental_m3 = keep_choice;
  entry->sega_cd.file_first = keep_file_first;
  if (record.program.path.empty()) {
    entry->sega_cd.program = keep_program;
    entry->sega_cd.disc_files = keep_files;
    entry->sega_cd.disc_verified = keep_verified;
    entry->sega_cd.last_stop = keep_stop;
    entry->sega_cd.program_experimental = keep_program_experimental;
    entry->sega_cd.program_built_mode = keep_built_mode;
    entry->sega_cd.program_file_first_inventory = keep_inventory;
  }
  entry->rom_paths.assign(1, record.cue_path);
  entry->rom_path_unknown = false;
  result.path_added = !result.added && location_changed;
  result.entry = entry;
  return result;
}

CartridgeCheck check_entry_cartridge(const LibraryEntry &entry) {
  CartridgeCheck check;
  if (entry.platform_id != "genesis") return check;
  for (const auto &text : entry.rom_paths) {
    const fs::path path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str())));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) continue;
    const auto size = fs::file_size(path, ec);
    if (ec) continue;
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(std::min<std::uintmax_t>(size, cartridge_header_bytes));
    if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) continue;
    check.examined = true;
    // An ordinary game stays ordinary without hashing it: only an image that could change the stored declaration is read whole.
    if (entry.mapper.empty() && !cartridge_header_resembles_lock_on(bytes)) continue;
    if (size > (std::uintmax_t{64} << 20U)) continue;   // no Genesis image is near this: a stray file is not read into memory
    bytes.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) continue;
    if (segarecomp::sha256_hex(bytes) != entry.sha256) continue;   // the file changed: it says nothing about this game
    check.verified = true;
    check.finding = recognize_genesis_cartridge(bytes);
    return check;
  }
  return check;
}

CartridgeFinding finding_of(const RomView &rom) {
  if (rom.mapper == "lock_on") return {CartridgeKind::lock_on, {}};
  return {rom.cartridge_note.empty() ? CartridgeKind::standard : CartridgeKind::ambiguous, rom.cartridge_note};
}

bool apply_cartridge_finding(LibraryEntry &entry, const CartridgeFinding &finding) {
  if (entry.platform_id != "genesis" || entry.mapper_explicit) return false;
  entry.cartridge_note = finding.note;
  entry.cartridge_unconfirmed = false;
  if (entry.mapper == finding.mapper()) return false;
  entry.mapper = finding.mapper();
  return true;
}

void select_cartridge(LibraryEntry &entry, int choice) {
  entry.cartridge_note.clear();
  entry.mapper_explicit = choice != 0;
  if (choice == 0) {
    entry.cartridge_examined = false;
    entry.cartridge_unconfirmed = true;
    ++entry.cartridge_generation;
  } else {
    entry.cartridge_unconfirmed = false;
    entry.mapper = choice == 2 ? "lock_on" : "";
  }
}

std::string cartridge_status_text(const LibraryEntry &entry) {
  if (entry.platform_id != "genesis") return {};
  if (entry.mapper_explicit) return entry.mapper.empty() ? "Standard cartridge - chosen manually" : "Lock-on cartridge - chosen manually";
  if (!entry.mapper.empty()) return entry.cartridge_unconfirmed ? "Lock-on cartridge - kept until the original ROM can be checked" : "Lock-on cartridge - automatically detected";
  return entry.cartridge_note.empty() ? std::string() : "Standard cartridge. " + entry.cartridge_note + " You can choose Lock-on in Advanced build options.";
}

bool Catalog::relocate(LibraryEntry &entry, const fs::path &path, const std::string &sha256_of_file) {
  if (sha256_of_file != entry.sha256) return false;  // identity is the content, never the name
  entry.cartridge_examined = false;   // SEG-122-T009: the original is available again
  ++entry.cartridge_generation;
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

const InstalledPatch *LibraryEntry::find_patch(const std::string &identity) const {
  if (identity.empty()) return nullptr;
  for (const auto &p : patches)
    if (p.identity == identity) return &p;
  return nullptr;
}

bool LibraryEntry::is_current_key(const std::string &key) const {
  if (key.empty()) return false;
  if (key == current_build_key) return true;
  return std::any_of(patches.begin(), patches.end(), [&](const InstalledPatch &p) { return p.build_key == key; });
}

void Catalog::record_build(LibraryEntry &entry, const BuildRecord &record) {
  entry.builds.erase(std::remove_if(entry.builds.begin(), entry.builds.end(), [&](const BuildRecord &b) { return b.key == record.key; }), entry.builds.end());
  entry.builds.push_back(record);
  if (entry.builds.size() > max_builds) entry.builds.erase(entry.builds.begin(), entry.builds.end() - static_cast<std::ptrdiff_t>(max_builds));
  if (record.variant.empty()) {
    entry.current_build_key = record.key;
  } else {  // a Patched build never replaces the Vanilla build: it becomes the published build of exactly its package
    for (auto &p : entry.patches)
      if (p.identity == record.variant) p.build_key = record.key;
  }
  if (record.built_at > 0) entry.last_built_at = record.built_at;  // an adopted build whose time is unknown never erases a recorded one
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
    if (!meta->str("patch_identity").empty()) continue;   // SEG-122: a Patched build is never a game of its own; it is adopted only through its library entry
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
      fresh.aot_model = platform == "genesis" ? "gen2" : "v1";
      fresh.rom_path_unknown = true;  // older metadata only says "referenced, not copied": a path is never invented
      fresh.added_at = now;
      fresh.mapper = (platform == "master-system" && (meta->str("mapper") == "sega" || meta->str("mapper") == "rom_only")) || (platform == "genesis" && meta->str("mapper") == "lock_on") ? meta->str("mapper") : std::string();
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

std::filesystem::path patch_package_path(const std::string &identity) { return library_root() / "patches" / (identity + ".segapatch"); }

RomView view_for_variant(const LibraryEntry &entry, const std::string &identity) {
  RomView view = view_from_entry_vanilla(entry);
  if (const InstalledPatch *p = entry.find_patch(identity); p != nullptr && entry.platform_id == "genesis") {
    view.patch_identity = p->identity;
    view.patch_package = patch_package_path(p->identity);
    view.patch_target_sha256 = p->target_sha256;
    view.patch_native_source_sha256 = p->native_source_sha256;
    view.patch_native_abi = p->native_abi;
    view.patch_native_caps = p->native_caps;
    view.patch_native_approved = p->native_approved;
    view.patch_label = p->name + (p->version.empty() ? std::string() : " " + p->version);
    view.patch_widescreen = p->widescreen;
    view.patch_side_style = p->side_style;
  }
  return view;
}

RomView view_from_entry(const LibraryEntry &entry) { return view_for_variant(entry, entry.selected_variant); }

RomView view_from_entry_vanilla(const LibraryEntry &entry) {
  RomView view;
  if (!entry.rom_paths.empty()) view.path = fs::path(std::u8string(reinterpret_cast<const char8_t *>(entry.rom_paths.front().c_str())));
  view.sha256 = entry.sha256;
  view.title = entry.title();
  view.platform_id = entry.platform_id;
  view.supported_platform = true;
  view.platform = entry.platform_id == "master-system" ? "Master System" : entry.platform_id == "sega-cd" ? "Sega CD" : "Genesis / Mega Drive";
  view.mapper = entry.mapper;
  view.mapper_manifest_sha256 = entry.mapper_manifest_sha256;
  view.aot_policy = entry.aot_policy;
  view.aot_model = entry.platform_id == "genesis" && entry.aot_policy == "optimized" ? "gen2" : entry.aot_model;  // Genesis Optimized is always gen2
  view.hint_alias_discovery = entry.hint_alias_discovery && entry.platform_id == "genesis";
  view.region = entry.region;
  // The experimental SMS Optimized policy is only valid when the product states it; a stored request is honoured by the caller.
  view.sms_optimized_available = entry.platform_id == "master-system" && entry.aot_policy == "optimized";
  return view;
}

} // namespace launcher
