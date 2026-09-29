// SEG-008-T002: per-byte Z80 decode probe. Prints one tab-separated line per (space, opcode byte) of the seven
// canonical spaces using only the public cpu_z80 entry points; tools/z80_capability_coverage.py compares the
// lines with the independent legal-form dataset. Later pipeline stages are not implemented and never reported.
#include <cstdio>

#include "segarecomp/cpu/z80/forms.hpp"

using namespace segarecomp::cpu::z80;

int main() {
  std::printf("# space\tbyte\tclass\tform\tmnemonic\tdst\tsrc\tdocumented\talias_of\tlength\tm1\topcode_index\t"
              "displacement_index\timmediate_index\timmediate_size\ttiming_class\ttiming_primary\ttiming_alternate\n");
  constexpr const char* kClass[] = {"fixed", "conditional", "repeat", "halt"};
  for (unsigned s = 0; s < kSpaceCount; ++s) {
    const Space space = static_cast<Space>(s);
    for (unsigned b = 0; b < 256; ++b) {
      const ByteClass bc = classify_opcode_byte(space, static_cast<std::uint8_t>(b));
      if (bc.form == kNoForm) {
        std::printf("%s\t%u\t%s\t-\t-\t-\t-\t-\t-\t0\t0\t0\t0\t0\t0\t-\t0\t0\n", space_name(space), b,
                    byte_class_name(bc.kind));
        continue;
      }
      const FormDescriptor& f = form_descriptor(bc.form);
      std::printf("%s\t%u\t%s\t%s\t%s\t%s\t%s\t%d\t%s\t%u\t%u\t%u\t%d\t%d\t%u\t%s\t%u\t%u\n", space_name(space), b, byte_class_name(bc.kind),
                  form_name(f.id).c_str(), mnemonic_name(f.mnemonic), operand_name(f.dst), operand_name(f.src),
                  f.documented ? 1 : 0, f.alias_of == kNoForm ? "-" : form_name(f.alias_of).c_str(), f.length,
                  f.m1_fetches, f.opcode_index, f.displacement_index == kNoIndex ? -1 : f.displacement_index,
                  f.immediate_index == kNoIndex ? -1 : f.immediate_index, f.immediate_size,
                  kClass[static_cast<unsigned>(f.timing.klass)], f.timing.primary, f.timing.alternate);
    }
  }
  return 0;
}
