// Storage maintenance contract on fake cache trees: exact reclaim semantics of every action, and fail-closed behaviour for symlink
// escapes, traversal, forged plans, busy entries and read-only trees. No window, no product CLI.

#include "storage.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

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
std::string key32(char c) { return std::string(32, c); }
std::string digest(char c) { return std::string(64, c); }

#if defined(_WIN32)
const char *exe_name = "game.exe";
#else
const char *exe_name = "game";
#endif

void make_build(const fs::path &dir, std::size_t exe_bytes = 1000) {
  spit(dir / exe_name, std::string(exe_bytes, 'x'));
  spit(dir / "status.json", "{\"status\":\"ok\"}");
  spit(dir / "metadata.json", "{}");
  spit(dir / "build.log", std::string(100, 'l'));
  spit(dir / "generated.c", std::string(200, 'c'));
  spit(dir / "generated" / "sms_0.c", std::string(300, 'c'));
  spit(dir / "generated-z80" / "z.c", std::string(50, 'c'));
  spit(dir / "obj" / "0.o", std::string(400, 'o'));
}

LibraryEntry entry(char sha, const std::string &platform = "genesis") {
  LibraryEntry e;
  e.platform_id = platform;
  e.sha256 = digest(sha);
  e.detected_title = std::string("Game ") + sha;
  return e;
}
} // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "segarecomp_storage_test";
  fs::remove_all(dir);
  const fs::path cache = dir / "cache", library = dir / "library", outside = dir / "outside";
  fs::create_directories(cache / "games");
  fs::create_directories(library / "covers");
  spit(outside / "precious.rom", "ROM BYTES");
  spit(library / "covers" / "a.png", "cover");
  const std::string rom_path = (outside / "precious.rom").string();

  expect(parse_dir_name(key32('a')).valid && parse_dir_name(key32('a')).kind == DirKind::current, "plain key");
  expect(parse_dir_name(key32('a') + ".partial").kind == DirKind::partial && parse_dir_name(key32('a') + ".failed").kind == DirKind::failed, "suffixes");
  expect(!parse_dir_name("zig-cache").valid && !parse_dir_name(key32('a') + ".bak").valid && !parse_dir_name(std::string(31, 'a')).valid, "non-keys");
  expect(is_within("/a/b", "/a/b/c") && !is_within("/a/b", "/a/b") && !is_within("/a/b", "/a/bc") && !is_within("/a/b", "/a/b/../c"), "is_within is strict and lexical");

  Catalog catalog(library / "library.json");
  catalog.load();
  LibraryEntry a = entry('a'); a.rom_paths = {rom_path}; a.current_build_key = key32('1'); a.builds = {{key32('2'), 1, "compatibility", "", ""}, {key32('1'), 2, "compatibility", "", ""}};
  LibraryEntry b = entry('b'); b.current_build_key = key32('3'); b.builds = {{key32('3'), 3, "compatibility", "", ""}}; b.favorite = true;
  catalog.entries() = {a, b};
  const std::string ida = a.id(), idb = b.id();

  make_build(cache / "games" / key32('1'));
  make_build(cache / "games" / key32('2'), 500);                                   // superseded build of A
  make_build(cache / "games" / key32('3'));
  spit(cache / "games" / (key32('1') + ".failed") / "build.log", std::string(70, 'f'));
  spit(cache / "games" / (key32('1') + ".failed") / "obj" / "x.o", std::string(30, 'f'));
  spit(cache / "games" / (key32('9') + ".partial") / "obj" / "y.o", std::string(40, 'p'));   // unowned partial
  spit(cache / "games" / "mystery" / "keep.me", "unknown");                                 // unrecognized folder
  spit(cache / "zig-cache" / "o" / "blob", std::string(900, 'z'));
  spit(cache / "zig-cache" / "h" / "blob2", std::string(100, 'z'));

  BusyGuard busy;
  StorageReport report = scan_storage(catalog, cache, library);
  expect(report.complete, "scan completes");
  const EntryUsage *ua = report.find(ida);
  expect(ua != nullptr && ua->dirs.size() == 3, "A owns current, superseded and failed folders");
  expect(ua != nullptr && ua->intermediates == (200 + 300 + 50 + 400) * 2 && report.zig_cache == 1000, "intermediates and zig cache sized exactly");
  expect(report.unlinked.size() == 2 && report.failed_partial == 100 + 40, "unrecognized and unowned partial folders are reported as unlinked; failed/partial sized");
  expect(report.catalog_and_covers >= 5, "library data is accounted separately");

  {  // intermediates: executable survives, game stays ready
    Plan plan = plan_intermediates(report, {ida}, busy);
    expect(plan.bytes == (200 + 300 + 50 + 400) * 2, "plan bytes = exactly the generated C and objects of every tracked build");
    expect(busy.try_begin_maintenance(), "maintenance lock");
    ExecResult r = execute_plan(plan, cache, busy);
    busy.end_maintenance();
    expect(r.ok() && r.bytes_freed == plan.bytes, "freed what was planned");
    expect(entry_ready(cache / "games" / key32('1')) && fs::exists(cache / "games" / key32('1') / "build.log") && !fs::exists(cache / "games" / key32('1') / "obj") &&
               !fs::exists(cache / "games" / key32('1') / "generated.c") && !fs::exists(cache / "games" / key32('1') / "generated"),
           "executable and status survive, intermediates are gone");
    expect(fs::exists(cache / "games" / key32('3') / "obj"), "other games are untouched");
  }
  report = scan_storage(catalog, cache, library);
  {  // superseded
    Plan plan = plan_superseded(report, {ida}, busy);
    expect(plan.items.size() == 1 && plan.items[0].path.filename() == key32('2'), "only the superseded build");
  }
  {  // failed/partial: diagnostics kept on request
    Plan keep = plan_failed_partial(report, true, busy);
    expect(busy.try_begin_maintenance(), "lock");
    ExecResult r = execute_plan(keep, cache, busy);
    busy.end_maintenance();
    expect(r.ok() && fs::exists(cache / "games" / (key32('1') + ".failed") / "build.log") && !fs::exists(cache / "games" / (key32('1') + ".failed") / "obj") &&
               !fs::exists(cache / "games" / (key32('9') + ".partial")),
           "failed build keeps build.log; partial folder removed");
    expect(fs::exists(cache / "games" / "mystery" / "keep.me") && entry_ready(cache / "games" / key32('1')), "unknown folders and finished builds untouched");
    report = scan_storage(catalog, cache, library);
    Plan all = plan_failed_partial(report, false, busy);
    busy.try_begin_maintenance();
    ExecResult r2 = execute_plan(all, cache, busy);
    busy.end_maintenance();
    expect(r2.ok() && !fs::exists(cache / "games" / (key32('1') + ".failed")), "full failed cleanup removes the failed folder");
  }
  report = scan_storage(catalog, cache, library);
  {  // zig cache: refused while a build runs, then cleared; games untouched
    expect(busy.try_begin_build(key32('3')), "build starts");
    Plan blocked = plan_zig_cache(report, cache, busy);
    expect(blocked.items.empty() && !blocked.excluded.empty(), "no plan while a build runs");
    expect(!busy.try_begin_maintenance(), "maintenance refused while a build runs");
    busy.end_build(key32('3'));
    Plan plan = plan_zig_cache(report, cache, busy);
    expect(plan.bytes == 1000, "zig plan sized");
    busy.try_begin_maintenance();
    ExecResult r = execute_plan(plan, cache, busy);
    busy.end_maintenance();
    expect(r.ok() && fs::is_empty(cache / "zig-cache") && entry_ready(cache / "games" / key32('3')), "compiler cache cleared independently");
  }
  {  // busy entry: excluded from planning, and a stale plan is refused at execution
    Plan stale = plan_archive(report, {idb}, busy);
    expect(!stale.items.empty(), "archive plan for B");
    expect(busy.try_begin_run(key32('3')), "game B runs");
    Plan fresh = plan_archive(report, {idb}, busy);
    expect(fresh.items.empty() && !fresh.excluded.empty(), "running game excluded from the plan");
    expect(!busy.try_begin_maintenance(), "no maintenance while a game runs");
    busy.end_run(key32('3'));
    expect(busy.try_begin_maintenance(), "lock");
    busy.end_maintenance();
    ExecResult unlocked = execute_plan(stale, cache, busy);
    expect(!unlocked.ok() && fs::exists(cache / "games" / key32('3')), "execution without the maintenance lock is refused");
  }
  {  // symlink escapes: a link inside a build folder pointing at the user's files
    std::error_code ec;
    fs::create_directory_symlink(outside, cache / "games" / key32('3') / "linked", ec);
    fs::create_symlink(outside / "precious.rom", cache / "games" / key32('3') / "linked.rom", ec);
    if (!ec) {
      report = scan_storage(catalog, cache, library);
      expect(report.find(idb)->dirs[0].has_symlink, "links are reported");
      Plan plan = plan_archive(report, {idb}, busy);
      busy.try_begin_maintenance();
      ExecResult r = execute_plan(plan, cache, busy);
      busy.end_maintenance();
      expect(fs::exists(outside / "precious.rom") && fs::exists(outside), "the target of a link is never deleted or followed");
      expect(!r.ok() && fs::is_symlink(cache / "games" / key32('3') / "linked"), "the link is skipped and reported, not removed");
      fs::remove(cache / "games" / key32('3') / "linked", ec);
      fs::remove(cache / "games" / key32('3') / "linked.rom", ec);
      Plan again = plan_archive(scan_storage(catalog, cache, library), {idb}, busy);
      busy.try_begin_maintenance();
      ExecResult r2 = execute_plan(again, cache, busy);
      busy.end_maintenance();
      expect(r2.ok() && !fs::exists(cache / "games" / key32('3')), "once the link is gone the folder is removed");
    }
    // a symlinked games/ directory and a symlinked entry folder
    const fs::path evil = dir / "evil";
    spit(evil / key32('4') / "precious.txt", "keep");
    fs::create_directory_symlink(evil / key32('4'), cache / "games" / key32('4'), ec);
    if (!ec) {
      Plan forged;
      forged.kind = PlanKind::archive;
      forged.items.push_back({cache / "games" / key32('4'), 4, true});
      busy.try_begin_maintenance();
      ExecResult r = execute_plan(forged, cache, busy);
      busy.end_maintenance();
      expect(!r.ok() && fs::exists(evil / key32('4') / "precious.txt"), "a symlinked entry folder is refused");
    }
  }
  {  // forged plans: traversal, absolute paths, unmanaged names
    const fs::path victim = outside / "precious.rom";
    const std::vector<fs::path> hostile = {
        cache / ".." / "outside" / "precious.rom", outside / "precious.rom", cache / "games" / key32('1') / ".." / ".." / ".." / "outside" / "precious.rom",
        cache / "library.json", cache / "games" / "mystery", cache / "games" / "mystery" / "keep.me", cache / "games", cache, cache / "zig-cache" / "a" / "b" / "c"};
    for (const auto &path : hostile) {
      Plan forged;
      forged.kind = PlanKind::archive;
      forged.items.push_back({path, 1, false});
      forged.items.push_back({path, 1, true});
      busy.try_begin_maintenance();
      ExecResult r = execute_plan(forged, cache, busy);
      busy.end_maintenance();
      expect(!r.ok() && fs::exists(victim) && fs::exists(cache / "games" / "mystery" / "keep.me") && fs::exists(cache / "games"), "forged item refused: " + path.string());
    }
  }
  {  // Legitimate plans become stale: links INTO the cache are not ownership.
    const fs::path protected_dir = cache / "library";
    spit(protected_dir / "generated.c", "protected sentinel");
    spit(protected_dir / "obj" / "keep.rom", "protected ROM");
    const fs::path owned = cache / "games" / key32('1');
    make_build(owned);
    const Plan original = plan_intermediates(scan_storage(catalog, cache, library), {ida}, busy);
    fs::rename(owned, cache / "saved-build");
    std::error_code ec;
    fs::create_directory_symlink(protected_dir, owned, ec);
    if (!ec) {
      busy.try_begin_maintenance();
      const auto r = execute_plan(original, cache, busy);
      busy.end_maintenance();
      expect(!r.ok() && fs::exists(protected_dir / "generated.c") && fs::exists(protected_dir / "obj" / "keep.rom"), "original intermediates plan rejects in-cache ancestor link");
      fs::remove(owned);
    }
    fs::rename(cache / "saved-build", owned);
    // All deletion kinds use the same traversal, including games/ and zig-cache/ ancestors.
    const auto rep = scan_storage(catalog, cache, library);
    spit(cache / "games" / (key32('1') + ".failed") / "generated.c", "failed intermediate");
    const auto failed_report = scan_storage(catalog, cache, library);
    const std::vector<Plan> plans = {plan_intermediates(rep, {ida}, busy), plan_archive(rep, {ida}, busy), plan_superseded(rep, {ida}, busy), plan_remove_from_library(rep, {ida}, true, busy),
                                  plan_failed_partial(failed_report, true, busy), plan_failed_partial(failed_report, false, busy)};
    spit(protected_dir / key32('1') / "generated.c", "protected");
    fs::rename(cache / "games", cache / "saved-games");
    fs::create_directory_symlink(protected_dir, cache / "games", ec);
    if (!ec) {
      for (const auto &plan : plans) {
        busy.try_begin_maintenance(); const auto r = execute_plan(plan, cache, busy); busy.end_maintenance();
        expect(!r.ok() && fs::exists(protected_dir / key32('1') / "generated.c"), "every games deletion kind rejects ancestor link");
      }
      fs::remove(cache / "games");
    }
    fs::rename(cache / "saved-games", cache / "games");
    spit(cache / "zig-cache" / "generated.c", "compiler intermediate");
    const auto zig = plan_zig_cache(rep, cache, busy);
    fs::rename(cache / "zig-cache", cache / "saved-zig");
    fs::create_directory_symlink(protected_dir, cache / "zig-cache", ec);
    if (!ec) {
      busy.try_begin_maintenance(); const auto r = execute_plan(zig, cache, busy); busy.end_maintenance();
      expect(!r.ok() && fs::exists(protected_dir / "generated.c"), "compiler cache rejects ancestor link into library");
      fs::remove(cache / "zig-cache");
    }
    fs::rename(cache / "saved-zig", cache / "zig-cache");
  }
#if !defined(_WIN32)
  {  // Bounded concurrent rename/link replacement of a managed ancestor.
    const fs::path owned = cache / "games" / key32('7'), saved = cache / "racing-build", protected_dir = cache / "library";
    spit(owned / "generated.c", "intermediate");
    Plan plan;
    plan.items.push_back({owned / "generated.c", 12, false});
    std::atomic<bool> started{false};
    std::thread attacker([&] {
      for (int i = 0; i < 300; ++i) {
        fs::rename(owned, saved);
        fs::create_directory_symlink(protected_dir, owned);
        started = true;
        std::this_thread::yield();
        fs::remove(owned);
        fs::rename(saved, owned);
      }
    });
    while (!started.load()) std::this_thread::yield();
    busy.try_begin_maintenance();
    for (int i = 0; i < 300; ++i) (void)execute_plan(plan, cache, busy);
    busy.end_maintenance();
    attacker.join();
    expect(fs::exists(protected_dir / "generated.c") && fs::file_size(protected_dir / "generated.c") == 18, "concurrent ancestor link swaps never delete the protected target");
  }
#endif
  {  // archive + catalog consequence; ROM and cover untouched
    report = scan_storage(catalog, cache, library);
    Plan plan = plan_archive(report, {ida}, busy);
    busy.try_begin_maintenance();
    ExecResult r = execute_plan(plan, cache, busy);
    busy.end_maintenance();
    apply_plan_to_catalog(plan, catalog);
    expect(r.ok() && !fs::exists(cache / "games" / key32('1')) && !fs::exists(cache / "games" / key32('2')), "all of A's build folders are gone");
    LibraryEntry *e = catalog.find_id(ida);
    expect(e != nullptr && e->archived && e->builds.size() == 2 && !e->rom_paths.empty(), "the library entry, history and ROM reference stay");
    expect(fs::exists(outside / "precious.rom") && fs::exists(library / "covers" / "a.png"), "the original ROM and covers are never deleted");
  }
  {  // remove from library, with and without the owned cache
    make_build(cache / "games" / key32('5'));
    LibraryEntry c = entry('c'); c.current_build_key = key32('5'); c.builds = {{key32('5'), 1, "compatibility", "", ""}};
    catalog.entries().push_back(c);
    report = scan_storage(catalog, cache, library);
    Plan keep = plan_remove_from_library(report, {c.id()}, false, busy);
    expect(keep.items.empty(), "removing only the entry deletes no files");
    apply_plan_to_catalog(keep, catalog);
    expect(catalog.find_id(c.id()) == nullptr && fs::exists(cache / "games" / key32('5')), "entry gone, cache untouched");
    catalog.entries().push_back(c);
    report = scan_storage(catalog, cache, library);
    Plan owned = plan_remove_from_library(report, {c.id()}, true, busy);
    busy.try_begin_maintenance();
    ExecResult r = execute_plan(owned, cache, busy);
    busy.end_maintenance();
    apply_plan_to_catalog(owned, catalog);
    expect(r.ok() && !fs::exists(cache / "games" / key32('5')) && catalog.find_id(c.id()) == nullptr && fs::exists(outside / "precious.rom"), "owned cache removed with the entry; ROM kept");
  }
#if !defined(_WIN32)
  if (geteuid() != 0) {  // read-only tree: denial is reported, nothing crashes, nothing outside is touched
    make_build(cache / "games" / key32('6'));
    LibraryEntry d = entry('d'); d.current_build_key = key32('6'); d.builds = {{key32('6'), 1, "compatibility", "", ""}};
    catalog.entries().push_back(d);
    fs::permissions(cache / "games" / key32('6') / "obj", fs::perms::owner_read | fs::perms::owner_exec);
    report = scan_storage(catalog, cache, library);
    Plan plan = plan_intermediates(report, {d.id()}, busy);
    busy.try_begin_maintenance();
    ExecResult r = execute_plan(plan, cache, busy);
    busy.end_maintenance();
    expect(!r.ok() && fs::exists(cache / "games" / key32('6') / "obj" / "0.o"), "a read-only folder is reported as denied");
    fs::permissions(cache / "games" / key32('6') / "obj", fs::perms::owner_all);
  }
#endif
  {  // policy: keep the newest N, never favorites or active games
    Catalog p(dir / "policy.json");
    p.load();
    StorageReport rep;
    for (int i = 0; i < 5; ++i) {
      LibraryEntry e = entry(static_cast<char>('0' + i));
      e.last_played_at = 100 + i;
      e.favorite = i == 0;
      p.entries().push_back(e);
      EntryUsage u;
      u.id = e.id();
      BuildDirUsage d;
      d.kind = DirKind::current;
      d.valid_build = true;
      u.dirs.push_back(d);
      rep.entries.push_back(u);
    }
    CleanupPolicy policy;
    policy.mode = "keep_last_n";
    policy.keep_last_n = 2;
    const auto candidates = policy_eviction_candidates(p, rep, policy, {entry('1').id()});
    // newest two (4, 3) kept; 2, 1, 0 are older: 1 is active, 0 is a favorite => only 2
    expect(candidates.size() == 1 && candidates[0] == entry('2').id(), "eviction skips favorites and active games");
    policy.mode = "never";
    expect(policy_eviction_candidates(p, rep, policy, {}).empty(), "no eviction unless the policy is on");
    expect(describe_policy(policy).find("Nothing is cleaned automatically") == 0, "policy text states exactly what happens");
  }
  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "storage_test: ok\n";
  return 0;
}
