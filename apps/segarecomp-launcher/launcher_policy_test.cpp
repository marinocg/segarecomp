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
    expect(entry_ready_for(make_entry("r7", report("compatibility", "none", c)), genesis, layout), "a matching Compatibility entry is reusable");
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
