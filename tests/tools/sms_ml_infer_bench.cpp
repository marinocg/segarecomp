// SEG-057-T002 (ADR 0119): measurement CLI for the native D512HP producer (test-side, never shipped).
//
//   sms_ml_infer_bench <rom> [--threads N] [--dump-logits file] [--dump-cells file]
//
// Prints one JSON line: model initialization time (embedded artifact verification + weight layout), inference time over the whole image,
// the selection, and the process peak resident set. `--dump-*` write raw little-endian float32 logits / u32 retained cells to private files
// for the offline PyTorch comparison; nothing else is printed about the image.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "segarecomp/machine/master_system/ml_region_z80_producer.hpp"

using namespace segarecomp::machine::master_system;

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  unsigned threads = 0;
  std::string dump_logits, dump_cells;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--threads")) threads = static_cast<unsigned>(std::stoul(argv[i + 1]));
    else if (!std::strcmp(argv[i], "--dump-logits")) dump_logits = argv[i + 1];
    else if (!std::strcmp(argv[i], "--dump-cells")) dump_cells = argv[i + 1];
    else return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const std::string failure = ml_identity_failure();  // first call: verifies and lays out the embedded weights
  const auto t1 = clock::now();
  const std::vector<float> logits = ml_byte_logits(rom, threads);
  const auto t2 = clock::now();
  const std::vector<float> scores = ml_cell_scores(logits);
  const std::vector<std::uint32_t> cells = ml_select_cells(scores);
  const auto t3 = clock::now();
  if (!failure.empty() || logits.empty() || cells.empty()) {
    std::printf("{\"ok\":false}\n");
    return 1;
  }
  if (!dump_logits.empty()) std::ofstream(dump_logits, std::ios::binary).write(reinterpret_cast<const char *>(logits.data()), static_cast<std::streamsize>(logits.size() * 4));
  if (!dump_cells.empty()) std::ofstream(dump_cells, std::ios::binary).write(reinterpret_cast<const char *>(cells.data()), static_cast<std::streamsize>(cells.size() * 4));
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto seconds = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
  std::printf("{\"ok\":true,\"rom_bytes\":%zu,\"threads\":%u,\"init_s\":%.4f,\"infer_s\":%.4f,\"select_s\":%.4f,\"cells\":%zu,\"retained\":%zu,\"user_cpu_s\":%.3f,\"peak_rss_kib\":%ld}\n",
              rom.size(), threads, seconds(t0, t1), seconds(t1, t2), seconds(t2, t3), scores.size(), cells.size(),
              static_cast<double>(usage.ru_utime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec) / 1e6, usage.ru_maxrss);
  return 0;
}
