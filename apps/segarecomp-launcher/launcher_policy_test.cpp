// SEG-047-T009 (ADR 0096): the launcher's AOT-policy contract, without a window: Compatibility is the default, the two modes never share a
// cache entry, Optimized is Genesis-only, and the build status is read from the machine-readable `aot_policy` member (requested /
// effective / fallback / stable reason) with visible fallback wording. Project-authored synthetic status files only.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

fs::path write_status(const fs::path &dir, const std::string &name, const std::string &body) {
  const fs::path entry = dir / name;
  fs::create_directories(entry);
  std::ofstream(entry / "status.json", std::ios::binary) << body;
  return entry;
}
}  // namespace

int main(int argc, char **argv) {
  const fs::path dir = fs::temp_directory_path() / "segarecomp-launcher-policy-test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  Layout layout;
  layout.root = dir / "pkg";
  const bool have_cli = argc > 1;  // optional: the real `segarecomp` binary (product-provided policy identity)
  if (have_cli) layout.cli = argv[1];
  RomView genesis;
  genesis.sha256 = std::string(64, 'a');
  genesis.platform_id = "genesis";
  genesis.supported_platform = true;
  RomView sms = genesis;
  sms.platform_id = "master-system";
  sms.mapper = "sega";

  // Compatibility is the default; Optimized is a Genesis-only policy.
  expect(genesis.aot_policy == "compatibility" && !genesis.optimized(), "Compatibility is the default");
  expect(genesis.supports_optimized() && !sms.supports_optimized(), "Optimized is offered for Genesis only");

  // Cache identity: modes never alias; Compatibility keeps the historical key; Master System ignores the request.
  const auto compat_entry = entry_dir(genesis, layout);
  RomView optimized = genesis;
  optimized.aot_policy = "optimized";
  expect(optimized.optimized(), "Optimized is selectable for Genesis");
  expect(entry_dir(optimized, layout) != compat_entry, "Compatibility and Optimized have different cache entries");

  // SEG-055: console region (run-time only) and the advanced H-interrupt alias discovery (default off, part of the cache identity).
  expect(genesis.supports_region() && !sms.supports_region() && genesis.region == "usa", "the region selector is Genesis only and defaults to USA NTSC");
  expect(!genesis.hint_alias_discovery && genesis.supports_hint_alias_discovery() && !sms.supports_hint_alias_discovery(), "alias discovery defaults off and is Genesis only");
  {
    const char *before_raw = SDL_getenv("SEGARECOMP_GENESIS_REGION");
    const std::string process_region_before_text = before_raw ? before_raw : "";
    const char *process_region_before = before_raw ? process_region_before_text.c_str() : nullptr;
    RomView japan = genesis, europe = genesis, discovery = genesis;
    japan.region = "japan";
    europe.region = "europe";
    discovery.hint_alias_discovery = true;
    // Region is run-time configuration: switching it reuses the same compiled artifact (identical cache entry); discovery changes the program.
    expect(entry_dir(japan, layout) == compat_entry && entry_dir(europe, layout) == compat_entry, "switching region reuses the same compiled artifact");
    expect(entry_dir(discovery, layout) != compat_entry, "alias discovery selects a different cache entry");
    expect(entry_dir(discovery, layout) == entry_dir(discovery, layout), "discovery cache identity is deterministic");
    RomView discovery_opt = discovery;
    discovery_opt.aot_policy = "optimized";
    expect(entry_dir(discovery_opt, layout) != entry_dir(optimized, layout), "discovery also separates Optimized entries");
    RomView sms_discovery = sms;
    sms_discovery.hint_alias_discovery = true;
    expect(entry_dir(sms_discovery, layout) == entry_dir(sms, layout), "Master System ignores discovery (non-Genesis paths unchanged)");

    // PAL refuses cleanly; NTSC profiles and non-Genesis launches are never refused.
    expect(launch_refusal(europe).find("PAL") != std::string::npos && launch_refusal(japan).empty() && launch_refusal(genesis).empty() && launch_refusal(sms).empty(),
           "Europe PAL is refused with an explanation; USA, Japan and Master System are launchable");
    expect(region_executable("usa") && region_executable("japan") && !region_executable("europe") && !region_executable("bogus"), "only the NTSC profiles execute");

    // The child environment (injectable seam): the selection is applied to the child's environment object, not the process environment,
    // and it overrides any inherited value, including for USA.
    SDL_Environment *inherited = SDL_CreateEnvironment(true);
    SDL_SetEnvironmentVariable(inherited, "SEGARECOMP_GENESIS_REGION", "japan", true);
    apply_game_environment(inherited, genesis);
    expect(std::string(SDL_GetEnvironmentVariable(inherited, "SEGARECOMP_GENESIS_REGION")) == "usa", "USA selection overrides an inherited Japan value");
    apply_game_environment(inherited, japan);
    expect(std::string(SDL_GetEnvironmentVariable(inherited, "SEGARECOMP_GENESIS_REGION")) == "japan", "Japan selection reaches the child environment");
    SDL_DestroyEnvironment(inherited);
    SDL_Environment *clean = SDL_CreateEnvironment(false);
    apply_game_environment(clean, sms);
    expect(SDL_GetEnvironmentVariable(clean, "SEGARECOMP_GENESIS_REGION") == nullptr, "a Master System launch sets no region variable");
    SDL_DestroyEnvironment(clean);
    const char *after = SDL_getenv("SEGARECOMP_GENESIS_REGION");
    expect((after == nullptr) == (process_region_before == nullptr) && (after == nullptr || std::string(after) == process_region_before),
           "the launcher's own process environment is untouched by the seam");

    // Header region hints: both documented encodings; the lone 'E' is intrinsically ambiguous and keeps the default (explicit selection overrides).
    const auto hint = [](const char *text) { return region_hint_from_header_field(std::string(text)); };
    expect(hint("J").region == "japan" && !hint("J").ambiguous, "J alone selects Japan NTSC");
    expect(hint("JUE").region == "usa" && hint("U").region == "usa" && hint("JU").region == "usa", "an overseas NTSC declaration keeps USA");
    expect(hint("1").region == "japan" && hint("4").region == "usa" && hint("5").region == "usa" && hint("8").region == "usa", "hexadecimal bitfield digits");
    expect(hint("E").region == "usa" && hint("E").ambiguous && hint("E               ").ambiguous, "a bare E is ambiguous and keeps the USA default");
    expect(hint("").region == "usa" && !hint("").ambiguous, "an empty field keeps the default");
  }
  expect(entry_dir(optimized, layout) == entry_dir(optimized, layout), "cache identity is deterministic");
  RomView other_rom = optimized;
  other_rom.sha256 = std::string(64, 'b');
  expect(entry_dir(other_rom, layout) != entry_dir(optimized, layout), "a different ROM selects a different entry");
  RomView sms_optimized = sms;
  sms_optimized.aot_policy = "optimized";
  expect(!sms_optimized.optimized() && entry_dir(sms_optimized, layout) == entry_dir(sms, layout), "Master System ignores the Optimized request");

  // SEG-049: the experimental model family is part of the cache identity; Compatibility ignores it; the product-provided policy identity binds the rest.
  RomView gen2 = optimized;
  gen2.aot_model = "gen2";
  expect(gen2.optimized_gen2() && !optimized.optimized_gen2() && !genesis.optimized_gen2(), "gen2 is an explicit opt-in of Optimized only");
  expect(entry_dir(gen2, layout) != entry_dir(optimized, layout) && entry_dir(gen2, layout) != compat_entry, "v1, gen2 and Compatibility never share an entry");
  RomView compat_gen2 = genesis;
  compat_gen2.aot_model = "gen2";
  expect(!compat_gen2.optimized_gen2() && entry_dir(compat_gen2, layout) == compat_entry, "a model request never changes a Compatibility entry");
  RomView sms_gen2 = sms_optimized;
  sms_gen2.aot_model = "gen2";
  expect(!sms_gen2.optimized_gen2() && entry_dir(sms_gen2, layout) == entry_dir(sms, layout), "Master System ignores the model request");
  if (have_cli) {
    const std::string c = product_policy_identity(layout, "compatibility", "v1"), v1 = product_policy_identity(layout, "optimized", "v1"),
                      g2 = product_policy_identity(layout, "optimized", "gen2");
    const auto hex64 = [](const std::string &s) { return s.size() == 64U && s.find_first_not_of("0123456789abcdef") == std::string::npos; };
    expect(hex64(c) && hex64(v1) && hex64(g2) && c != v1 && v1 != g2 && c != g2, "product-provided policy identities are distinct 64-hex digests");
    expect(g2 == product_policy_identity(layout, "optimized", "gen2"), "policy identity is stable (memoized)");
    // Entry reuse: only an entry whose recorded request and policy identity equal the current ones is reusable.
    const auto make_entry = [&](const std::string &name, const std::string &object) {
      const fs::path entry = write_status(dir, name, R"({"status":"ok","aot_policy":)" + object + "}");
      std::ofstream(entry_executable(entry), std::ios::binary) << "x";
      return entry;
    };
    const auto report = [](const std::string &requested, const std::string &model, const std::string &pid) {
      return R"({"requested":")" + requested + R"(","requested_model":")" + model + R"(","effective":"broad","fallback":false,"reason":"none","policy_identity":")" + pid + R"("})";
    };
    expect(entry_ready_for(make_entry("r1", report("optimized", "gen2", g2)), gen2, layout), "a matching gen2 entry is reusable");
    expect(!entry_ready_for(make_entry("r2", report("optimized", "v1", v1)), gen2, layout), "a v1 entry is never reused for a gen2 request");
    expect(!entry_ready_for(make_entry("r3", report("optimized", "gen2", g2)), optimized, layout), "a gen2 entry is never reused for a v1 request");
    expect(!entry_ready_for(make_entry("r4", report("compatibility", "none", c)), optimized, layout), "a Compatibility entry is never reused for Optimized");
    expect(!entry_ready_for(make_entry("r5", report("optimized", "v1", v1)), genesis, layout), "an Optimized entry is never reused for Compatibility");
    expect(!entry_ready_for(make_entry("r6", report("optimized", "gen2", std::string(64U, '0'))), gen2, layout), "a stale policy identity (changed model/schema/contract) is rebuilt");
    expect(!entry_ready_for(make_entry("r8", R"({"requested":"optimized","requested_model":"gen2","effective":"broad","fallback":false,"reason":"none"})"), gen2, layout),
           "a missing recorded policy identity is refused when the product identity is known (fail closed)");
    expect(entry_ready_for(make_entry("r7", report("compatibility", "none", c)), genesis, layout), "a matching Compatibility entry is reusable");
    // ---- fail-closed identity (PR #94 correction) ----
    const auto entry_with = [&](const std::string &name, const std::string &requested, const std::string &model, const std::string &pid) {
      return make_entry(name, report(requested, model, pid));
    };
    const std::string good_v1 = v1, good_g2 = g2;
    expect(entry_ready_for(entry_with("m1", "optimized", "v1", good_v1), optimized, layout), "matching v1 identity: reuse accepted");
    expect(entry_ready_for(entry_with("m2", "optimized", "gen2", good_g2), gen2, layout), "matching gen2 identity: reuse accepted");
    expect(!entry_ready_for(entry_with("m3", "optimized", "v1", good_g2), optimized, layout), "a v1 request never reuses an entry carrying the gen2 identity");
    expect(!entry_ready_for(entry_with("m4", "optimized", "gen2", good_v1), gen2, layout), "a gen2 request never reuses an entry carrying the v1 identity");
    expect(!entry_ready_for(entry_with("m5", "optimized", "gen2", good_g2), optimized, layout) && !entry_ready_for(entry_with("m6", "optimized", "v1", good_v1), gen2, layout),
           "v1 and gen2 never reuse one another's entries");
    expect(!entry_ready_for(entry_with("m7", "optimized", "v1", std::string(64U, 'f')), optimized, layout), "a different recorded identity refuses reuse");
    // Identity unavailable: the tool path does not exist yet. Reuse is refused for v1 and gen2, Compatibility is unaffected, nothing is deleted.
    Layout broken = layout;
    broken.cli = dir / "no-such-tool" / "segarecomp-missing";
    expect(product_policy_identity(broken, "optimized", "v1").empty() && product_policy_identity(broken, "optimized", "gen2").empty(), "lookup fails without the tool");
    const fs::path keep_v1 = entry_with("u1", "optimized", "v1", good_v1), keep_g2 = entry_with("u2", "optimized", "gen2", good_g2);
    expect(!entry_ready_for(keep_v1, optimized, broken) && !entry_ready_for(keep_g2, gen2, broken), "identity unavailable: Optimized reuse refused (v1 and gen2)");
    expect(fs::exists(entry_executable(keep_v1)) && fs::exists(keep_v1 / "status.json"), "a failed lookup deletes nothing");
    expect(entry_ready_for(entry_with("u3", "compatibility", "none", c), genesis, broken), "Compatibility reuse needs no product identity");
    const fs::path legacy = write_status(dir, "u4", R"({"status":"ok"})");
    std::ofstream(entry_executable(legacy), std::ios::binary) << "x";
    expect(entry_ready_for(legacy, genesis, broken), "a historical Compatibility entry without the member stays reusable");
    expect(!entry_ready_for(legacy, optimized, broken), "a historical entry never satisfies Optimized");
    expect(entry_ready_for(entry_with("u5", "compatibility", "none", c), genesis, broken), "Compatibility regression");
    {
      RomView sms_rom = sms;
      const fs::path sms_entry = write_status(dir, "u6", R"({"status":"ok"})");
      std::ofstream(entry_executable(sms_entry), std::ios::binary) << "x";
      expect(entry_ready_for(sms_entry, sms_rom, broken), "Master System does not depend on the product identity");
    }
    // Recovery: the same path becomes a working tool; the earlier failure was not memoized.
    fs::create_directories(broken.cli.parent_path());
    fs::copy_file(argv[1], broken.cli, fs::copy_options::overwrite_existing);
    expect(product_policy_identity(broken, "optimized", "v1") == good_v1 && product_policy_identity(broken, "optimized", "gen2") == good_g2, "recovery: lookup succeeds later");
    expect(entry_ready_for(keep_v1, optimized, broken) && entry_ready_for(keep_g2, gen2, broken), "recovery: the matching entries are reusable again");
    // Cache key: an unavailable identity never selects the directory of a verified entry.
    Layout missing = layout;
    missing.cli = dir / "gone" / "segarecomp-missing";
    expect(entry_dir(optimized, missing) != entry_dir(optimized, layout) && entry_dir(gen2, missing) != entry_dir(gen2, layout), "an unavailable identity selects a different directory");
  } else {
    expect(product_policy_identity(layout, "optimized", "gen2").empty(), "no tool, no identity (the key then still separates by policy and model)");
  }

  // Machine-readable status.
  const auto compat = read_policy_report(write_status(dir, "c",
      R"({"status":"ok","stage":"done","aot_policy":{"requested":"compatibility","effective":"broad","fallback":false,"reason":"none","identity":"x"},"z":1})"));
  expect(compat.present && compat.requested == "compatibility" && compat.effective == "broad" && !compat.fallback, "Compatibility report");
  expect(policy_summary(compat) == "Compatibility build", "Compatibility wording");
  const auto good = read_policy_report(write_status(dir, "o",
      R"({"status":"ok","aot_policy":{"requested":"optimized","effective":"ml_region","fallback":false,"reason":"none","model":"seg046-features-v1","k":5}})"));
  expect(good.present && good.effective == "ml_region" && !good.fallback && good.reason == "none", "Optimized report");
  expect(policy_summary(good) == "Optimized build", "Optimized wording");
  const auto fell = read_policy_report(write_status(dir, "f",
      R"({"status":"ok","aot_policy":{"requested":"optimized","effective":"broad","fallback":true,"reason":"validator_rejected"}})"));
  expect(fell.present && fell.fallback && fell.reason == "validator_rejected" && fell.effective == "broad", "fallback report");
  expect(policy_summary(fell).find("unavailable") != std::string::npos && policy_summary(fell).find("Compatibility") != std::string::npos,
         "a fallback is stated, never silent");
  const auto exact = read_policy_report(write_status(dir, "e",
      R"({"aot_policy":{"requested":"optimized","effective":"admission_plan","fallback":false,"reason":"exact_plan_precedence"}})"));
  expect(policy_summary(exact) == "Optimized build (exact map)", "exact-map wording");
  const auto g2report = read_policy_report(write_status(dir, "g",
      R"({"status":"ok","aot_policy":{"requested":"optimized","requested_model":"gen2","effective":"ml_region_gen2","fallback":false,"reason":"none","fallback_class":"none","model_family":"gen2","model":"1125870828","rank_policy":"B35","selected_ppm":350097,"admitted_ppm":351832,"validator":"accepted","policy_identity":"aa","identity":"bb"}})"));
  expect(g2report.present && g2report.effective == "ml_region_gen2" && g2report.model_family == "gen2" && g2report.selected_ppm == 350097 && g2report.admitted_ppm == 351832 &&
             g2report.validator == "accepted" && g2report.requested_model == "gen2" && g2report.rank_policy == "B35",
         "gen2 report fields");
  expect(policy_summary(g2report) == "Optimized build (experimental model)", "gen2 wording");
  const std::string details = policy_details(g2report);
  expect(details.find("Selected: 35.0 %") != std::string::npos && details.find("Admitted: 35.1 %") != std::string::npos && details.find("Validator: accepted") != std::string::npos &&
             details.find("Model family: gen2") != std::string::npos,
         "advanced details: " + details);
  const auto g2fell = read_policy_report(write_status(dir, "gf",
      R"({"aot_policy":{"requested":"optimized","requested_model":"gen2","effective":"broad","fallback":true,"reason":"prune_rejected","fallback_class":"proposal_rejected","model_family":"none","validator":"not_run"}})"));
  expect(g2fell.fallback && policy_summary(g2fell).find("unavailable") != std::string::npos && policy_details(g2fell).find("proposal_rejected") != std::string::npos &&
             policy_details(g2fell).find("fell back") != std::string::npos,
         "gen2 fallback is stated, never silent");
  expect(!read_policy_report(write_status(dir, "n", R"({"status":"ok"})")).present, "older status without the member");
  expect(!read_policy_report(dir / "missing").present, "missing status");
  expect(policy_summary(PolicyReport{}).empty(), "nothing to say without a report");

  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "launcher_policy_test: ok\n";
  return 0;
}
