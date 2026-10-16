// SEG-068-T008 (ADR 0135): report-only D0 driver. usage: z80_static_mutation_cli <8192-byte image> [--detail]
// The input is a KNOWN image (label it known_image_from_prior_execution when it came from a materialized snapshot).
// Nothing is executed. Default output is aggregates only; --detail prints addresses and is for private local diagnosis.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "z80_static_mutation.hpp"

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s <image> [--detail]\n", argv[0]); return 2; }
  std::ifstream in(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const bool detail = argc > 2 && std::strcmp(argv[2], "--detail") == 0;
  namespace zm = segarecomp::experiments::z80_mutation;
  const zm::Report report = zm::analyze(image, zm::hardware_roots());
  std::fputs(zm::format_report(report, detail).c_str(), stdout);
  return image.size() == zm::kRamBytes ? 0 : 3;
}
