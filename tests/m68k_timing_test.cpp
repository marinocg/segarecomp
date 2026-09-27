#include "segarecomp/cpu/m68k/timing.hpp"

#include <cassert>

using namespace segarecomp;

namespace {
M68kIrOperation operation(M68kIrKind kind, M68kMemoryAccessWidth size,
                          M68kEaMode source, M68kEaMode destination) {
  M68kIrOperation value{};
  value.kind = kind;
  value.size = size;
  value.source_ea.mode = source;
  value.destination_ea.mode = destination;
  return value;
}
}  // namespace

int main() {
  // Table 8-1 composes size-aware literal EA cells, including immediate.
  assert(m68k_instruction_cycles(operation(M68kIrKind::write_move, M68kMemoryAccessWidth::long_word,
                                            M68kEaMode::data_register, M68kEaMode::address_indirect)) == 12U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::write_move, M68kMemoryAccessWidth::word,
                                           M68kEaMode::immediate, M68kEaMode::data_register)) == 8U);
  // Table 8-10 direct control rows.
  assert(m68k_instruction_cycles(operation(M68kIrKind::jump_general, M68kMemoryAccessWidth::word,
                                           M68kEaMode::address_indirect, M68kEaMode::unused)) == 8U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::call_general, M68kMemoryAccessWidth::word,
                                           M68kEaMode::absolute_long, M68kEaMode::unused)) == 20U);
  // Table 8-4 has CMPA-specific rows (6 + EA at both sizes; SEG-021-T022 corrected the former word row of 8 + EA),
  // and the ADDA/SUBA long exceptions.
  assert(m68k_instruction_cycles(operation(M68kIrKind::compare_address, M68kMemoryAccessWidth::word,
                                            M68kEaMode::address_indirect, M68kEaMode::address_register)) == 10U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::add_address, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::data_register, M68kEaMode::address_register)) == 8U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::subtract_address, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::immediate, M68kEaMode::address_register)) == 16U);
  // Table 8-4: CMP.L's Dn source row is six cycles, not CMP.B/W's four.
  assert(m68k_instruction_cycles(operation(M68kIrKind::compare, M68kMemoryAccessWidth::long_word,
                                            M68kEaMode::data_register, M68kEaMode::data_register)) == 6U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::write_clr, M68kMemoryAccessWidth::long_word,
                                            M68kEaMode::unused, M68kEaMode::address_indirect)) == 20U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::logical_not, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::unused, M68kEaMode::data_register)) == 6U);
  // Table 8-6 TST memory is 4 + EA (SEG-021-T022 corrected the former EA-only value).
  assert(m68k_instruction_cycles(operation(M68kIrKind::test_operand, M68kMemoryAccessWidth::word,
                                           M68kEaMode::address_indirect, M68kEaMode::unused)) == 8U);
  // Tables 8-2/8-3: the -(An) destination column equals (An) (SEG-021-T022).
  assert(m68k_instruction_cycles(operation(M68kIrKind::write_move, M68kMemoryAccessWidth::word,
                                           M68kEaMode::data_register, M68kEaMode::address_predec)) == 8U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::write_move, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::address_predec, M68kEaMode::address_predec)) == 22U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::logical_or, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::data_register, M68kEaMode::address_indirect)) == 20U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::logical_and_immediate, M68kMemoryAccessWidth::word,
                                             M68kEaMode::immediate, M68kEaMode::address_indirect)) == 16U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::compare_immediate, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::immediate, M68kEaMode::data_register)) == 14U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::compare_immediate, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::immediate, M68kEaMode::address_indirect)) == 20U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::add_quick, M68kMemoryAccessWidth::word,
                                           M68kEaMode::immediate, M68kEaMode::data_register)) == 4U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::subtract_quick, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::immediate, M68kEaMode::address_register)) == 8U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::bit_test, M68kMemoryAccessWidth::byte,
                                            M68kEaMode::data_register, M68kEaMode::address_indirect)) == 8U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::bit_test, M68kMemoryAccessWidth::byte,
                                           M68kEaMode::immediate, M68kEaMode::address_indirect)) == 12U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::bit_set, M68kMemoryAccessWidth::byte,
                                             M68kEaMode::immediate, M68kEaMode::address_indirect)) == 16U);
  // Table 8-8 register rows: BCLR is 10 / 14 (dynamic / static), two above BCHG/BSET (SEG-021-T022).
  assert(m68k_instruction_cycles(operation(M68kIrKind::bit_clear, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::data_register, M68kEaMode::data_register)) == 10U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::bit_clear, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::immediate, M68kEaMode::data_register)) == 14U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::bit_change, M68kMemoryAccessWidth::long_word,
                                           M68kEaMode::data_register, M68kEaMode::data_register)) == 8U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::load_effective_address, M68kMemoryAccessWidth::word,
                                           M68kEaMode::address_indirect, M68kEaMode::address_register)) == 4U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::push_effective_address, M68kMemoryAccessWidth::word,
                                           M68kEaMode::absolute_long, M68kEaMode::unused)) == 20U);
  // Table 8-10: the indexed LEA / PEA rows are 12 / 20 (SEG-021-T022).
  assert(m68k_instruction_cycles(operation(M68kIrKind::load_effective_address, M68kMemoryAccessWidth::word,
                                           M68kEaMode::pc_index8, M68kEaMode::address_register)) == 12U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::push_effective_address, M68kMemoryAccessWidth::word,
                                           M68kEaMode::address_index8, M68kEaMode::unused)) == 20U);
  auto movem = operation(M68kIrKind::movem_transfer, M68kMemoryAccessWidth::word,
                         M68kEaMode::unused, M68kEaMode::address_predec);
  movem.movem_register_mask = 0x0003U;
  assert(m68k_instruction_cycles(movem) == 16U);
  // Table 8-13 status-register transfers have literal rows, never the
  // generic four-cycle row; unsupported destination forms fail closed.
  assert(m68k_instruction_cycles(operation(M68kIrKind::write_status_register, M68kMemoryAccessWidth::word,
                                            M68kEaMode::data_register, M68kEaMode::unused)) == 12U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::write_condition_codes, M68kMemoryAccessWidth::word,
                                            M68kEaMode::immediate, M68kEaMode::unused)) == 16U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::read_status_register, M68kMemoryAccessWidth::word,
                                            M68kEaMode::unused, M68kEaMode::data_register)) == 6U);
  assert(!m68k_instruction_cycles(operation(M68kIrKind::read_status_register, M68kMemoryAccessWidth::word,
                                             M68kEaMode::unused, M68kEaMode::pc_disp16)));
  // SEG-021-T022: DIV (exact data-dependent count, replacing the former Table 8-7 maximum policy 140/158 + EA) and
  // MUL (38 + 2n) are data-dependent, so the complete-scalar API fails closed; `m68k_instruction_timing` owns their
  // rules (tests/m68k_dynamic_timing_test.cpp).
  assert(!m68k_instruction_cycles(operation(M68kIrKind::divide_unsigned_word, M68kMemoryAccessWidth::word,
                                            M68kEaMode::address_indirect, M68kEaMode::data_register)));
  assert(!m68k_instruction_cycles(operation(M68kIrKind::divide_signed_word, M68kMemoryAccessWidth::word,
                                            M68kEaMode::immediate, M68kEaMode::data_register)));
  assert(!m68k_instruction_cycles(operation(M68kIrKind::multiply_unsigned_word, M68kMemoryAccessWidth::word,
                                             M68kEaMode::address_indirect, M68kEaMode::data_register)));
  M68kEffectiveAddress indirect{};
  indirect.mode = M68kEaMode::address_indirect;
  assert(m68k_effective_address_cycles(indirect, M68kMemoryAccessWidth::word) == 4U);
  assert(m68k_effective_address_cycles(indirect, M68kMemoryAccessWidth::long_word) == 8U);
}
