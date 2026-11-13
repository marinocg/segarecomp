#pragma once

// `segarecomp mod diff-run` and `segarecomp mod test` (SEG-136-T007, ADR 0300). See mod_run.cpp.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace segarecomp_cli {

struct RunOptions {
  std::string target;                 // project directory or package
  std::filesystem::path rom;          // the exact original image
  std::string cc;
  std::filesystem::path sdk;          // the platforms directory (`<sdk>/genesis` is the runtime directory)
  std::string input;                  // pad script "<frame>:<player>:<mask>[,...]"
  std::uint64_t frames = 0, budget = 0, first = 1;   // `first`: the 1-based ordinal of the first observed frame (the window is `frames` consecutive frames from it)
  unsigned timeout = 0;
  bool has_frames = false, has_budget = false, has_timeout = false;
  std::vector<std::string> ram, code; // "<offset>:<length>" regions
  std::string scenario;               // test: run only this scenario
  bool aggregate = false;             // counts and digests only (no byte values)
  std::filesystem::path work;         // keep the builds and runs here (default: a private temporary directory, removed)
};

int run_diff(const RunOptions &options);
int run_test(const RunOptions &options);

}  // namespace segarecomp_cli
