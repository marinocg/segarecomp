// SEG-029-T003 (ADR 0078, report-only). See finite_adapter.hpp.

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>
#include <variant>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"

namespace segarecomp {
namespace {

using analysis::EdgeKind;
using analysis::FiniteValue;
using analysis::UnknownReason;

constexpr std::uint32_t bus_mask = UINT32_C(0x00FFFFFF);
constexpr std::array<unsigned, 2> widths{16U, 32U};

M68kFiniteValues to_cpu(const FiniteValue &value, bool width_derived) {
  if (value.is_unknown()) return M68kFiniteValues::unknown();
  M68kFiniteValues out{};
  out.known = true;
  for (const auto v : value.values()) out.values.push_back(static_cast<std::uint32_t>(v));
  out.width_derived = width_derived;
  return out;
}

void store(M68kAnalysisState &state, std::size_t slot, const M68kFiniteValues &value, UnknownReason reason) {
  if (!value.known) {
    state.values.values[slot] = FiniteValue::unknown(reason);
    state.width_derived[slot] = false;
    return;
  }
  state.values.values[slot] = FiniteValue::of(std::vector<std::uint64_t>(value.values.begin(), value.values.end()));
  state.width_derived[slot] = !state.values.values[slot].is_unknown() && value.width_derived;
}

// The register inputs of one transfer, read from the input state. Records the smallest Unknown reason read so an
// Unknown result can carry its input's reason.
class StateInputs final : public M68kFiniteValueInputs {
public:
  StateInputs(const M68kAnalysisState &state, const M68kAnalysisImage &image) : state_(state), image_(image) {}
  M68kFiniteValues data_register_before(unsigned reg, unsigned width) override {
    if (reg >= 8U || (width != 16U && width != 32U)) return M68kFiniteValues::unknown();
    const auto slot = m68k_analysis_slot(reg, width);
    const auto &value = state_.values.values[slot];
    if (value.is_unknown()) unknown_read_ = unknown_read_ ? std::min(*unknown_read_, value.reason()) : value.reason();
    return to_cpu(value, state_.width_derived[slot]);
  }
  std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) override {
    return image_.immutable_read(address, bytes);
  }
  void reset() { unknown_read_.reset(); }
  [[nodiscard]] std::optional<UnknownReason> unknown_read() const { return unknown_read_; }

private:
  const M68kAnalysisState &state_;
  const M68kAnalysisImage &image_;
  std::optional<UnknownReason> unknown_read_;
};

bool is_pc_index_site(const M68kIrOperation &operation) {
  return (operation.kind == M68kIrKind::jump_general || operation.kind == M68kIrKind::call_general) &&
         operation.source_ea.mode == M68kEaMode::pc_index8;
}

UnknownReason site_reason(M68kPcIndexOutcome outcome, UnknownReason index_reason) {
  switch (outcome) {
  case M68kPcIndexOutcome::index_unknown: return index_reason;
  case M68kPcIndexOutcome::entry_outside_immutable_image:
  case M68kPcIndexOutcome::target_outside_image: return UnknownReason::non_immutable_read;
  case M68kPcIndexOutcome::resolved:
  case M68kPcIndexOutcome::address_register_index:
  case M68kPcIndexOutcome::empty_domain:
  case M68kPcIndexOutcome::width_only_domain:
  case M68kPcIndexOutcome::invalidated: return UnknownReason::unsupported_transfer;
  }
  return UnknownReason::unsupported_transfer;
}

std::string hex(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(6) << std::setfill('0') << value;
  return out.str();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Flat image view.

bool M68kFlatAnalysisImage::contains(std::uint32_t address) const noexcept {
  return address >= base_ && static_cast<std::uint64_t>(address) - base_ < bytes_.size();
}

std::optional<M68kAnalysisImage::Instruction> M68kFlatAnalysisImage::decode(std::uint32_t pc) const {
  if ((pc & 1U) != 0U || !contains(pc)) return std::nullopt;
  const std::uint64_t local = static_cast<std::uint64_t>(pc) - base_;
  DecodeSource source{CpuVariant::mc68000, {TargetAddressSpace::m68k_program, pc}, MoveqImageOffset{local}};
  auto result = decode_m68k_instruction(bytes_, source, M68kDecodeProfile::general_startup);
  auto *decoded = std::get_if<M68kDecodedInstruction>(&result);
  if (decoded == nullptr) return std::nullopt;
  const std::uint64_t length = decoded->provenance.length.value;
  if (length < 2U || local + length > bytes_.size()) return std::nullopt;
  return Instruction{lift_m68k_instruction(*decoded), static_cast<std::uint32_t>(length)};
}

bool M68kFlatAnalysisImage::mapped(std::uint32_t pc) const { return (pc & 1U) == 0U && contains(pc); }

std::optional<std::uint32_t> M68kFlatAnalysisImage::immutable_read(std::uint32_t address, unsigned bytes) const {
  if (bytes != 1U && bytes != 2U && bytes != 4U) return std::nullopt;
  std::uint32_t value = 0U;
  for (unsigned i = 0; i < bytes; ++i) {
    const auto at = (address + i) & bus_mask;
    if (at < address || !contains(at)) return std::nullopt;  // wrapped past the bus, or outside the image
    value = (value << 8U) | bytes_[static_cast<std::size_t>(at - base_)];
  }
  return value;
}

// ---------------------------------------------------------------------------------------------------------------
// State lattice.

M68kAnalysisState M68kAnalysisState::all_unknown(UnknownReason reason) {
  M68kAnalysisState out;
  out.values = analysis::ValueVector<m68k_analysis_slot_count>::all_unknown(reason);
  return out;
}

M68kAnalysisState join(const M68kAnalysisState &left, const M68kAnalysisState &right) {
  if (!left.values.reachable) return right;
  if (!right.values.reachable) return left;
  M68kAnalysisState out;
  out.values = join(left.values, right.values);
  for (std::size_t i = 0; i < m68k_analysis_slot_count; ++i)
    out.width_derived[i] = !out.values.values[i].is_unknown() && (left.width_derived[i] || right.width_derived[i]);
  out.flag_setter = left.flag_setter == right.flag_setter ? left.flag_setter : std::nullopt;
  return out;
}

bool leq(const M68kAnalysisState &left, const M68kAnalysisState &right) {
  if (!left.values.reachable) return true;
  if (!right.values.reachable) return false;
  if (!leq(left.values, right.values)) return false;
  for (std::size_t i = 0; i < m68k_analysis_slot_count; ++i)
    if (!right.values.values[i].is_unknown() && left.width_derived[i] && !right.width_derived[i]) return false;
  return !right.flag_setter || left.flag_setter == right.flag_setter;
}

// ---------------------------------------------------------------------------------------------------------------
// Adapter.

std::optional<M68kAnalysisImage::Instruction> M68kFiniteAdapter::decode(std::uint32_t pc) const {
  const auto found = decoded_.find(pc);
  if (found != decoded_.end()) return found->second;
  return decoded_.emplace(pc, image_.decode(pc)).first->second;
}

M68kPcIndexSiteReport M68kFiniteAdapter::evaluate_pc_index_site(std::uint32_t pc, const M68kIrOperation &operation,
                                                                const State &in) const {
  M68kPcIndexSiteReport out{};
  out.call = operation.kind == M68kIrKind::call_general;
  const auto finish = [&](M68kPcIndexOutcome outcome, UnknownReason index_reason = UnknownReason::unsupported_transfer) {
    out.outcome = outcome;
    out.reason = site_reason(outcome, index_reason);
    if (outcome != M68kPcIndexOutcome::resolved) out.targets.clear();
    return out;
  };
  const auto &ea = operation.source_ea;
  if (config_.pinned_sites.contains(pc)) return finish(M68kPcIndexOutcome::invalidated);
  if (ea.index_is_address) return finish(M68kPcIndexOutcome::address_register_index);
  const unsigned width = ea.index_is_long ? 32U : 16U;
  const auto slot = m68k_analysis_slot(ea.index_reg, width);
  const auto &index = in.values.values[slot];
  if (index.is_unknown())
    return finish(index.reason() == UnknownReason::non_immutable_read ? M68kPcIndexOutcome::entry_outside_immutable_image
                                                                       : M68kPcIndexOutcome::index_unknown,
                  index.reason());
  if (in.width_derived[slot] && !config_.accept_width_domains) return finish(M68kPcIndexOutcome::width_only_domain);
  std::set<std::uint32_t> targets;
  for (const auto value : index.values()) {
    const auto target = m68k_pc_index_address(ea, static_cast<std::uint32_t>(value));
    if ((target & 1U) != 0U) {
      ++out.odd_targets_excluded;  // an odd JMP/JSR target raises an address error: never a normal target
      continue;
    }
    if (!image_.mapped(target)) return finish(M68kPcIndexOutcome::target_outside_image);
    targets.insert(target);
  }
  if (targets.empty()) return finish(M68kPcIndexOutcome::empty_domain);
  out.targets.assign(targets.begin(), targets.end());
  return finish(M68kPcIndexOutcome::resolved);
}

analysis::TransferResult<M68kAnalysisState> M68kFiniteAdapter::transfer(std::uint64_t point, const State &in) {
  analysis::TransferResult<State> result;
  if (!in.values.reachable) return result;
  const auto pc = static_cast<std::uint32_t>(point) & bus_mask;
  const auto decoded = decode(pc);
  if (!decoded) return result;  // odd, unmapped or rejected: reached, no successor
  const auto &operation = decoded->operation;
  const auto next = (pc + decoded->length) & bus_mask;

  // Register values after the instruction, each slot through the CPU semantic owner.
  State out = in;
  out.flag_setter.reset();
  StateInputs inputs{in, image_};
  for (unsigned reg = 0; reg < 8U; ++reg) {
    for (const auto width : widths) {
      inputs.reset();
      const auto transfer = m68k_finite_register_after(operation, reg, width, inputs);
      if (!transfer.writes) continue;
      const auto reason = transfer.immutable_read_failed ? UnknownReason::non_immutable_read
                                                         : inputs.unknown_read().value_or(UnknownReason::unsupported_transfer);
      store(out, m68k_analysis_slot(reg, width), transfer.immutable_read_failed ? M68kFiniteValues::unknown() : transfer.values,
            reason);
    }
  }

  const auto control = m68k_control_successors(operation);
  // Flags at this branch are exact only when its sole predecessor is the physically preceding flag setter.
  std::optional<M68kAnalysisImage::Instruction> setter;
  if (in.flag_setter) {
    setter = decode(*in.flag_setter);
    if (setter && ((*in.flag_setter + setter->length) & bus_mask) != pc) setter.reset();
  }
  for (const auto &successor : control.successors) {
    const auto target = successor.target & bus_mask;
    State edge = out;
    EdgeKind kind = EdgeKind::fallthrough;
    switch (successor.kind) {
    case M68kControlSuccessorKind::fallthrough:
    case M68kControlSuccessorKind::branch_target:
      kind = successor.kind == M68kControlSuccessorKind::fallthrough ? EdgeKind::fallthrough : EdgeKind::branch;
      if (target == next) edge.flag_setter = pc;
      break;
    case M68kControlSuccessorKind::conditional_target:
    case M68kControlSuccessorKind::conditional_fallthrough: {
      kind = EdgeKind::branch;
      if (!setter) break;
      const bool taken = successor.kind == M68kControlSuccessorKind::conditional_target;
      for (unsigned reg = 0; reg < 8U; ++reg) {
        for (const auto width : widths) {
          const auto slot = m68k_analysis_slot(reg, width);
          if (!edge.values.values[slot].is_precise() && !edge.values.values[slot].is_bottom()) continue;
          auto values = to_cpu(edge.values.values[slot], edge.width_derived[slot]);
          if (m68k_finite_branch_filter(setter->operation, operation, taken, reg, width, values))
            store(edge, slot, values, UnknownReason::unsupported_transfer);
        }
      }
      break;
    }
    case M68kControlSuccessorKind::call_target: kind = EdgeKind::call; break;
    }
    result.edges.push_back({target, kind, std::move(edge)});
  }

  const auto stacked = control.stacked_address & bus_mask;
  switch (control.stacked) {
  case M68kStackedContinuationKind::call_continuation:
    if (config_.call_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, State::all_unknown()});
    break;
  case M68kStackedContinuationKind::exception_continuation:
    if (config_.exception_continuations) result.edges.push_back({stacked, EdgeKind::exceptional, State::all_unknown()});
    break;
  case M68kStackedContinuationKind::pushed_code_address:
    if (config_.pushed_code_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, State::all_unknown()});
    break;
  case M68kStackedContinuationKind::none: break;
  }

  switch (control.dynamic) {
  case M68kDynamicControlFamily::none:
  case M68kDynamicControlFamily::return_from_subroutine:
  case M68kDynamicControlFamily::return_from_exception:
  case M68kDynamicControlFamily::return_restore_condition_codes: break;  // modelled through continuations
  case M68kDynamicControlFamily::jump_pc_index:
  case M68kDynamicControlFamily::call_pc_index:
    if (is_pc_index_site(operation)) {
      const auto report = evaluate_pc_index_site(pc, operation, in);
      if (report.outcome == M68kPcIndexOutcome::resolved) {
        State edge = out;
        edge.flag_setter.reset();
        for (const auto target : report.targets) result.edges.push_back({target, EdgeKind::computed, edge});
      } else {
        result.unresolved_computed = report.reason;
      }
      break;
    }
    result.unresolved_computed = UnknownReason::unsupported_transfer;
    break;
  default: result.unresolved_computed = UnknownReason::unsupported_transfer; break;
  }
  return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Driver.

M68kFiniteAnalysisResult analyze_m68k_finite_values(const M68kAnalysisImage &image, const std::vector<std::uint32_t> &entries,
                                                    M68kAnalysisConfig config, const analysis::Bounds &bounds) {
  std::vector<std::pair<std::uint64_t, M68kAnalysisState>> seeds;
  for (const auto entry : entries) seeds.emplace_back(entry & bus_mask, M68kAnalysisState::all_unknown());
  for (std::uint32_t restarts = 0;; ++restarts) {
    M68kFiniteAdapter adapter{image, config};
    M68kFiniteAnalysisResult out{};
    out.restarts = restarts;
    out.solution = analysis::solve(adapter, seeds, bounds);
    out.complete = out.solution.complete;
    out.reason = out.solution.reason;
    out.iterations = out.solution.iterations;
    if (!out.complete) return out;  // every query is Unknown(bound): no partial truth
    for (const auto &[point, reason] : out.solution.unresolved_computed) out.unresolved_computed.emplace(static_cast<std::uint32_t>(point), reason);
    std::set<std::uint32_t> invalidated;
    for (const auto &[point, state] : out.solution.in_states) {
      const auto pc = static_cast<std::uint32_t>(point);
      const auto decoded = adapter.decode(pc);
      if (!decoded) {
        out.undecodable.insert(pc);
        continue;
      }
      out.reached.emplace(pc, decoded->length);
      if (!is_pc_index_site(decoded->operation)) continue;
      auto report = adapter.evaluate_pc_index_site(pc, decoded->operation, state);
      // Lost computed targets are detected by the generic solver, which pins the site and restarts (ADR 0078 decision 2). A pinned
      // site is never reported resolved, even when the restarted solve's narrower input re-derives a precise set: the driver maps
      // it to `invalidated` and restarts once more with the site pinned at the adapter, so it emits nothing (SEG-029-T006).
      if (out.solution.pinned.contains(point) && !config.pinned_sites.contains(pc)) invalidated.insert(pc);
      out.pc_index_sites.emplace(pc, std::move(report));
    }
    if (invalidated.empty()) return out;
    config.pinned_sites.insert(invalidated.begin(), invalidated.end());  // monotone: terminates
  }
}

analysis::FiniteValue m68k_query_data_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg,
                                               unsigned width) {
  return result.solution.query(pc & bus_mask, [&](const M68kAnalysisState &state) {
    return state.values.values[m68k_analysis_slot(reg, width)];
  });
}

const char *m68k_pc_index_outcome_name(M68kPcIndexOutcome outcome) noexcept {
  switch (outcome) {
  case M68kPcIndexOutcome::resolved: return "resolved";
  case M68kPcIndexOutcome::index_unknown: return "index_unknown";
  case M68kPcIndexOutcome::address_register_index: return "address_register_index";
  case M68kPcIndexOutcome::entry_outside_immutable_image: return "entry_outside_immutable_image";
  case M68kPcIndexOutcome::target_outside_image: return "target_outside_image";
  case M68kPcIndexOutcome::empty_domain: return "empty_domain";
  case M68kPcIndexOutcome::width_only_domain: return "width_only_domain";
  case M68kPcIndexOutcome::invalidated: return "invalidated";
  }
  return "unknown";
}

std::string format_m68k_finite_analysis(const M68kFiniteAnalysisResult &result) {
  std::ostringstream out;
  out << "complete=" << (result.complete ? 1 : 0);
  if (!result.complete) {
    out << " reason=" << analysis::unknown_reason_name(result.reason) << '\n';
    return out.str();
  }
  out << " restarts=" << result.restarts << " iterations=" << result.iterations << '\n';
  for (const auto &[point, state] : result.solution.in_states) {
    out << hex(point);
    const auto found = result.reached.find(static_cast<std::uint32_t>(point));
    if (found == result.reached.end()) out << " undecodable";
    else out << " len=" << found->second;
    out << " flags=" << (state.flag_setter ? hex(*state.flag_setter) : std::string("-"));
    for (std::size_t slot = 0; slot < m68k_analysis_slot_count; ++slot)
      out << " d" << slot / 2U << (slot % 2U == 0U ? ".w=" : ".l=") << state.values.values[slot].describe()
          << (state.width_derived[slot] ? "~" : "");
    out << '\n';
  }
  for (const auto &[pc, site] : result.pc_index_sites) {
    out << "site " << hex(pc) << (site.call ? " jsr " : " jmp ") << m68k_pc_index_outcome_name(site.outcome);
    if (site.outcome != M68kPcIndexOutcome::resolved) out << " reason=" << analysis::unknown_reason_name(site.reason);
    out << " odd=" << site.odd_targets_excluded << " targets=";
    for (const auto target : site.targets) out << hex(target) << ',';
    out << '\n';
  }
  for (const auto &[pc, reason] : result.unresolved_computed)
    out << "unresolved " << hex(pc) << ' ' << analysis::unknown_reason_name(reason) << '\n';
  return out.str();
}

}  // namespace segarecomp
