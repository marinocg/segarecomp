// SEG-078-T002 (ADR 0204): test double of a generated Sega CD viewer program (never shipped). Behaviour is chosen by the files beside it, so the
// launcher's verification can be exercised without a compiler or a window:
//   <exe>.describe   the text printed for `--describe` (absent: status 2, the way an older program rejects the unknown flag)
//   FAKE_SCD_MODE    `hang` (never answers --describe), `garbage` (prints nonsense), `slow-describe` is not modelled
//   a run (any other arguments): the arguments are appended to $FAKE_SCD_ARGS; $FAKE_SCD_REPORT (a file name) is copied to the --report path;
//   the exit status is $FAKE_SCD_EXIT (default 0); $FAKE_SCD_RECORD, when set, is written to the --record-input path (a recording).
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char **argv) {
  const std::string mode = std::getenv("FAKE_SCD_MODE") != nullptr ? std::getenv("FAKE_SCD_MODE") : "";
  if (argc == 2 && std::string(argv[1]) == "--describe") {
    if (mode == "hang") { std::this_thread::sleep_for(std::chrono::seconds(60)); return 0; }
    if (mode == "garbage") { std::cout << "this is not a statement\n"; return 0; }
    const std::string self = argv[0];
    std::ifstream in(self + ".describe");
    if (!in) return 2;
    std::cout << in.rdbuf();
    return 0;
  }
  std::vector<std::string> args(argv + 1, argv + argc);
  if (const char *log = std::getenv("FAKE_SCD_ARGS")) {
    std::ofstream out(log, std::ios::app);
    for (const auto &a : args) out << a << '\n';
    out << "--\n";
  }
  std::string report, record;
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--report") report = args[i + 1];
    if (args[i] == "--record-input") record = args[i + 1];
  }
  if (const char *src = std::getenv("FAKE_SCD_REPORT"); src != nullptr && !report.empty()) fs::copy_file(src, report, fs::copy_options::overwrite_existing);
  if (const char *rec = std::getenv("FAKE_SCD_RECORD"); rec != nullptr && !record.empty()) std::ofstream(record) << rec;
  const char *code = std::getenv("FAKE_SCD_EXIT");
  return code != nullptr ? std::atoi(code) : 0;
}
