// SEG-057-T002: test double of a generated native program (never shipped). Its exit status models the run of the entry it sits in:
// <entry>/policy.txt names the policy it was built for; FAKE_OPT_EXIT / FAKE_COMPAT_EXIT give the status of an Optimized / Compatibility program.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int main(int, char **argv) {
  if (const char *log = std::getenv("FAKE_GAME_LOG")) std::ofstream(log, std::ios::app) << argv[0] << '\n';  // SEG-069-T005: which executable was started
  // SEG-117: models the in-game Video menu changing the shared viewer settings file during a play (the real viewer rewrites the same file on exit).
  if (const char *mode = std::getenv("FAKE_VIEWER_WIDESCREEN"); mode != nullptr && std::getenv("SEGARECOMP_VIEWER_SETTINGS") != nullptr) {
    const std::filesystem::path file = std::getenv("SEGARECOMP_VIEWER_SETTINGS");
    std::string kept, line;
    std::ifstream in(file);
    while (std::getline(in, line)) if (line.rfind("video.widescreen=", 0) != 0) kept += line + '\n';
    in.close();
    std::ofstream(file, std::ios::trunc) << kept << "video.widescreen=" << mode << '\n';
  }
  std::string policy;
  std::ifstream(std::filesystem::path(argv[0]).parent_path() / "policy.txt") >> policy;
  const char *code = std::getenv(policy == "optimized" ? "FAKE_OPT_EXIT" : "FAKE_COMPAT_EXIT");
  return code ? std::atoi(code) : 0;
}
