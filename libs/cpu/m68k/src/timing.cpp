#include "segarecomp/cpu/m68k/timing.hpp"

#include "segarecomp/cpu/m68k/effects.hpp"
#include "segarecomp/cpu/m68k/timing_core.h"

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
  // SEG-021-T022 (audit of the SEG-021-T005 row): Tables 8-2/8-3's -(An) DESTINATION column equals the (An) column
  // (the predecrement overlaps the write; e.g. MOVE.W Dn,-(An) is 8, MOVE.L Dn,-(An) 12), unlike the -(An) SOURCE
  // cell, which does cost the extra 2. The pinned Musashi core agrees.
  M68kEffectiveAddress destination = o.destination_ea;
  if (destination.mode == M68kEaMode::address_predec) destination.mode = M68kEaMode::address_indirect;
  const auto destination_ea = ea_cycles(destination, o.size);
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
// Table 8-4 CMPA: 6(1/0) + the size's EA column for both word and long (SEG-021-T022 audit: the former word
// row of 8 + EA was not the published row; the pinned Musashi core also reports 6 + EA).
std::optional<std::uint32_t> cmpa_cycles(const M68kIrOperation &o) noexcept {
  const auto value = ea(o.source_ea, o.size);
  if (!value) return std::nullopt;
  return 6U + *value;
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
  // Table 8-6 TST: Dn 4(1/0); memory 4(1/0) + the table 8-1 cell (the read-only row, not the read-modify-write
  // unary row). SEG-021-T022 audit: the former memory value omitted the 4-clock base; the pinned Musashi core
  // reports 4 + EA.
  return o.source_ea.mode == M68kEaMode::data_register ? 4U : 4U + *value;
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
  // Table 8-8 register rows (published maxima): BTST 6 / 10, BCHG and BSET 8 / 12, BCLR 10 / 14 (dynamic / static).
  // SEG-021-T022 audit: BCLR's register row is 2 clocks above BCHG/BSET's; the pinned Musashi core agrees.
  if (o.destination_ea.mode == M68kEaMode::data_register)
    return (modifies ? (o.kind == M68kIrKind::bit_clear ? 10U : 8U) : 6U) + (immediate_selector ? 4U : 0U);
  // SEG-021-T022: BTST Dn,#<data> (the only bit form whose destination may be immediate) is Table 8-8's dynamic
  // BTST memory row, 4 + the Table 8-1 #<data> byte/word cell (4) = 8 (1/0 + 1/0).
  if (!modifies && !immediate_selector && o.destination_ea.mode == M68kEaMode::immediate)
    return 4U + *ea(o.destination_ea, M68kMemoryAccessWidth::byte);
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

// Table 8-4 (ABCD/SBCD rows, byte only): Dy,Dx is 6; -(Ay),-(Ax) is 18.
std::optional<std::uint32_t> decimal_pair_cycles(const M68kIrOperation &o) noexcept {
  if (o.source_ea.mode == M68kEaMode::data_register) return 6U;
  if (o.source_ea.mode == M68kEaMode::address_predec) return 18U;
  return std::nullopt;
}

// Table 8-6 (NBCD row): Dn is 6; memory is 8 + the byte EA calculation time. Anything else has no row.
std::optional<std::uint32_t> negate_decimal_cycles(const M68kIrOperation &o) noexcept {
  if (o.destination_ea.mode == M68kEaMode::data_register) return 6U;
  const auto value = ea(o.destination_ea, M68kMemoryAccessWidth::byte);
  if (!value || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  return 8U + *value;
}

// Table 8-6 (TAS row, byte only): Dn is 4; memory is 10 + the byte EA calculation time. The published row includes the
// indivisible read-modify-write bus cycle, which stays platform-owned; the CPU-visible total is the same.
std::optional<std::uint32_t> test_and_set_cycles(const M68kIrOperation &o) noexcept {
  if (o.destination_ea.mode == M68kEaMode::data_register) return 4U;
  const auto value = ea(o.destination_ea, M68kMemoryAccessWidth::byte);
  if (!value || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  return 10U + *value;
}

// Table 8-6 (Scc row, byte only): memory is 8 + the byte EA calculation time regardless of the condition. The Dn row is
// condition dependent (4 false / 6 true) and therefore has no static scalar: it is handled by the generated retirement
// expression, and this function returns no static row for it.
std::optional<std::uint32_t> set_conditional_cycles(const M68kIrOperation &o) noexcept {
  if (o.destination_ea.mode == M68kEaMode::data_register) return std::nullopt;
  const auto value = ea(o.destination_ea, M68kMemoryAccessWidth::byte);
  if (!value || !memory_ea(o.destination_ea.mode)) return std::nullopt;
  return 8U + *value;
}

// Table 8-10 LEA / PEA literal rows: (An) 4 / 12, d16 8 / 16, (d8,An,Xn) 12 / 20, abs.W 8 / 16, abs.L 12 / 20,
// d16(PC) 8 / 16, (d8,PC,Xn) 12 / 20. They follow the table 8-1 word cells except the indexed rows, which are 2
// clocks longer (SEG-021-T022 audit: the former indexed values 10 / 18 were not the published rows; the pinned
// Musashi core reports 12 / 20).
std::optional<std::uint32_t> control_address_cycles(const M68kIrOperation &o) noexcept {
  const auto value = ea(o.source_ea, M68kMemoryAccessWidth::word);
  if (!value || !memory_ea(o.source_ea.mode)) return std::nullopt;
  const bool indexed = o.source_ea.mode == M68kEaMode::address_index8 || o.source_ea.mode == M68kEaMode::pc_index8;
  return *value + (indexed ? 2U : 0U);
}

std::optional<std::uint32_t> lea_cycles(const M68kIrOperation &o) noexcept { return control_address_cycles(o); }

std::optional<std::uint32_t> pea_cycles(const M68kIrOperation &o) noexcept {
  const auto value = control_address_cycles(o);
  return value ? std::optional<std::uint32_t>(8U + *value) : std::nullopt;
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
  case M68kIrKind::add_decimal:
  case M68kIrKind::subtract_decimal:
    return decimal_pair_cycles(operation);
  case M68kIrKind::negate_decimal:
    return negate_decimal_cycles(operation);
  case M68kIrKind::bit_change:
  case M68kIrKind::bit_clear:
  case M68kIrKind::bit_set:
    return bit_cycles(operation, true);
  // SEG-021-T016: Table 8-4 EXG is 6; Table 8-3 MOVEP is 16 (word) / 24 (long) in either direction; TAS and Scc above.
  case M68kIrKind::exchange_registers: return 6U;
  case M68kIrKind::movep_transfer: return operation.size == M68kMemoryAccessWidth::long_word ? 24U : 16U;
  case M68kIrKind::test_and_set: return test_and_set_cycles(operation);
  case M68kIrKind::set_conditional: return set_conditional_cycles(operation);
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
  case M68kIrKind::read_user_stack_pointer:  // SEG-021-T018: Table 8-11 MOVE USP is 4 in either direction
    return 4U;
  // SEG-021-T018: Table 8-11 ANDI/EORI/ORI to CCR and to SR are all 20 (3/0).
  case M68kIrKind::logical_immediate_to_ccr:
  case M68kIrKind::logical_immediate_to_sr:
    return 20U;
  // SEG-021-T020: Table 8-11 STOP is 4 (0/0) -- the retirement of the instruction itself; the halted interval that
  // follows is machine scheduler time (ADR 0041), not an instruction cost.
  case M68kIrKind::stop_until_interrupt: return 4U;
  case M68kIrKind::write_status_register:
  case M68kIrKind::write_condition_codes:
    return move_to_status_cycles(operation);
  case M68kIrKind::read_status_register:
    return move_from_status_cycles(operation);
  // These instruction tables depend on runtime CCR, register count, or
  // operand value: no complete scalar exists. SEG-021-T021: the Bcc/DBcc/
  // shift-rotate outcome rules are owned by `m68k_instruction_timing` below;
  // the direct_flow-profile-only BNE.S compatibility kind has no generated
  // outcome hook and stays fail-closed.
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::dbcc_loop:
  case M68kIrKind::shift_rotate_register:
    return std::nullopt;
  // SEG-021-T022: MULU/MULS (38 + 2n) and DIVU/DIVS (exact data-dependent count; Table 8-4 publishes only the
  // < 140 / < 158 bounds) depend on the operand values: their rules are owned by `m68k_instruction_timing`.
  case M68kIrKind::multiply_signed_word:
  case M68kIrKind::multiply_unsigned_word:
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word:
    return std::nullopt;
  // SEG-021-T019 (MC68000 User's Manual Tables 8-11 and 8-14). RTR is 20 (5/0). TRAP #n and the
  // instruction-word exceptions (ILLEGAL, line 1010/1111, every other illegal word) are 34 (4/3); these forms ALWAYS
  // take the exception, whose entry cost is charged by the exception-entry timing owner (ADR 0043 §8, SEG-021-T022),
  // never by an instruction retirement. TRAPV is 4 (1/0) when V = 0 (the only path that retires; V = 1 takes the
  // vector-7 entry, 34). CHK.W is 10 (1/0) + the word EA cell when the bound check passes (the only path that
  // retires; a failed check takes the vector-6 entry, 40 + EA).
  case M68kIrKind::return_restore_condition_codes: return 20U;
  case M68kIrKind::trap_exception:
  case M68kIrKind::instruction_exception:
    return 34U;
  case M68kIrKind::trap_on_overflow: return 4U;
  case M68kIrKind::check_bounds:
    if (const auto value = ea(operation.source_ea, M68kMemoryAccessWidth::word)) return 10U + *value;
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> m68k_exception_entry_cycles(const M68kIrOperation &operation) noexcept {
  const auto effect = m68k_operation_effect(operation);
  if (!effect.may_raise_synchronous_exception) return std::nullopt;
  const auto base = segarecomp_m68k_exception_entry_cycles(effect.exception_vector);
  if (base == 0U) return std::nullopt;
  // The zero-divide and CHK rows are "+ EA": the <ea> word operand is fetched before the exception is recognized.
  if (effect.exception_vector == 5U || effect.exception_vector == 6U) {
    const auto value = ea(operation.source_ea, M68kMemoryAccessWidth::word);
    if (!value) return std::nullopt;
    return base + *value;
  }
  return base;
}

namespace {

std::optional<M68kInstructionTiming> retirement_timing(const M68kIrOperation &operation) noexcept {
  if (const auto cycles = m68k_instruction_cycles(operation)) {
    M68kInstructionTiming timing{};
    timing.cycles = *cycles;
    return timing;
  }
  M68kInstructionTiming timing{};
  switch (operation.kind) {
  // Table 8-10 Bcc: taken 10 (2/0) for either displacement size; not taken 8 (1/0) for the byte
  // displacement and 12 (2/0) for the word displacement (the extension word is still fetched).
  case M68kIrKind::general_branch:
    if (operation.condition == M68kCondition::always) return std::nullopt;  // BRA is the static row above
    if (operation.size != M68kMemoryAccessWidth::byte && operation.size != M68kMemoryAccessWidth::word)
      return std::nullopt;
    timing.rule = M68kTimingRule::condition;
    timing.cycles = 10U;
    timing.false_cycles = operation.size == M68kMemoryAccessWidth::byte ? 8U : 12U;
    return timing;
  // Table 8-10 DBcc: condition true 12 (2/0); condition false, counter not expired (branch taken) 10 (2/0);
  // condition false, counter expired 14 (3/0).
  case M68kIrKind::dbcc_loop:
    timing.rule = M68kTimingRule::dbcc;
    timing.cycles = 12U;
    timing.false_cycles = 10U;
    timing.expired_cycles = 14U;
    return timing;
  // Table 8-6 Scc: the Dn row is 6 when the condition is true and 4 when it is false (memory rows are static).
  case M68kIrKind::set_conditional:
    if (operation.destination_ea.mode != M68kEaMode::data_register) return std::nullopt;
    timing.rule = M68kTimingRule::condition;
    timing.cycles = 6U;
    timing.false_cycles = 4U;
    return timing;
  // Table 8-9 register shift/rotate (ASd, LSd, ROd, ROXd): byte/word 6 + 2n, long 8 + 2n.
  case M68kIrKind::shift_rotate_register:
    timing.rule = M68kTimingRule::register_count;
    timing.cycles = operation.size == M68kMemoryAccessWidth::long_word ? 8U : 6U;
    timing.per_count_cycles = 2U;
    return timing;
  // SEG-021-T022: Table 8-4 MULU.W/MULS.W 38 + 2n (+ the word EA cell held in `cycles`); DIVU.W/DIVS.W the exact
  // count of timing_core.h (+ the word EA cell). The operand-dependent part is evaluated by timing_core.h.
  case M68kIrKind::multiply_unsigned_word:
  case M68kIrKind::multiply_signed_word:
  case M68kIrKind::divide_unsigned_word:
  case M68kIrKind::divide_signed_word: {
    const auto value = ea(operation.source_ea, M68kMemoryAccessWidth::word);
    if (!value) return std::nullopt;
    timing.rule = operation.kind == M68kIrKind::multiply_unsigned_word ? M68kTimingRule::multiply_unsigned
                  : operation.kind == M68kIrKind::multiply_signed_word ? M68kTimingRule::multiply_signed
                  : operation.kind == M68kIrKind::divide_unsigned_word ? M68kTimingRule::divide_unsigned
                                                                         : M68kTimingRule::divide_signed;
    timing.cycles = *value;
    return timing;
  }
  default: return std::nullopt;
  }
}

}  // namespace

std::optional<M68kInstructionTiming> m68k_instruction_timing(const M68kIrOperation &operation) noexcept {
  auto timing = retirement_timing(operation);
  if (!timing) return std::nullopt;
  const auto effect = m68k_operation_effect(operation);
  if (effect.may_raise_synchronous_exception) {
    // A form that can take an exception but whose entry has no published count stays fail-closed.
    const auto entry = m68k_exception_entry_cycles(operation);
    if (!entry) return std::nullopt;
    timing->exception_entry_cycles = *entry;
  }
  return timing;
}

std::uint32_t m68k_timing_cycles(const M68kInstructionTiming &timing, const M68kTimingOutcome &outcome) noexcept {
  if (outcome.exception_taken && timing.exception_entry_cycles != 0U) return timing.exception_entry_cycles;
  switch (timing.rule) {
  case M68kTimingRule::fixed: return timing.cycles;
  case M68kTimingRule::condition: return outcome.condition_true ? timing.cycles : timing.false_cycles;
  case M68kTimingRule::dbcc:
    return outcome.condition_true ? timing.cycles : (outcome.counter_expired ? timing.expired_cycles : timing.false_cycles);
  case M68kTimingRule::register_count: return timing.cycles + timing.per_count_cycles * outcome.count;
  case M68kTimingRule::multiply_unsigned: return segarecomp_m68k_mulu_word_cycles(outcome.source_word) + timing.cycles;
  case M68kTimingRule::multiply_signed: return segarecomp_m68k_muls_word_cycles(outcome.source_word) + timing.cycles;
  case M68kTimingRule::divide_unsigned:
    return segarecomp_m68k_divu_word_cycles(outcome.dividend, outcome.source_word) + timing.cycles;
  case M68kTimingRule::divide_signed:
    return segarecomp_m68k_divs_word_cycles(outcome.dividend, outcome.source_word) + timing.cycles;
  }
  return timing.cycles;
}

std::optional<std::uint32_t> m68k_effective_address_cycles(
    const M68kEffectiveAddress &effective_address, M68kMemoryAccessWidth size) noexcept {
  return ea_cycles(effective_address, size);
}

}  // namespace segarecomp
