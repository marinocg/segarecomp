#pragma once

#include "segarecomp/cpu/m68k/ir.hpp"

#include <cstdint>
#include <optional>

namespace segarecomp {

// MC68000 instruction timing is a CPU contract.  Machine code consumes this
// typed result; it must not infer timing from generated dispatch structure.
[[nodiscard]] std::optional<std::uint32_t>
m68k_instruction_cycles(const M68kIrOperation &operation) noexcept;

// SEG-021-T021: the complete MC68000 retirement-time rule of one operation, including the rows whose
// published cycle count depends on a runtime outcome (MC68000 User's Manual section 8: Table 8-6 Scc Dn,
// Table 8-9 register shift/rotate, Table 8-10 Bcc and DBcc). Fidelity: published instruction cycle totals
// consumed by the deterministic scheduler at instruction retirement; this is NOT a bus-cycle model (no
// wait states, no prefetch/bus arbitration, no intra-instruction access timing).
//
//   fixed            cycles
//   condition        condition true (Bcc taken / Scc sets) = cycles, condition false = false_cycles
//   dbcc             condition true = cycles; condition false and counter expired (Dn.W == -1 after the
//                    decrement) = expired_cycles; condition false and branch taken = false_cycles
//   register_count   cycles + per_count_cycles * n, where n is the architectural shift count: the immediate
//                    count 1..8, or the count register modulo 64 (n = 0 is legal and costs `cycles`).
//                    ROXL/ROXR use the same modulo-64 n for timing even though the rotation itself is
//                    taken modulo size+1.
//
//   multiply_unsigned MULU.W (Table 8-4): 38 + 2n + `cycles`, n = the 1 bits of the source word; `cycles` holds the
//                    word EA cell (segarecomp_m68k_mulu_word_cycles, timing_core.h)
//   multiply_signed  MULS.W (Table 8-4): 38 + 2n + `cycles`, n = the 01/10 pairs of the source word with a 0 appended
//                    (segarecomp_m68k_muls_word_cycles)
//   divide_unsigned  DIVU.W, non-zero divisor: the exact data-dependent count of segarecomp_m68k_divu_word_cycles
//                    (dividend, divisor) + `cycles` (word EA cell); overflow included
//   divide_signed    DIVS.W, non-zero divisor: segarecomp_m68k_divs_word_cycles(dividend, divisor) + `cycles`
//
// SEG-021-T022 exception entry: `exception_entry_cycles` is the published exception-processing time (MC68000 User's
// Manual §8 Table 8-14, ADR 0043 §8) of the ONE synchronous exception this form can take (vector 8
// for the privileged forms, 4/10/11 for the instruction-word exceptions, 32-47 TRAP, 7 TRAPV, 6 CHK + word EA, 5 zero
// divide + word EA), counted from the start of the instruction; 0 when the form never takes one. The exception path
// never retires: the machine charges this value at the entry commit instead of the retirement rule.
//
// RESET (still a decode frontier) and the `BTST Dn,#<data>` form (no verified table cell) yield no descriptor;
// callers must fail closed rather than guess.
enum class M68kTimingRule : std::uint8_t {
  fixed,
  condition,
  dbcc,
  register_count,
  multiply_unsigned,
  multiply_signed,
  divide_unsigned,
  divide_signed
};

struct M68kInstructionTiming {
  M68kTimingRule rule = M68kTimingRule::fixed;
  std::uint32_t cycles = 0U;
  std::uint32_t false_cycles = 0U;
  std::uint32_t expired_cycles = 0U;
  std::uint32_t per_count_cycles = 0U;
  std::uint32_t exception_entry_cycles = 0U;
};

// The runtime outcome a dynamic rule is evaluated against (fields a rule does not use are ignored).
struct M68kTimingOutcome {
  bool condition_true = false;
  bool counter_expired = false;
  std::uint32_t count = 0U;  // already reduced: immediate 1..8, or Dn & 63
  std::uint16_t source_word = 0U;  // MULU/MULS multiplier, DIVU/DIVS divisor (the <ea> word)
  std::uint32_t dividend = 0U;     // DIVU/DIVS: the 32-bit Dn before the instruction
  bool exception_taken = false;    // the form's synchronous exception was taken (entry cycles, no retirement)
};

[[nodiscard]] std::optional<M68kInstructionTiming>
m68k_instruction_timing(const M68kIrOperation &operation) noexcept;

// Host-side evaluation of a rule for one outcome (tests, reports); generated code evaluates the same
// rule through the C expression rendered by the codegen owner.
[[nodiscard]] std::uint32_t m68k_timing_cycles(const M68kInstructionTiming &timing,
                                               const M68kTimingOutcome &outcome) noexcept;

// SEG-021-T022: the exception-entry cycles of the operation's synchronous exception path (see
// `M68kInstructionTiming::exception_entry_cycles`), or nullopt when the form takes none / has no published EA cell.
[[nodiscard]] std::optional<std::uint32_t> m68k_exception_entry_cycles(const M68kIrOperation &operation) noexcept;

// Table 8-1 effective-address cells.  Dynamic instruction timing expressions
// consume this typed CPU fact rather than duplicating an EA timing table in a
// machine/codegen owner.
[[nodiscard]] std::optional<std::uint32_t>
m68k_effective_address_cycles(const M68kEffectiveAddress &effective_address,
                              M68kMemoryAccessWidth size) noexcept;

}  // namespace segarecomp
