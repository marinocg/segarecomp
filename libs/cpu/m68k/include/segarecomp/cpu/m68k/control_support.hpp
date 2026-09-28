#pragma once

// SEG-024-T001 (experiment, report-only): MC68000-owned executable-support facts for one lifted operation.
//
// This is a thin projection of the existing semantic owner `m68k_operation_effect` (PC-effect kind,
// direct target, stack intent) plus the lifted operation's already-decoded condition and control EA.
// It adds no decoding, lifting or execution semantics. It answers only: which exact PCs does this
// instruction start make architecturally selectable (fixed successors), and does it own a runtime-derived
// PC source (dynamic-control site), and if so which generic control family and which architectural
// interval (from operand width alone) bounds that runtime PC.
//
// Targets are architectural 32-bit PC values; bus/address-space canonicalization belongs to the machine.

#include <cstdint>
#include <optional>
#include <vector>

#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

enum class M68kControlSuccessorKind : std::uint8_t {
  fallthrough,              // sequential advance (pc_delta)
  branch_target,            // unconditional direct branch/jump
  conditional_target,       // Bcc/DBcc taken outcome
  conditional_fallthrough,  // Bcc/DBcc not-taken outcome
  call_target,              // JSR/BSR direct callee
  call_continuation,        // JSR/BSR pushed continuation (reached only if something returns there)
  pushed_code_address,      // PEA of a statically foldable address (manual-call continuation)
  exception_continuation,   // TRAP #n: the stacked next instruction the handler's RTE resumes at
};

enum class M68kDynamicControlFamily : std::uint8_t {
  none,
  return_from_subroutine,
  return_from_exception,
  return_restore_condition_codes,
  jump_address_indirect,    // JMP (An)
  call_address_indirect,    // JSR (An)
  jump_address_disp16,      // JMP d16(An)
  call_address_disp16,      // JSR d16(An)
  jump_address_index,       // JMP (d8,An,Xn)
  call_address_index,       // JSR (d8,An,Xn)
  jump_pc_index_word,       // JMP (d8,PC,Xn.W)
  call_pc_index_word,       // JSR (d8,PC,Xn.W)
  jump_pc_index_long,       // JMP (d8,PC,Xn.L)
  call_pc_index_long,       // JSR (d8,PC,Xn.L)
  unclassified,             // no PC effect this projection understands: callers must stay conservative
};
inline constexpr std::uint32_t m68k_dynamic_control_family_count = 15U;

struct M68kControlSuccessor {
  M68kControlSuccessorKind kind{M68kControlSuccessorKind::fallthrough};
  std::uint32_t target{};
};

// Half-open interval of architectural 32-bit PC values, as signed 64-bit so wrap is explicit.
struct M68kArchitecturalTargetInterval {
  std::int64_t begin{};
  std::int64_t end{};
};

struct M68kControlSupport {
  std::vector<M68kControlSuccessor> successors;
  M68kDynamicControlFamily dynamic{M68kDynamicControlFamily::none};
  // Present only when the runtime target is bounded by operand width alone (a sign-extended 16-bit
  // index added to a PC-relative base): no register value, producer or path fact is consulted.
  std::optional<M68kArchitecturalTargetInterval> architectural_target_interval;
};

[[nodiscard]] M68kControlSupport m68k_control_support(const M68kIrOperation &operation);
[[nodiscard]] const char *m68k_dynamic_control_family_name(M68kDynamicControlFamily family) noexcept;

}  // namespace segarecomp
