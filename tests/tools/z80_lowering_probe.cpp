// SEG-008-T003: prints the legal forms that currently have a lowering row (`lowered <form name>` per line), through
// the public codegen_c11_z80 entry point. tools/z80_capability_coverage.py credits the `lowers` stage from it.
#include <cstdio>

#include "segarecomp/codegen/c11/z80_lowering.hpp"
#include "segarecomp/cpu/z80/forms.hpp"

int main() {
  using namespace segarecomp;
  for (const cpu::z80::FormDescriptor& form : cpu::z80::all_forms())
    if (codegen::z80::has_lowering(form.id)) std::printf("lowered %s\n", cpu::z80::form_name(form.id).c_str());
  return 0;
}
