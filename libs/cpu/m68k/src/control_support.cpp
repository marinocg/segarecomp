// SEG-024-T001 (experiment, report-only): see control_support.hpp. Every fact below is read from the
// existing semantic owner `m68k_operation_effect` or from the lifted operation's own decoded fields.

#include "segarecomp/cpu/m68k/control_support.hpp"

#include "segarecomp/cpu/m68k/effective_address.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"

namespace segarecomp {
namespace {

M68kDynamicControlFamily indirect_family(const M68kIrOperation &operation) {
  const bool call = operation.kind == M68kIrKind::call_general;
  const auto &ea = operation.source_ea;
  switch (ea.mode) {
  case M68kEaMode::address_indirect:
    return call ? M68kDynamicControlFamily::call_address_indirect : M68kDynamicControlFamily::jump_address_indirect;
  case M68kEaMode::address_disp16:
    return call ? M68kDynamicControlFamily::call_address_disp16 : M68kDynamicControlFamily::jump_address_disp16;
  case M68kEaMode::address_index8:
    return call ? M68kDynamicControlFamily::call_address_index : M68kDynamicControlFamily::jump_address_index;
  case M68kEaMode::pc_index8:
    if (ea.index_is_long)
      return call ? M68kDynamicControlFamily::call_pc_index_long : M68kDynamicControlFamily::jump_pc_index_long;
    return call ? M68kDynamicControlFamily::call_pc_index_word : M68kDynamicControlFamily::jump_pc_index_word;
  default:
    return M68kDynamicControlFamily::unclassified;
  }
}

}  // namespace

M68kControlSupport m68k_control_support(const M68kIrOperation &operation) {
  M68kControlSupport support{};
  const auto effect = m68k_operation_effect(operation);
  const auto address = operation.provenance.source.address.value;
  const auto next = static_cast<std::uint32_t>(address + operation.provenance.length.value);
  switch (effect.pc) {
  case M68kPcEffectKind::advance:
    support.successors.push_back({operation.kind == M68kIrKind::trap_exception
                                      ? M68kControlSuccessorKind::exception_continuation
                                      : M68kControlSuccessorKind::fallthrough,
                                  static_cast<std::uint32_t>(address + effect.pc_delta)});
    break;
  case M68kPcEffectKind::direct_target:
    // The effect layer states the taken target only; whether a not-taken outcome exists is the
    // lifted condition's fact (Bcc/DBcc), which this projection reads without evaluating it.
    if (operation.kind == M68kIrKind::general_branch) {
      if (operation.condition == M68kCondition::always) {
        support.successors.push_back({M68kControlSuccessorKind::branch_target, effect.direct_target});
      } else {
        support.successors.push_back({M68kControlSuccessorKind::conditional_target, effect.direct_target});
        support.successors.push_back({M68kControlSuccessorKind::conditional_fallthrough, next});
      }
    } else if (operation.kind == M68kIrKind::dbcc_loop) {
      // Conservative: both outcomes for every condition (DBT never branches; keeping it costs nothing).
      support.successors.push_back({M68kControlSuccessorKind::conditional_target, effect.direct_target});
      support.successors.push_back({M68kControlSuccessorKind::conditional_fallthrough, next});
    } else if (effect.stack == M68kStackEffectKind::push_static_continuation) {
      support.successors.push_back({M68kControlSuccessorKind::call_target, effect.direct_target});
      support.successors.push_back({M68kControlSuccessorKind::call_continuation, next});
    } else {
      support.successors.push_back({M68kControlSuccessorKind::branch_target, effect.direct_target});
    }
    break;
  case M68kPcEffectKind::observed_stack_return:
    support.dynamic = M68kDynamicControlFamily::return_from_subroutine;
    break;
  case M68kPcEffectKind::observed_exception_return:
    support.dynamic = operation.kind == M68kIrKind::return_restore_condition_codes
                          ? M68kDynamicControlFamily::return_restore_condition_codes
                          : M68kDynamicControlFamily::return_from_exception;
    break;
  case M68kPcEffectKind::exception_entry:
    // Handler is a vector root owned by the machine; the stacked PC is this instruction itself.
    break;
  case M68kPcEffectKind::none:
    if (operation.kind == M68kIrKind::jump_general || operation.kind == M68kIrKind::call_general) {
      support.dynamic = indirect_family(operation);
      if (effect.stack == M68kStackEffectKind::push_static_continuation)
        support.successors.push_back({M68kControlSuccessorKind::call_continuation, next});
      const auto &ea = operation.source_ea;
      if (ea.mode == M68kEaMode::pc_index8 && !ea.index_is_long) {
        // EA = PC-relative base + d8 + sign-extended 16-bit index: bounded by operand width alone.
        const std::int64_t base = static_cast<std::int64_t>(ea.pc_base_address) +
                                  static_cast<std::int64_t>(static_cast<std::int8_t>(ea.displacement));
        support.architectural_target_interval = M68kArchitecturalTargetInterval{base - 32768, base + 32768};
      }
    } else {
      support.dynamic = M68kDynamicControlFamily::unclassified;
    }
    break;
  }
  if (operation.kind == M68kIrKind::push_effective_address && m68k_is_statically_foldable_control_ea(operation.source_ea))
    support.successors.push_back({M68kControlSuccessorKind::pushed_code_address, m68k_canonical_ea_address(operation.source_ea)});
  return support;
}

const char *m68k_dynamic_control_family_name(M68kDynamicControlFamily family) noexcept {
  switch (family) {
  case M68kDynamicControlFamily::none: return "none";
  case M68kDynamicControlFamily::return_from_subroutine: return "rts";
  case M68kDynamicControlFamily::return_from_exception: return "rte";
  case M68kDynamicControlFamily::return_restore_condition_codes: return "rtr";
  case M68kDynamicControlFamily::jump_address_indirect: return "jmp_(An)";
  case M68kDynamicControlFamily::call_address_indirect: return "jsr_(An)";
  case M68kDynamicControlFamily::jump_address_disp16: return "jmp_d16(An)";
  case M68kDynamicControlFamily::call_address_disp16: return "jsr_d16(An)";
  case M68kDynamicControlFamily::jump_address_index: return "jmp_(d8,An,Xn)";
  case M68kDynamicControlFamily::call_address_index: return "jsr_(d8,An,Xn)";
  case M68kDynamicControlFamily::jump_pc_index_word: return "jmp_(d8,PC,Xn.W)";
  case M68kDynamicControlFamily::call_pc_index_word: return "jsr_(d8,PC,Xn.W)";
  case M68kDynamicControlFamily::jump_pc_index_long: return "jmp_(d8,PC,Xn.L)";
  case M68kDynamicControlFamily::call_pc_index_long: return "jsr_(d8,PC,Xn.L)";
  case M68kDynamicControlFamily::unclassified: return "unclassified";
  }
  return "unclassified";
}

}  // namespace segarecomp
