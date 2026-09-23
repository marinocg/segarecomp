#include "segarecomp/cpu/m68k/timing.hpp"

namespace segarecomp {

namespace {

// MC68000UM table 8-1.  These are literal effective-address calculation
// cells for the base MC68000; instruction tables which do not use table 8-1
// are deliberately handled by their own literal rows below.
std::optional<std::uint32_t> ea_cycles(const M68kEffectiveAddress &ea,
                                       M68kMemoryAccessWidth size) noexcept {
  switch (ea.mode) {
  case M68kEaMode::unused:
  case M68kEaMode::data_register:
  case M68kEaMode::address_register: return 0U;
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc: return size == M68kMemoryAccessWidth::long_word ? 8U : 4U;
  case M68kEaMode::address_predec: return size == M68kMemoryAccessWidth::long_word ? 10U : 6U;
  case M68kEaMode::address_disp16:
  case M68kEaMode::pc_disp16:
  case M68kEaMode::absolute_word: return size == M68kMemoryAccessWidth::long_word ? 12U : 8U;
  case M68kEaMode::address_index8:
  case M68kEaMode::pc_index8: return size == M68kMemoryAccessWidth::long_word ? 14U : 10U;
  case M68kEaMode::absolute_long: return size == M68kMemoryAccessWidth::long_word ? 16U : 12U;
  case M68kEaMode::immediate: return size == M68kMemoryAccessWidth::long_word ? 8U : 4U;
  }
  return std::nullopt;
}

std::uint32_t popcount(std::uint16_t value) noexcept {
  std::uint32_t count = 0;
  while (value != 0U) { count += value & 1U; value >>= 1U; }
  return count;
}

bool memory_ea(M68kEaMode mode) noexcept {
  return mode != M68kEaMode::unused && mode != M68kEaMode::data_register &&
         mode != M68kEaMode::address_register && mode != M68kEaMode::immediate;
}

std::optional<std::uint32_t> move_cycles(const M68kIrOperation &o) noexcept {
  const auto source_ea = ea_cycles(o.source_ea, o.size);
  const auto destination_ea = ea_cycles(o.destination_ea, o.size);
  if (!source_ea || !destination_ea) return std::nullopt;
  // Table 8-2: the literal table-8-1 source and destination cells compose
  // with the four-clock MOVE base.
  return 4U + *source_ea + *destination_ea;
}

std::optional<std::uint32_t> ea(const M68kEffectiveAddress &value, M68kMemoryAccessWidth size) noexcept {
  return ea_cycles(value, size);
}
std::optional<std::uint32_t> source_alu(const M68kIrOperation &o, std::uint32_t word_base, std::uint32_t long_base) noexcept {
  const auto value = ea(o.source_ea, o.size);
  return value ? std::optional<std::uint32_t>((o.size == M68kMemoryAccessWidth::long_word ? long_base : word_base) + *value) : std::nullopt;
}
// Table 8-4 has separate CMPA rows: word sign-extension costs 8 plus the
// B/W EA column, while long costs 6 plus the long EA column.
std::optional<std::uint32_t> cmpa_cycles(const M68kIrOperation &o) noexcept {
  const auto value = ea(o.source_ea, o.size);
  if (!value) return std::nullopt;
  return (o.size == M68kMemoryAccessWidth::long_word ? 6U : 8U) + *value;
}

// Table 8-4: CMP.L has its own base cycle column.  In particular, Dn -> Dn
// is six cycles, rather than the four-cycle B/W row.
std::optional<std::uint32_t> compare_cycles(const M68kIrOperation &o) noexcept {
  return source_alu(o, 4U, 6U);
}

// Table 8-13 MOVE <ea>,SR / MOVE <ea>,CCR source rows.  These are not ALU
// EA additions: the instruction has distinct literal totals, including the
// immediate extension word.  Unsupported (and decoder-illegal) forms remain
// unaccounted rather than inheriting a register-direct timing.
std::optional<std::uint32_t> move_to_status_cycles(const M68kIrOperation &o) noexcept {
  switch (o.source_ea.mode) {
  case M68kEaMode::data_register: return 12U;
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc: return 16U;
  case M68kEaMode::address_predec: return 18U;
  case M68kEaMode::address_disp16:
  case M68kEaMode::pc_disp16:
  case M68kEaMode::absolute_word: return 20U;
  case M68kEaMode::address_index8:
  case M68kEaMode::pc_index8: return 22U;
  case M68kEaMode::absolute_long: return 24U;
  case M68kEaMode::immediate: return 16U;
  default: return std::nullopt;
  }
}

// Table 8-13 MOVE SR,<ea> destination rows.  Its Dn row is also distinct
// from the generic four-cycle instruction row.
std::optional<std::uint32_t> move_from_status_cycles(const M68kIrOperation &o) noexcept {
  switch (o.destination_ea.mode) {
  case M68kEaMode::data_register: return 6U;
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc: return 12U;
  case M68kEaMode::address_predec: return 14U;
  case M68kEaMode::address_disp16:
  case M68kEaMode::absolute_word: return 16U;
  case M68kEaMode::address_index8: return 18U;
  case M68kEaMode::absolute_long: return 20U;
  default: return std::nullopt;
  }
}

// Table 8-6 CLR/NEG/NOT literal rows.  TST is a distinct read-only row.
std::optional<std::uint32_t> single_operand_cycles(const M68kIrOperation &o) noexcept {
  if (o.destination_ea.mode == M68kEaMode::data_register)
    return o.size == M68kMemoryAccessWidth::long_word ? 6U : 4U;
  const auto value = ea(o.destination_ea, o.size);
  if (!value || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  return (o.size == M68kMemoryAccessWidth::long_word ? 12U : 8U) + *value;
}

std::optional<std::uint32_t> test_cycles(const M68kIrOperation &o) noexcept {
  const auto value = ea(o.source_ea, o.size);
  if (!value || (!memory_ea(o.source_ea.mode) && o.source_ea.mode != M68kEaMode::data_register)) return std::nullopt;
  // Table 8-6: Dn is four clocks at every size; memory is exactly its table
  // 8-1 cell, rather than the read-modify-write unary row.
  return o.source_ea.mode == M68kEaMode::data_register ? 4U : *value;
}

std::optional<std::uint32_t> logical_cycles(const M68kIrOperation &o) noexcept {
  const bool long_size = o.size == M68kMemoryAccessWidth::long_word;
  if (o.destination_ea.mode == M68kEaMode::data_register) {
    const auto source = ea(o.source_ea, o.size);
    if (!source) return std::nullopt;
    const bool direct_or_immediate = o.source_ea.mode == M68kEaMode::data_register ||
                                     o.source_ea.mode == M68kEaMode::immediate;
    return (long_size ? (direct_or_immediate ? 8U : 6U) : 4U) + *source;
  }
  const auto destination = ea(o.destination_ea, o.size);
  if (!destination || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  return (long_size ? 12U : 8U) + *destination;
}

std::optional<std::uint32_t> logical_immediate_cycles(const M68kIrOperation &o) noexcept {
  const bool long_size = o.size == M68kMemoryAccessWidth::long_word;
  if (o.destination_ea.mode == M68kEaMode::data_register) return long_size ? 16U : 8U;
  const auto destination = ea(o.destination_ea, o.size);
  if (!destination || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  return (long_size ? 20U : 12U) + *destination;
}

std::optional<std::uint32_t> compare_immediate_cycles(const M68kIrOperation &o) noexcept {
  if (o.destination_ea.mode == M68kEaMode::data_register)
    return o.size == M68kMemoryAccessWidth::long_word ? 14U : 8U;
  const auto destination = ea(o.destination_ea, o.size);
  if (!destination || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  // Table 8-5's CMPI memory row is distinct from ADDI/SUBI/ANDI/ORI/EORI.
  return (o.size == M68kMemoryAccessWidth::long_word ? 12U : 8U) + *destination;
}

std::optional<std::uint32_t> bit_cycles(const M68kIrOperation &o, bool modifies) noexcept {
  const bool immediate_selector = o.source_ea.mode == M68kEaMode::immediate;
  if (o.destination_ea.mode == M68kEaMode::data_register)
    return (modifies ? 8U : 6U) + (immediate_selector ? 4U : 0U);
  const auto target = ea(o.destination_ea, M68kMemoryAccessWidth::word);
  if (!target || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  // Table 8-8: BTST's memory row is 4 + word EA; BCHG/BCLR/BSET's is
  // 8 + word EA.  The immediate selector adds its own four-clock cell.
  return (modifies ? 8U : 4U) + (immediate_selector ? 4U : 0U) + *target;
}

std::optional<std::uint32_t> addq_subq_cycles(const M68kIrOperation &o) noexcept {
  if (o.destination_ea.mode == M68kEaMode::data_register || o.destination_ea.mode == M68kEaMode::address_register)
    return o.size == M68kMemoryAccessWidth::long_word && o.destination_ea.mode == M68kEaMode::data_register ? 8U : 4U + (o.destination_ea.mode == M68kEaMode::address_register ? 4U : 0U);
  const auto value = ea(o.destination_ea, o.size);
  if (!value || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  return (o.size == M68kMemoryAccessWidth::long_word ? 12U : 8U) + *value;
}

std::optional<std::uint32_t> adda_suba_cycles(const M68kIrOperation &o) noexcept {
  const auto value = ea(o.source_ea, o.size);
  if (!value) return std::nullopt;
  if (o.size != M68kMemoryAccessWidth::long_word) return 8U + *value;
  // Table 8-4's long row has direct-register and immediate literal cells;
  // neither is represented by its otherwise six-clock base plus table 8-1.
  if (o.source_ea.mode == M68kEaMode::data_register || o.source_ea.mode == M68kEaMode::address_register) return 8U;
  if (o.source_ea.mode == M68kEaMode::immediate) return 16U;
  return 6U + *value;
}

// Table 8-4 (ADDX/SUBX/CMPM rows): Dy,Dx is 4 (B/W) / 8 (L); -(Ay),-(Ax) is 18 / 30; CMPM (Ay)+,(Ax)+ is
// 12 / 20. All are literal totals independent of any other EA cell.
std::optional<std::uint32_t> extended_pair_cycles(const M68kIrOperation &o) noexcept {
  const bool long_size = o.size == M68kMemoryAccessWidth::long_word;
  if (o.kind == M68kIrKind::compare_memory) return long_size ? 20U : 12U;
  if (o.source_ea.mode == M68kEaMode::data_register) return long_size ? 8U : 4U;
  if (o.source_ea.mode == M68kEaMode::address_predec) return long_size ? 30U : 18U;
  return std::nullopt;
}

std::optional<std::uint32_t> lea_cycles(const M68kIrOperation &o) noexcept {
  const auto value = ea(o.source_ea, M68kMemoryAccessWidth::word);
  if (!value || !memory_ea(o.source_ea.mode)) return std::nullopt;
  return *value;
}

std::optional<std::uint32_t> pea_cycles(const M68kIrOperation &o) noexcept {
  const auto value = ea(o.source_ea, M68kMemoryAccessWidth::word);
  if (!value || !memory_ea(o.source_ea.mode)) return std::nullopt;
  return 8U + *value;
}

} // namespace

std::optional<std::uint32_t> m68k_instruction_cycles(const M68kIrOperation &operation) noexcept {
  // Only forms whose complete timing does not depend on EA, data, mask, count,
  // or branch outcome are admitted until the descriptor/codegen-expression
  // contract exists. Do not turn a partial table into a false timing claim.
  switch (operation.kind) {
  case M68kIrKind::write_moveq: return 4U;
  case M68kIrKind::no_operation: return 4U;
  case M68kIrKind::return_from_exception: return 20U;
  case M68kIrKind::return_from_subroutine: return 16U;
  case M68kIrKind::branch_always_short: return 10U;
  case M68kIrKind::general_branch:
    return operation.condition == M68kCondition::always ? std::optional<std::uint32_t>(10U) : std::nullopt;
  case M68kIrKind::bsr_call: return 18U;
  case M68kIrKind::jump_general:
    switch (operation.source_ea.mode) {
    case M68kEaMode::address_indirect: return 8U;
    case M68kEaMode::address_disp16: case M68kEaMode::pc_disp16: case M68kEaMode::absolute_word: return 10U;
    case M68kEaMode::address_index8: case M68kEaMode::pc_index8: return 14U;
    case M68kEaMode::absolute_long: return 12U;
    default: return std::nullopt;
    }
  case M68kIrKind::call_general:
    switch (operation.source_ea.mode) {
    case M68kEaMode::address_indirect: return 16U;
    case M68kEaMode::address_disp16: case M68kEaMode::pc_disp16: case M68kEaMode::absolute_word: return 18U;
    case M68kEaMode::address_index8: case M68kEaMode::pc_index8: return 22U;
    case M68kEaMode::absolute_long: return 20U;
    default: return std::nullopt;
    }
  case M68kIrKind::write_move:
    return move_cycles(operation);
  case M68kIrKind::write_movea:
    return source_alu(operation, 4U, 4U);
  case M68kIrKind::compare:
    return compare_cycles(operation);
  case M68kIrKind::test_operand:
    return test_cycles(operation);
  case M68kIrKind::bit_test: return bit_cycles(operation, false);
  case M68kIrKind::compare_address:
    return cmpa_cycles(operation);
  case M68kIrKind::add:
  case M68kIrKind::subtract:
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_or:
  case M68kIrKind::exclusive_or:
    return logical_cycles(operation);
  case M68kIrKind::add_address:
  case M68kIrKind::subtract_address:
    return adda_suba_cycles(operation);
  case M68kIrKind::add_quick:
  case M68kIrKind::subtract_quick:
    return addq_subq_cycles(operation);
  case M68kIrKind::add_immediate:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or_immediate:
    return logical_immediate_cycles(operation);
  case M68kIrKind::compare_immediate:
    return compare_immediate_cycles(operation);
  case M68kIrKind::write_clr:
  case M68kIrKind::logical_not:
  case M68kIrKind::negate_word:
  case M68kIrKind::negate_extended:
  case M68kIrKind::shift_rotate_memory:
    return single_operand_cycles(operation);
  case M68kIrKind::add_extended:
  case M68kIrKind::subtract_extended:
  case M68kIrKind::compare_memory:
    return extended_pair_cycles(operation);
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set:
    return bit_cycles(operation, true);
  case M68kIrKind::load_effective_address:
    return lea_cycles(operation);
  case M68kIrKind::push_effective_address:
    return pea_cycles(operation);
  case M68kIrKind::link_frame: return 16U;
  case M68kIrKind::unlink_frame: return 12U;
  case M68kIrKind::movem_transfer: {
    const auto &operand = operation.movem_direction == M68kMovemDirection::registers_to_memory
                              ? operation.destination_ea : operation.source_ea;
    if (!memory_ea(operand.mode)) return std::nullopt;
    const auto words_per_register = operation.size == M68kMemoryAccessWidth::long_word ? 2U : 1U;
    // Table 8-10's literal MOVEM bases include the predecrement exception.
    std::optional<std::uint32_t> base;
    if (operation.movem_direction == M68kMovemDirection::registers_to_memory) {
      switch (operand.mode) {
      case M68kEaMode::address_indirect:
      case M68kEaMode::address_predec: base = 8U; break;
      case M68kEaMode::address_disp16:
      case M68kEaMode::absolute_word: base = 12U; break;
      case M68kEaMode::address_index8: base = 14U; break;
      case M68kEaMode::absolute_long: base = 16U; break;
      default: return std::nullopt;
      }
    } else {
      switch (operand.mode) {
      case M68kEaMode::address_indirect:
      case M68kEaMode::address_postinc: base = 12U; break;
      case M68kEaMode::address_disp16:
      case M68kEaMode::pc_disp16:
      case M68kEaMode::absolute_word: base = 16U; break;
      // SEG-021-T012: Table 8-10 "MOVEM Instruction Execution Times"
      // (Motorola M68000 8-/16-/32-Bit Microprocessor User's Manual)
      // publishes a literal `(d8,PC,Xn)` row for the memory->register
      // direction, identical to the `(d8,An,Xn)` row's base of 18 cycles --
      // the exact same An-relative/PC-relative pairing this same switch
      // already applies one row above for `d16(An)`/`d16(PC)` (both 16).
      case M68kEaMode::address_index8:
      case M68kEaMode::pc_index8: base = 18U; break;
      case M68kEaMode::absolute_long: base = 20U; break;
      default: return std::nullopt;
      }
    }
    return *base + 4U * words_per_register * popcount(operation.movem_register_mask);
  }
  case M68kIrKind::subtract_quick_long_d0: return 8U;
  case M68kIrKind::write_swap:
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long:
  case M68kIrKind::write_user_stack_pointer:
    return 4U;
  case M68kIrKind::write_status_register:
  case M68kIrKind::write_condition_codes:
    return move_to_status_cycles(operation);
  case M68kIrKind::read_status_register:
    return move_from_status_cycles(operation);
  // These instruction tables depend on runtime CCR, register count, or
  // operand value. They require a generated timing expression, not a guessed
  // scalar, so translation fails closed for now.
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::dbcc_loop:
  case M68kIrKind::shift_rotate_register:
    return std::nullopt;
  case M68kIrKind::multiply_signed_word:
  case M68kIrKind::multiply_unsigned_word:
    return std::nullopt;
  case M68kIrKind::divide_signed_word:
    if (const auto value = ea(operation.source_ea, M68kMemoryAccessWidth::word)) return 158U + *value;
    return std::nullopt;
  case M68kIrKind::divide_unsigned_word:
    if (const auto value = ea(operation.source_ea, M68kMemoryAccessWidth::word)) return 140U + *value;
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> m68k_effective_address_cycles(
    const M68kEffectiveAddress &effective_address, M68kMemoryAccessWidth size) noexcept {
  return ea_cycles(effective_address, size);
}

}  // namespace segarecomp
