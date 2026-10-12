// SEG-051 (ADR 0116): segarecomp_z80_token_report --rom <image> --output <path>
// Report-only Z80-owned decode tokens of every image position (see token_report.hpp). Offline ML experiment input.
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

#include "segarecomp/cpu/z80/token_report.hpp"

int main(int argc, char **argv) {
  if (argc != 5 || std::string_view(argv[1]) != "--rom" || std::string_view(argv[3]) != "--output") {
    std::cerr << "usage: segarecomp_z80_token_report --rom <image> --output <path>\n";
    return 2;
  }
  std::ifstream in{argv[2], std::ios::binary};
  if (!in) { std::cerr << "segarecomp_z80_token_report: cannot read the image\n"; return 1; }
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const auto report = segarecomp::cpu::z80::decode_token_report(bytes);
  std::ofstream sink{argv[4], std::ios::binary};
  sink << report;
  if (!sink) { std::cerr << "segarecomp_z80_token_report: cannot write the report\n"; return 2; }
  return 0;
}
