// SEG-009-T002: test-side CLI over the public SMS generation route (segarecomp::machine::master_system::emit_cartridge).
//
//   sms_image_emitter <rom> <outdir> <stem> [--mapper NAME] [--manifest FILE] [--explicit-profile] [--list]
//
// Exit 0: emitted (prints `ok ...`, `stats ...`, optional `owner ...` lines as z80_image_emitter). Exit 3: typed SMS
// error (prints `sms_error <NAME> <detail>`). Exit 1: other failure. Exit 2: usage.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "segarecomp/machine/master_system/cartridge.hpp"
#include "segarecomp/machine/master_system/emit.hpp"
#include "segarecomp/machine/master_system/image_set.hpp"
#include "segarecomp/sha256.hpp"

using namespace segarecomp::machine::master_system;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: sms_image_emitter <rom> <outdir> <stem> [--mapper NAME] [--manifest FILE] [--explicit-profile] [--list]\n";
    return 2;
  }
  std::ifstream rom_file(argv[1], std::ios::binary);
  if (!rom_file) {
    std::cerr << "cannot read ROM\n";
    return 2;
  }
  const std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(rom_file)), std::istreambuf_iterator<char>());
  IngestOptions options;
  bool list = false;
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mapper" && i + 1 < argc) {
      options.declarations.push_back({argv[++i], DeclarationSource::build_option});
    } else if (arg == "--manifest" && i + 1 < argc) {
      std::ifstream file(argv[++i], std::ios::binary);
      std::ostringstream text;
      text << file.rdbuf();
      const auto parsed = parse_mapper_manifest(text.str(), segarecomp::sha256_hex(rom));
      if (parsed.error != SMS_OK) {
        std::printf("sms_error %s %s\n", sms_error_name(parsed.error), parsed.detail.c_str());
        return 3;
      }
      options.declarations.push_back(parsed.declaration);
    } else if (arg == "--explicit-profile") {
      options.explicit_profile = true;
    } else if (arg == "--list") {
      list = true;
    } else {
      std::cerr << "unknown argument " << arg << "\n";
      return 2;
    }
  }
  EmitRequest request;
  request.directory = argv[2];
  request.stem = argv[3];
  request.codegen.record_owners = list;
  const EmitOutcome outcome = emit_cartridge(rom, options, request);
  if (!outcome.ok()) {
    if (outcome.sms_error != SMS_OK) {
      std::printf("sms_error %s %s\n", sms_error_name(outcome.sms_error), outcome.error.c_str());
      return 3;
    }
    std::printf("error %s\n", outcome.error.c_str());
    return 1;
  }
  std::printf("ok mapper=%s source=%s sha256=%s size=%zu banks=%u\n", mapper_family_name(outcome.identity.mapper),
              declaration_source_name(outcome.identity.declaration_source), outcome.identity.sha256.c_str(),
              outcome.identity.size, outcome.identity.bank_count);
  std::printf("stats full=%zu prefix_lock=%zu stubs=%zu unlowered=%zu variants=%zu bound=%zu units=%zu\n",
              outcome.stats.full_owners, outcome.stats.prefix_lock_owners, outcome.stats.stub_owners,
              outcome.stats.unlowered_starts, outcome.stats.variant_owners, outcome.stats.bound_successors,
              outcome.stats.translation_units);
  for (const auto& r : outcome.owners) {
    std::printf("owner %u %04X %s %u %s %d %d\n", r.identity, r.key, segarecomp::codegen::z80::owner_kind_name(r.kind),
                r.variants, r.form == segarecomp::cpu::z80::kNoForm ? "-" : segarecomp::cpu::z80::form_name(r.form).c_str(),
                r.window_relative ? 1 : 0, r.bound_successor ? 1 : 0);
  }
  return 0;
}
