// SEG-057-T002 (ADR 0119): the launcher's experimental Master System Optimized contract, without a window.
//
//   launcher_sms_policy_test <real segarecomp> <sms_machine.h> <launcher> <fake_segarecomp> <fake_game>
//
//   * availability is the product CLI's statement (never the launcher's); an unavailable policy is not offered;
//   * Compatibility is the default; the Genesis model opt-in is never shown for or forwarded to Master System;
//   * Compatibility and Optimized builds never share a cache entry, and a changed product model identity selects a new one;
//   * the restart decision fires for exactly the typed unadmitted_pc exit status of an Optimized Master System program, once;
//   * the headless `--build --run` flow (the same core as the window): success, cached vs uncached, unadmitted_pc -> Compatibility build/reuse
//     -> restart from reset, broad build failure (no retry loop), other runtime errors (no restart), unavailable Optimized.
// Project-authored synthetic files and product/program doubles only.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "launcher_core.hpp"

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void set_env(const char *name, const std::string &value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
}

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

#if defined(_WIN32)
constexpr const char *exe = ".exe";
#else
constexpr const char *exe = "";
#endif

// A synthetic package: empty files where the launcher only checks existence, the product double as `bin/segarecomp`.
fs::path make_package(const fs::path &root, const fs::path &fake_cli) {
  for (const char *file : {"toolchain/zig", "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                           "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "sdl3/include/SDL3/SDL.h"}) {
    const fs::path path = root / (std::string(file) + (std::string(file) == "toolchain/zig" ? exe : ""));
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
  }
  fs::create_directories(root / "bin");
  fs::copy_file(fake_cli, root / "bin" / (std::string("segarecomp") + exe), fs::copy_options::overwrite_existing);
  return root;
}

// A synthetic 32 KiB Master System image: the recognizable `TMR SEGA` header, nothing else.
fs::path make_sms_rom(const fs::path &path, unsigned seed) {
  std::vector<char> bytes(0x8000);
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 31U + seed) & 0xFFU);
  std::memcpy(&bytes[0x7FF0], "TMR SEGA", 8);
  bytes[0x7FFF] = static_cast<char>(0x4C);  // SMS export, 32 KiB
  std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

std::map<std::string, std::string> parse_report(const std::string &text) {
  std::map<std::string, std::string> kv;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    const auto eq = line.find('=');
    if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return kv;
}

int line_count(const fs::path &path) {
  int n = 0;
  std::istringstream lines(read_file(path));
  for (std::string line; std::getline(lines, line);) ++n;
  return n;
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 6) {
    std::cerr << "usage: launcher_sms_policy_test <segarecomp> <sms_machine.h> <launcher> <fake_segarecomp> <fake_game>\n";
    return 2;
  }
  const fs::path real_cli = argv[1], runtime_header = argv[2], launcher_exe = argv[3], fake_cli = argv[4], fake_game = argv[5];
  const fs::path dir = fs::temp_directory_path() / "segarecomp-launcher-sms-policy-test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  // ---- the exit status is the runtime's, pinned: no drift between launcher and generated program ----
  {
    const std::string header = read_file(runtime_header);
    expect(header.find("#define SMS_EXIT_Z80_UNADMITTED_PC " + std::to_string(sms_exit_unadmitted_pc)) != std::string::npos,
           "launcher sms_exit_unadmitted_pc must equal SMS_EXIT_Z80_UNADMITTED_PC of the runtime");
  }

  // ---- RomView gating (no process) ----
  RomView sms;
  sms.sha256 = std::string(64, 'b');
  sms.platform_id = "master-system";
  sms.supported_platform = true;
  sms.mapper = "sega";
  RomView genesis = sms;
  genesis.platform_id = "genesis";
  genesis.mapper.clear();
  expect(!sms.supports_optimized() && !sms.optimized(), "Optimized is not offered while the product has not said it exists");
  sms.aot_policy = "optimized";
  expect(!sms.optimized(), "a request the product does not support is not honoured");
  sms.sms_optimized_available = true;
  expect(sms.supports_optimized() && sms.optimized(), "Optimized is offered once the product says so");
  sms.aot_model = "gen2";
  expect(!sms.optimized_gen2(), "the Genesis model opt-in never applies to Master System");
  expect(genesis.supports_optimized() && !genesis.optimized() && genesis.aot_policy == "compatibility", "Genesis is unchanged: offered, Compatibility default");
  RomView fresh;
  expect(fresh.aot_policy == "compatibility" && !fresh.sms_optimized_available, "Compatibility is the default and availability starts unknown/false");

  // ---- restart decision ----
  {
    RomView opt = sms;
    opt.aot_model = "v1";
    expect(classify_game_exit(opt, 0) == RunVerdict::clean, "exit 0 is a clean end");
    expect(classify_game_exit(opt, sms_exit_unadmitted_pc) == RunVerdict::unadmitted_pc, "exactly the typed status is unadmitted_pc");
    for (const int other : {1, 2, 3, 4, 5, 6, 8, 64, 139, 255, -1, 134, -1073741819}) {
      expect(classify_game_exit(opt, other) == RunVerdict::failed, "status " + std::to_string(other) + " is a plain failure (no restart)");
    }
    RomView compat = compatibility_view(opt);
    expect(compat.aot_policy == "compatibility" && compat.aot_model == "v1" && !compat.optimized(), "the restart target is Compatibility");
    expect(classify_game_exit(compat, sms_exit_unadmitted_pc) == RunVerdict::failed, "a Compatibility program cannot trigger the restart");
    RomView gen = genesis;
    gen.aot_policy = "optimized";
    expect(classify_game_exit(gen, sms_exit_unadmitted_pc) == RunVerdict::failed, "a Genesis program never triggers the SMS restart");
    const RestartDecision first = decide_after_exit(opt, sms_exit_unadmitted_pc, false);
    expect(first.restart && first.next.aot_policy == "compatibility" && !first.notice.empty() && first.notice.find("Compatibility") != std::string::npos,
           "the first unadmitted_pc stop restarts in Compatibility with a visible notice");
    expect(!decide_after_exit(opt, sms_exit_unadmitted_pc, true).restart, "the restart is one-shot");
    expect(!decide_after_exit(opt, 3, false).restart && !decide_after_exit(opt, 0, false).restart, "no restart for other ends");
  }

  // ---- real product CLI: availability and identities ----
  {
    Layout layout;
    layout.root = dir / "real";
    layout.cli = real_cli;
    const std::string compat = product_policy_identity(layout, "compatibility", "", "master-system");
    const std::string opt = product_policy_identity(layout, "optimized", "", "master-system");
    expect(compat.size() == 64 && opt.size() == 64 && compat != opt, "the real CLI states distinct Compatibility and Optimized Master System identities");
    expect(sms_optimized_available_blocking(layout) && sms_optimized_available_cached(layout), "the real CLI states that Master System Optimized is available");
    const std::string g_opt = product_policy_identity(layout, "optimized", "v1");
    expect(g_opt.size() == 64 && g_opt != opt, "the Genesis Optimized identity is a different identity");
    RomView rom = sms;
    rom.sms_optimized_available = true;
    rom.aot_policy = "optimized";
    expect(rom_policy_identity(layout, rom) == opt && rom_policy_identity_cached(layout, rom) == opt, "a Master System ROM is keyed on the Master System identity");
    RomView compat_rom = compatibility_view(rom);
    expect(entry_dir(rom, layout) != entry_dir(compat_rom, layout), "SMS Optimized and Compatibility never share a cache entry");
    RomView g = genesis;
    g.aot_policy = "optimized";
    expect(entry_dir(rom, layout) != entry_dir(g, layout), "SMS Optimized and Genesis Optimized never share a cache entry");
    // Genesis keys are unchanged by this milestone (the historical key text).
    const fs::path genesis_compat_key = entry_dir(genesis, layout);
    expect(!genesis_compat_key.empty(), "Genesis Compatibility key still resolves");
    // The status of a finished entry decides reuse.
    const std::string good = std::string("{\"status\":\"ok\",\"aot_policy\":{\"requested\":\"optimized\",\"requested_model\":\"d512hp\",\"effective\":\"ml_region_z80\",\"fallback\":false,\"reason\":\"none\",\"fallback_class\":\"none\",\"model_family\":\"sms_z80_d512hp\",\"analysis_contract\":\"segarecomp.z80_admission.v1\",\"model\":\"6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11\",\"architecture\":\"D512HP/3seeds\",\"rank_policy\":\"cell32-top2mean-roots(0000,0038,0066)-budget65-ceil-tie:lower-index\",\"schema\":\"sgz80w01/tokens256+pad/logit-per-byte/context512\",\"selected_ppm\":650390,\"admitted_ppm\":657196,\"validator\":\"accepted\",\"policy_identity\":\"") + opt + "\",\"identity\":\"x\",\"cache_reuse\":\"none\"}}";
    const auto make_entry = [&](const std::string &name, const std::string &status) {
      const fs::path entry = dir / name;
      fs::create_directories(entry);
      std::ofstream(entry / (std::string("game") + exe)) << "x";
      std::ofstream(entry / "status.json") << status;
      return entry;
    };
    expect(entry_ready_for(make_entry("e_good", good), rom, layout), "a matching Optimized entry is reusable");
    expect(!entry_ready_for(make_entry("e_good2", good), compat_rom, layout), "an Optimized entry is never reused for Compatibility");
    std::string stale = good;
    stale.replace(stale.find(opt), opt.size(), std::string(64, '0'));
    expect(!entry_ready_for(make_entry("e_stale", stale), rom, layout), "an Optimized entry of another model identity is not reused");
    const std::string compat_status = "{\"status\":\"ok\",\"aot_policy\":{\"requested\":\"compatibility\",\"effective\":\"broad\",\"fallback\":false,\"reason\":\"none\"}}";
    expect(entry_ready_for(make_entry("e_compat", compat_status), compat_rom, layout), "a Compatibility entry is reusable for Compatibility");
    expect(!entry_ready_for(make_entry("e_compat2", compat_status), rom, layout), "a Compatibility entry is never reused for Optimized");
    // Wording
    const auto report = read_policy_report(make_entry("e_report", good));
    expect(report.present && report.effective == "ml_region_z80" && policy_summary(report) == "Optimized build (experimental CNN)", "SMS Optimized report wording");
    const auto fell = read_policy_report(make_entry("e_fell",
        R"({"status":"ok","aot_policy":{"requested":"optimized","requested_model":"d512hp","effective":"broad","fallback":true,"reason":"inference","fallback_class":"producer_failed","model_family":"none","validator":"not_run"}})"));
    expect(fell.fallback && policy_summary(fell).find("built Compatibility") != std::string::npos && policy_details(fell).find("producer_failed") != std::string::npos,
           "a build-time fallback and its reason are reported");
  }

  // ---- fake package: cache separation on a changed model identity (same package, same tool path: only the product identity differs) ----
  {
    const auto ship = [&](const char *model) { std::ofstream(dir / "pkgA" / "bin" / "model.txt") << model; reset_policy_identity_memo_for_tests(); };
    fs::create_directories(dir / "pkgA" / "bin");
    ship("m1");
    Layout a;
    a.root = make_package(dir / "pkgA", fake_cli);
    a.cli = a.root / "bin" / (std::string("segarecomp") + exe);
    RomView rom = sms;
    rom.sms_optimized_available = true;
    rom.aot_policy = "optimized";
    const fs::path before = entry_dir(rom, a);
    expect(entry_dir(rom, a) == before, "the entry is stable for an unchanged product identity");
    ship("m2");
    expect(entry_dir(rom, a) != before, "a different shipped model identity selects a different cache entry (same package and tool path)");
    RomView compat = compatibility_view(rom);
    ship("m1");
    const fs::path compat_before = entry_dir(compat, a);
    ship("m2");
    expect(entry_dir(compat, a) == compat_before, "a Compatibility entry does not depend on the Optimized model identity");
    fs::remove(dir / "pkgA" / "bin" / "model.txt");
    reset_policy_identity_memo_for_tests();
  }

  // ---- end to end: the headless launcher against the product/program doubles ----
  {
    const fs::path package = make_package(dir / "pkg", fake_cli);
    const fs::path rom_path = make_sms_rom(dir / "synthetic.sms", 1);
    set_env("SEGARECOMP_HOME", package.string());
    set_env("FAKE_GAME_BIN", fake_game.string());
    set_env("FAKE_MODEL", "m1");
    int scenario = 0;
    const auto run_launcher = [&](const std::vector<std::string> &flags, std::map<std::string, std::string> env_overrides, int &rc) {
      ++scenario;
      const fs::path report = dir / ("report" + std::to_string(scenario) + ".txt");
      for (const char *name : {"FAKE_SMS_OPTIMIZED", "FAKE_OPT_EXIT", "FAKE_COMPAT_EXIT", "FAKE_BUILD_FAIL", "FAKE_OPT_FALLBACK"}) set_env(name, "");
      for (const auto &[name, value] : env_overrides) set_env(name.c_str(), value);
      std::string command = "\"" + launcher_exe.string() + "\" --build \"" + rom_path.string() + "\" --mapper sega --run --report \"" + report.string() + "\"";
      for (const std::string &flag : flags) command += " " + flag;
#if defined(_WIN32)
      command = "\"" + command + " > NUL 2>&1\"";
#else
      command += " > /dev/null 2>&1";
#endif
      rc = std::system(command.c_str());
#if !defined(_WIN32)
      if (rc != -1) rc = WEXITSTATUS(rc);
#endif
      return parse_report(read_file(report));
    };
    const auto fresh_cache = [&](const char *name) {
      const fs::path cache = dir / name;
      fs::remove_all(cache);
      fs::create_directories(cache);
      set_env("SEGARECOMP_CACHE_DIR", cache.string());
      const fs::path log = dir / (std::string(name) + "-builds.txt");
      fs::remove(log);
      set_env("FAKE_BUILD_LOG", log.string());
      return log;
    };
    int rc = 0;

    // a) Compatibility is the default
    fs::path builds = fresh_cache("cache_a");
    auto r = run_launcher({}, {}, rc);
    expect(rc == 0 && r["aot_policy_requested"] == "compatibility" && r["cache"] == "miss" && r["build"] == "ok" && r["run"] == "exited code=0" && r["run_mode"] == "compatibility",
           "a) default request is Compatibility and runs");
    expect(r["aot_policy_effective"] == "broad" && r.count("fallback") == 0, "a) Compatibility builds the broad program");
    r = run_launcher({}, {}, rc);
    expect(rc == 0 && r["cache"] == "hit" && line_count(builds) == 1, "a) the second Compatibility run reuses the build");

    // b) explicit Optimized, success; cached vs uncached; never shares the Compatibility entry
    r = run_launcher({"--aot-policy", "optimized"}, {}, rc);
    const std::string compat_entry = r["entry"];
    expect(rc == 0 && r["aot_policy_requested"] == "optimized" && r["cache"] == "miss" && r["aot_policy_effective"] == "ml_region_z80" && r["aot_policy_fallback"] == "no" &&
               r["run"] == "exited code=0" && r["run_mode"] == "optimized" && r.count("fallback") == 0 && line_count(builds) == 2,
           "b) Optimized builds its own entry (miss) and runs without a restart");
    r = run_launcher({"--aot-policy", "optimized"}, {}, rc);
    expect(rc == 0 && r["cache"] == "hit" && line_count(builds) == 2, "b) the second Optimized run reuses its entry");
    expect(r["entry"] != "" && !compat_entry.empty(), "b) entries reported");

    // c) unadmitted_pc: visible fallback, Compatibility build, restart from reset, once
    builds = fresh_cache("cache_c");
    r = run_launcher({"--aot-policy", "optimized"}, {{"FAKE_OPT_EXIT", "7"}}, rc);
    expect(rc == 0, "c) the run ends successfully after the restart (rc " + std::to_string(rc) + ")");
    expect(r["run"] == "exited code=0" && r["run_mode"] == "compatibility", "c) the final run is the Compatibility program");
    expect(r["fallback"] == "compatibility_restart" && r.count("notice") == 1 && r["notice"].find("Compatibility") != std::string::npos, "c) the restart is reported with its notice");
    expect(r["fallback_aot_policy_requested"] == "compatibility" && r["fallback_cache"] == "miss" && r["fallback_build"] == "ok", "c) the Compatibility build is made");
    expect(line_count(builds) == 2, "c) exactly one Optimized and one Compatibility build");
    // again: both entries are reused, no build at all
    r = run_launcher({"--aot-policy", "optimized"}, {{"FAKE_OPT_EXIT", "7"}}, rc);
    expect(rc == 0 && r["cache"] == "hit" && r["fallback_cache"] == "hit" && line_count(builds) == 2, "c) a matching Compatibility build is reused on the next fallback");

    // d) the Compatibility build fails: the real failure is shown, no retry loop
    builds = fresh_cache("cache_d");
    r = run_launcher({"--aot-policy", "optimized"}, {{"FAKE_OPT_EXIT", "7"}, {"FAKE_BUILD_FAIL", "compatibility"}}, rc);
    expect(rc == 3 && r["fallback_build"] == "failed" && r["fallback_message"].find("fake build failed") != std::string::npos && line_count(builds) == 2,
           "d) a failing Compatibility build is reported once (rc " + std::to_string(rc) + ")");

    // e) other runtime errors do not restart
    builds = fresh_cache("cache_e");
    r = run_launcher({"--aot-policy", "optimized"}, {{"FAKE_OPT_EXIT", "3"}}, rc);
    expect(rc == 4 && r["run"] == "exited code=3" && r["run_mode"] == "optimized" && r.count("fallback") == 0 && line_count(builds) == 1, "e) a generic guest fault is not a restart");
    r = run_launcher({"--aot-policy", "optimized"}, {{"FAKE_OPT_EXIT", "139"}}, rc);
    expect(rc == 4 && r.count("fallback") == 0, "e) a crash status is not a restart");
    r = run_launcher({}, {{"FAKE_COMPAT_EXIT", "7"}}, rc);
    expect(rc == 4 && r.count("fallback") == 0, "e) status 7 from a Compatibility program is not a restart");

    // f) Optimized unavailable: not honoured, Compatibility is built, stated
    builds = fresh_cache("cache_f");
    r = run_launcher({"--aot-policy", "optimized"}, {{"FAKE_SMS_OPTIMIZED", "0"}}, rc);
    expect(rc == 0 && r.count("aot_policy_unavailable") == 1 && r["aot_policy_requested"] == "compatibility" && read_file(builds) == "compatibility\n",
           "f) an unavailable Optimized request builds Compatibility and says so");

    // g) the build-time fallback (producer failed) is visible in the report
    builds = fresh_cache("cache_g");
    r = run_launcher({"--aot-policy", "optimized"}, {{"FAKE_OPT_FALLBACK", "1"}}, rc);
    expect(rc == 0 && r["aot_policy_fallback"] == "yes" && r["aot_policy_reason"] == "inference" && r["aot_policy_fallback_class"] == "producer_failed" &&
               r["aot_policy_effective"] == "broad",
           "g) a build-time fallback and its reason are reported");

    // h) --aot-model is a Genesis option
    builds = fresh_cache("cache_h");
    r = run_launcher({"--aot-policy", "optimized", "--aot-model", "gen2"}, {}, rc);
    expect(rc == 2 && line_count(builds) == 0, "h) a Genesis model name is refused for Master System before any build");
  }

  // ---- end to end: the real window loop (SDL dummy video driver, automated Play, state trace) ----
#if defined(__linux__)
  {
    const fs::path package = dir / "pkg";  // the package of the headless section (product double installed)
    const fs::path rom_path = dir / "synthetic.sms";
    {  // the mapper is declared by a sidecar manifest, exactly as a user would
      const std::string sha = [&] {
        std::ostringstream hex;
        const auto view = inspect_rom_file(rom_path, locate_layout());
        return view.sha256;
      }();
      std::ofstream(dir / "synthetic.mapper.json") << "{\"mapper\":\"sega\",\"sha256\":\"" << sha << "\"}";
    }
    set_env("SDL_VIDEODRIVER", "dummy");
    set_env("SDL_RENDER_DRIVER", "software");
    set_env("SEGARECOMP_HOME", package.string());
    set_env("FAKE_GAME_BIN", fake_game.string());
    int scenario = 0;
    const auto gui = [&](const std::string &policy, std::map<std::string, std::string> env_overrides, const char *cache_name, bool fresh) {
      ++scenario;
      const fs::path cache = dir / cache_name;
      if (fresh) { fs::remove_all(cache); fs::create_directories(cache); fs::remove(dir / (std::string(cache_name) + "-builds.txt")); }
      set_env("SEGARECOMP_CACHE_DIR", cache.string());
      set_env("FAKE_BUILD_LOG", (dir / (std::string(cache_name) + "-builds.txt")).string());
      for (const char *name : {"FAKE_SMS_OPTIMIZED", "FAKE_OPT_EXIT", "FAKE_COMPAT_EXIT", "FAKE_BUILD_FAIL", "FAKE_OPT_FALLBACK", "SEGARECOMP_SHOT_POLICY"}) set_env(name, "");
      for (const auto &[name, value] : env_overrides) set_env(name.c_str(), value);
      if (!policy.empty()) set_env("SEGARECOMP_SHOT_POLICY", policy);
      const fs::path trace = dir / ("gui-trace" + std::to_string(scenario) + ".txt");
      set_env("SEGARECOMP_SHOT_ROM", rom_path.string());
      set_env("SEGARECOMP_GUI_AUTOPLAY", "1");
      set_env("SEGARECOMP_GUI_TRACE", trace.string());
      const std::string command = "timeout 120 \"" + launcher_exe.string() + "\" > /dev/null 2>&1";
      const int status = std::system(command.c_str());
      std::vector<std::string> lines;
      std::istringstream in(read_file(trace));
      for (std::string line; std::getline(in, line);) lines.push_back(line);
      expect(status == 0, "gui scenario " + std::to_string(scenario) + ": the window process must exit normally");
      return lines;
    };
    const auto has = [](const std::vector<std::string> &lines, const std::string &prefix) {
      for (const auto &line : lines) if (line.rfind(prefix, 0) == 0) return true;
      return false;
    };
    const auto index_of = [](const std::vector<std::string> &lines, const std::string &prefix) {
      for (std::size_t i = 0; i < lines.size(); ++i) if (lines[i].rfind(prefix, 0) == 0) return static_cast<int>(i);
      return -1;
    };
    const auto builds_of = [&](const char *cache_name) { return line_count(dir / (std::string(cache_name) + "-builds.txt")); };

    // G1: Compatibility is the default for a Master System ROM, even though the product offers Optimized
    auto g = gui("", {}, "gcache_1", true);
    expect(has(g, "run=started mode=compatibility restarted=0") && has(g, "final=ready mode=compatibility restarted=0") && !has(g, "restart="), "G1: default is Compatibility");
    // G2: explicit Optimized, uncached then cached
    g = gui("optimized", {}, "gcache_2", true);
    expect(has(g, "autoplay=build mode=optimized") && has(g, "run=started mode=optimized restarted=0") && has(g, "final=ready mode=optimized restarted=0 entry_requested=optimized") && !has(g, "restart="),
           "G2: Optimized builds and runs");
    expect(builds_of("gcache_2") == 1, "G2: one Optimized build");
    g = gui("optimized", {}, "gcache_2", false);
    expect(!has(g, "autoplay=build") && has(g, "run=started mode=optimized") && builds_of("gcache_2") == 1, "G2: a cached Optimized entry is reused (no build)");
    // G3: the typed stop: visible notice, Compatibility build, restart from reset, once
    g = gui("optimized", {{"FAKE_OPT_EXIT", "7"}}, "gcache_3", true);
    expect(index_of(g, "run=exited code=7 mode=optimized") >= 0 && index_of(g, "restart=compatibility notice=Optimized stopped") > index_of(g, "run=exited code=7 mode=optimized") &&
               index_of(g, "build=started mode=compatibility") > index_of(g, "restart=compatibility") && index_of(g, "run=started mode=compatibility restarted=1") > index_of(g, "build=started mode=compatibility") &&
               has(g, "run=exited code=0 mode=compatibility") && has(g, "final=ready mode=compatibility restarted=1"),
           "G3: unadmitted_pc -> notice -> Compatibility build -> restart");
    expect(builds_of("gcache_3") == 2, "G3: one Optimized and one Compatibility build");
    int restarts = 0;
    for (const auto &line : g) restarts += line.rfind("restart=", 0) == 0 ? 1 : 0;
    expect(restarts == 1, "G3: exactly one restart");
    g = gui("optimized", {{"FAKE_OPT_EXIT", "7"}}, "gcache_3", false);
    expect(has(g, "restart=compatibility") && !has(g, "build=started") && builds_of("gcache_3") == 2, "G3: a matching Compatibility build is reused on the next fallback");
    // G4: the Compatibility build fails: the real failure, no loop
    g = gui("optimized", {{"FAKE_OPT_EXIT", "7"}, {"FAKE_BUILD_FAIL", "compatibility"}}, "gcache_4", true);
    expect(has(g, "restart=compatibility") && has(g, "build=failed restart_pending=1 message=The fake build failed") && has(g, "final=failed") && builds_of("gcache_4") == 2,
           "G4: a failing Compatibility build is shown once, never retried");
    // G5: other runtime errors do not restart
    g = gui("optimized", {{"FAKE_OPT_EXIT", "3"}}, "gcache_5", true);
    expect(has(g, "run=exited code=3 mode=optimized") && has(g, "state=failed") && !has(g, "restart=") && builds_of("gcache_5") == 1, "G5: another guest error is a failure, not a restart");
    g = gui("optimized", {{"FAKE_OPT_EXIT", "139"}}, "gcache_5", false);
    expect(has(g, "run=exited code=139") && !has(g, "restart="), "G5: a crash status is not a restart");
    g = gui("", {{"FAKE_COMPAT_EXIT", "7"}}, "gcache_5b", true);
    expect(has(g, "run=exited code=7 mode=compatibility") && has(g, "state=failed") && !has(g, "restart="), "G5: status 7 from a Compatibility program is not a restart");
    // G6: unavailable Optimized is not offered: a request for it is not honoured
    g = gui("optimized", {{"FAKE_SMS_OPTIMIZED", "0"}}, "gcache_6", true);
    expect(has(g, "autoplay=build mode=compatibility") && !has(g, "mode=optimized") && builds_of("gcache_6") == 1 && read_file(dir / "gcache_6-builds.txt") == "compatibility\n",
           "G6: Optimized is not offered when the product does not provide it");
    // G7: returning to Compatibility and reopening with another policy never collides
    g = gui("", {}, "gcache_7", true);
    g = gui("optimized", {}, "gcache_7", false);
    g = gui("", {}, "gcache_7", false);
    expect(builds_of("gcache_7") == 2 && has(g, "run=started mode=compatibility") && !has(g, "autoplay=build"), "G7: one build per policy, Compatibility reused after Optimized");
  }
#endif

  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "launcher_sms_policy_test: ok\n";
  return 0;
}
