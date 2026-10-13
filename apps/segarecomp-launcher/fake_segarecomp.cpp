// SEG-057-T002: test double of the product CLI for the launcher tests (never shipped). It implements only what the launcher consumes:
// `aot-policy-identity` and `build`. Behaviour is selected by environment variables so one binary serves every scenario:
//   FAKE_SMS_OPTIMIZED=0  the Master System Optimized policy is unavailable (the identity query fails)
//   FAKE_MODEL=<text>     part of the Optimized identity (a different value models a changed shipped model); a file `model.txt` next to the
//                         executable takes precedence (it models the model that is SHIPPED with this tool, and reaches children spawned by SDL)
//   FAKE_GAME_BIN=<path>  the program a successful build installs as <output>/game
//   FAKE_BUILD_LOG=<file> one line is appended per `build`: "<requested policy>"
//   FAKE_BUILD_FAIL=compatibility|optimized  that policy's build fails
//   FAKE_OPT_FALLBACK=1   an Optimized request builds the broad program and reports a fallback
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string env(const char *name, const char *fallback = "") {
  const char *value = std::getenv(name);
  return value ? value : fallback;
}

static std::string hex64(const std::string &text) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  std::size_t h = std::hash<std::string>{}(text);
  for (int i = 0; i < 64; ++i) {
    h = h * 6364136223846793005ULL + 1442695040888963407ULL;
    out.push_back(digits[(h >> 59) & 15U]);
  }
  return out;
}

static std::string shipped_model(const char *argv0) {
  std::string model;
  std::ifstream(fs::path(argv0).parent_path() / "model.txt") >> model;
  return model.empty() ? env("FAKE_MODEL", "m1") : model;
}

static std::string g_model;

static std::string identity(const std::string &policy, const std::string &platform) {
  return hex64("id|" + policy + "|" + platform + "|" + (policy == "optimized" ? g_model : std::string()));
}

int main(int argc, char **argv) {
  g_model = shipped_model(argv[0]);
  const std::string command = argc > 1 ? argv[1] : "";
  std::vector<std::string> args(argv + 2, argv + argc);
  const auto option = [&](const std::string &key) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
      if (args[i] == key) return args[i + 1];
    return std::string();
  };
  if (command == "aot-policy-identity") {
    const std::string policy = option("--aot-policy"), platform = option("--platform").empty() ? "genesis" : option("--platform");
    if (platform == "master-system" && policy == "optimized" && env("FAKE_SMS_OPTIMIZED", "1") == "0") return 3;
    std::printf("policy_identity=%s requested=%s\n", identity(policy, platform).c_str(), policy.c_str());
    return 0;
  }
  if (command == "build") {
    const std::string policy = option("--aot-policy").empty() ? "compatibility" : option("--aot-policy");
    const fs::path out = option("--output");
    const std::string platform = option("--mapper").empty() && option("--mapper-manifest").empty() ? "genesis" : "master-system";
    if (!env("FAKE_BUILD_LOG").empty()) std::ofstream(env("FAKE_BUILD_LOG"), std::ios::app) << policy << '\n';
    fs::create_directories(out);
    std::puts("@stage analyze begin\n@stage analyze done\n@stage generate begin\n@stage generate done");
    if (env("FAKE_BUILD_FAIL") == policy) {
      std::puts("@result failed stage=compile message=The fake build failed on request.");
      return 1;
    }
    const bool fallback = policy == "optimized" && env("FAKE_OPT_FALLBACK") == "1";
    const std::string effective = policy == "optimized" && !fallback ? (platform == "master-system" ? "ml_region_z80" : "ml_region") : "broad";
    std::ofstream(out / "status.json") << "{\"status\":\"ok\",\"stage\":\"done\",\"aot_policy\":{\"requested\":\"" << policy << "\",\"requested_model\":\""
                                       << (policy == "optimized" ? (platform == "master-system" ? "d512hp" : "v1") : "none") << "\",\"effective\":\"" << effective
                                       << "\",\"fallback\":" << (fallback ? "true" : "false") << ",\"reason\":\"" << (fallback ? "inference" : "none")
                                       << "\",\"fallback_class\":\"" << (fallback ? "producer_failed" : "none") << "\",\"model_family\":\""
                                       << (effective == "ml_region_z80" ? "sms_z80_d512hp" : "none") << "\",\"validator\":\"not_run\",\"selected_ppm\":650000,\"admitted_ppm\":650000"
                                       << ",\"policy_identity\":\"" << identity(policy, platform) << "\",\"identity\":\"" << hex64("b|" + out.string()) << "\"}}\n";
    std::ofstream(out / "policy.txt") << policy;
#if defined(_WIN32)
    const char *suffix = ".exe";
#else
    const char *suffix = "";
#endif
    std::error_code ec;
    fs::copy_file(env("FAKE_GAME_BIN"), out / (std::string("game") + suffix), fs::copy_options::overwrite_existing, ec);
    if (ec) { std::puts("@result failed stage=link message=The fake game could not be installed."); return 1; }
    std::puts("@stage compile begin\n@stage compile done\n@stage link begin\n@stage link done");
    return 0;
  }
  return 2;
}
