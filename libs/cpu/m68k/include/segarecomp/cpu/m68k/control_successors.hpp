#pragma once

// SEG-026-T001 (experiment, report-only): the MC68000-owned fixed control successors of one lifted operation.
//
// A thin projection of the existing semantic owner `m68k_operation_effect` (PC-effect kind, direct target,
// stack intent) plus the lifted operation's own decoded condition and control EA. It adds no decoding, lifting
// or execution semantics. It answers only: which exact PCs does this instruction make architecturally
// selectable by fixed control flow, which continuation does it stack for a later return, and -- when the
// next PC is runtime-derived -- which generic dynamic-control family owns it. No register value, table,
// operand-width interval or path fact is consulted. (Restored in reduced form from the SEG-024-T001
// experiment, commit e0902b9, which was not retained; see ADR 0051.)
//
// Targets are architectural 32-bit PC values; bus/address-space canonicalization belongs to the machine.

#include <cstdint>
#include <vector>

#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

enum class M68kControlSuccessorKind : std::uint8_t {
  fallthrough,              // sequential advance
  branch_target,            // BRA / direct JMP
  conditional_target,       // Bcc / DBcc taken outcome
  conditional_fallthrough,  // Bcc / DBcc not-taken outcome
  call_target,              // BSR / direct JSR callee
};

// A PC this instruction stacks for a later return; it is NOT itself selected by this instruction.
enum class M68kStackedContinuationKind : std::uint8_t {
  none,
  call_continuation,       // BSR / JSR (direct or indirect): the pushed return address
  exception_continuation,  // TRAP #n / TRAPV: the stacked next instruction an RTE resumes
  pushed_code_address,     // PEA of a statically foldable address (possible manual-call continuation)
};

enum class M68kDynamicControlFamily : std::uint8_t {
  none,
  return_from_subroutine,          // RTS
  return_from_exception,           // RTE
  return_restore_condition_codes,  // RTR
  jump_address_indirect,           // JMP (An)
  call_address_indirect,           // JSR (An)
  jump_address_disp16,             // JMP d16(An)
  call_address_disp16,             // JSR d16(An)
  jump_address_index,              // JMP (d8,An,Xn)
  call_address_index,              // JSR (d8,An,Xn)
  jump_pc_index,                   // JMP (d8,PC,Xn)
  call_pc_index,                   // JSR (d8,PC,Xn)
  unclassified,                    // a PC effect this projection does not understand: callers stop
};
inline constexpr std::uint32_t m68k_dynamic_control_family_count = 13U;

struct M68kControlSuccessor {
  M68kControlSuccessorKind kind{M68kControlSuccessorKind::fallthrough};
  std::uint32_t target{};
};

struct M68kControlSuccessors {
  std::vector<M68kControlSuccessor> successors;
  M68kStackedContinuationKind stacked{M68kStackedContinuationKind::none};
  std::uint32_t stacked_address{};
  M68kDynamicControlFamily dynamic{M68kDynamicControlFamily::none};
  // True when the instruction has no fixed successor because it always enters an exception handler with its
  // own address stacked (ILLEGAL, line 1010/1111, other illegal words); the handler is a machine vector root.
  bool always_raises_exception{};
};

[[nodiscard]] M68kControlSuccessors m68k_control_successors(const M68kIrOperation &operation);
[[nodiscard]] const char *m68k_dynamic_control_family_name(M68kDynamicControlFamily family) noexcept;

}  // namespace segarecomp
