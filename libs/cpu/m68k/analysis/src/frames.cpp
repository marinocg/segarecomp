// SEG-030-T006 (ADR 0079, report-only). See frames.hpp.

#include "segarecomp/cpu/m68k/analysis/frames.hpp"

#include <algorithm>

namespace segarecomp {

using analysis::FiniteValue;
using analysis::UnknownReason;

bool m68k_status_supervisor_proven(const FiniteValue &status) noexcept {
  if (!status.is_precise() || status.values().empty()) return false;
  return std::all_of(status.values().begin(), status.values().end(), [](std::uint64_t v) { return m68k_status_supervisor(v); });
}

bool m68k_status_may_be_user(const FiniteValue &status) noexcept {
  if (status.is_unknown()) return true;
  return std::any_of(status.values().begin(), status.values().end(), [](std::uint64_t v) { return !m68k_status_supervisor(v); });
}

bool m68k_status_register_writer(M68kIrKind kind) noexcept {
  return kind == M68kIrKind::write_status_register || kind == M68kIrKind::logical_immediate_to_sr ||
         kind == M68kIrKind::stop_until_interrupt;
}

bool m68k_privileged_kind(M68kIrKind kind) noexcept {
  switch (kind) {
  case M68kIrKind::write_status_register:
  case M68kIrKind::logical_immediate_to_sr:
  case M68kIrKind::stop_until_interrupt:
  case M68kIrKind::return_from_exception:
  case M68kIrKind::write_user_stack_pointer:
  case M68kIrKind::read_user_stack_pointer: return true;
  default: return false;
  }
}

FiniteValue m68k_status_after(const M68kIrOperation &operation, const FiniteValue &in,
                              const std::optional<std::vector<std::uint32_t>> &source) {
  switch (operation.kind) {
  case M68kIrKind::write_status_register: {
    if (!source) return FiniteValue::unknown(UnknownReason::unsupported_transfer);
    std::vector<std::uint64_t> out;
    for (const auto sr : *source) out.push_back(m68k_status_of_sr(sr));
    return FiniteValue::of(std::move(out));
  }
  case M68kIrKind::stop_until_interrupt:
    return FiniteValue::of({m68k_status_of_sr(operation.source_ea.immediate_value)});
  case M68kIrKind::logical_immediate_to_sr: {
    if (!in.is_precise()) return in.is_bottom() ? in : FiniteValue::unknown(in.reason());
    // AND/OR/EOR are bitwise, so S and I2-I0 follow from the same immediate bits exactly.
    const auto immediate = m68k_status_of_sr(operation.source_ea.immediate_value);
    return in.map([&](std::uint64_t v) {
      switch (operation.status_operation) {
      case M68kStatusLogicalOperation::and_op: return v & immediate;
      case M68kStatusLogicalOperation::or_op: return v | immediate;
      case M68kStatusLogicalOperation::eor_op: return v ^ immediate;
      }
      return v;
    });
  }
  default: return in;
  }
}

M68kVectorClass m68k_vector_class(std::uint32_t vector) noexcept {
  if (vector == 15U || (vector >= 24U && vector <= 31U) || vector >= 64U) return M68kVectorClass::interrupt;
  if (vector == 5U || vector == 6U || vector == 7U) return M68kVectorClass::synchronous_resuming;
  return M68kVectorClass::synchronous;
}

bool m68k_exception_stacks_next(std::uint32_t vector) noexcept {
  return vector == 5U || vector == 6U || vector == 7U || (vector >= 32U && vector <= 47U);
}

std::optional<unsigned> m68k_interrupt_level(std::uint32_t vector) noexcept {
  if (vector >= 25U && vector <= 31U) return vector - 24U;
  return std::nullopt;
}

bool m68k_interrupt_eligible(const FiniteValue &status, std::optional<unsigned> level) noexcept {
  if (status.is_bottom()) return false;
  if (status.is_unknown()) return true;
  if (!level || *level == 7U) return !status.values().empty();
  return std::any_of(status.values().begin(), status.values().end(), [&](std::uint64_t v) { return m68k_status_mask(v) < *level; });
}

std::vector<std::uint32_t> m68k_raised_vectors(const M68kIrOperation *operation, const FiniteValue &status) {
  std::vector<std::uint32_t> out;
  if (operation == nullptr) return {4U, 8U, 10U, 11U};
  switch (operation->kind) {
  case M68kIrKind::trap_exception: out.push_back(operation->exception_vector); break;
  case M68kIrKind::trap_on_overflow: out.push_back(7U); break;
  case M68kIrKind::check_bounds: out.push_back(6U); break;
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word: out.push_back(5U); break;
  case M68kIrKind::instruction_exception:
    if (operation->exception_vector != 0U) out.push_back(operation->exception_vector);
    else out.insert(out.end(), {4U, 10U, 11U});
    break;
  default: break;
  }
  if (m68k_privileged_kind(operation->kind) && m68k_status_may_be_user(status)) out.push_back(8U);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

FiniteValue m68k_handler_entry_status(std::uint32_t vector, const FiniteValue &from) {
  if (m68k_vector_class(vector) == M68kVectorClass::interrupt) {
    if (const auto level = m68k_interrupt_level(vector)) return FiniteValue::of({UINT64_C(8) | *level});
    return FiniteValue::of({9U, 10U, 11U, 12U, 13U, 14U, 15U});
  }
  if (!from.is_precise()) return FiniteValue::of({8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U});
  return from.map([](std::uint64_t v) { return UINT64_C(8) | m68k_status_mask(v); });
}

}  // namespace segarecomp
