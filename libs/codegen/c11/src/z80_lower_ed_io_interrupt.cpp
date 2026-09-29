// ED, I/O and interrupt-state family rows (SEG-008-T007 owns the growth of this file). T003 seed: HALT.
#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::Mnemonic;
using cpu::z80::Operand;
using cpu::z80::Space;

// HALT: PC is already advanced past the opcode; the runtime accounts halted M1 cycles up to the deadline and leaves
// the halted state when an interrupt is accepted at a boundary (resumable `halted` outcome, ADR 0058 section 7).
Lowered lower_halt(const LowerContext&) {
  Lowered lowered;
  lowered.statements = "s->halted = 1u;\n";
  lowered.flow = Flow::dispatch_fallthrough;
  return lowered;
}

constexpr LoweringRow kRows[] = {
    {Space::base, Mnemonic::halt, Operand::none, Operand::none, lower_halt},
};

}  // namespace

std::span<const LoweringRow> ed_io_interrupt_lowering_rows() { return kRows; }

}  // namespace segarecomp::codegen::z80
