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
  // Table 8-4 has CMPA-specific rows, including ADDA/SUBA long exceptions.
  assert(m68k_instruction_cycles(operation(M68kIrKind::compare_address, M68kMemoryAccessWidth::word,
                                            M68kEaMode::address_indirect, M68kEaMode::address_register)) == 12U);
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
  assert(m68k_instruction_cycles(operation(M68kIrKind::test_operand, M68kMemoryAccessWidth::word,
                                           M68kEaMode::address_indirect, M68kEaMode::unused)) == 4U);
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
  assert(m68k_instruction_cycles(operation(M68kIrKind::load_effective_address, M68kMemoryAccessWidth::word,
                                           M68kEaMode::address_indirect, M68kEaMode::address_register)) == 4U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::push_effective_address, M68kMemoryAccessWidth::word,
                                           M68kEaMode::absolute_long, M68kEaMode::unused)) == 20U);
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
  // DIV uses the Table 8-4 maximum policy. MUL is data-dependent, so the
  // complete-scalar API fails closed; its generated expression obtains the
  // separately exposed, size-aware Table 8-1 EA term.
  assert(m68k_instruction_cycles(operation(M68kIrKind::divide_unsigned_word, M68kMemoryAccessWidth::word,
                                           M68kEaMode::address_indirect, M68kEaMode::data_register)) == 144U);
  assert(m68k_instruction_cycles(operation(M68kIrKind::divide_signed_word, M68kMemoryAccessWidth::word,
                                           M68kEaMode::immediate, M68kEaMode::data_register)) == 162U);
  assert(!m68k_instruction_cycles(operation(M68kIrKind::multiply_unsigned_word, M68kMemoryAccessWidth::word,
                                             M68kEaMode::address_indirect, M68kEaMode::data_register)));
  M68kEffectiveAddress indirect{};
  indirect.mode = M68kEaMode::address_indirect;
  assert(m68k_effective_address_cycles(indirect, M68kMemoryAccessWidth::word) == 4U);
  assert(m68k_effective_address_cycles(indirect, M68kMemoryAccessWidth::long_word) == 8U);
}
