// Data-transfer and ALU family rows (SEG-008-T004 owns the growth of this file). T003 seed: LD r,r' and LD r,n.
#include "segarecomp/codegen/c11/z80_lowering.hpp"

namespace segarecomp::codegen::z80 {
namespace {

using cpu::z80::Mnemonic;
using cpu::z80::Operand;
using cpu::z80::Space;

// LD r,r' (base space): 01 ddd sss with neither field being (HL); the field values select the registers.
Lowered lower_ld_r_r(const LowerContext& c) {
  const unsigned op = c.instruction.provenance.opcode;
  return effect(reg8((op >> 3) & 7u) + " = " + reg8(op & 7u) + ";\n");
}

// LD r,n (base space): 00 ddd 110 n.
Lowered lower_ld_r_n(const LowerContext& c) {
  const unsigned op = c.instruction.provenance.opcode;
  const unsigned n = cpu::z80::immediate(c.instruction).value_or(0);
  return effect(reg8((op >> 3) & 7u) + " = " + hex_literal(n, 2) + ";\n");
}

constexpr LoweringRow kRows[] = {
    {Space::base, Mnemonic::ld, Operand::r, Operand::r, lower_ld_r_r},
    {Space::base, Mnemonic::ld, Operand::r, Operand::n, lower_ld_r_n},
};

}  // namespace

std::span<const LoweringRow> data_alu_lowering_rows() { return kRows; }

}  // namespace segarecomp::codegen::z80
