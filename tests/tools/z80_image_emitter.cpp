// SEG-008-T003: test-side CLI over the public Z80 image emitter. It reads a small line-based image spec and emits the
// sharded generated C, so hermetic tests, the conformance harness and the coverage tool never link production
// internals.
//
//   z80_image_emitter <spec> <outdir> <stem> [--list]
//
// spec lines (`#` starts a comment):
//   image  <identity> invariant|banked
//   window <identity> <base-hex> <first-offset-hex> <length-hex>
//   bytes  <identity> <hex-bytes>          (appended; may repeat)
//   fill   <identity> <byte-hex> <count-hex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "segarecomp/codegen/c11/z80.hpp"

using namespace segarecomp::codegen::z80;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: z80_image_emitter <spec> <outdir> <stem> [--list]\n";
    return 2;
  }
  const bool list = argc > 4 && std::string(argv[4]) == "--list";
  std::ifstream spec(argv[1]);
  if (!spec) {
    std::cerr << "cannot read spec\n";
    return 2;
  }
  ImageSet set;
  std::map<unsigned long, std::size_t> index;
  const auto image_of = [&](unsigned long identity) -> CodeImage& { return set.images.at(index.at(identity)); };
  for (std::string line; std::getline(spec, line);) {
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string verb;
    if (!(in >> verb)) continue;
    unsigned long identity = 0;
    in >> identity;
    if (verb == "image") {
      std::string kind;
      in >> kind;
      CodeImage image;
      image.identity = static_cast<std::uint32_t>(identity);
      image.kind = kind == "invariant" ? ImageKind::invariant : ImageKind::banked;
      index[identity] = set.images.size();
      set.images.push_back(std::move(image));
    } else if (verb == "window") {
      std::string base, first, length;
      in >> base >> first >> length;
      image_of(identity).windows.push_back({static_cast<std::uint16_t>(std::stoul(base, nullptr, 16)),
                                            static_cast<std::uint32_t>(std::stoul(first, nullptr, 16)),
                                            static_cast<std::uint32_t>(std::stoul(length, nullptr, 16))});
    } else if (verb == "bytes") {
      std::string hex;
      in >> hex;
      for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        image_of(identity).bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    } else if (verb == "fill") {
      std::string value, count;
      in >> value >> count;
      image_of(identity).bytes.insert(image_of(identity).bytes.end(), std::stoul(count, nullptr, 16),
                                      static_cast<std::uint8_t>(std::stoul(value, nullptr, 16)));
    } else {
      std::cerr << "unknown spec verb: " << verb << "\n";
      return 2;
    }
  }
  EmitOptions options;
  options.directory = argv[2];
  options.stem = argv[3];
  options.record_owners = list;
  const EmitResult result = emit_image_set(set, options);
  if (!result.error.empty()) {
    std::cout << "error " << result.error << "\n";
    return 1;
  }
  std::printf("stats full=%zu prefix_lock=%zu stubs=%zu unlowered=%zu variants=%zu bound=%zu units=%zu\n",
              result.stats.full_owners, result.stats.prefix_lock_owners, result.stats.stub_owners,
              result.stats.unlowered_starts, result.stats.variant_owners, result.stats.bound_successors,
              result.stats.translation_units);
  for (const OwnerRecord& r : result.owners) {
    std::printf("owner %u %04X %s %u %s %d %d\n", r.identity, r.key, owner_kind_name(r.kind), r.variants,
                r.form == segarecomp::cpu::z80::kNoForm ? "-" : segarecomp::cpu::z80::form_name(r.form).c_str(),
                r.window_relative ? 1 : 0, r.bound_successor ? 1 : 0);
  }
  return 0;
}
