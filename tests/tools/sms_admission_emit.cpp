// SEG-057: test-side SMS generation CLI for the Z80 selective-admission economics benchmark (ADR 0118).
//
//   sms_admission_emit <rom> <outdir> <stem> [--plan <file>]
//
// The production SMS route (`emit_cartridge`: ingest -> executable images -> Z80 emitter -> embedded cartridge) with an optional explicit
// admission plan. The plan file is private measurement input, never a committed artifact: one `<identity> <lo> <hi>` decimal line per run
// of admitted image offsets [lo, hi) (identity 1 = the fixed image, 2+n = bank n). Each image of the cartridge is bound to its identity,
// size and digest here; an image without a line admits nothing. Prints one JSON stats line.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "segarecomp/machine/master_system/emit.hpp"
#include "segarecomp/machine/master_system/image_set.hpp"

namespace sms = segarecomp::machine::master_system;
namespace z80 = segarecomp::codegen::z80;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: sms_admission_emit <rom> <outdir> <stem> [--plan <file>]\n";
    return 2;
  }
  std::ifstream rom_file(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(rom_file)), std::istreambuf_iterator<char>());
  if (rom.empty()) {
    std::cerr << "cannot read the ROM\n";
    return 2;
  }
  sms::IngestOptions ingest;
  ingest.explicit_profile = true;
  ingest.declarations.push_back({"sega", sms::DeclarationSource::build_option});
  sms::EmitRequest request;
  request.directory = argv[2];
  request.stem = argv[3];
  for (int i = 4; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) != "--plan") {
      std::cerr << "unknown option\n";
      return 2;
    }
    std::ifstream plan_file(argv[i + 1]);
    std::map<std::uint32_t, std::vector<std::uint32_t>> offsets;
    std::uint32_t identity = 0, lo = 0, hi = 0;
    for (std::string line; std::getline(plan_file, line);) {
      std::istringstream in(line);
      if (!(in >> identity >> lo >> hi)) continue;
      for (std::uint32_t o = lo; o < hi; ++o) offsets[identity].push_back(o);
    }
    const sms::IngestResult cartridge = sms::ingest_cartridge(rom, ingest);
    if (!cartridge.ok()) {
      std::cerr << "ingest: " << cartridge.detail << "\n";
      return 1;
    }
    z80::AdmissionPlan plan;
    for (const z80::CodeImage& image : sms::build_image_set(cartridge).images)
      plan.images.push_back(z80::bind_admission(image, offsets[image.identity]));
    request.codegen.admission = std::move(plan);
  }
  const sms::EmitOutcome outcome = sms::emit_cartridge(rom, ingest, request);
  if (!outcome.ok()) {
    std::cout << "error " << outcome.error << "\n";
    return 1;
  }
  const z80::EmitStats& s = outcome.stats;
  std::printf("{\"selective\":%s,\"entries\":%zu,\"owners\":%zu,\"shared_bodies\":%zu,\"translation_units\":%zu,\"full_owners\":%zu,"
              "\"unlowered_starts\":%zu,\"bound_successors\":%zu,\"admitted\":%zu,\"omitted\":%zu,\"omitted_ranges\":%zu}\n",
              request.codegen.admission ? "true" : "false", s.entries, s.owners, s.shared_bodies, s.translation_units, s.full_owners,
              s.unlowered_starts, s.bound_successors, s.admitted_offsets, s.omitted_offsets, s.omitted_ranges);
  return 0;
}
