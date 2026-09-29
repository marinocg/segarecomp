// Control-flow and stack family rows (SEG-008-T005 owns the growth of this file). T003 seed: NOP.
#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::Mnemonic;
using cpu::z80::Operand;
using cpu::z80::Space;

Lowered lower_nop(const LowerContext&) { return effect(""); }

constexpr LoweringRow kRows[] = {
    {Space::base, Mnemonic::nop, Operand::none, Operand::none, lower_nop},
};

}  // namespace

std::span<const LoweringRow> control_stack_lowering_rows() { return kRows; }

}  // namespace segarecomp::codegen::z80
