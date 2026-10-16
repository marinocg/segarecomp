// SEG-069-T005: the playability predicate for executables built by an earlier release (older_build.hpp), without a window or the product CLI.
// It is deliberately separate from the exact reuse identity (entry_dir/entry_ready_for): these tests pin both what is accepted and, above all, what must
// never be offered. Synthetic metadata only.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "launcher_core.hpp"
#include "older_build.hpp"

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
void spit(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }

#if defined(_WIN32)
constexpr const char *exe = ".exe";
#else
constexpr const char *exe = "";
#endif

std::string key32(char c) { return std::string(32, c); }
std::string digest(char c) { return std::string(64, c); }

struct Build {
  std::string key;
  std::string version = "0.5.0";          // a listed legacy release
  std::string contract;                   // "" = no marker, else the number
  std::string rom = digest('a');
  std::string platform = "genesis";
  std::string host = host_description();
  std::string policy_member;              // "" = legacy Compatibility; else the aot_policy object body
  bool executable = true;
  bool status_ok = true;
  bool metadata = true;
  std::string extra;                      // more metadata members
};

std::string optimized_member(const std::string &model, bool complete = true) {
  return std::string("\"aot_policy\":{\"requested\":\"optimized\",\"requested_model\":\"") + model + "\",\"effective\":\"ml_region\",\"fallback\":false,\"reason\":\"none\",\"fallback_class\":\"none\",\"model_family\":\"" +
         model + "\",\"validator\":\"" + (complete ? "accepted" : "") + "\",\"selected_ppm\":1,\"admitted_ppm\":1,\"policy_identity\":\"" + (complete ? std::string(64, 'c') : std::string()) + "\"}";
}

template <class F> Build build(const std::string &key, F tweak) {
  Build b;
  b.key = key;
  tweak(b);
  return b;
}
Build build(const std::string &key) {
  Build b;
  b.key = key;
  return b;
}

void make(const fs::path &games, const Build &b) {
  const fs::path dir = games / b.key;
  fs::create_directories(dir);
  if (b.executable) spit(dir / (std::string("game") + exe), "x");
  if (b.status_ok) spit(dir / "status.json", std::string("{\"status\":\"ok\"") + (b.policy_member.empty() ? "" : "," + b.policy_member) + "}");
  else spit(dir / "status.json", "{\"status\":\"failed\"}");
  if (b.metadata) {
    std::string meta = "{\"rom_sha256\":\"" + b.rom + "\",\"title\":\"T\",\"segarecomp_version\":\"" + b.version + "\",\"platform\":\"" + b.platform + "\",";
    if (!b.contract.empty()) meta += "\"runtime_contract\":" + b.contract + ",";
    meta += b.extra;
    meta += "\"host\":\"" + b.host + "\",\"toolchain\":\"zig 0.15.2\",\"rom_path\":\"referenced, not copied\"}";
    spit(dir / "metadata.json", meta);
  }
}

LibraryEntry entry_with(std::initializer_list<std::string> keys, const std::string &platform = "genesis") {
  LibraryEntry e;
  e.platform_id = platform;
  e.sha256 = digest('a');
  for (const auto &k : keys) { BuildRecord r; r.key = k; e.builds.push_back(r); }
  return e;
}
} // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "segarecomp_older_build_test";
  fs::remove_all(dir);
  const fs::path games = dir / "games";
  Layout layout;
  layout.sdl_lib = dir / "sdl3" / "lib";
  spit(layout.sdl_lib / "libSDL3.so.0", "x");
  const std::string current = key32('f');

  {  // a valid build of the 0.5.0 release (no marker) is playable; the current identity is never confused with it
    make(games, build(key32('1')));
    const OlderBuild o = find_playable_older_build(entry_with({key32('1')}), current, "compatibility", layout, games);
    expect(o.playable && o.dir == games / key32('1') && o.mode == "compatibility" && o.version == "0.5.0" && !o.contract_marked, "a listed legacy release's Compatibility build is playable at its own folder");
    expect(o.dir != games / current, "the older build is never the current-key path");
    expect(!find_playable_older_build(entry_with({key32('1')}), key32('1'), "compatibility", layout, games).playable, "the current key itself is not an 'older' build");
  }
  {  // a build that records the current contract is playable whatever its release; a different contract is not
    make(games, build(key32('2'), [&](Build &b) { b.version = "9.9.9"; b.contract = "1"; }));
    expect(find_playable_older_build(entry_with({key32('2')}), current, "compatibility", layout, games).playable, "a build that records runtime_contract 1 is playable");
    make(games, build(key32('3'), [&](Build &b) { b.version = "9.9.9"; b.contract = "2"; }));
    const OlderBuild o = find_playable_older_build(entry_with({key32('3')}), current, "compatibility", layout, games);
    expect(!o.playable && o.candidate_seen && o.reason.find("runtime contract") != std::string::npos, "a different runtime contract is refused with its reason");
  }
  {  // version numbers alone are never proof
    make(games, build(key32('4'), [&](Build &b) { b.version = "0.4.0"; }));
    expect(!find_playable_older_build(entry_with({key32('4')}), current, "compatibility", layout, games).playable, "an unlisted release without the marker is refused");
    make(games, build(key32('5'), [&](Build &b) { b.version = ""; }));
    const OlderBuild o = find_playable_older_build(entry_with({key32('5')}), current, "compatibility", layout, games);
    expect(!o.playable && o.reason.find("does not record") != std::string::npos, "a build that does not record its release is refused");
    make(games, build(key32('6'), [&](Build &b) { b.version = "99.0.0"; }));
    expect(!find_playable_older_build(entry_with({key32('6')}), current, "compatibility", layout, games).playable, "a newer-looking release without the marker is refused too");
  }
  {  // negatives: corrupt or contradictory records never become playable
    struct Case { const char *name; Build build; };
    const Case cases[] = {
        {"missing executable", build(key32('7'), [&](Build &b) { b.executable = false; })},
        {"status not ok", build(key32('8'), [&](Build &b) { b.status_ok = false; })},
        {"missing metadata", build(key32('9'), [&](Build &b) { b.metadata = false; })},
        {"different ROM", build(key32('b'), [&](Build &b) { b.rom = digest('d'); })},
        {"different platform", build(key32('c'), [&](Build &b) { b.platform = "master-system"; })},
        {"different host", build(key32('d'), [&](Build &b) { b.host = "mips Plan9"; })},
        {"optimized policy without identity", build(key32('e'), [&](Build &b) { b.policy_member = optimized_member("gen2", false); })},
    };
    for (const Case &c : cases) {
      make(games, c.build);
      const OlderBuild o = find_playable_older_build(entry_with({c.build.key}), current, c.build.policy_member.empty() ? "compatibility" : "optimized", layout, games);
      expect(!o.playable && o.candidate_seen && !o.reason.empty(), std::string("refused: ") + c.name);
    }
    spit(games / key32('0') / (std::string("game") + exe), "x");
    spit(games / key32('0') / "status.json", "{\"status\":\"ok\"}");
    spit(games / key32('0') / "metadata.json", "not json");
    expect(!find_playable_older_build(entry_with({key32('0')}), current, "compatibility", layout, games).playable, "unreadable metadata is refused");
    expect(!find_playable_older_build(entry_with({"nope"}), current, "compatibility", layout, games).playable, "a record with an invalid key is refused");
    expect(!find_playable_older_build(entry_with({key32('a')}), current, "compatibility", layout, games).playable, "a deleted build folder is refused");
    // a record whose status has an aot_policy member that cannot be read
    make(games, build(key32('b'), [&](Build &b) { b.policy_member = "\"aot_policy\":{\"requested\":\"\"}"; }));
    expect(!find_playable_older_build(entry_with({key32('b')}), current, "compatibility", layout, games).playable, "an incomplete recorded policy is refused");
  }
  {  // symlinked build folders are never followed
    std::error_code ec;
    make(games, build(key32('1')));
    fs::create_directories(dir / "elsewhere");
    fs::rename(games / key32('1'), dir / "elsewhere" / key32('1'));
    fs::create_directory_symlink(dir / "elsewhere" / key32('1'), games / key32('1'), ec);
    if (!ec) expect(!find_playable_older_build(entry_with({key32('1')}), current, "compatibility", layout, games).playable, "a symlinked build folder is refused");
    fs::remove(games / key32('1'), ec);
    fs::rename(dir / "elsewhere" / key32('1'), games / key32('1'), ec);
  }
  {  // the mode must match what is requested now; the model is reported as recorded (never relabelled)
    make(games, build(key32('a'), [&](Build &b) { b.contract = "1"; b.policy_member = optimized_member("v1"); }));
    const LibraryEntry e = entry_with({key32('a')});
    const OlderBuild optimized = find_playable_older_build(e, current, "optimized", layout, games);
    expect(optimized.playable && optimized.mode == "optimized" && optimized.model == "v1", "an Optimized v1 build is playable for an Optimized request and stays v1");
    const OlderBuild compat = find_playable_older_build(e, current, "compatibility", layout, games);
    expect(!compat.playable && compat.reason.find("Optimized mode") != std::string::npos, "an Optimized build is not offered for a Compatibility request");
    make(games, build(key32('b'), [&](Build &b) { b.contract = "1"; }));
    expect(!find_playable_older_build(entry_with({key32('b')}), current, "optimized", layout, games).playable, "a Compatibility build is not offered for an Optimized request");
  }
  {  // the newest playable candidate wins; a refused newest build does not hide an older good one
    make(games, build(key32('1')));
    make(games, build(key32('2'), [&](Build &b) { b.version = "9.9.9"; b.contract = "1"; }));
    make(games, build(key32('4'), [&](Build &b) { b.version = "0.4.0"; }));
    const OlderBuild o = find_playable_older_build(entry_with({key32('1'), key32('2'), key32('4')}), current, "compatibility", layout, games);
    expect(o.playable && o.key == key32('2'), "the newest playable build is chosen, skipping a refused newer one");
  }
  {  // H-interrupt discovery: a build that cannot be shown to include the game's setting is refused
    make(games, build(key32('1')));
    LibraryEntry e = entry_with({key32('1')});
    e.hint_alias_discovery = true;
    expect(!find_playable_older_build(e, current, "compatibility", layout, games).playable, "a build that does not record H-interrupt discovery is refused when the game turns it on");
    make(games, build(key32('2'), [&](Build &b) { b.version = "9.9.9"; b.contract = "1"; b.extra = "\"hint_alias_discovery\":true,"; }));
    LibraryEntry on = entry_with({key32('2')});
    on.hint_alias_discovery = true;
    expect(find_playable_older_build(on, current, "compatibility", layout, games).playable, "a build that recorded the setting matches a game with the setting");
    expect(!find_playable_older_build(entry_with({key32('2')}), current, "compatibility", layout, games).playable, "a build made with the setting is refused for a game without it");
  }
  {  // Master System: only a matching declared mapper and the baseline profile
    const std::string sms_meta = "\"profile\":\"" + std::string(sms_profile) + "\",\"mapper\":\"sega\",\"mapper_declaration\":\"option\",";
    make(games, build(key32('1'), [&](Build &b) { b.platform = "master-system"; b.extra = sms_meta; }));
    LibraryEntry e = entry_with({key32('1')}, "master-system");
    e.mapper = "sega";
    expect(find_playable_older_build(e, current, "compatibility", layout, games).playable, "an SMS build with the same declared mapper and profile is playable");
    e.mapper = "rom_only";
    expect(!find_playable_older_build(e, current, "compatibility", layout, games).playable, "a different declared mapper is refused");
    e.mapper.clear();
    expect(!find_playable_older_build(e, current, "compatibility", layout, games).playable, "an undeclared mapper cannot be matched");
    make(games, build(key32('2'), [&](Build &b) { b.platform = "master-system"; b.extra = "\"profile\":\"other\",\"mapper\":\"sega\",\"mapper_declaration\":\"option\","; }));
    LibraryEntry p = entry_with({key32('2')}, "master-system");
    p.mapper = "sega";
    expect(!find_playable_older_build(p, current, "compatibility", layout, games).playable, "a different console profile is refused");
    make(games, build(key32('3'), [&](Build &b) { b.platform = "master-system"; b.extra = "\"profile\":\"" + std::string(sms_profile) + "\",\"mapper\":\"\",\"mapper_declaration\":\"manifest\","; }));
    LibraryEntry m = entry_with({key32('3')}, "master-system");
    m.mapper_manifest_sha256 = digest('9');
    expect(!find_playable_older_build(m, current, "compatibility", layout, games).playable, "a manifest-declared mapper cannot be verified and is refused");
  }
  {  // the installed SDL3 runtime is a required dependency
    make(games, build(key32('1')));
    Layout bare = layout;
    bare.sdl_lib = dir / "no-sdl";
    const OlderBuild o = find_playable_older_build(entry_with({key32('1')}), current, "compatibility", bare, games);
    expect(!o.playable && o.reason.find("SDL3") != std::string::npos, "a missing SDL3 runtime refuses every older build");
  }
  {  // the exact reuse predicate is unchanged: a different mode or model is never "reused" by identity
    make(games, build(key32('a'), [&](Build &b) { b.contract = "1"; b.policy_member = optimized_member("v1"); }));
    RomView view;
    view.platform_id = "genesis";
    view.sha256 = digest('a');
    view.aot_policy = "optimized";
    view.aot_model = "gen2";
    expect(!entry_ready_for(games / key32('a'), view, layout), "entry_ready_for still refuses a v1 build for a gen2 request");
  }
  fs::remove_all(dir);
  if (failures == 0) std::cout << "older_build_test: ok\n";
  return failures == 0 ? 0 : 1;
}
