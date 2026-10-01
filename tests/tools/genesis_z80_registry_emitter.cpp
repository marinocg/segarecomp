// SEG-032-T003: test-side CLI over the Genesis materialized-image registry and its emission.
//
//   genesis_z80_registry_emitter <spec> <outdir> <stem>
// spec lines: `epoch <8192-byte ram hex> <1024-byte written-bitmap hex>` (activation order); `#` starts a comment.
// stdout: one `epoch <n> <added|existing|restart|bound_exceeded> bound=<ordinal>` line per epoch, one
// `image <ordinal> content=<hex> signature=<hex>` per registered image, then `emitted ok` (or `error ...`, exit 1).
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "segarecomp/machine/genesis/z80_images.hpp"

using namespace segarecomp::machine::genesis::z80;

static bool parse_hex(const std::string& text, std::uint8_t* out, std::size_t count) {
  if (text.size() != count * 2) return false;
  for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<std::uint8_t>(std::stoul(text.substr(i * 2, 2), nullptr, 16));
  return true;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: genesis_z80_registry_emitter <spec> <outdir> <stem>\n";
    return 2;
  }
  std::ifstream spec(argv[1]);
  if (!spec) return 2;
  Registry registry;
  unsigned n = 0;
  bool exceeded = false;
  for (std::string line; std::getline(spec, line);) {
    if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream in(line);
    std::string verb, ram, written;
    if (!(in >> verb)) continue;
    if (verb != "epoch" || !(in >> ram >> written)) return 2;
    Epoch epoch;
    if (!parse_hex(ram, epoch.ram.data(), kRamBytes) || !parse_hex(written, epoch.written.data(), kBitmapBytes)) return 2;
    std::uint32_t bound = 0;
    const AddOutcome outcome = registry.add(epoch, bound);
    const char* name = outcome == AddOutcome::added ? "added" : outcome == AddOutcome::existing ? "existing"
                       : outcome == AddOutcome::restart ? "restart" : "bound_exceeded";
    std::printf("epoch %u %s bound=%u\n", ++n, name, static_cast<unsigned>(bound));
    exceeded |= outcome == AddOutcome::bound_exceeded;
  }
  for (const Image& image : registry.images())
    std::printf("image %u content=%s signature=%s\n", static_cast<unsigned>(image.ordinal), to_hex(image.content).c_str(),
                to_hex(image.signature).c_str());
  if (exceeded) {
    std::printf("error image bound exceeded\n");
    return 1;
  }
  EmitRequest request;
  request.directory = argv[2];
  request.stem = argv[3];
  const EmitOutcome outcome = emit_registry(registry, request);
  if (!outcome.ok()) {
    std::printf("error %s\n", outcome.error.c_str());
    return 1;
  }
  std::printf("emitted ok units=%zu entries=%zu\n", outcome.stats.translation_units, outcome.stats.entries);
  return 0;
}
