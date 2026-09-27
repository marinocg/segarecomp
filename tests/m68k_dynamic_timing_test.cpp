// SEG-021-T021: the CPU-owned outcome-dependent MC68000 timing rules (Bcc, DBcc, Scc Dn, register
// shift/rotate), checked row by row against the published MC68000 User's Manual section 8 tables, and the
// kinds that must still fail closed. The generated-side evaluation of the same rules is cross-checked against
// the pinned Musashi core by the conformance harness (tools/m68k_conformance.py, `timing` rows).
#include "segarecomp/cpu/m68k/timing.hpp"

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

  // Families without a CPU-owned rule still fail closed (enumerated): the data-dependent MULU/MULS word table
  // (SEG-021-T022) and the direct_flow-profile-only BNE.S compatibility kind.
  expect(!m68k_instruction_timing(operation(M68kIrKind::multiply_unsigned_word, M68kMemoryAccessWidth::word)),
         "MULU has no descriptor");
  expect(!m68k_instruction_timing(operation(M68kIrKind::multiply_signed_word, M68kMemoryAccessWidth::word)),
         "MULS has no descriptor");
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
