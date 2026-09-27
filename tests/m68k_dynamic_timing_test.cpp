// SEG-021-T021: the CPU-owned outcome-dependent MC68000 timing rules (Bcc, DBcc, Scc Dn, register
// shift/rotate), checked row by row against the published MC68000 User's Manual section 8 tables, and the
// kinds that must still fail closed. SEG-021-T022: MULU/MULS (Table 8-4, every source word against a test-owned
// transcription), the exact DIVU/DIVS count, BTST Dn,#<data>, and the exception-entry times (the MC68000
// exception-processing table) of every synchronous exception path. The generated-side evaluation of the same rules is cross-checked against
// the pinned Musashi core by the conformance harness (tools/m68k_conformance.py, `timing` rows).
#include "segarecomp/cpu/m68k/timing.hpp"
#include "segarecomp/cpu/m68k/timing_core.h"

#include <cstdio>
#include <cstdlib>

using namespace segarecomp;

namespace {
int failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

M68kIrOperation operation(M68kIrKind kind, M68kMemoryAccessWidth size) {
  M68kIrOperation value{};
  value.kind = kind;
  value.size = size;
  return value;
}

std::uint32_t cycles(const M68kIrOperation &op, bool condition_true, bool expired = false, std::uint32_t count = 0U) {
  const auto timing = m68k_instruction_timing(op);
  if (!timing) std::abort();
  M68kTimingOutcome outcome{};
  outcome.condition_true = condition_true;
  outcome.counter_expired = expired;
  outcome.count = count;
  return m68k_timing_cycles(*timing, outcome);
}

std::uint32_t data_cycles(const M68kIrOperation &op, std::uint16_t source, std::uint32_t dividend = 0U,
                          bool exception_taken = false) {
  const auto timing = m68k_instruction_timing(op);
  if (!timing) std::abort();
  M68kTimingOutcome outcome{};
  outcome.source_word = source;
  outcome.dividend = dividend;
  outcome.exception_taken = exception_taken;
  return m68k_timing_cycles(*timing, outcome);
}

// Test-owned transcriptions of Table 8-4's n: MULU counts the 1 bits; MULS counts the 01/10 pairs of the 17-bit
// value <source word>:0, walking the pairs explicitly.
std::uint32_t mulu_n(std::uint32_t source) {
  std::uint32_t n = 0U;
  for (std::uint32_t bit = 0U; bit < 16U; ++bit) n += (source >> bit) & 1U;
  return n;
}
std::uint32_t muls_n(std::uint32_t source) {
  std::uint32_t n = 0U;
  std::uint32_t previous = 0U;  // the appended 0 below bit 0
  for (std::uint32_t bit = 0U; bit < 16U; ++bit) {
    const std::uint32_t current = (source >> bit) & 1U;
    n += current != previous ? 1U : 0U;
    previous = current;
  }
  return n;
}

M68kIrOperation moveq_like() {
  M68kIrOperation value{};
  value.kind = M68kIrKind::write_moveq;
  value.size = M68kMemoryAccessWidth::long_word;
  return value;
}

M68kIrOperation with_source(M68kIrKind kind, M68kEaMode mode) {
  auto op = operation(kind, M68kMemoryAccessWidth::word);
  op.source_ea.mode = mode;
  op.destination_ea.mode = M68kEaMode::data_register;
  return op;
}
}  // namespace

int main() {
  // Table 8-10 Bcc: taken 10 for both displacement sizes; not taken 8 (byte) / 12 (word).
  constexpr M68kCondition conditions[] = {
      M68kCondition::hi, M68kCondition::ls, M68kCondition::cc, M68kCondition::cs, M68kCondition::ne,
      M68kCondition::eq, M68kCondition::vc, M68kCondition::vs, M68kCondition::pl, M68kCondition::mi,
      M68kCondition::ge, M68kCondition::lt, M68kCondition::gt, M68kCondition::le};
  for (const auto condition : conditions) {
    for (const auto size : {M68kMemoryAccessWidth::byte, M68kMemoryAccessWidth::word}) {
      auto bcc = operation(M68kIrKind::general_branch, size);
      bcc.condition = condition;
      expect(!m68k_instruction_cycles(bcc), "Bcc has no complete static scalar");
      expect(m68k_instruction_timing(bcc)->rule == M68kTimingRule::condition, "Bcc is a condition rule");
      expect(cycles(bcc, true) == 10U, "Bcc taken is 10");
      expect(cycles(bcc, false) == (size == M68kMemoryAccessWidth::byte ? 8U : 12U), "Bcc not taken is 8/12");
    }
  }
  // BRA stays the static 10-cycle row.
  auto bra = operation(M68kIrKind::general_branch, M68kMemoryAccessWidth::word);
  bra.condition = M68kCondition::always;
  expect(m68k_instruction_timing(bra)->rule == M68kTimingRule::fixed && m68k_instruction_timing(bra)->cycles == 10U,
         "BRA is fixed 10");

  // Table 8-10 DBcc: condition true 12; false and expired 14; false and branch taken 10.
  auto dbcc = operation(M68kIrKind::dbcc_loop, M68kMemoryAccessWidth::word);
  expect(!m68k_instruction_cycles(dbcc), "DBcc has no complete static scalar");
  expect(cycles(dbcc, true, false) == 12U && cycles(dbcc, true, true) == 12U, "DBcc condition true is 12");
  expect(cycles(dbcc, false, true) == 14U, "DBcc counter expired is 14");
  expect(cycles(dbcc, false, false) == 10U, "DBcc branch taken is 10");

  // Table 8-6 Scc Dn: 6 true / 4 false; the memory rows stay static (8 + byte EA).
  auto scc = operation(M68kIrKind::set_conditional, M68kMemoryAccessWidth::byte);
  scc.destination_ea.mode = M68kEaMode::data_register;
  expect(cycles(scc, true) == 6U && cycles(scc, false) == 4U, "Scc Dn is 6/4");
  scc.destination_ea.mode = M68kEaMode::address_indirect;
  expect(m68k_instruction_timing(scc)->rule == M68kTimingRule::fixed && m68k_instruction_timing(scc)->cycles == 12U,
         "Scc (An) is fixed 12");

  // Table 8-9 register shift/rotate: byte/word 6 + 2n, long 8 + 2n, n = immediate 1..8 or Dn mod 64
  // (count 0 legal). The same row holds for every family, ROXL/ROXR included.
  for (const auto size : {M68kMemoryAccessWidth::byte, M68kMemoryAccessWidth::word, M68kMemoryAccessWidth::long_word}) {
    auto shift = operation(M68kIrKind::shift_rotate_register, size);
    shift.source_ea.mode = M68kEaMode::data_register;
    shift.destination_ea.mode = M68kEaMode::data_register;
    const std::uint32_t base = size == M68kMemoryAccessWidth::long_word ? 8U : 6U;
    expect(!m68k_instruction_cycles(shift), "register shift has no complete static scalar");
    for (const std::uint32_t n : {0U, 1U, 8U, 63U})
      expect(cycles(shift, false, false, n) == base + 2U * n, "register shift is base + 2n");
  }
  // Memory shifts are the static Table 8-9 memory row: 8 + word EA.
  auto memory_shift = operation(M68kIrKind::shift_rotate_memory, M68kMemoryAccessWidth::word);
  memory_shift.destination_ea.mode = M68kEaMode::address_indirect;
  expect(m68k_instruction_cycles(memory_shift) == 12U, "memory shift (An) is 12");

  // SEG-021-T022 Table 8-4 MULU/MULS: 38 + 2n + the word EA cell, for every source word.
  const auto mulu = with_source(M68kIrKind::multiply_unsigned_word, M68kEaMode::data_register);
  const auto muls = with_source(M68kIrKind::multiply_signed_word, M68kEaMode::data_register);
  expect(!m68k_instruction_cycles(mulu) && !m68k_instruction_cycles(muls), "MUL has no complete static scalar");
  expect(m68k_instruction_timing(mulu)->rule == M68kTimingRule::multiply_unsigned &&
             m68k_instruction_timing(muls)->rule == M68kTimingRule::multiply_signed,
         "MULU/MULS are multiply rules");
  bool mul_all = true;
  for (std::uint32_t source = 0U; source <= 0xFFFFU; ++source) {
    mul_all &= data_cycles(mulu, static_cast<std::uint16_t>(source)) == 38U + 2U * mulu_n(source);
    mul_all &= data_cycles(muls, static_cast<std::uint16_t>(source)) == 38U + 2U * muls_n(source);
  }
  expect(mul_all, "MULU/MULS match the published n for all 65,536 source words");
  expect(data_cycles(mulu, 0x0000U) == 38U && data_cycles(mulu, 0xFFFFU) == 70U && data_cycles(mulu, 0x5555U) == 54U &&
             data_cycles(mulu, 0xAAAAU) == 54U,
         "MULU published points (n = 0, 16, 8, 8)");
  expect(data_cycles(muls, 0x0000U) == 38U && data_cycles(muls, 0xFFFFU) == 40U && data_cycles(muls, 0x5555U) == 70U &&
             data_cycles(muls, 0xAAAAU) == 68U && data_cycles(muls, 0x0001U) == 42U && data_cycles(muls, 0x8000U) == 40U,
         "MULS published points (n = 0, 1, 16, 15, 2, 1)");
  expect(data_cycles(with_source(M68kIrKind::multiply_unsigned_word, M68kEaMode::address_indirect), 0x0003U) == 46U &&
             data_cycles(with_source(M68kIrKind::multiply_signed_word, M68kEaMode::absolute_long), 0x0000U) == 50U &&
             data_cycles(with_source(M68kIrKind::multiply_unsigned_word, M68kEaMode::immediate), 0x0000U) == 42U,
         "MUL adds the Table 8-1 word EA cell");

  // SEG-021-T022 DIVU/DIVS: the exact data-dependent count (Table 8-4 bounds < 140 / < 158) + the word EA cell;
  // values computed independently by tests/m68k_muldiv_auto_update_generated_test.py's transcription.
  const auto divu = with_source(M68kIrKind::divide_unsigned_word, M68kEaMode::data_register);
  const auto divs = with_source(M68kIrKind::divide_signed_word, M68kEaMode::data_register);
  expect(!m68k_instruction_cycles(divu) && !m68k_instruction_cycles(divs), "DIV has no static worst-case scalar any more");
  expect(m68k_instruction_timing(divu)->rule == M68kTimingRule::divide_unsigned &&
             m68k_instruction_timing(divs)->rule == M68kTimingRule::divide_signed,
         "DIVU/DIVS are divide rules");
  struct DivCase { std::uint32_t dividend; std::uint16_t divisor; std::uint32_t divu, divs; };
  constexpr DivCase div_cases[] = {{100U, 5U, 132U, 146U},           {0x0000FFFFU, 1U, 106U, 120U},
                                   {0x00010000U, 1U, 10U, 16U},       {0x12345678U, 0x9ABCU, 112U, 142U},
                                   {0xFFFFFFFFU, 1U, 10U, 156U},      {0x80000000U, 0xFFFFU, 132U, 18U},
                                   {0xFFFB0000U, 4U, 10U, 18U},       {0x00007FFFU, 2U, 110U, 124U},
                                   {0U, 1U, 136U, 150U}};
  for (const auto &c : div_cases) {
    expect(data_cycles(divu, c.divisor, c.dividend) == c.divu, "DIVU exact count");
    expect(data_cycles(divs, c.divisor, c.dividend) == c.divs, "DIVS exact count");
  }
  expect(data_cycles(with_source(M68kIrKind::divide_unsigned_word, M68kEaMode::address_predec), 5U, 100U) == 138U,
         "DIVU adds the Table 8-1 word EA cell");
  bool div_bounds = true;
  for (std::uint32_t dividend = 0U; dividend < 0xFFFFF000U; dividend += 0x00F00F0FU)
    for (const std::uint16_t divisor : {std::uint16_t{1U}, std::uint16_t{7U}, std::uint16_t{0x8000U}, std::uint16_t{0xFFFFU}}) {
      div_bounds &= segarecomp_m68k_divu_word_cycles(dividend, divisor) < 140U;
      div_bounds &= segarecomp_m68k_divs_word_cycles(dividend, divisor) < 158U;
    }
  expect(div_bounds, "DIVU/DIVS never exceed the published maxima");

  // SEG-021-T022: Table 8-8 BTST Dn,#<data> = 4 + the #<data> cell 4.
  auto btst_immediate = operation(M68kIrKind::bit_test, M68kMemoryAccessWidth::byte);
  btst_immediate.source_ea.mode = M68kEaMode::data_register;
  btst_immediate.destination_ea.mode = M68kEaMode::immediate;
  expect(m68k_instruction_cycles(btst_immediate) == 8U, "BTST Dn,#<data> is 8");

  // SEG-021-T022: exception-processing times (MC68000 UM §8 exception table) of every synchronous exception path;
  // the exception path reports the entry time instead of the retirement rule.
  auto trap = operation(M68kIrKind::trap_exception, M68kMemoryAccessWidth::byte);
  bool traps = true;
  for (std::uint8_t n = 0U; n < 16U; ++n) {
    trap.exception_vector = static_cast<std::uint8_t>(32U + n);
    traps &= m68k_exception_entry_cycles(trap) == 34U && m68k_instruction_timing(trap)->exception_entry_cycles == 34U;
  }
  expect(traps, "TRAP #0-15 entry is 34");
  auto trapv = operation(M68kIrKind::trap_on_overflow, M68kMemoryAccessWidth::byte);
  trapv.exception_vector = 7U;
  expect(data_cycles(trapv, 0U, 0U, false) == 4U && data_cycles(trapv, 0U, 0U, true) == 34U, "TRAPV is 4 / 34");
  auto chk = with_source(M68kIrKind::check_bounds, M68kEaMode::data_register);
  chk.exception_vector = 6U;
  expect(data_cycles(chk, 0U, 0U, false) == 10U && data_cycles(chk, 0U, 0U, true) == 40U, "CHK Dn is 10 / 40");
  chk.source_ea.mode = M68kEaMode::address_indirect;
  expect(data_cycles(chk, 0U, 0U, false) == 14U && data_cycles(chk, 0U, 0U, true) == 44U, "CHK (An) is 14 / 40 + 4");
  chk.source_ea.mode = M68kEaMode::immediate;
  expect(m68k_exception_entry_cycles(chk) == 44U, "CHK #<data> entry is 40 + 4");
  expect(data_cycles(divu, 0U, 0x1234U, true) == 38U && data_cycles(divs, 0U, 0x1234U, true) == 38U,
         "DIV zero divide (Dn) is 38");
  expect(m68k_exception_entry_cycles(with_source(M68kIrKind::divide_signed_word, M68kEaMode::absolute_long)) == 50U,
         "DIV zero divide abs.l is 38 + 12");
  for (const std::uint8_t vector : {std::uint8_t{4U}, std::uint8_t{10U}, std::uint8_t{11U}}) {
    auto illegal = operation(M68kIrKind::instruction_exception, M68kMemoryAccessWidth::byte);
    illegal.exception_vector = vector;
    expect(m68k_exception_entry_cycles(illegal) == 34U, "illegal / line 1010 / line 1111 entry is 34");
  }
  auto move_to_sr = with_source(M68kIrKind::write_status_register, M68kEaMode::data_register);
  expect(data_cycles(move_to_sr, 0U, 0U, false) == 12U && data_cycles(move_to_sr, 0U, 0U, true) == 34U,
         "MOVE Dn,SR is 12 / privilege violation 34");
  expect(m68k_exception_entry_cycles(operation(M68kIrKind::stop_until_interrupt, M68kMemoryAccessWidth::word)) == 34U &&
             m68k_exception_entry_cycles(operation(M68kIrKind::return_from_exception, M68kMemoryAccessWidth::word)) == 34U,
         "STOP / RTE privilege violation is 34");
  expect(!m68k_exception_entry_cycles(moveq_like()) && m68k_instruction_timing(moveq_like())->exception_entry_cycles == 0U,
         "a form without an exception path has no entry time");
  // The CPU-owned C table the machine charges for interrupts: 44 for every interrupt vector; nothing for vectors no
  // MC68000 exception is delivered through.
  bool table = true;
  for (std::uint32_t vector = 0U; vector < 256U; ++vector) {
    const auto value = segarecomp_m68k_exception_entry_cycles(vector);
    const bool interrupt = vector == 15U || (vector >= 24U && vector <= 31U) || vector >= 64U;
    const bool none = vector <= 1U || (vector >= 12U && vector <= 14U) || (vector >= 16U && vector <= 23U) ||
                      (vector >= 48U && vector <= 63U);
    if (interrupt) table &= value == 44U;
    else if (none) table &= value == 0U;
    else table &= value == (vector == 2U || vector == 3U ? 50U : vector == 5U ? 38U : vector == 6U ? 40U : 34U);
  }
  expect(table, "exception-processing table by vector");

  // Families without a CPU-owned rule still fail closed (enumerated): the direct_flow-profile-only BNE.S
  // compatibility kind (RESET stays a decode frontier and never reaches the lifter).
  expect(!m68k_instruction_timing(operation(M68kIrKind::branch_ne_short, M68kMemoryAccessWidth::byte)),
         "direct_flow BNE.S has no descriptor");

  // Every static row is carried unchanged as a fixed rule.
  auto moveq = operation(M68kIrKind::write_moveq, M68kMemoryAccessWidth::long_word);
  expect(m68k_instruction_timing(moveq)->rule == M68kTimingRule::fixed && m68k_instruction_timing(moveq)->cycles == 4U,
         "static rows are fixed rules");
  if (failures != 0) return 1;
  std::puts("m68k dynamic timing: ok");
  return 0;
}
