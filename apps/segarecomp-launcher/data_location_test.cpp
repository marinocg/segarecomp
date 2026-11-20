// SEG-150-T008 (ADR 0317): the data-location contract on synthetic trees: bootstrap + environment precedence, the plan, the journaled copy-verify-commit
// migration with every failure leaving the old root unchanged, recovery after a crash at each phase, and the separate old-root cleanup. No window, no product CLI.

#include "data_location.hpp"

#include <fstream>
#include <iostream>
#include <atomic>
#include <map>
#include <set>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
void spit(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
std::string slurp(const fs::path &p) { std::ifstream in(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); }
std::string pstr(const fs::path &p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }

// rel -> "size:sha" for every regular file, "link" for a symlink, "dir" for a directory: a whole-tree fingerprint.
using Snapshot = std::map<std::string, std::string>;
Snapshot snapshot(const fs::path &root) {
  Snapshot s;
  std::error_code ec;
  if (!fs::exists(root, ec)) { s["<absent>"] = ""; return s; }
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string rel = pstr(it->path().lexically_relative(root));
    const fs::file_status st = it->symlink_status(ec);
    if (fs::is_symlink(st)) s[rel] = "link";
    else if (fs::is_directory(st)) s[rel] = "dir";
    else { const scd::Digest d = scd::hash_file(it->path(), std::uintmax_t(1) << 32U); s[rel] = std::to_string(d.size) + ":" + d.sha256; }
  }
  return s;
}

std::string key32(char c) { return std::string(32, c); }
std::string sha64(char c) { return std::string(64, c); }
const std::string scd_id = "0123456789abcdef";

struct World {
  fs::path base, S, D, ext, bootstrap;
};

// A synthetic data root the way the launcher lays it out today (library/ durable, games/ zig-cache/ ... regenerable), plus external references.
World make_world(const std::string &name) {
  World w;
  w.base = fs::weakly_canonical(fs::temp_directory_path()) / ("segarecomp_data_location_" + name);
  fs::remove_all(w.base);
  w.S = w.base / "old";
  w.D = w.base / "new";
  w.ext = w.base / "external";
  w.bootstrap = w.base / "pointer" / "location.json";
  const fs::path L = w.S / "library";
  spit(w.ext / "game.md", "EXTERNAL ROM BYTES");
  spit(w.ext / "bios.bin", "EXTERNAL BIOS BYTES");
  spit(L / "launcher-settings.json", "{\"schema\":1,\"view\":\"list\"}\n");
  spit(L / "viewer-settings.cfg", "scale=3\n");
  spit(L / "sega-cd-bios.json", "{\"profiles\":[{\"path\":\"" + pstr(w.ext / "bios.bin") + "\"}]}\n");
  spit(L / "covers" / "a.png", std::string(300, 'c'));
  spit(L / "roms" / "extracted.md", "MANAGED ROM COPY");
  spit(L / "patches" / "pkg.segapatch", "PATCH PACKAGE BYTES");
  spit(L / "saves" / "genesis" / sha64('a') / "save.srm", std::string(64, 's'));
  spit(L / "saves" / "master-system" / sha64('b') / "slot0.state", std::string(80, 't'));
  spit(L / "sessions" / "genesis" / scd_id / "run.session", "frame 1\n");
  spit(L / "sega-cd" / scd_id / "backup.ram", std::string(32, 'r'));
  spit(L / "sega-cd" / scd_id / "input.rec", "rec\n");
  spit(L / "sega-cd" / scd_id / "build" / "prog", std::string(500, 'p'));
  spit(L / "sega-cd" / scd_id / "build.partial" / "x", "x");
  spit(L / "library.json.bak", "{}\n");
  spit(L / "notes.txt", "user dropped this");
  spit(w.S / "games" / key32('1') / "game", std::string(1000, 'g'));
  spit(w.S / "games" / key32('1') / "status.json", "{\"status\":\"ok\",\"path\":\"/old/abs/path\"}");
  spit(w.S / "games" / (key32('2') + ".failed") / "build.log", "log");
  spit(w.S / "zig-cache" / "z" / "o", std::string(200, 'z'));
  spit(w.S / "sega-cd-objects" / "o.o", "o");
  spit(w.S / "cover-downloads" / "c.tmp", "c");
  spit(w.S / "unknown.txt", "not ours");
  // The catalog: an external reference, a managed (extracted) ROM reference, and a Sega CD game whose program lives in a regenerable build folder.
  const std::string bios_digest = sha64('e'), cue_digest = sha64('f');
  Catalog catalog(L / "library.json");
  catalog.load();
  LibraryEntry g;
  g.platform_id = "genesis"; g.sha256 = sha64('a'); g.detected_title = "Synthetic A";
  g.rom_paths = {pstr(w.ext / "game.md"), pstr(L / "roms" / "extracted.md")};
  g.current_build_key = key32('1'); g.builds = {{key32('1'), 1, "compatibility", "", "", ""}};
  LibraryEntry m;
  m.platform_id = "genesis"; m.sha256 = sha64('c'); m.detected_title = "Managed only";
  m.rom_paths = {pstr(L / "roms" / "extracted.md")};
  LibraryEntry c;
  c.platform_id = "sega-cd"; c.sha256 = scd::entry_identity(bios_digest, cue_digest); c.detected_title = "Synthetic CD";
  c.sega_cd.bios_sha256 = bios_digest; c.sega_cd.cue_sha256 = cue_digest;
  c.sega_cd.bios_path = pstr(w.ext / "bios.bin");
  c.sega_cd.cue_path = pstr(w.ext / "disc.cue");
  c.sega_cd.program.path = pstr(L / "sega-cd" / scd_id / "build" / "prog");
  c.sega_cd.program.size = 500;
  c.sega_cd.program.sha256 = sha64('1'); c.sega_cd.program.program_identity = sha64('2'); c.sega_cd.program.disc_identity = sha64('3');
  catalog.entries() = {g, m, c};
  std::string error;
  if (!catalog.save(&error)) std::cerr << "fixture catalog: " << error << "\n";
  return w;
}

MigrationOptions options_for(const World &w) {
  MigrationOptions o;
  o.source = w.S;
  o.destination = w.D;
  o.bootstrap_file = w.bootstrap;
  o.free_space = [](const fs::path &) { return std::uintmax_t(1) << 40U; };
  return o;
}

bool has_refusal(const std::vector<Refused> &list, MigrationRefusal code) {
  for (const auto &r : list) if (r.code == code) return true;
  return false;
}

// The invariants every failed or refused attempt keeps: old root byte-for-byte, no bootstrap, no journal, destination exactly as before.
void expect_untouched(const World &w, const Snapshot &old_before, const Snapshot &dest_before, const std::string &what) {
  expect(snapshot(w.S) == old_before, what + ": old root unchanged");
  expect(snapshot(w.D) == dest_before, what + ": destination exactly as before");
  expect(!fs::exists(w.bootstrap), what + ": no bootstrap written");
  expect(!fs::exists(fs::path(w.bootstrap).concat(".journal")), what + ": no journal left");
  expect(!fs::exists(fs::path(w.bootstrap).concat(".manifest")), what + ": no manifest left");
  expect(read_bootstrap(w.bootstrap).state == BootstrapState::absent, what + ": bootstrap still absent");
}

void test_bootstrap_and_resolution() {
  const World w = make_world("resolve");
  const fs::path def = w.S;
  {
    const ResolvedRoots r = resolve_roots({}, def, w.bootstrap);
    expect(r.cache == def && r.library == def / "library" && r.cache_source == RootSource::default_location && r.notice.empty(), "absent bootstrap = today's defaults");
  }
  std::string error;
  expect(write_bootstrap(w.bootstrap, w.D, &error), "bootstrap write");
  expect(!fs::exists(fs::path(w.bootstrap).concat(".tmp")), "no temporary file left");
  expect(!write_bootstrap(w.base / "p2.json", "relative/root", &error) && !fs::exists(w.base / "p2.json"), "a relative root is refused");
  fs::create_directories(w.D);
  {
    const ResolvedRoots r = resolve_roots({}, def, w.bootstrap);
    expect(r.cache == w.D && r.library == w.D / "library" && r.cache_source == RootSource::bootstrap, "bootstrap names the active root");
  }
  {
    DataEnvironment env; env.cache_dir = pstr(w.base / "envcache");
    const ResolvedRoots r = resolve_roots(env, def, w.bootstrap);
    expect(r.cache == w.base / "envcache" && r.cache_source == RootSource::environment && r.bootstrap_ignored && !r.notice.empty(), "SEGARECOMP_CACHE_DIR beats the bootstrap and says so");
    DataEnvironment lib; lib.library_dir = pstr(w.base / "envlib");
    const ResolvedRoots r2 = resolve_roots(lib, def, w.bootstrap);
    expect(r2.cache == w.D && r2.library == w.base / "envlib" && r2.library_source == RootSource::environment, "SEGARECOMP_LIBRARY_DIR only moves the library");
  }
  fs::remove_all(w.D);
  {
    const ResolvedRoots r = resolve_roots({}, def, w.bootstrap);
    expect(r.cache == def && r.bootstrap_ignored && !r.notice.empty() && !fs::exists(w.D), "an unavailable bootstrapped root falls back to the default, creating nothing");
  }
  spit(w.bootstrap, "{ not json");
  expect(read_bootstrap(w.bootstrap).state == BootstrapState::damaged && resolve_roots({}, def, w.bootstrap).cache == def, "damaged bootstrap: default + notice");
  spit(w.bootstrap, "{\"schema\":9,\"root\":\"/x\"}");
  expect(read_bootstrap(w.bootstrap).state == BootstrapState::newer_schema && !resolve_roots({}, def, w.bootstrap).notice.empty(), "newer bootstrap schema: default + notice");
  fs::remove_all(w.base);
}

void test_upgrade_noop_and_plan() {
  const World w = make_world("plan");
  const Snapshot before = snapshot(w.S);
  expect(recover_pending_migration(w.bootstrap).action == RecoveryAction::none, "no journal: nothing to recover");
  expect(resolve_roots({}, w.S, w.bootstrap).cache == w.S, "an existing profile without a bootstrap keeps its root");
  expect(snapshot(w.S) == before && !fs::exists(w.bootstrap.parent_path()), "resolving and recovering create and move nothing");

  const MigrationPlan plan = build_migration_plan(options_for(w));
  expect(plan.ok(), "the synthetic tree plans cleanly");
  std::map<std::string, std::string> area_of;
  for (const auto &i : plan.items) { const auto g = i.rel.generic_u8string(); area_of[std::string(g.begin(), g.end())] = i.area; }
  expect(area_of["library/library.json"] == "catalog" && area_of["library/launcher-settings.json"] == "settings" && area_of["library/viewer-settings.cfg"] == "settings", "catalog and settings are durable");
  expect(area_of["library/sega-cd-bios.json"] == "bios_profiles" && area_of["library/covers/a.png"] == "covers" && area_of["library/roms/extracted.md"] == "managed_roms", "BIOS profile store, covers and managed ROM copies are durable");
  expect(area_of["library/patches/pkg.segapatch"] == "mods" && area_of["library/saves/genesis/" + sha64('a') + "/save.srm"] == "saves" && area_of["library/sessions/genesis/" + scd_id + "/run.session"] == "recordings", "mods, saves and recordings are durable");
  expect(area_of["library/sega-cd/" + scd_id + "/backup.ram"] == "sega_cd_data" && area_of["library/sega-cd/" + scd_id + "/input.rec"] == "sega_cd_data", "Sega CD backup RAM and recordings are durable");
  expect(area_of.size() == 12, "exactly the durable files are planned (" + std::to_string(area_of.size()) + ")");
  expect(area_of.count("library/sega-cd/" + scd_id + "/build/prog") == 0 && area_of.count("library/library.json.bak") == 0, "builds and the catalog backup are not planned");
  bool games = false, zig = false, scd_objects = false, downloads = false;
  for (const auto &a : plan.areas) if (a.cls == ItemClass::regenerable) { games |= a.area == "builds"; zig |= a.area == "compiler_cache"; downloads |= a.area == "downloads"; scd_objects |= a.area == "catalog_backup"; }
  expect(games && zig && downloads && scd_objects && plan.regenerable_entries == 7, "builds, compiler cache, downloads and the catalog backup are classed regenerable (entries " + std::to_string(plan.regenerable_entries) + ")");
  expect(plan.left_behind.size() == 2 && plan.left_behind[0] == "library/notes.txt" && plan.left_behind[1] == "unknown.txt", "unknown files are listed as left behind");
  expect(plan.required_bytes > plan.durable_bytes && plan.durable_bytes > 0, "the space estimate carries a margin");
  expect(snapshot(w.S) == before && !fs::exists(w.D), "planning is read-only");
  fs::remove_all(w.base);
}

void test_env_override_refusal() {
  const World w = make_world("env");
  const Snapshot old_before = snapshot(w.S), dest_before = snapshot(w.D);
  MigrationOptions o = options_for(w);
  o.env.cache_dir = pstr(w.base / "elsewhere");
  MigrationPlan plan = build_migration_plan(o);
  expect(!plan.ok() && has_refusal(plan.refusals, MigrationRefusal::env_override_cache), "SEGARECOMP_CACHE_DIR refuses migration");
  expect(plan.refusals.front().detail.find(*o.env.cache_dir) != std::string::npos && plan.refusals.front().detail.find("SEGARECOMP_CACHE_DIR") != std::string::npos, "the refusal explains the effective path");
  o.env = {};
  o.env.library_dir = pstr(w.base / "lib");
  plan = build_migration_plan(o);
  expect(has_refusal(plan.refusals, MigrationRefusal::env_override_library), "SEGARECOMP_LIBRARY_DIR refuses migration");
  BusyGuard busy;
  const MigrationResult r = run_migration(o, busy);
  expect(r.outcome == MigrationOutcome::refused && r.reason == "env_override_library" && !busy.maintenance(), "run_migration refuses, releases the guard");
  expect_untouched(w, old_before, dest_before, "env override");
  fs::remove_all(w.base);
}

void test_success_restart_and_cleanup() {
  const World w = make_world("success");
  const Snapshot old_before = snapshot(w.S);
  const Snapshot ext_before = snapshot(w.ext);
  BusyGuard busy;
  std::vector<std::string> phases;
  const MigrationResult r = run_migration(options_for(w), busy, nullptr, [&](const MigrationProgress &p) { if (phases.empty() || phases.back() != p.phase) phases.push_back(p.phase); });
  expect(r.ok() && r.files == 12, "migration completes (" + r.reason + " " + r.detail + ")");
  expect(phases == std::vector<std::string>({"plan", "copy", "rebase", "verify", "commit", "done"}), "progress reports every phase in order");
  expect(snapshot(w.S) == old_before, "the old root is byte-for-byte unchanged");
  expect(snapshot(w.ext) == ext_before, "external ROM/BIOS files are untouched");
  expect(!busy.any_activity(), "the maintenance guard is released");

  const Snapshot dest = snapshot(w.D);
  for (const auto &[rel, fp] : dest) {
    if (rel == "library/library.json" || rel == "library/library.json.bak" || fp == "dir") continue;
    expect(old_before.count(rel) != 0 && old_before.at(rel) == fp, "copied identically: " + rel);
  }
  expect(dest.count("games") == 0 && dest.count("zig-cache") == 0 && dest.count("library/sega-cd/" + scd_id + "/build") == 0 && dest.count("unknown.txt") == 0, "regenerable and unknown items are not copied");
  expect(dest.count(".segarecomp-migration-" + r.migration_id) == 0, "the ownership marker is gone after completion");
  expect(slurp(w.D / "library" / "sega-cd-bios.json") == slurp(w.S / "library" / "sega-cd-bios.json"), "the BIOS profile store keeps its external reference byte-for-byte");

  Catalog moved(w.D / "library" / "library.json");
  expect(moved.load() == CatalogLoad::loaded && moved.entries().size() == 3, "the new catalog loads with all games");
  const LibraryEntry *g = moved.find("genesis", sha64('a'));
  expect(g != nullptr && g->rom_paths.size() == 2 && g->rom_paths[0] == pstr(w.ext / "game.md") && g->rom_paths[1] == pstr(w.D / "library" / "roms" / "extracted.md"), "external reference kept, managed reference rebased");
  expect(fs::is_regular_file(w.D / "library" / "roms" / "extracted.md"), "the rebased reference resolves");
  const LibraryEntry *c = moved.find("sega-cd", scd::entry_identity(sha64('e'), sha64('f')));
  expect(c != nullptr && c->sega_cd.program.path.empty() && c->sega_cd.bios_path == pstr(w.ext / "bios.bin") && c->sega_cd.cue_path == pstr(w.ext / "disc.cue"), "a program in a regenerable build folder is cleared, disc/BIOS references untouched");
  expect(r.rebased_paths == 2 && r.cleared_programs == 1 && r.entries_needing_rebuild == 1, "the result counts rebased references, cleared programs and games needing a rebuild");
  Catalog source_catalog(w.S / "library" / "library.json");
  expect(source_catalog.load() == CatalogLoad::loaded && source_catalog.find("genesis", sha64('a'))->rom_paths[1] == pstr(w.S / "library" / "roms" / "extracted.md"), "the old catalog still points at the old root");

  // restart: a fresh process re-resolves and recovers
  const ResolvedRoots resolved = resolve_roots({}, w.S, w.bootstrap);
  expect(resolved.cache == w.D && resolved.library == w.D / "library" && resolved.cache_source == RootSource::bootstrap, "after restart the bootstrap names the new root");
  const RecoveryResult rec = recover_pending_migration(w.bootstrap);
  expect(rec.action == RecoveryAction::kept_completed, "restart keeps the completed record");
  expect(read_completed_migration(w.bootstrap).present && snapshot(w.D) == dest, "recovery changes nothing");

  // a second migration is refused until the first is cleaned up or dismissed
  MigrationOptions second = options_for(w);
  second.source = w.D;
  second.destination = w.base / "third";
  second.free_space = options_for(w).free_space;
  expect(has_refusal(build_migration_plan(second).refusals, MigrationRefusal::previous_migration_pending_cleanup), "double migration refused while the old root is pending cleanup");

  // ---- old-root cleanup ----
  DataEnvironment none;
  CleanupPlan plan = plan_old_root_cleanup(w.bootstrap, none, false);
  expect(plan.eligible, "cleanup is offered after a completed migration");
  std::set<std::string> listed;
  for (const auto &item : plan.items) listed.insert(pstr(item.path.lexically_relative(w.S)));
  const std::set<std::string> expected = {"games/" + key32('1'), "games/" + key32('2') + ".failed", "zig-cache", "sega-cd-objects", "cover-downloads", "library/sega-cd/" + scd_id + "/build", "library/sega-cd/" + scd_id + "/build.partial"};
  expect(listed == expected, "only regenerable items are listed by default");
  DataEnvironment active; active.cache_dir = pstr(w.S);
  expect(!plan_old_root_cleanup(w.bootstrap, active, false).eligible && plan_old_root_cleanup(w.bootstrap, active, false).reason == "old_root_active", "an old root still in use is never cleaned");
  BusyGuard other;
  expect(other.try_begin_build("x") && run_old_root_cleanup(w.bootstrap, none, false, other).refused, "cleanup refuses while a build runs");
  other.end_build("x");
  const CleanupResult cr = run_old_root_cleanup(w.bootstrap, none, false, busy);
  expect(cr.ok() && cr.removed > 0, "cleanup of regenerable items succeeds");
  Snapshot after = snapshot(w.S);
  for (const auto &[rel, fp] : old_before) {
    const bool regenerable = rel.rfind("games", 0) == 0 || rel.rfind("zig-cache", 0) == 0 || rel.rfind("sega-cd-objects", 0) == 0 || rel.rfind("cover-downloads", 0) == 0 || rel.find("/build") != std::string::npos;
    if (!regenerable) expect(after.count(rel) != 0 && after.at(rel) == fp, "kept after build cleanup: " + rel);
    else if (fp != "dir") expect(after.count(rel) == 0, "removed by build cleanup: " + rel);
  }
  expect(fs::exists(w.bootstrap) && read_completed_migration(w.bootstrap).present, "the bootstrap and the completion record survive build cleanup");
  expect(snapshot(w.D) == dest && snapshot(w.ext) == ext_before, "cleanup never touches the new root or external files");

  // migrated copies: only those still identical to what was verified
  spit(w.S / "library" / "covers" / "a.png", "edited after the move");
  const CleanupPlan copies = plan_old_root_cleanup(w.bootstrap, none, true);
  bool offered_changed = false, offered_save = false;
  for (const auto &item : copies.items) { offered_changed |= item.path.filename() == "a.png"; offered_save |= item.path.filename() == "save.srm"; }
  expect(!offered_changed && offered_save && !copies.kept.empty(), "a file changed since the move is kept; unchanged migrated copies are offered only on request");
  const CleanupResult full = run_old_root_cleanup(w.bootstrap, none, true, busy);
  expect(full.ok(), "cleanup of migrated copies succeeds");
  expect(fs::exists(w.S / "library" / "covers" / "a.png") && fs::exists(w.S / "unknown.txt") && fs::exists(w.S / "library" / "notes.txt"), "changed and unknown files are never removed");
  expect(!fs::exists(w.S / "library" / "saves") && !fs::exists(w.S / "library" / "library.json"), "verified-identical durable copies are removed on request");
  expect(snapshot(w.D) == dest, "the new root still intact");
  expect(fs::exists(w.bootstrap) && fs::exists(fs::path(w.bootstrap).concat(".journal")), "the record stays while something was kept");
  expect(dismiss_completed_migration(w.bootstrap) && !read_completed_migration(w.bootstrap).present && fs::exists(w.bootstrap), "dismissing forgets the record only");
  expect(run_old_root_cleanup(w.bootstrap, none, false, busy).refused, "after dismissal no old root is ever offered");
  fs::remove_all(w.base);
}

void test_cleanup_refusals() {
  const World w = make_world("cleanup_refuse");
  BusyGuard busy;
  DataEnvironment none;
  expect(!plan_old_root_cleanup(w.bootstrap, none, true).eligible && plan_old_root_cleanup(w.bootstrap, none, true).reason == "not_completed", "no migration: cleanup refused");
  expect(run_old_root_cleanup(w.bootstrap, none, true, busy).refused, "no migration: run refused");
  // an interrupted (not completed) migration is not cleanable either
  MigrationHooks hooks;
  hooks.crash_at = [](const char *point, std::size_t) { return std::string(point) == "verify"; };
  expect(run_migration(options_for(w), busy, nullptr, {}, &hooks).outcome == MigrationOutcome::interrupted, "crash during verify");
  expect(plan_old_root_cleanup(w.bootstrap, none, true).reason == "not_completed", "an unfinished migration is not cleanable");
  expect(recover_pending_migration(w.bootstrap).action == RecoveryAction::rolled_back, "recovered");
  // a completed migration whose bootstrap was changed since is not cleanable (the old root may now be the active one)
  expect(run_migration(options_for(w), busy).ok(), "migration");
  expect(write_bootstrap(w.bootstrap, w.S), "bootstrap pointed back at the old root");
  expect(plan_old_root_cleanup(w.bootstrap, none, true).reason == "bootstrap_mismatch", "bootstrap no longer names the destination: refused");
  fs::remove_all(w.base);
}

void test_destination_refusals() {
  const World w = make_world("refuse");
  const Snapshot old_at_start = snapshot(w.S);
  BusyGuard busy;
  const auto attempt = [&](const std::string &label, MigrationOptions o, MigrationRefusal expected, const fs::path &dest_probe) {
    const Snapshot old_before = snapshot(w.S);
    const Snapshot dest_before = snapshot(dest_probe);
    const MigrationPlan plan = build_migration_plan(o);
    expect(has_refusal(plan.refusals, expected), label + ": plan refuses with " + refusal_name(expected));
    const MigrationResult r = run_migration(o, busy);
    expect(r.outcome == MigrationOutcome::refused && has_refusal(r.refusals, expected), label + ": run refused");
    expect(snapshot(w.S) == old_before && snapshot(dest_probe) == dest_before && !fs::exists(w.bootstrap) && !fs::exists(fs::path(w.bootstrap).concat(".journal")), label + ": nothing changed");
  };
  MigrationOptions o = options_for(w);

  o.destination = w.S / "library" / "inside";
  attempt("dest inside source", o, MigrationRefusal::destination_inside_source, w.S);
  o.destination = w.base;
  attempt("source inside dest", o, MigrationRefusal::source_inside_destination, w.base / "external");
  o.destination = pstr(w.S) + "/";
  attempt("same location, other spelling", o, MigrationRefusal::destination_same, w.S);
  o.destination = w.base / "new" / ".." / "old";
  attempt("dotted path", o, MigrationRefusal::destination_unsafe, w.S);
  o.destination = "relative/dir";
  attempt("relative", o, MigrationRefusal::destination_not_absolute, w.base);
  o.destination = w.base.root_path();
  attempt("filesystem root", o, MigrationRefusal::destination_unsafe, w.ext);
  o.destination = w.base / "install" / "data";
  o.protected_paths = {w.base / "install"};
  attempt("inside the installation", o, MigrationRefusal::destination_unsafe, w.base / "install");
  o.protected_paths.clear();

  spit(w.base / "plainfile", "x");
  o.destination = w.base / "plainfile";
  attempt("destination is a file", o, MigrationRefusal::destination_not_directory, w.base / "plainfile");
  spit(w.base / "foreign" / "keep.txt", "foreign");
  o.destination = w.base / "foreign";
  attempt("non-empty foreign destination", o, MigrationRefusal::destination_not_empty, w.base / "foreign");

#if !defined(_WIN32)
  fs::create_directories(w.base / "target");
  fs::create_directory_symlink(w.base / "target", w.base / "alias");
  o.destination = w.base / "alias" / "data";
  attempt("symlinked parent", o, MigrationRefusal::destination_symlink, w.base / "target");
  o.destination = w.base / "alias";
  attempt("destination is itself a symlink", o, MigrationRefusal::destination_symlink, w.base / "target");
  // a link that points back into the source is also a nesting/alias attempt
  fs::create_directory_symlink(w.S, w.base / "alias2");
  o.destination = w.base / "alias2" / "x";
  attempt("symlink into the source", o, MigrationRefusal::destination_symlink, w.S);

  // permission: a parent without any write permission
  fs::create_directories(w.base / "ro");
  fs::permissions(w.base / "ro", fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
  o.destination = w.base / "ro" / "data";
  attempt("read-only parent", o, MigrationRefusal::destination_not_writable, w.base / "ro");
  fs::permissions(w.base / "ro", fs::perms::owner_all);
  if (geteuid() == 0) std::cout << "note: running as root; the refusal comes from the permission bits, an OS-level denial cannot be exercised here\n";
#endif

  // a link inside the source is never followed
#if !defined(_WIN32)
  fs::create_directory_symlink(w.ext, w.S / "library" / "covers" / "escape");
  o.destination = w.D;
  attempt("symlink inside the source", o, MigrationRefusal::source_symlink, w.D);
  fs::remove(w.S / "library" / "covers" / "escape");
#endif

  // space
  o = options_for(w);
  o.free_space = [](const fs::path &) { return std::uintmax_t(1000); };
  attempt("insufficient space", o, MigrationRefusal::insufficient_space, w.D);
  expect(build_migration_plan(o).available_bytes == 1000, "the plan reports the injected free space");

  // missing source
  o = options_for(w);
  o.source = w.base / "nowhere";
  attempt("missing source", o, MigrationRefusal::source_missing, w.D);

  // concurrent work
  o = options_for(w);
  const Snapshot dest_before = snapshot(w.D);
  expect(busy.try_begin_build("1234"), "a build is running");
  MigrationResult r = run_migration(o, busy);
  expect(r.outcome == MigrationOutcome::busy && snapshot(w.S) == old_at_start && snapshot(w.D) == dest_before && !fs::exists(w.bootstrap.parent_path()), "a running build blocks the migration; nothing written");
  busy.end_build("1234");
  expect(busy.try_begin_run("9"), "a game is running");
  expect(run_migration(o, busy).outcome == MigrationOutcome::busy, "a running game blocks the migration");
  busy.end_run("9");
  expect(busy.try_begin_maintenance() && run_migration(o, busy).outcome == MigrationOutcome::busy, "maintenance blocks the migration");
  busy.end_maintenance();
  expect(run_migration(o, busy).ok() && !busy.any_activity(), "with nothing running the same request succeeds");
  fs::remove_all(w.base);
}

void test_failures_and_recovery() {
  BusyGuard busy;
  // (point, index, expected recovery action)
  struct Crash { const char *point; std::size_t index; bool existing_dest; };
  for (const Crash &c : {Crash{"copy", 0, false}, Crash{"copy", 5, false}, Crash{"copy", 5, true}, Crash{"rebase", 0, false}, Crash{"verify", 0, false}, Crash{"verify", 7, true}, Crash{"commit_before", 0, false}}) {
    const std::string label = std::string("crash at ") + c.point + "#" + std::to_string(c.index) + (c.existing_dest ? " (existing empty destination)" : "");
    const World w = make_world("crash");
    if (c.existing_dest) fs::create_directories(w.D);
    const Snapshot old_before = snapshot(w.S), dest_before = snapshot(w.D);
    MigrationHooks hooks;
    hooks.crash_at = [&](const char *point, std::size_t index) { return std::string(point) == c.point && index == c.index; };
    const MigrationResult r = run_migration(options_for(w), busy, nullptr, {}, &hooks);
    expect(r.outcome == MigrationOutcome::interrupted && !busy.any_activity(), label + ": interrupted");
    expect(fs::exists(fs::path(w.bootstrap).concat(".journal")) && !fs::exists(w.bootstrap), label + ": journal left, bootstrap not written");
    expect(snapshot(w.S) == old_before, label + ": old root unchanged at the crash");
    expect(resolve_roots({}, w.S, w.bootstrap).cache == w.S, label + ": the old root is authoritative before recovery");
    // a foreign file that appears in the destination meanwhile must survive recovery
    const bool plant = std::string(c.point) == "copy" && c.index == 5;
    if (plant) spit(w.D / "foreign.txt", "not ours");
    const RecoveryResult rec = recover_pending_migration(w.bootstrap);
    expect(rec.action == RecoveryAction::rolled_back, label + ": rolled back (" + rec.detail + ")");
    expect(snapshot(w.S) == old_before && !fs::exists(w.bootstrap), label + ": old root unchanged after recovery");
    if (plant) {
      expect(fs::exists(w.D / "foreign.txt") && snapshot(w.D).size() == 1, label + ": only files this journal created were removed");
    } else {
      expect(snapshot(w.D) == dest_before, label + ": destination back to what it was");
    }
    expect(!fs::exists(fs::path(w.bootstrap).concat(".journal")) && !fs::exists(fs::path(w.bootstrap).concat(".manifest")), label + ": journal removed");
    expect(recover_pending_migration(w.bootstrap).action == RecoveryAction::none, label + ": recovery is idempotent");
    // and the same migration then succeeds from scratch
    if (!plant) expect(run_migration(options_for(w), busy).ok(), label + ": retry succeeds");
    fs::remove_all(w.base);
  }
  {
    // crash after the journal says "commit" but before the flip: still the old root
    const World w = make_world("commit_phase");
    const Snapshot old_before = snapshot(w.S);
    MigrationHooks hooks;
    hooks.crash_at = [](const char *point, std::size_t) { return std::string(point) == "commit_before"; };
    expect(run_migration(options_for(w), busy, nullptr, {}, &hooks).outcome == MigrationOutcome::interrupted, "crash before commit");
    const fs::path journal = fs::path(w.bootstrap).concat(".journal");
    std::string text = slurp(journal);
    const auto at = text.find("\"verify\"");
    expect(at != std::string::npos, "journal was in phase verify");
    text.replace(at, 8, "\"commit\"");
    spit(journal, text);
    expect(recover_pending_migration(w.bootstrap).action == RecoveryAction::rolled_back && !fs::exists(w.D) && snapshot(w.S) == old_before && !fs::exists(w.bootstrap), "phase commit without a flip: rolled back, old root authoritative");
    fs::remove_all(w.base);
  }
  {
    // crash right after the flip: the new root is authoritative and recovery completes the record
    const World w = make_world("after_flip");
    const Snapshot old_before = snapshot(w.S);
    MigrationHooks hooks;
    hooks.crash_at = [](const char *point, std::size_t) { return std::string(point) == "commit_after"; };
    expect(run_migration(options_for(w), busy, nullptr, {}, &hooks).outcome == MigrationOutcome::interrupted, "crash after the flip");
    expect(resolve_roots({}, w.S, w.bootstrap).cache == w.D, "the flip already happened: the new root is authoritative");
    const Snapshot dest = snapshot(w.D);
    const RecoveryResult rec = recover_pending_migration(w.bootstrap);
    expect(rec.action == RecoveryAction::completed && read_completed_migration(w.bootstrap).present, "recovery completes the record");
    expect(snapshot(w.S) == old_before && fs::is_regular_file(w.D / "library" / "library.json"), "old root retained, new root complete");
    for (const auto &[rel, fp] : snapshot(w.D)) expect(rel.rfind(".segarecomp-migration-", 0) != 0, "the marker is gone: " + rel);
    (void)dest;
    fs::remove_all(w.base);
  }
  {
    // verification mismatch: a copied file differs (same size) -> nothing is committed
    const World w = make_world("mismatch");
    const Snapshot old_before = snapshot(w.S), dest_before = snapshot(w.D);
    MigrationHooks hooks;
    std::size_t n = 0;
    hooks.after_copy = [&](const fs::path &copied) { if (++n == 4) { std::string text = slurp(copied); text[0] = static_cast<char>(text[0] ^ 1); spit(copied, text); } };
    const MigrationResult r = run_migration(options_for(w), busy, nullptr, {}, &hooks);
    expect(r.outcome == MigrationOutcome::failed && r.reason == "verify_failed", "a corrupted copy fails verification (" + r.reason + ")");
    expect_untouched(w, old_before, dest_before, "verification mismatch");
    fs::remove_all(w.base);
  }
  {
    // a source file that changes between plan and copy
    const World w = make_world("source_changed");
    const Snapshot dest_before = snapshot(w.D);
    MigrationHooks hooks;
    std::size_t n = 0;
    hooks.after_copy = [&](const fs::path &) { if (++n == 2) spit(w.S / "library" / "viewer-settings.cfg", "scale=4 and longer\n"); };
    const MigrationResult r = run_migration(options_for(w), busy, nullptr, {}, &hooks);
    expect(r.outcome == MigrationOutcome::failed && (r.reason == "source_changed"), "a source that changes mid-copy fails (" + r.reason + ")");
    expect(!fs::exists(w.bootstrap) && snapshot(w.D) == dest_before && !fs::exists(fs::path(w.bootstrap).concat(".journal")), "nothing committed, destination removed");
    fs::remove_all(w.base);
  }
  {
    // cancellation at several points
    for (const char *when : {"before", "copy", "verify"}) {
      const World w = make_world("cancel");
      const Snapshot old_before = snapshot(w.S), dest_before = snapshot(w.D);
      std::atomic<bool> cancel{std::string(when) == "before"};
      const MigrationResult r = run_migration(options_for(w), busy, &cancel, [&](const MigrationProgress &p) { if (p.phase == when && p.files_done == 3) cancel = true; });
      expect(r.outcome == MigrationOutcome::cancelled && !busy.any_activity(), std::string("cancel ") + when + ": cancelled");
      expect_untouched(w, old_before, dest_before, std::string("cancel ") + when);
      fs::remove_all(w.base);
    }
  }
  {
    // unreadable catalog copy cannot be rebased: fail closed
    const World w = make_world("bad_catalog");
    spit(w.S / "library" / "library.json", "{ not a catalog");
    const Snapshot old_before = snapshot(w.S), dest_before = snapshot(w.D);
    const MigrationResult r = run_migration(options_for(w), busy);
    expect(r.outcome == MigrationOutcome::failed && r.reason == "rebase_failed", "a damaged catalog is not migrated (" + r.reason + ")");
    expect_untouched(w, old_before, dest_before, "damaged catalog");
    fs::remove_all(w.base);
  }
}

void test_journal_damage() {
  const World w = make_world("journal");
  const Snapshot old_before = snapshot(w.S);
  const fs::path journal = fs::path(w.bootstrap).concat(".journal");
  for (const char *content : {"{ garbage", "{\"schema\":7,\"id\":\"x\"}", "{\"schema\":1,\"id\":\"0123456789abcdef\",\"phase\":\"bogus\",\"source\":\"/a\",\"destination\":\"/b\"}"}) {
    spit(journal, content);
    BusyGuard busy;
    const RecoveryResult rec = recover_pending_migration(w.bootstrap);
    expect(rec.action == RecoveryAction::journal_invalid, std::string("damaged or newer journal is left alone: ") + content);
    expect(has_refusal(build_migration_plan(options_for(w)).refusals, MigrationRefusal::journal_present), "a new migration is refused while the journal is unreadable");
    expect(run_migration(options_for(w), busy).outcome == MigrationOutcome::refused && !fs::exists(w.D) && snapshot(w.S) == old_before, "run refused, nothing changed");
    expect(fs::exists(journal), "the journal is kept for inspection");
  }
  expect(dismiss_completed_migration(w.bootstrap) && !fs::exists(journal), "an unreadable journal can be dismissed explicitly");
  BusyGuard busy;
  expect(run_migration(options_for(w), busy).ok(), "after dismissal the migration runs");

  // a journal whose manifest is missing: nothing may be removed
  const World v = make_world("journal2");
  MigrationHooks hooks;
  hooks.crash_at = [](const char *point, std::size_t index) { return std::string(point) == "copy" && index == 3; };
  expect(run_migration(options_for(v), busy, nullptr, {}, &hooks).outcome == MigrationOutcome::interrupted, "crash");
  const Snapshot partial = snapshot(v.D);
  fs::remove(fs::path(v.bootstrap).concat(".manifest"));
  expect(recover_pending_migration(v.bootstrap).action == RecoveryAction::journal_invalid && snapshot(v.D) == partial, "without the manifest recovery removes nothing");
  fs::remove_all(v.base);

#if !defined(_WIN32)
  // another live process owns the journal: not recovered underneath it
  const World x = make_world("journal3");
  expect(run_migration(options_for(x), busy, nullptr, {}, &hooks).outcome == MigrationOutcome::interrupted, "crash");
  const fs::path xj = fs::path(x.bootstrap).concat(".journal");
  std::string text = slurp(xj);
  const std::string own = "\"pid\": " + std::to_string(static_cast<long long>(getpid()));
  const auto at = text.find(own);
  expect(at != std::string::npos, "journal records the owner pid");
  text.replace(at, own.size(), "\"pid\": " + std::to_string(static_cast<long long>(getppid())));
  spit(xj, text);
  const Snapshot mid = snapshot(x.D);
  expect(recover_pending_migration(x.bootstrap).action == RecoveryAction::owner_alive && snapshot(x.D) == mid, "a journal owned by a live process is not recovered");
  fs::remove_all(x.base);
#endif
  fs::remove_all(w.base);
}

void test_double_migration_chain() {
  const World w = make_world("chain");
  BusyGuard busy;
  expect(run_migration(options_for(w), busy).ok(), "A -> B");
  expect(dismiss_completed_migration(w.bootstrap), "dismiss the record of A -> B");
  MigrationOptions second = options_for(w);
  second.source = resolve_roots({}, w.S, w.bootstrap).cache;
  second.destination = w.base / "third";
  expect(second.source == w.D, "B is the effective source");
  const Snapshot b_before = snapshot(w.D);
  const MigrationResult r = run_migration(second, busy);
  expect(r.ok() && resolve_roots({}, w.S, w.bootstrap).cache == w.base / "third", "B -> C");
  expect(snapshot(w.D) == b_before, "B unchanged");
  Catalog c(w.base / "third" / "library" / "library.json");
  expect(c.load() == CatalogLoad::loaded && c.find("genesis", sha64('a')) != nullptr && c.find("genesis", sha64('a'))->rom_paths[1] == pstr(w.base / "third" / "library" / "roms" / "extracted.md"), "references are rebased again, external ones kept");
  expect(c.find("genesis", sha64('a'))->rom_paths[0] == pstr(w.ext / "game.md"), "external reference survives a chain of moves");
  fs::remove_all(w.base);
}

// SEG-150-T009: every typed reason has plain text AND a next step (the screens show both); no code falls through to the generic fallback.
void test_plain_text_for_every_reason() {
  std::set<std::string> seen;
  for (int i = static_cast<int>(MigrationRefusal::env_override_cache); i <= static_cast<int>(MigrationRefusal::insufficient_space); ++i) {
    const auto code = static_cast<MigrationRefusal>(i);
    const PlainText t = refusal_text(code);
    expect(!t.what.empty() && !t.next.empty(), std::string("refusal text for ") + refusal_name(code));
    expect(t.what != "The move was refused.", std::string("refusal has a specific text: ") + refusal_name(code));
    expect(seen.insert(t.what).second, std::string("refusal text is distinct: ") + refusal_name(code));
    expect(failure_text(refusal_name(code)).what == t.what, std::string("a refusal name maps through failure_text: ") + refusal_name(code));
  }
  expect(refusal_text(MigrationRefusal::env_override_cache).what.find("SEGARECOMP_CACHE_DIR") != std::string::npos, "the cache override text names the variable");
  for (const char *reason : {"busy", "cancelled", "interrupted", "copy_failed", "source_changed", "rebase_failed", "verify_failed", "journal_write_failed", "bootstrap_write_failed"}) {
    const PlainText t = failure_text(reason);
    expect(!t.what.empty() && !t.next.empty() && t.what != "The move did not complete.", std::string("failure text for ") + reason);
  }
  for (const char *reason : {"not_completed", "bootstrap_mismatch", "old_root_active", "old_root_missing", "busy", "restart_needed"}) {
    const PlainText t = cleanup_refusal_text(reason);
    expect(!t.what.empty() && !t.next.empty() && t.what != "The cleanup was refused.", std::string("cleanup refusal text for ") + reason);
  }
}

// F1 (validation round 1): a valid saved location whose folder is missing (unplugged drive) must never be re-pointed by a move planned from the default location.
void test_saved_location_unavailable_refusal() {
  const World w = make_world("unavailable");
  const fs::path gone = w.base / "unplugged" / "Segarecomp";
  expect(write_bootstrap(w.bootstrap, gone), "a saved location is recorded");
  const std::string boot_before = slurp(w.bootstrap);
  const Snapshot old_before = snapshot(w.S), dest_before = snapshot(w.D);
  MigrationOptions o = options_for(w);
  MigrationPlan plan = build_migration_plan(o);
  expect(!plan.ok() && has_refusal(plan.refusals, MigrationRefusal::saved_location_unavailable), "an unavailable saved location refuses the plan");
  BusyGuard busy;
  const MigrationResult r = run_migration(o, busy);
  expect(r.outcome == MigrationOutcome::refused && r.reason == "saved_location_unavailable" && !busy.maintenance(), "run_migration refuses with the typed reason and releases the guard");
  expect(slurp(w.bootstrap) == boot_before, "the saved location file is byte-identical afterwards");
  expect(snapshot(w.S) == old_before && snapshot(w.D) == dest_before && !fs::exists(gone.parent_path()), "nothing was created or changed");
  expect(!fs::exists(fs::path(w.bootstrap).concat(".journal")) && !fs::exists(fs::path(w.bootstrap).concat(".manifest")), "no journal or manifest");
  const PlainText t = refusal_text(MigrationRefusal::saved_location_unavailable);
  expect(t.what.find("not available") != std::string::npos && t.what.find("drive") != std::string::npos && t.next.find("Reconnect") != std::string::npos, "the typed text names the cause and the way out");
  expect(std::string(refusal_name(MigrationRefusal::saved_location_unavailable)) == "saved_location_unavailable", "stable code");
  // reconnecting (the folder exists again) clears the state: the move is planned from the saved location
  fs::create_directories(gone);
  expect(!has_refusal(build_migration_plan(o).refusals, MigrationRefusal::saved_location_unavailable), "once the folder is back the refusal clears");
  fs::remove_all(w.base);
}

// F2/F6: a new copy that no longer matches what was verified keeps the old file; an intermediate directory replaced by a link is never followed by rollback.
void test_cleanup_verifies_new_copy_and_rollback_links() {
  {
    const World w = make_world("newcopy");
    BusyGuard busy;
    expect(run_migration(options_for(w), busy).ok(), "migration");
    DataEnvironment none;
    spit(w.D / "library" / "covers" / "a.png", "trunc");
    const CleanupPlan copies = plan_old_root_cleanup(w.bootstrap, none, true);
    bool offered_cover = false, offered_save = false;
    for (const auto &item : copies.items) { offered_cover |= item.path.filename() == "a.png"; offered_save |= item.path.filename() == "save.srm"; }
    expect(!offered_cover && offered_save, "a truncated new copy keeps the old file; intact ones are still offered");
    expect(run_old_root_cleanup(w.bootstrap, none, true, busy).ok() && fs::exists(w.S / "library" / "covers" / "a.png"), "run keeps the old file whose new copy differs");
    fs::remove_all(w.base);
  }
  {
    const World w = make_world("rollback_link");
    BusyGuard busy;
    MigrationHooks hooks;
    hooks.crash_at = [](const char *point, std::size_t) { return std::string(point) == "verify"; };
    expect(run_migration(options_for(w), busy, nullptr, {}, &hooks).outcome == MigrationOutcome::interrupted, "crash during verify");
    const fs::path real = w.base / "decoy";
    std::error_code ec;
    fs::rename(w.D / "library", real, ec);
    fs::create_directory_symlink(real, w.D / "library", ec);
    if (!ec) {
      const Snapshot decoy = snapshot(real);
      const RecoveryResult rec = recover_pending_migration(w.bootstrap);
      expect(rec.action == RecoveryAction::rollback_incomplete && snapshot(real) == decoy, "rollback never follows a link that replaced an intermediate directory");
    }
    fs::remove_all(w.base);
  }
}
} // namespace

int main() {
  test_plain_text_for_every_reason();
  test_bootstrap_and_resolution();
  test_upgrade_noop_and_plan();
  test_env_override_refusal();
  test_saved_location_unavailable_refusal();
  test_cleanup_verifies_new_copy_and_rollback_links();
  test_success_restart_and_cleanup();
  test_cleanup_refusals();
  test_destination_refusals();
  test_failures_and_recovery();
  test_journal_damage();
  test_double_migration_chain();
  if (failures != 0) { std::cerr << failures << " failure(s)\n"; return 1; }
  std::cout << "data_location_test: all checks passed\n";
  return 0;
}
