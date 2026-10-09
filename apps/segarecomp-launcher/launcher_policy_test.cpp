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

int main() {
  const fs::path dir = fs::temp_directory_path() / "segarecomp-launcher-policy-test";
  fs::remove_all(dir);
  fs::create_directories(dir);

  Layout layout;
  layout.root = dir / "pkg";
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
  expect(!read_policy_report(write_status(dir, "n", R"({"status":"ok"})")).present, "older status without the member");
  expect(!read_policy_report(dir / "missing").present, "missing status");
  expect(policy_summary(PolicyReport{}).empty(), "nothing to say without a report");

  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "launcher_policy_test: ok\n";
  return 0;
}
