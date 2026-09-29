// SEG-026-T001 (experiment, report-only): see control_successors.hpp. Every fact below is read from the
// existing semantic owner `m68k_operation_effect` or from the lifted operation's own decoded fields.

#include "segarecomp/cpu/m68k/control_successors.hpp"

#include "segarecomp/cpu/m68k/effective_address.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"

namespace segarecomp {
namespace {

M68kDynamicControlFamily indirect_family(const M68kIrOperation &operation) {
  const bool call = operation.kind == M68kIrKind::call_general;
  switch (operation.source_ea.mode) {
  case M68kEaMode::address_indirect:
    return call ? M68kDynamicControlFamily::call_address_indirect : M68kDynamicControlFamily::jump_address_indirect;
  case M68kEaMode::address_disp16:
    return call ? M68kDynamicControlFamily::call_address_disp16 : M68kDynamicControlFamily::jump_address_disp16;
  case M68kEaMode::address_index8:
    return call ? M68kDynamicControlFamily::call_address_index : M68kDynamicControlFamily::jump_address_index;
  case M68kEaMode::pc_index8:
    return call ? M68kDynamicControlFamily::call_pc_index : M68kDynamicControlFamily::jump_pc_index;
  default:
    return M68kDynamicControlFamily::unclassified;
  }
}

}  // namespace

M68kControlSuccessors m68k_control_successors(const M68kIrOperation &operation) {
  M68kControlSuccessors out{};
  const auto effect = m68k_operation_effect(operation);
  const auto address = operation.provenance.source.address.value;
  const auto next = static_cast<std::uint32_t>(address + operation.provenance.length.value);
  if (effect.stack == M68kStackEffectKind::push_static_continuation) {
    out.stacked = M68kStackedContinuationKind::call_continuation;
    out.stacked_address = next;
  }
  switch (effect.pc) {
  case M68kPcEffectKind::advance:
    out.successors.push_back({M68kControlSuccessorKind::fallthrough, static_cast<std::uint32_t>(address + effect.pc_delta)});
    if (operation.kind == M68kIrKind::trap_exception || operation.kind == M68kIrKind::trap_on_overflow) {
      out.stacked = M68kStackedContinuationKind::exception_continuation;
      out.stacked_address = next;
      // TRAP #n never falls through: the handler is entered and its RTE resumes at the stacked next PC.
      if (operation.kind == M68kIrKind::trap_exception) out.successors.clear();
    }
    break;
  case M68kPcEffectKind::direct_target:
    // The effect layer states the taken target only; whether a not-taken outcome exists is the lifted
    // condition's fact (Bcc/DBcc), read here without evaluating it.
    if (operation.kind == M68kIrKind::general_branch && operation.condition == M68kCondition::always) {
      out.successors.push_back({M68kControlSuccessorKind::branch_target, effect.direct_target});
    } else if (operation.kind == M68kIrKind::general_branch || operation.kind == M68kIrKind::dbcc_loop ||
               operation.kind == M68kIrKind::branch_ne_short) {
      out.successors.push_back({M68kControlSuccessorKind::conditional_target, effect.direct_target});
      out.successors.push_back({M68kControlSuccessorKind::conditional_fallthrough, next});
    } else if (effect.stack == M68kStackEffectKind::push_static_continuation) {
      out.successors.push_back({M68kControlSuccessorKind::call_target, effect.direct_target});
    } else {
      out.successors.push_back({M68kControlSuccessorKind::branch_target, effect.direct_target});
    }
    break;
  case M68kPcEffectKind::observed_stack_return:
    out.dynamic = M68kDynamicControlFamily::return_from_subroutine;
    break;
  case M68kPcEffectKind::observed_exception_return:
    out.dynamic = operation.kind == M68kIrKind::return_restore_condition_codes
                      ? M68kDynamicControlFamily::return_restore_condition_codes
                      : M68kDynamicControlFamily::return_from_exception;
    break;
  case M68kPcEffectKind::exception_entry:
    out.always_raises_exception = true;
    break;
  case M68kPcEffectKind::none:
    if (operation.kind == M68kIrKind::jump_general || operation.kind == M68kIrKind::call_general)
      out.dynamic = indirect_family(operation);
    else
      out.dynamic = M68kDynamicControlFamily::unclassified;
    break;
  }
  if (operation.kind == M68kIrKind::push_effective_address && m68k_is_statically_foldable_control_ea(operation.source_ea)) {
    out.stacked = M68kStackedContinuationKind::pushed_code_address;
    out.stacked_address = m68k_canonical_ea_address(operation.source_ea);
  }
  return out;
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
  case M68kDynamicControlFamily::jump_pc_index: return "jmp_(d8,PC,Xn)";
  case M68kDynamicControlFamily::call_pc_index: return "jsr_(d8,PC,Xn)";
  case M68kDynamicControlFamily::unclassified: return "unclassified";
  }
  return "unclassified";
}

}  // namespace segarecomp
