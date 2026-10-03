// SEG-029-T003 (ADR 0078, report-only). See finite_adapter.hpp.

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"

#include <algorithm>
#include <bit>
#include <iomanip>
#include <sstream>
#include <utility>
#include <variant>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/effects.hpp"

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
    if (override_ && override_->first == reg) {
      // SEG-030-T004: the substituted source register carries a memory operand's precise value.
      const auto mask = width == 32U ? UINT64_C(0xFFFFFFFF) : UINT64_C(0xFFFF);
      std::vector<std::uint64_t> values;
      for (const auto v : override_->second.data.values()) values.push_back(v & mask);
      return to_cpu(FiniteValue::of(std::move(values)), override_->second.width_derived);
    }
    const auto slot = m68k_analysis_slot(reg, width);
    const auto &value = state_.values.values[slot];
    if (value.is_unknown()) unknown_read_ = unknown_read_ ? std::min(*unknown_read_, value.reason()) : value.reason();
    return to_cpu(value, state_.width_derived[slot]);
  }
  std::optional<std::uint32_t> immutable_read(std::uint32_t address, unsigned bytes) override {
    return image_.immutable_read(address, bytes);
  }
  void reset() { unknown_read_.reset(); }
  void override_register(unsigned reg, M68kCellValue value) { override_.emplace(reg, std::move(value)); }
  [[nodiscard]] std::optional<UnknownReason> unknown_read() const { return unknown_read_; }

private:
  const M68kAnalysisState &state_;
  const M68kAnalysisImage &image_;
  std::optional<UnknownReason> unknown_read_;
  std::optional<std::pair<unsigned, M68kCellValue>> override_;
};

bool is_address_site_family(M68kDynamicControlFamily family) {
  switch (family) {
  case M68kDynamicControlFamily::jump_address_indirect:
  case M68kDynamicControlFamily::call_address_indirect:
  case M68kDynamicControlFamily::jump_address_disp16:
  case M68kDynamicControlFamily::call_address_disp16:
  case M68kDynamicControlFamily::jump_address_index:
  case M68kDynamicControlFamily::call_address_index: return true;
  default: return false;
  }
}

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

// ---------------------------------------------------------------------------------------------------------------
// SEG-030-T003: address-domain helpers.

using Sub = M68kAnalysisSubReason;

std::uint32_t sext16(std::uint32_t value) {
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFFU)));
}

// A concrete set of 32-bit values (addresses, deltas, loaded entries), or a typed failure.
struct Concrete {
  bool ok{};
  std::vector<std::uint32_t> values;  // sorted distinct when ok
  bool width_derived{};
  UnknownReason reason{UnknownReason::unsupported_transfer};
  Sub sub{Sub::none};

  static Concrete fail(UnknownReason reason, Sub sub = Sub::none) {
    Concrete out;
    out.reason = reason;
    out.sub = sub;
    return out;
  }
  static Concrete of(std::vector<std::uint32_t> values, bool width_derived) {
    if (values.size() > m68k_address_enumeration_bound) return fail(UnknownReason::set_bound, Sub::set_bound);
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    Concrete out;
    out.ok = true;
    out.values = std::move(values);
    out.width_derived = width_derived;
    return out;
  }
};

// Dn as 32-bit values (`.W`: the low word sign extended, as an index or an address-register source uses it).
Concrete data_register_values(const M68kAnalysisState &state, unsigned reg, bool is_long) {
  const auto slot = m68k_analysis_slot(reg, is_long ? 32U : 16U);
  const auto &value = state.values.values[slot];
  if (value.is_unknown()) return Concrete::fail(value.reason());
  std::vector<std::uint32_t> out;
  for (const auto v : value.values()) out.push_back(is_long ? static_cast<std::uint32_t>(v) : sext16(static_cast<std::uint32_t>(v)));
  return Concrete::of(std::move(out), state.width_derived[slot]);
}

// An as 32-bit values; a strided set is never enumerated.
Concrete address_register_values(const M68kAnalysisState &state, unsigned reg, bool is_long) {
  const auto &pointer = state.address[reg & 7U];
  if (pointer.is_unknown()) return Concrete::fail(pointer.reason, pointer.sub);
  if (pointer.is_bottom()) return Concrete::of({}, false);
  const auto values = pointer.values();
  if (!values) return Concrete::fail(UnknownReason::set_bound, Sub::set_bound);
  std::vector<std::uint32_t> out;
  for (const auto v : *values) out.push_back(is_long ? v : sext16(v));
  return Concrete::of(std::move(out), pointer.width_derived);
}

Concrete index_values(const M68kAnalysisState &state, const M68kEffectiveAddress &ea) {
  return ea.index_is_address ? address_register_values(state, ea.index_reg, ea.index_is_long)
                             : data_register_values(state, ea.index_reg, ea.index_is_long);
}

// { a + b + constant } over the cartesian product (32-bit wrap), bounded.
Concrete sum(const Concrete &left, const Concrete &right, std::uint32_t constant) {
  if (!left.ok) return left;
  if (!right.ok) return right;
  if (left.values.size() * right.values.size() > m68k_address_enumeration_bound)
    return Concrete::fail(UnknownReason::set_bound, Sub::set_bound);
  std::vector<std::uint32_t> out;
  for (const auto a : left.values)
    for (const auto b : right.values) out.push_back(a + b + constant);
  return Concrete::of(std::move(out), left.width_derived || right.width_derived);
}

Concrete constant(std::uint32_t value) { return Concrete::of({value}, false); }

std::uint32_t access_bytes(M68kMemoryAccessWidth width) { return static_cast<std::uint32_t>(width); }

// The (An)+/-(An) step of one access: the operand size, except that a byte access through A7 keeps the stack word aligned.
std::uint32_t auto_update_step(unsigned reg, M68kMemoryAccessWidth width) {
  return width == M68kMemoryAccessWidth::byte && reg == 7U ? 2U : access_bytes(width);
}

// The 32-bit effective addresses of a memory/control operand accessed with `width`, from the input state.
Concrete effective_addresses(const M68kAnalysisState &state, const M68kEffectiveAddress &ea, M68kMemoryAccessWidth width) {
  const auto displacement = static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement));
  switch (ea.mode) {
  case M68kEaMode::absolute_word:
  case M68kEaMode::absolute_long: return constant(ea.absolute_address);
  case M68kEaMode::pc_disp16: return constant(ea.pc_base_address + displacement);
  case M68kEaMode::pc_index8: return sum(constant(ea.pc_base_address), index_values(state, ea), displacement);
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc: return address_register_values(state, ea.reg, true);
  case M68kEaMode::address_predec:
    return sum(address_register_values(state, ea.reg, true), constant(0U), 0U - auto_update_step(ea.reg, width));
  case M68kEaMode::address_disp16: return sum(address_register_values(state, ea.reg, true), constant(0U), displacement);
  case M68kEaMode::address_index8:
    return sum(address_register_values(state, ea.reg, true), index_values(state, ea), displacement);
  default: break;
  }
  return Concrete::fail(UnknownReason::unsupported_transfer);
}

// Kinds whose `size` is the width of every (An)+/-(An) operand access (the auto-update step is exact for them).
bool sized_auto_update_kind(M68kIrKind kind) {
  switch (kind) {
  case M68kIrKind::write_move:
  case M68kIrKind::write_movea:
  case M68kIrKind::write_clr:
  case M68kIrKind::test_operand:
  case M68kIrKind::compare:
  case M68kIrKind::compare_immediate:
  case M68kIrKind::compare_address:
  case M68kIrKind::compare_memory:
  case M68kIrKind::add:
  case M68kIrKind::add_address:
  case M68kIrKind::add_immediate:
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_address:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate: return true;
  default: return false;
  }
}

bool is_address_register(const M68kEffectiveAddress &ea) { return ea.mode == M68kEaMode::address_register && ea.reg < 8U; }

std::uint8_t auto_update_mask(const M68kIrOperation &operation) {
  std::uint8_t mask = 0U;
  for (const auto *ea : {&operation.source_ea, &operation.destination_ea})
    if ((ea->mode == M68kEaMode::address_postinc || ea->mode == M68kEaMode::address_predec) && ea->reg < 8U)
      mask |= static_cast<std::uint8_t>(1U << ea->reg);
  return mask;
}

// The address registers an operation may write on its normal (non-exception) successors. The effect owner's footprint is used
// when it is complete. For the lifted kinds whose footprint it does not claim complete, the An writes are stated here from the
// M68000PRM instruction entries (as the data owner states EXT's): LEA writes only its An; MOVEQ, DBcc, EXT, SWAP, MULx and DIVx write
// only a Dn (a divide exception enters a machine vector, never the fallthrough); JSR/BSR/PEA move only A7; MOVEM writes the
// loaded address registers and its base auto-update. Every other incomplete kind is assumed to write all of A0-A7. Decoded
// (An)+/-(An) auto-updates are always included.
std::uint8_t address_write_mask(const M68kIrOperation &operation) {
  const auto updates = auto_update_mask(operation);
  switch (operation.kind) {
  case M68kIrKind::jump_general:
  case M68kIrKind::general_branch:
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::branch_always_short:
  case M68kIrKind::no_operation:
  case M68kIrKind::write_moveq:
  case M68kIrKind::subtract_quick_long_d0:
  case M68kIrKind::dbcc_loop:
  case M68kIrKind::sign_extend_word:
  case M68kIrKind::sign_extend_long:
  case M68kIrKind::write_swap:
  case M68kIrKind::multiply_signed_word:
  case M68kIrKind::multiply_unsigned_word:
  case M68kIrKind::divide_signed_word:
  case M68kIrKind::divide_unsigned_word: return updates;
  case M68kIrKind::call_general:
  case M68kIrKind::bsr_call:
  case M68kIrKind::push_effective_address: return static_cast<std::uint8_t>(updates | 0x80U);
  case M68kIrKind::load_effective_address:
    return is_address_register(operation.destination_ea) ? static_cast<std::uint8_t>(updates | 1U << operation.destination_ea.reg)
                                                         : 0xFFU;
  case M68kIrKind::movem_transfer:
    return operation.movem_direction == M68kMovemDirection::memory_to_registers
               ? static_cast<std::uint8_t>(updates | (operation.movem_register_mask >> 8U))
               : updates;
  default: break;
  }
  const auto effect = m68k_operation_effect(operation);
  return effect.register_write_footprint_complete ? static_cast<std::uint8_t>(effect.address_register_write_mask | updates) : 0xFFU;
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

std::optional<M68kRegionExtent> M68kFlatAnalysisImage::region_of(std::uint32_t address) const {
  if (!contains(address)) return std::nullopt;
  return M68kRegionExtent{M68kRegionKind::image, 0U, base_, static_cast<std::uint32_t>(bytes_.size())};
}

// ---------------------------------------------------------------------------------------------------------------
// State lattice.

M68kAnalysisState M68kAnalysisState::all_unknown(UnknownReason reason) {
  M68kAnalysisState out;
  out.values = analysis::ValueVector<m68k_analysis_slot_count>::all_unknown(reason);
  out.address.fill(M68kPointsTo::unknown(reason));
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
  for (std::size_t i = 0; i < out.address.size(); ++i) out.address[i] = join(left.address[i], right.address[i]);
  out.memory = join(left.memory, right.memory);
  out.stack_delta = join(left.stack_delta, right.stack_delta, m68k_exact_offset_bound);
  return out;
}

bool leq(const M68kAnalysisState &left, const M68kAnalysisState &right) {
  if (!left.values.reachable) return true;
  if (!right.values.reachable) return false;
  if (!leq(left.values, right.values)) return false;
  for (std::size_t i = 0; i < m68k_analysis_slot_count; ++i)
    if (!right.values.values[i].is_unknown() && left.width_derived[i] && !right.width_derived[i]) return false;
  for (std::size_t i = 0; i < left.address.size(); ++i)
    if (!leq(left.address[i], right.address[i])) return false;
  if (!leq(left.memory, right.memory)) return false;
  if (!leq(left.stack_delta, right.stack_delta)) return false;
  return !right.flag_setter || left.flag_setter == right.flag_setter;
}

// ---------------------------------------------------------------------------------------------------------------
// SEG-030-T003: address registers.

namespace {

// Concrete register values -> points-to, classified by the machine's region extents.
M68kPointsTo classify(const M68kAnalysisImage &image, const Concrete &concrete) {
  if (!concrete.ok) return M68kPointsTo::unknown(concrete.reason, concrete.sub);
  std::map<M68kRegion, std::vector<std::uint32_t>> grouped;
  for (const auto value : concrete.values) {
    const auto extent = image.region_of(value & bus_mask);
    if (!extent) return M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::region_exit);
    const M68kRegion region{extent->kind, extent->id, (value & ~bus_mask) | extent->base, extent->size, extent->mirror};
    grouped[region].push_back(value - region.base);
  }
  std::vector<std::pair<M68kRegion, M68kOffsetSet>> pairs;
  for (auto &[region, offsets] : grouped) pairs.emplace_back(region, M68kOffsetSet::of(std::move(offsets)));
  return M68kPointsTo::of(std::move(pairs), concrete.width_derived);
}

// Values read from exact immutable image bytes at every address (`.W` sign extended to 32 bits, as MOVEA/ADDA use it). An odd
// word/long address raises an address error and is not a value; any non-immutable byte fails the whole read.
Concrete immutable_load(const M68kAnalysisImage &image, const Concrete &addresses, M68kMemoryAccessWidth width) {
  if (!addresses.ok) return addresses;
  const auto bytes = access_bytes(width);
  std::vector<std::uint32_t> out;
  for (const auto address : addresses.values) {
    if (bytes > 1U && (address & 1U) != 0U) continue;
    const auto read = image.immutable_read(address & bus_mask, bytes);
    if (!read) return Concrete::fail(UnknownReason::non_immutable_read);
    out.push_back(width == M68kMemoryAccessWidth::word ? sext16(*read) : *read);
  }
  return Concrete::of(std::move(out), addresses.width_derived);
}

// The 32-bit source operand of MOVEA/ADDA/SUBA (`.W` sign extended).
Concrete source_values(const M68kAnalysisImage &image, const M68kAnalysisState &state, const M68kEffectiveAddress &ea,
                       M68kMemoryAccessWidth width) {
  const bool is_long = width == M68kMemoryAccessWidth::long_word;
  switch (ea.mode) {
  case M68kEaMode::data_register: return data_register_values(state, ea.reg, is_long);
  case M68kEaMode::address_register: return address_register_values(state, ea.reg, is_long);
  case M68kEaMode::immediate: return constant(is_long ? ea.immediate_value : sext16(ea.immediate_value));
  default: return immutable_load(image, effective_addresses(state, ea, width), width);
  }
}

std::vector<std::int64_t> deltas_of(const Concrete &values, bool negate) {
  std::vector<std::int64_t> out;
  for (const auto value : values.values) {
    const auto delta = static_cast<std::int64_t>(static_cast<std::int32_t>(value));
    out.push_back(negate ? -delta : delta);
  }
  return out;
}

// `pointer` plus every delta, keeping the region and congruence.
M68kPointsTo add_deltas(const M68kPointsTo &pointer, const Concrete &deltas, bool negate) {
  if (!deltas.ok) return M68kPointsTo::unknown(deltas.reason, deltas.sub);
  auto out = m68k_points_to_add(pointer, deltas_of(deltas, negate));
  if (out.is_known() && deltas.width_derived) out.width_derived = true;
  return out;
}

// CMPA #imm,An + Bcc: restrict An on one edge. Exact sets are filtered through the shared subtraction/condition owners; a
// strided set only by an unsigned (or same-signed-half) interval bound. Returns false when no exact filter applies.
bool address_branch_filter(const M68kIrOperation &setter, const M68kIrOperation &branch, bool taken, unsigned reg,
                           M68kPointsTo &pointer) {
  if (!pointer.is_known() || branch.kind != M68kIrKind::general_branch || branch.condition == M68kCondition::always ||
      branch.condition == M68kCondition::never)
    return false;
  if (setter.kind != M68kIrKind::compare_address || !is_address_register(setter.destination_ea) ||
      setter.destination_ea.reg != reg || setter.source_ea.mode != M68kEaMode::immediate)
    return false;
  const auto immediate =
      setter.size == M68kMemoryAccessWidth::long_word ? setter.source_ea.immediate_value : sext16(setter.source_ea.immediate_value);
  const auto keeps = [&](std::uint32_t value) {
    const auto r = m68k_evaluate_subtraction(immediate, value, M68kMemoryAccessWidth::long_word);
    const auto ccr = static_cast<std::uint16_t>((r.negative ? 0x8U : 0U) | (r.zero ? 0x4U : 0U) | (r.overflow ? 0x2U : 0U) |
                                                (r.carry ? 0x1U : 0U));
    return M68kConditionSpecification::evaluate(branch.condition, ccr) == taken;
  };
  std::vector<std::pair<M68kRegion, M68kOffsetSet>> pairs;
  for (const auto &[region, offsets] : pointer.pairs) {
    if (!offsets.is_strided()) {
      std::vector<std::uint32_t> kept;
      for (const auto offset : offsets.exact())
        if (keeps(region.base + offset)) kept.push_back(offset);
      pairs.emplace_back(region, M68kOffsetSet::of(std::move(kept)));
      continue;
    }
    // Strided: the condition as an unsigned bound on the value, when the region and the immediate share a signed half.
    auto condition = branch.condition;
    const bool same_half = ((region.base ^ immediate) & UINT32_C(0x80000000)) == 0U &&
                           ((region.base ^ (region.base + region.limit())) & UINT32_C(0x80000000)) == 0U;
    switch (condition) {
    case M68kCondition::lt: condition = M68kCondition::cs; break;
    case M68kCondition::ge: condition = M68kCondition::cc; break;
    case M68kCondition::gt: condition = M68kCondition::hi; break;
    case M68kCondition::le: condition = M68kCondition::ls; break;
    default: break;
    }
    if (condition != branch.condition && !same_half) return false;
    if (!taken) {
      switch (condition) {
      case M68kCondition::cs: condition = M68kCondition::cc; break;
      case M68kCondition::cc: condition = M68kCondition::cs; break;
      case M68kCondition::hi: condition = M68kCondition::ls; break;
      case M68kCondition::ls: condition = M68kCondition::hi; break;
      default: return false;
      }
    }
    // Value bounds [lo, hi] (inclusive, 64-bit) kept by the condition, then as offsets.
    std::int64_t lo = 0, hi = INT64_C(0xFFFFFFFF);
    const auto imm = static_cast<std::int64_t>(immediate);
    switch (condition) {
    case M68kCondition::cs: hi = imm - 1; break;  // value < imm
    case M68kCondition::cc: lo = imm; break;      // value >= imm
    case M68kCondition::hi: lo = imm + 1; break;  // value > imm
    case M68kCondition::ls: hi = imm; break;      // value <= imm
    default: return false;
    }
    const auto base = static_cast<std::int64_t>(region.base);
    const auto lo_offset = std::max<std::int64_t>(lo - base, 0);
    const auto hi_offset = std::min<std::int64_t>(hi - base, region.limit());
    if (lo_offset > hi_offset) continue;  // no member survives
    pairs.emplace_back(region, offsets.restricted(static_cast<std::uint32_t>(lo_offset), static_cast<std::uint32_t>(hi_offset)));
  }
  pointer = M68kPointsTo::of(std::move(pairs), pointer.width_derived);
  return true;
}

}  // namespace

void M68kFiniteAdapter::transfer_address_registers(const M68kIrOperation &operation, const State &in, State &out) const {
  const std::uint8_t written = address_write_mask(operation);
  if (written == 0U) return;
  std::array<std::optional<M68kPointsTo>, 8> next{};

  // (An)+ / -(An) auto-updates (accumulated: CMPM (Ay)+,(Ax)+ on one register steps it twice).
  std::array<std::int64_t, 8> step{};
  std::uint8_t stepped = 0U;
  bool exact_steps = true;
  const auto note = [&](const M68kEffectiveAddress &ea, std::int64_t amount) {
    stepped |= static_cast<std::uint8_t>(1U << ea.reg);
    step[ea.reg] += amount;
  };
  if (operation.kind == M68kIrKind::movem_transfer) {
    const auto count = static_cast<std::int64_t>(std::popcount(operation.movem_register_mask));
    const auto bytes = static_cast<std::int64_t>(access_bytes(operation.size));
    const bool to_memory = operation.movem_direction == M68kMovemDirection::registers_to_memory;
    const auto &ea = to_memory ? operation.destination_ea : operation.source_ea;
    if (ea.reg < 8U && ea.mode == M68kEaMode::address_predec && to_memory) note(ea, -count * bytes);
    if (ea.reg < 8U && ea.mode == M68kEaMode::address_postinc && !to_memory) {
      // A base register that is also loaded is not an exact pointer here.
      if (((operation.movem_register_mask >> (8U + ea.reg)) & 1U) == 0U) note(ea, count * bytes);
    }
  } else {
    for (const auto *ea : {&operation.source_ea, &operation.destination_ea}) {
      if ((ea->mode != M68kEaMode::address_postinc && ea->mode != M68kEaMode::address_predec) || ea->reg >= 8U) continue;
      if (!sized_auto_update_kind(operation.kind)) {
        exact_steps = false;
        continue;
      }
      const auto amount = static_cast<std::int64_t>(auto_update_step(ea->reg, operation.size));
      note(*ea, ea->mode == M68kEaMode::address_postinc ? amount : -amount);
    }
  }
  if (exact_steps) {
    for (unsigned reg = 0; reg < 8U; ++reg)
      if (((stepped >> reg) & 1U) != 0U) next[reg] = m68k_points_to_add(in.address[reg], {step[reg]});
  }

  // Explicit address-register destinations (computed from the input state; an auto-updated destination is not exact).
  const auto destination = [&](unsigned reg, M68kPointsTo value) {
    next[reg] = ((stepped >> reg) & 1U) != 0U ? M68kPointsTo::unknown(UnknownReason::unsupported_transfer) : std::move(value);
  };
  const auto &src = operation.source_ea;
  const auto &dst = operation.destination_ea;
  switch (operation.kind) {
  case M68kIrKind::load_effective_address:
    if (!is_address_register(dst)) break;
    switch (src.mode) {
    case M68kEaMode::address_indirect: destination(dst.reg, in.address[src.reg & 7U]); break;
    case M68kEaMode::address_disp16:
      destination(dst.reg, add_deltas(in.address[src.reg & 7U], constant(static_cast<std::uint32_t>(src.displacement)), false));
      break;
    case M68kEaMode::address_index8:
      destination(dst.reg, add_deltas(in.address[src.reg & 7U],
                                      sum(index_values(in, src), constant(0U), static_cast<std::uint32_t>(src.displacement)),
                                      false));
      break;
    default: destination(dst.reg, classify(image_, effective_addresses(in, src, M68kMemoryAccessWidth::long_word))); break;
    }
    break;
  case M68kIrKind::write_movea:
    if (!is_address_register(dst)) break;
    if (src.mode == M68kEaMode::address_register && operation.size == M68kMemoryAccessWidth::long_word) {
      destination(dst.reg, in.address[src.reg & 7U]);
      break;
    }
    if (config_.domains.memory && m68k_memory_mode(src.mode)) {
      // SEG-030-T004: a code or data pointer stored in work-RAM cells (an object field) feeds the address domain.
      bool tracked = false;
      const auto read = read_memory_operand(in, src, access_bytes(operation.size), &tracked);
      if (tracked) {
        if (!read.known) {
          destination(dst.reg, M68kPointsTo::unknown(read.reason, read.sub));
        } else if (read.value.is_pointer()) {
          // Pointers are only stored in long cells, which a word read never matches.
          destination(dst.reg, read.value.pointer);
        } else {
          std::vector<std::uint32_t> values;
          for (const auto v : read.value.data.values()) {
            const auto value = static_cast<std::uint32_t>(v);
            values.push_back(operation.size == M68kMemoryAccessWidth::word ? sext16(value) : value);
          }
          destination(dst.reg, classify(image_, Concrete::of(std::move(values), read.value.width_derived)));
        }
        break;
      }
    }
    destination(dst.reg, classify(image_, source_values(image_, in, src, operation.size)));
    break;
  case M68kIrKind::add_address:
  case M68kIrKind::subtract_address:
  case M68kIrKind::add_quick:
  case M68kIrKind::subtract_quick: {
    if (!is_address_register(dst)) break;
    const bool negate = operation.kind == M68kIrKind::subtract_address || operation.kind == M68kIrKind::subtract_quick;
    const bool quick = operation.kind == M68kIrKind::add_quick || operation.kind == M68kIrKind::subtract_quick;
    // ADDQ/SUBQ to An operate on the whole register whatever the size; ADDA/SUBA sign extend a word source.
    const auto delta = quick ? constant(src.immediate_value) : source_values(image_, in, src, operation.size);
    destination(dst.reg, add_deltas(in.address[dst.reg], delta, negate));
    break;
  }
  case M68kIrKind::exchange_registers: {
    const auto value_of = [&](const M68kEffectiveAddress &ea) -> std::optional<M68kPointsTo> {
      if (is_address_register(ea)) return in.address[ea.reg];
      if (ea.mode == M68kEaMode::data_register && ea.reg < 8U) return classify(image_, data_register_values(in, ea.reg, true));
      return std::nullopt;
    };
    const auto from_src = value_of(src);
    const auto from_dst = value_of(dst);
    if (!from_src || !from_dst) break;
    if (is_address_register(src)) destination(src.reg, *from_dst);
    if (is_address_register(dst)) destination(dst.reg, *from_src);
    break;
  }
  default: break;
  }

  for (unsigned reg = 0; reg < 8U; ++reg) {
    if (((written >> reg) & 1U) == 0U) continue;
    out.address[reg] = next[reg] ? std::move(*next[reg]) : M68kPointsTo::unknown(UnknownReason::unsupported_transfer);
  }
}

M68kAddressSiteReport M68kFiniteAdapter::evaluate_address_site(std::uint32_t pc, const M68kIrOperation &operation,
                                                               M68kDynamicControlFamily family, const State &in) const {
  M68kAddressSiteReport out{};
  out.family = family;
  const auto finish = [&](UnknownReason reason, Sub sub) {
    out.resolved = false;
    out.reason = reason;
    out.sub = sub;
    out.targets.clear();
    return out;
  };
  if (config_.pinned_sites.contains(pc)) return finish(UnknownReason::unsupported_transfer, Sub::invalidated);
  const auto &ea = operation.source_ea;
  const auto &base = in.address[ea.reg & 7U];
  if (base.is_unknown()) return finish(base.reason, base.sub == Sub::none ? Sub::base_unknown : base.sub);
  if (base.is_known() && !base.is_exact()) return finish(UnknownReason::set_bound, Sub::set_bound);  // never enumerated
  Concrete targets = address_register_values(in, ea.reg, true);
  const auto displacement = static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement));
  if (ea.mode == M68kEaMode::address_disp16) targets = sum(targets, constant(0U), displacement);
  else if (ea.mode == M68kEaMode::address_index8) targets = sum(targets, index_values(in, ea), displacement);
  if (!targets.ok) return finish(targets.reason, targets.sub);
  if (targets.width_derived && !config_.accept_width_domains) return finish(UnknownReason::unsupported_transfer, Sub::width_only);
  std::set<std::uint32_t> exact;
  for (const auto value : targets.values) {
    const auto target = value & bus_mask;
    if ((target & 1U) == 1U) {
      ++out.odd_targets_excluded;  // an odd JMP/JSR target raises an address error: never a normal target
      continue;
    }
    if (!image_.mapped(target)) return finish(UnknownReason::non_immutable_read, Sub::target_outside_image);
    exact.insert(target);
  }
  if (exact.empty()) return finish(UnknownReason::unsupported_transfer, Sub::none);
  out.targets.assign(exact.begin(), exact.end());
  out.resolved = true;
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// SEG-030-T004: abstract memory.

namespace {

bool is_auto_update(const M68kEffectiveAddress &ea) {
  return (ea.mode == M68kEaMode::address_postinc || ea.mode == M68kEaMode::address_predec) && ea.reg < 8U;
}

bool uses_address_register(const M68kEffectiveAddress &ea, unsigned reg) {
  switch (ea.mode) {
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc:
  case M68kEaMode::address_predec:
  case M68kEaMode::address_disp16: return ea.reg == reg;
  case M68kEaMode::address_index8: return ea.reg == reg || (ea.index_is_address && ea.index_reg == reg);
  default: return false;
  }
}

M68kCellValue data_value(std::vector<std::uint64_t> values, bool width_derived) {
  M68kCellValue out;
  out.data = FiniteValue::of(std::move(values));
  out.width_derived = out.data.is_precise() && width_derived;
  return out;
}

std::uint64_t byte_mask(std::uint32_t bytes) { return bytes >= 4U ? UINT64_C(0xFFFFFFFF) : (UINT64_C(1) << (8U * bytes)) - 1U; }

// Kinds whose data-register result reads the source operand through the CPU data owner's `read_source`.
bool memory_source_kind(M68kIrKind kind) {
  switch (kind) {
  case M68kIrKind::write_move:
  case M68kIrKind::add:
  case M68kIrKind::subtract:
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_or: return true;
  default: return false;
  }
}

}  // namespace

M68kAnalysisState M68kFiniteAdapter::entry_state(bool continuation) const {
  auto state = State::all_unknown();
  if (config_.domains.memory) state.memory.absent = continuation ? Sub::store_poison : Sub::initial_memory;
  if (config_.domains.contexts && !continuation) state.stack_delta = FiniteValue::of({0U});
  return state;
}

// SEG-030-T005: the generic reason of an opaque continuation's CPU sub-reason.
namespace {
UnknownReason continuation_reason(Sub sub) {
  switch (sub) {
  case Sub::context_bound: return UnknownReason::state_bound;
  case Sub::stack_unbalanced: return UnknownReason::unsupported_transfer;
  default: return UnknownReason::unknown_input;
  }
}
}  // namespace

M68kAnalysisState M68kFiniteAdapter::opaque_continuation(Sub sub) const {
  if (sub == Sub::none) return entry_state(true);
  auto state = State::all_unknown(continuation_reason(sub));
  state.address.fill(M68kPointsTo::unknown(continuation_reason(sub), sub));
  if (config_.domains.memory) state.memory.absent = sub;
  return state;
}

namespace {
// The synthetic extent the stack delta is carried in through the CPU-owned A7 transfer (never a machine region: it is only ever
// read back from a scratch state, never stored, read or reported).
constexpr std::uint32_t delta_origin = UINT32_C(0x00800000);
const M68kRegion delta_region{M68kRegionKind::io_device, UINT32_C(0xFFFFFFFF), UINT32_C(0x40000000), UINT32_C(0x01000000), 0U};

// Kinds after which the stack delta is not tracked: an SR write may switch between the supervisor and user stack pointers (the
// active A7 changes); LINK/UNLK move A7 through another address register.
bool stack_delta_untracked(M68kIrKind kind) {
  switch (kind) {
  case M68kIrKind::write_status_register:
  case M68kIrKind::logical_immediate_to_sr:
  case M68kIrKind::link_frame:
  case M68kIrKind::unlink_frame: return true;
  default: return false;
  }
}
}  // namespace

FiniteValue M68kFiniteAdapter::stack_delta_after(const M68kIrOperation &operation, const State &in) const {
  const auto &delta = in.stack_delta;
  if (!delta.is_precise()) return delta;
  if (stack_delta_untracked(operation.kind)) return FiniteValue::unknown(UnknownReason::unsupported_transfer);
  // PEA pushes 4 bytes (the address transfer has no exact A7 effect for it); a call's own push is undone by its continuation, and its
  // callee starts a fresh delta.
  if (operation.kind == M68kIrKind::push_effective_address)
    return delta.map([](std::uint64_t v) { return (v - 4U) & UINT64_C(0xFFFFFFFF); });
  if ((address_write_mask(operation) & 0x80U) == 0U || operation.kind == M68kIrKind::call_general ||
      operation.kind == M68kIrKind::bsr_call)
    return delta;
  // Every other A7 writer goes through the CPU-owned address transfer, with A7 a synthetic exact pointer at the delta.
  std::vector<std::uint32_t> offsets;
  for (const auto v : delta.values()) {
    const auto signed_delta = static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v)));
    if (signed_delta < -static_cast<std::int64_t>(delta_origin) || signed_delta >= static_cast<std::int64_t>(delta_origin))
      return FiniteValue::unknown(UnknownReason::unsupported_transfer);
    offsets.push_back(static_cast<std::uint32_t>(static_cast<std::int64_t>(delta_origin) + signed_delta));
  }
  State scratch = in;
  scratch.address[7] = M68kPointsTo::of({{delta_region, M68kOffsetSet::of(std::move(offsets))}});
  State after = scratch;
  transfer_address_registers(operation, scratch, after);
  const auto &a7 = after.address[7];
  if (!a7.is_known() || !a7.is_exact() || a7.pairs.size() != 1U || a7.pairs.front().first != delta_region)
    return FiniteValue::unknown(UnknownReason::unsupported_transfer);
  std::vector<std::uint64_t> out;
  for (const auto offset : a7.pairs.front().second.exact()) out.push_back((offset - delta_origin) & UINT32_C(0xFFFFFFFF));
  return FiniteValue::of(std::move(out), m68k_exact_offset_bound);
}

// The 32-bit register values (as region points-to) an operand addresses; `predecrement` is the -(An) step.
M68kPointsTo M68kFiniteAdapter::operand_address(const State &in, const M68kEffectiveAddress &ea, std::uint32_t predecrement) const {
  const auto displacement = static_cast<std::int64_t>(ea.displacement);
  const auto &base = in.address[ea.reg & 7U];
  switch (ea.mode) {
  case M68kEaMode::absolute_word:
  case M68kEaMode::absolute_long: return classify(image_, constant(ea.absolute_address));
  case M68kEaMode::pc_disp16:
    return classify(image_, constant(ea.pc_base_address + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement))));
  case M68kEaMode::pc_index8:
    return classify(image_, sum(constant(ea.pc_base_address), index_values(in, ea),
                                static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement))));
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc: return base;
  case M68kEaMode::address_predec: return m68k_points_to_add(base, {-static_cast<std::int64_t>(predecrement)});
  case M68kEaMode::address_disp16: return m68k_points_to_add(base, {displacement});
  case M68kEaMode::address_index8: {
    const auto index = index_values(in, ea);
    if (!index.ok) return base.is_known() ? M68kPointsTo::unknown(index.reason, index.sub) : base;
    std::vector<std::int64_t> deltas;
    for (const auto value : index.values) deltas.push_back(static_cast<std::int64_t>(static_cast<std::int32_t>(value)) + displacement);
    auto out = m68k_points_to_add(base, deltas);
    if (out.is_known() && index.width_derived) out.width_derived = true;
    return out;
  }
  default: break;
  }
  return M68kPointsTo::unknown(UnknownReason::unsupported_transfer);
}

M68kMemoryRead M68kFiniteAdapter::read_memory_operand(const State &in, const M68kEffectiveAddress &ea, std::uint32_t bytes,
                                                      bool *tracked) const {
  if (tracked != nullptr) *tracked = false;
  M68kMemoryRead failed;
  const auto fail = [&](UnknownReason reason, Sub sub) {
    failed.known = false;
    failed.reason = reason;
    failed.sub = sub;
    return failed;
  };
  const auto width = bytes == 1U ? M68kMemoryAccessWidth::byte : bytes == 2U ? M68kMemoryAccessWidth::word : M68kMemoryAccessWidth::long_word;
  const auto targets = operand_address(in, ea, auto_update_step(ea.reg, width));
  if (targets.is_unknown()) {
    if (tracked != nullptr) *tracked = true;  // the address may name work RAM
    return fail(targets.reason, targets.sub == Sub::none ? Sub::base_unknown : targets.sub);
  }
  if (!targets.is_known()) return fail(UnknownReason::unsupported_transfer, Sub::none);
  std::optional<M68kCellValue> result;
  const auto add = [&](const M68kCellValue &value) {
    if (!result) result = value;
    else result = m68k_cell_join(*result, value, bytes);
    return result.has_value();
  };
  std::vector<std::pair<M68kRegion, M68kOffsetSet>> cells;
  for (const auto &[region, offsets] : targets.pairs) {
    if (m68k_memory_tracked(region.kind)) {
      cells.emplace_back(region, offsets);
      continue;
    }
    if (region.kind != M68kRegionKind::image) return fail(UnknownReason::unsupported_transfer, Sub::none);  // device registers
    if (offsets.is_strided()) return fail(UnknownReason::set_bound, Sub::set_bound);
    std::vector<std::uint64_t> values;
    for (const auto offset : offsets.exact()) {
      const auto address = region.base + offset;
      if (bytes > 1U && (address & 1U) != 0U) continue;  // an address error, never a value
      const auto read = image_.immutable_read(address & bus_mask, bytes);
      if (!read) return fail(UnknownReason::non_immutable_read, Sub::none);
      values.push_back(*read);
    }
    if (values.empty()) continue;
    if (!add(data_value(std::move(values), targets.width_derived))) return fail(UnknownReason::set_bound, Sub::set_bound);
  }
  if (!cells.empty()) {
    if (tracked != nullptr) *tracked = true;
    auto pointer = M68kPointsTo::of(std::move(cells), targets.width_derived);
    // Word and long cells are aligned: odd members raise an address error and are no value (an exact set is filtered).
    if (bytes > 1U && pointer.is_exact()) {
      std::vector<std::pair<M68kRegion, M68kOffsetSet>> even;
      for (const auto &[region, offsets] : pointer.pairs) {
        std::vector<std::uint32_t> kept;
        for (const auto offset : offsets.exact())
          if (((region.base + offset) & 1U) == 0U) kept.push_back(offset);
        even.emplace_back(region, M68kOffsetSet::of(std::move(kept)));
      }
      pointer = M68kPointsTo::of(std::move(even), pointer.width_derived);
    }
    if (pointer.is_known()) {
      auto read = m68k_memory_read(in.memory, pointer, bytes, config_.memory.policy);
      if (!read.known) return read;
      if (!add(read.value)) return fail(UnknownReason::set_bound, Sub::set_bound);
    }
  }
  if (!result) return fail(UnknownReason::unsupported_transfer, Sub::none);
  M68kMemoryRead out;
  out.known = true;
  out.value = std::move(*result);
  return out;
}

std::optional<M68kCellValue> M68kFiniteAdapter::operand_value(const State &in, const M68kEffectiveAddress &ea, std::uint32_t bytes) const {
  const auto mask = byte_mask(bytes);
  switch (ea.mode) {
  case M68kEaMode::immediate: return data_value({ea.immediate_value & mask}, false);
  case M68kEaMode::data_register: {
    const auto slot = m68k_analysis_slot(ea.reg & 7U, bytes == 4U ? 32U : 16U);
    const auto &value = in.values.values[slot];
    if (!value.is_precise()) return std::nullopt;
    std::vector<std::uint64_t> values;
    for (const auto v : value.values()) values.push_back(v & mask);
    return data_value(std::move(values), in.width_derived[slot]);
  }
  case M68kEaMode::address_register: {
    const auto &pointer = in.address[ea.reg & 7U];
    if (!pointer.is_known()) return std::nullopt;
    if (bytes == 4U) {
      M68kCellValue out;
      out.pointer = pointer;
      return out;
    }
    const auto values = pointer.values();
    if (!values) return std::nullopt;
    std::vector<std::uint64_t> low;
    for (const auto v : *values) low.push_back(v & mask);
    return data_value(std::move(low), pointer.width_derived);
  }
  default: break;
  }
  if (!m68k_memory_mode(ea.mode)) return std::nullopt;
  auto read = read_memory_operand(in, ea, bytes);
  if (!read.known) return std::nullopt;
  return std::move(read.value);
}

std::vector<std::pair<M68kPointsTo, std::uint32_t>> M68kFiniteAdapter::memory_write_targets(const M68kIrOperation &operation,
                                                                                            const State &in) const {
  std::vector<std::pair<M68kPointsTo, std::uint32_t>> out;
  const auto writes = m68k_memory_writes(operation);
  if (!writes.described) {
    out.emplace_back(M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::store_poison), 4U);
    return out;
  }
  for (const auto &write : writes.writes) {
    switch (write.target) {
    case M68kMemoryWrite::Target::unknown:
      out.emplace_back(M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::store_poison), write.span);
      break;
    case M68kMemoryWrite::Target::push:
      out.emplace_back(m68k_points_to_add(in.address[7], {-static_cast<std::int64_t>(write.span)}), write.span);
      break;
    case M68kMemoryWrite::Target::destination: {
      const auto &source = operation.source_ea;
      const auto &destination = operation.destination_ea;
      // The source operand is evaluated first: its (An)+/-(An) update is visible to the destination's address (MOVE (A0)+,(A0)).
      State effective = in;
      if (is_auto_update(source) && uses_address_register(destination, source.reg) && operation.kind != M68kIrKind::movem_transfer) {
        const auto step = static_cast<std::int64_t>(auto_update_step(source.reg, operation.size));
        effective.address[source.reg] =
            m68k_points_to_add(in.address[source.reg], {source.mode == M68kEaMode::address_postinc ? step : -step});
      }
      const auto predecrement =
          operation.kind == M68kIrKind::movem_transfer ? write.span : auto_update_step(destination.reg, operation.size);
      out.emplace_back(operand_address(effective, destination, predecrement), write.span);
      break;
    }
    }
  }
  return out;
}

void M68kFiniteAdapter::transfer_memory(const M68kIrOperation &operation, std::uint32_t next, const State &in, State &out) const {
  const auto writes = m68k_memory_writes(operation);
  const auto targets = memory_write_targets(operation, in);
  if (!writes.described) {
    out.memory.poison_all();
    return;
  }
  for (std::size_t i = 0; i < writes.writes.size(); ++i) {
    const auto &write = writes.writes[i];
    std::optional<M68kCellValue> value;
    switch (write.value) {
    case M68kMemoryWrite::Value::unknown: break;
    case M68kMemoryWrite::Value::zero: value = data_value({0U}, false); break;
    case M68kMemoryWrite::Value::return_address: value = data_value({next}, false); break;
    case M68kMemoryWrite::Value::effective_address: {
      const auto addresses = effective_addresses(in, operation.source_ea, M68kMemoryAccessWidth::long_word);
      if (addresses.ok) value = data_value(std::vector<std::uint64_t>(addresses.values.begin(), addresses.values.end()), addresses.width_derived);
      break;
    }
    case M68kMemoryWrite::Value::source: value = operand_value(in, operation.source_ea, write.span); break;
    }
    if (value && !value->is_pointer() && !value->data.is_precise()) value.reset();
    m68k_memory_store(out.memory, targets[i].first, targets[i].second, value, config_.memory.policy);
  }
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
  const auto context = m68k_point_context(point);  // always 0 unless the contexts domain is enabled
  const auto decoded = decode(pc);
  if (!decoded) return result;  // odd, unmapped or rejected: reached, no successor
  const auto &operation = decoded->operation;
  const auto next = (pc + decoded->length) & bus_mask;

  // Register values after the instruction, each slot through the CPU semantic owner.
  State out = in;
  out.flag_setter.reset();
  StateInputs inputs{in, image_};
  // SEG-030-T003: LEA and PEA only compute an address (M68000PRM: no data register is written). The data owner reports them as
  // writing every Dn (their footprint is not claimed complete); with the address domain enabled the data registers are kept.
  const bool address_only = config_.domains.address && (operation.kind == M68kIrKind::load_effective_address ||
                                                        operation.kind == M68kIrKind::push_effective_address);
  // SEG-030-T004: a memory source operand naming precise work-RAM cells is handed to the data owner as a substituted source register
  // carrying the cells' value (the owner's own semantics then apply unchanged). Without precise cells the owner sees the original
  // operation (a byte from mutable memory stays its width-only 0..255 domain).
  const M68kIrOperation *data_operation = &operation;
  M68kIrOperation substituted;
  if (config_.domains.memory && memory_source_kind(operation.kind) && operation.destination_ea.mode == M68kEaMode::data_register &&
      m68k_memory_mode(operation.source_ea.mode)) {
    bool tracked = false;
    const auto bytes = access_bytes(operation.size);
    auto read = read_memory_operand(in, operation.source_ea, bytes, &tracked);
    std::optional<M68kCellValue> value;
    if (read.known && tracked) {
      if (read.value.is_pointer()) {
        if (const auto values = read.value.pointer.values()) {
          std::vector<std::uint64_t> data;
          for (const auto v : *values) data.push_back(v & byte_mask(bytes));
          value = data_value(std::move(data), read.value.pointer.width_derived);
        }
      } else {
        value = std::move(read.value);
      }
    }
    if (value && value->data.is_precise()) {
      substituted = operation;
      substituted.source_ea.mode = M68kEaMode::data_register;
      substituted.source_ea.reg = static_cast<std::uint8_t>((operation.destination_ea.reg + 1U) & 7U);
      const auto original_effect = m68k_operation_effect(operation);
      const auto substituted_effect = m68k_operation_effect(substituted);
      if (original_effect.register_write_footprint_complete && substituted_effect.register_write_footprint_complete &&
          original_effect.data_register_write_mask == substituted_effect.data_register_write_mask) {
        inputs.override_register(substituted.source_ea.reg, std::move(*value));
        data_operation = &substituted;
      }
    }
  }
  for (unsigned reg = 0; reg < 8U && !address_only; ++reg) {
    for (const auto width : widths) {
      inputs.reset();
      const auto transfer = m68k_finite_register_after(*data_operation, reg, width, inputs);
      if (!transfer.writes) continue;
      const auto reason = transfer.immutable_read_failed ? UnknownReason::non_immutable_read
                                                         : inputs.unknown_read().value_or(UnknownReason::unsupported_transfer);
      store(out, m68k_analysis_slot(reg, width), transfer.immutable_read_failed ? M68kFiniteValues::unknown() : transfer.values,
            reason);
    }
  }
  if (config_.domains.address) transfer_address_registers(operation, in, out);
  if (config_.domains.memory) transfer_memory(operation, next, in, out);
  if (config_.domains.contexts) {
    out.stack_delta = stack_delta_after(operation, in);
    // The call frame (M68000PRM JSR/BSR/PEA: A7 - 4, then the push). The address domain alone leaves A7 Unknown there (T003, kept
    // byte-identical); the contexts domain tracks it so a callee is entered, and its summary returns, at an exact frame.
    if (operation.kind == M68kIrKind::call_general || operation.kind == M68kIrKind::bsr_call ||
        operation.kind == M68kIrKind::push_effective_address)
      out.address[7] = m68k_points_to_add(in.address[7], {-4});
  }

  const auto control = m68k_control_successors(operation);
  // SEG-030-T005: a call enters its callee in the call-site context (context 0 when the callee is merged past K) with a fresh
  // stack delta; every other edge stays in the current context. Without the contexts domain every point is its PC.
  const bool call = control.stacked == M68kStackedContinuationKind::call_continuation;
  std::vector<std::uint32_t> callees;
  const auto edge_target = [&](std::uint32_t target, bool enters_callee, State &edge) -> std::uint64_t {
    if (!config_.domains.contexts) return target;
    if (!enters_callee) return m68k_analysis_point(context, target);
    callees.push_back(target);
    edge.stack_delta = FiniteValue::of({0U});
    return m68k_analysis_point(config_.contexts.merged.contains(target) ? 0U : m68k_call_context(pc), target);
  };
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
      if (config_.domains.address)
        for (unsigned reg = 0; reg < 8U; ++reg) (void)address_branch_filter(setter->operation, operation, taken, reg, edge.address[reg]);
      break;
    }
    case M68kControlSuccessorKind::call_target: kind = EdgeKind::call; break;
    }
    const auto point_target = edge_target(target, successor.kind == M68kControlSuccessorKind::call_target, edge);
    result.edges.push_back({point_target, kind, std::move(edge)});
  }

  // Computed targets (resolved below) of a call are callees as well. They are emitted after the stacked continuation (the T004 edge
  // order), but evaluated first: the continuation depends on the callees.
  std::vector<analysis::Edge<State>> computed;
  const auto computed_edges = [&](const std::vector<std::uint32_t> &targets) {
    State edge = out;
    edge.flag_setter.reset();
    for (const auto target : targets) {
      State target_edge = edge;
      const auto point_target = edge_target(target, call, target_edge);
      computed.push_back({point_target, EdgeKind::computed, std::move(target_edge)});
    }
  };
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
        computed_edges(report.targets);
      } else {
        result.unresolved_computed = report.reason;
      }
      break;
    }
    result.unresolved_computed = UnknownReason::unsupported_transfer;
    break;
  case M68kDynamicControlFamily::jump_address_indirect:
  case M68kDynamicControlFamily::call_address_indirect:
  case M68kDynamicControlFamily::jump_address_disp16:
  case M68kDynamicControlFamily::call_address_disp16:
  case M68kDynamicControlFamily::jump_address_index:
  case M68kDynamicControlFamily::call_address_index:
    if (config_.domains.address) {
      const auto report = evaluate_address_site(pc, operation, control.dynamic, in);
      if (report.resolved) {
        computed_edges(report.targets);
      } else {
        result.unresolved_computed.emplace(report.reason);
      }
      break;
    }
    result.unresolved_computed = UnknownReason::unsupported_transfer;
    break;
  default: result.unresolved_computed = UnknownReason::unsupported_transfer; break;
  }

  const auto stacked = control.stacked_address & bus_mask;
  const auto stacked_point = m68k_analysis_point(context, stacked);
  switch (control.stacked) {
  case M68kStackedContinuationKind::call_continuation:
    if (config_.domains.contexts) {
      if (config_.call_continuations) result.edges.push_back({stacked_point, EdgeKind::return_edge, continuation(pc, callees, in)});
      break;
    }
    if (config_.call_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, entry_state(true)});
    break;
  case M68kStackedContinuationKind::exception_continuation:
    if (config_.exception_continuations) result.edges.push_back({stacked_point, EdgeKind::exceptional, entry_state(true)});
    break;
  case M68kStackedContinuationKind::pushed_code_address:
    if (config_.pushed_code_continuations) result.edges.push_back({stacked_point, EdgeKind::return_edge, entry_state(true)});
    break;
  case M68kStackedContinuationKind::none: break;
  }
  for (auto &edge : computed) result.edges.push_back(std::move(edge));
  return result;
}

// SEG-030-T005: the continuation state of the call at `pc` whose callees in this transfer are `callees`.
M68kAnalysisState M68kFiniteAdapter::continuation(std::uint32_t pc, const std::vector<std::uint32_t> &callees, const State &in) const {
  State state;
  const auto &contexts = config_.contexts;
  const auto context = m68k_call_context(pc);
  if (callees.empty()) {
    state = opaque_continuation(Sub::none);  // an unresolved callee: unknown effect
  } else if (std::any_of(callees.begin(), callees.end(), [&](std::uint32_t callee) { return contexts.merged.contains(callee); })) {
    state = opaque_continuation(Sub::context_bound);
  } else if (const auto summary = contexts.summaries.find(context); summary != contexts.summaries.end()) {
    state = summary->second;
  } else {
    const auto opaque = contexts.opaque.find(context);
    state = opaque_continuation(opaque == contexts.opaque.end() ? Sub::none : opaque->second);
  }
  state.flag_setter.reset();
  // The caller's own stack delta: a balanced callee restores A7 (an unbalanced one makes its callers unproven, see the driver).
  state.stack_delta = in.stack_delta;
  return state;
}

// ---------------------------------------------------------------------------------------------------------------
// Driver.

namespace {

bool site_resolved(const M68kPcIndexSiteReport &site) { return site.outcome == M68kPcIndexOutcome::resolved; }
bool site_resolved(const M68kAddressSiteReport &site) { return site.resolved; }

// SEG-030-T005: one PC reached in several contexts.
template <typename Report>
void merge_site(std::map<std::uint32_t, Report> &sites, std::uint32_t pc, Report report) {
  const auto [found, inserted] = sites.emplace(pc, report);
  if (inserted) return;
  auto &kept = found->second;
  if (!site_resolved(kept)) return;
  if (!site_resolved(report)) {
    kept = std::move(report);
    return;
  }
  std::set<std::uint32_t> targets(kept.targets.begin(), kept.targets.end());
  targets.insert(report.targets.begin(), report.targets.end());
  kept.targets.assign(targets.begin(), targets.end());
  kept.odd_targets_excluded = std::max(kept.odd_targets_excluded, report.odd_targets_excluded);
}

// One solve with the solver-owned pin-and-restart plus the driver's adapter pins (ADR 0078 decision 2; SEG-029-T006).
M68kFiniteAnalysisResult solve_pinned(const M68kAnalysisImage &image, const std::vector<std::uint32_t> &entries, M68kAnalysisConfig config,
                                      const analysis::Bounds &bounds) {
  for (std::uint32_t restarts = 0;; ++restarts) {
    M68kFiniteAdapter adapter{image, config};
    std::vector<std::pair<std::uint64_t, M68kAnalysisState>> seeds;
    for (const auto entry : entries) seeds.emplace_back(entry & bus_mask, adapter.entry_state(false));
    M68kFiniteAnalysisResult out{};
    out.restarts = restarts;
    out.address_domain = config.domains.address;
    out.solution = analysis::solve(adapter, seeds, bounds);
    out.complete = out.solution.complete;
    out.reason = out.solution.reason;
    out.iterations = out.solution.iterations;
    if (!out.complete) return out;  // every query is Unknown(bound): no partial truth
    // Per-PC results: one point per PC without the contexts domain; with it, a PC's site is resolved only when it resolves in every
    // context that reaches it (the union of the targets), else the first unresolved context's outcome (point order) is reported.
    for (const auto &[point, reason] : out.solution.unresolved_computed) out.unresolved_computed.emplace(m68k_point_pc(point), reason);
    std::set<std::uint32_t> invalidated;
    for (const auto &[point, state] : out.solution.in_states) {
      const auto pc = m68k_point_pc(point);
      const auto decoded = adapter.decode(pc);
      if (!decoded) {
        out.undecodable.insert(pc);
        continue;
      }
      out.reached.emplace(pc, decoded->length);
      if (config.domains.address) {
        const auto family = m68k_control_successors(decoded->operation).dynamic;
        if (is_address_site_family(family)) {
          auto report = adapter.evaluate_address_site(pc, decoded->operation, family, state);
          if (!config.pinned_sites.contains(pc) && out.solution.pinned.contains(point)) invalidated.insert(pc);
          merge_site(out.address_sites, pc, std::move(report));
          continue;
        }
      }
      if (!is_pc_index_site(decoded->operation)) continue;
      auto report = adapter.evaluate_pc_index_site(pc, decoded->operation, state);
      // Lost computed targets are detected by the generic solver, which pins the site and restarts (ADR 0078 decision 2). A pinned
      // site is never reported resolved, even when the restarted solve's narrower input re-derives a precise set: the driver maps
      // it to `invalidated` and restarts once more with the site pinned at the adapter, so it emits nothing (SEG-029-T006).
      if (out.solution.pinned.contains(point) && !config.pinned_sites.contains(pc)) invalidated.insert(pc);
      merge_site(out.pc_index_sites, pc, std::move(report));
    }
    if (invalidated.empty()) return out;
    config.pinned_sites.insert(invalidated.begin(), invalidated.end());  // monotone: terminates
  }
}

// True when a store of `span` bytes at `targets` may touch a release range.
bool may_release(const M68kPointsTo &targets, std::uint32_t span, const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges) {
  if (ranges.empty() || targets.is_bottom()) return false;  // no release range: no external bus master is modelled
  if (!targets.is_known()) return true;
  for (const auto &[region, offsets] : targets.pairs) {
    const std::uint64_t lo = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.lo();
    const std::uint64_t hi = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.hi() + span;
    for (const auto &[first, last] : ranges)
      if (lo < last && first < hi) return true;
  }
  return false;
}

// The policy the solution itself requires (ADR 0079 decisions 7 and 9) plus the memory statistics of the run.
M68kMemoryPolicy derive_memory_policy(const M68kAnalysisImage &image, const M68kAnalysisConfig &config, M68kFiniteAnalysisResult &result) {
  M68kMemoryPolicy derived;
  derived.async_all = config.memory.interrupts;
  auto &report = result.memory;
  report = M68kMemoryReport{};
  report.enabled = true;
  report.assumed_no_external_writer = config.memory.assume_no_external_writer;
  M68kFiniteAdapter adapter{image, config};
  const auto &states = result.solution.in_states;
  // Code reachable from the handler roots over every edge of the final solution (call, continuation and computed edges included).
  std::set<std::uint64_t> handler;
  std::vector<std::uint64_t> pending;
  for (const auto root : config.memory.handler_roots)
    if (states.contains(root & bus_mask) && handler.insert(root & bus_mask).second) pending.push_back(root & bus_mask);
  while (!pending.empty()) {
    const auto point = pending.back();
    pending.pop_back();
    for (const auto &edge : adapter.transfer(point, states.at(point)).edges)
      if (states.contains(edge.target) && handler.insert(edge.target).second) pending.push_back(edge.target);
  }
  report.handler_points = handler.size();
  bool release = false;
  // Memory-source reads per PC: precise only when precise in every context (one point per PC without the contexts domain).
  std::map<std::uint32_t, std::optional<std::pair<UnknownReason, M68kAnalysisSubReason>>> reads;
  for (const auto &[point, state] : states) {
    report.max_cells = std::max(report.max_cells, state.memory.cells.size());
    const auto decoded = adapter.decode(m68k_point_pc(point));
    if (!decoded) continue;
    const auto &operation = decoded->operation;
    if (!m68k_memory_writes(operation).described) ++report.undescribed_writers;
    const bool in_handler = handler.contains(point);
    bool stores = false;
    for (const auto &[targets, span] : adapter.memory_write_targets(operation, state)) {
      if (targets.is_bottom()) continue;
      stores = true;
      if (!targets.is_known()) ++report.unknown_target_stores;
      if (may_release(targets, span, config.memory.release_ranges)) {
        ++report.release_stores;
        release = true;
      }
      if (!in_handler) continue;
      const auto touched = m68k_memory_touched(targets, span);
      if (!touched) derived.async_all = true;
      else
        for (const auto &range : *touched) derived.add_async(range);
    }
    if (stores && in_handler) ++report.handler_store_sites;
    // Memory-source operands that may read work RAM: precise or Unknown by generic reason x CPU sub-reason.
    if (m68k_memory_mode(operation.source_ea.mode) && operation.kind != M68kIrKind::load_effective_address &&
        operation.kind != M68kIrKind::push_effective_address && operation.kind != M68kIrKind::jump_general &&
        operation.kind != M68kIrKind::call_general) {
      bool tracked = false;
      const auto read = adapter.read_memory_operand(state, operation.source_ea, access_bytes(operation.size), &tracked);
      if (tracked) {
        const auto [found, inserted] = reads.emplace(m68k_point_pc(point), std::nullopt);
        if (!read.known && !found->second) found->second = std::make_pair(read.reason, read.sub);
        (void)inserted;
      }
    }
  }
  for (const auto &[pc, read] : reads) {
    (void)pc;
    if (!read) ++report.precise_reads;
    else ++report.unknown_reads[*read];
  }
  report.release_store = release;
  if (release && !config.memory.assume_no_external_writer) derived.external_writer = true;
  return derived;
}


// ---------------------------------------------------------------------------------------------------------------
// SEG-030-T005: call contexts and callee summaries (ADR 0079 decisions 6 and 9).

// An activation: a call-site context (its key is the context) or a callee merged into context 0 (key `merged_key | entry`).
constexpr std::uint64_t merged_key = UINT64_C(1) << 32U;

struct PointFacts {
  std::vector<std::uint64_t> intra;    // successors inside the activation (every edge except a call's callee edges)
  std::vector<std::uint64_t> callees;  // callee entry points of a call
  bool call{};
  bool unknown_effect{};  // control may leave the analysed flow: unresolved computed control, an unmodelled exception
                          // continuation (TRAP/TRAPV), an exception-raising or undecodable instruction
  bool unbalanced{};      // RTS not at the entry stack delta, or RTE/RTR
  bool exit{};            // RTS at the entry stack delta
};

struct Activation {
  std::vector<std::uint64_t> points;
  std::set<std::uint64_t> nested;
  std::optional<Sub> fail;  // why its returns are not proven (none: unknown effect)
  bool balanced{};
  bool recursive{};
};

std::uint64_t activation_of(std::uint64_t callee_point) {
  const auto context = m68k_point_context(callee_point);
  return context == 0U ? merged_key | m68k_point_pc(callee_point) : context;
}

// The facts of every reached point, from one transfer of its final input state.
std::map<std::uint64_t, PointFacts> point_facts(M68kFiniteAdapter &adapter, const M68kAnalysisConfig &config,
                                                const M68kFiniteAnalysisResult &result) {
  std::map<std::uint64_t, PointFacts> out;
  const auto zero = FiniteValue::of({0U});
  for (const auto &[point, state] : result.solution.in_states) {
    auto &facts = out[point];
    const auto decoded = adapter.decode(m68k_point_pc(point));
    if (!decoded) {
      facts.unknown_effect = true;
      continue;
    }
    const auto control = m68k_control_successors(decoded->operation);
    facts.call = control.stacked == M68kStackedContinuationKind::call_continuation;
    // A pushed code address is not an escape by itself: a return through it is an RTS away from the entry stack delta.
    if (control.always_raises_exception || result.solution.unresolved_computed.contains(point) ||
        (control.stacked == M68kStackedContinuationKind::exception_continuation && !config.exception_continuations))
      facts.unknown_effect = true;
    if (control.dynamic == M68kDynamicControlFamily::return_from_subroutine) {
      if (state.stack_delta == zero) facts.exit = true;
      else facts.unbalanced = true;
    }
    if (control.dynamic == M68kDynamicControlFamily::return_from_exception ||
        control.dynamic == M68kDynamicControlFamily::return_restore_condition_codes)
      facts.unbalanced = true;
    for (const auto &edge : adapter.transfer(point, state).edges) {
      if (facts.call && (edge.kind == EdgeKind::call || edge.kind == EdgeKind::computed)) facts.callees.push_back(edge.target);
      else facts.intra.push_back(edge.target);
    }
  }
  return out;
}

struct ContextDerivation {
  M68kContextConfig next;  // the derived configuration (merges grown, summaries joined where the used ones were exceeded)
  bool valid{};            // every used summary is above the derived one and no callee exceeds K
};

// The summary of an exit (an RTS at the entry stack delta): the state after the RTS pops the return address.
M68kAnalysisState exit_state(const M68kAnalysisState &in) {
  auto out = in;
  out.address[7] = m68k_points_to_add(in.address[7], {4});
  out.flag_setter.reset();
  out.stack_delta = FiniteValue::bottom();
  return out;
}

// The precision content of a summary: every Unknown is the same top, whatever its generic reason or CPU sub-reason (those are labels
// of why a value is Unknown, which can change between rounds without any change of precision).
M68kAnalysisState precision_view(const M68kAnalysisState &state) {
  auto out = state;
  for (auto &value : out.values.values)
    if (value.is_unknown()) value = FiniteValue::unknown(UnknownReason::unknown_input);
  for (auto &pointer : out.address)
    if (pointer.is_unknown()) pointer = M68kPointsTo::unknown(UnknownReason::unknown_input);
  out.memory.absent = Sub::store_poison;
  return out;
}

ContextDerivation derive_contexts(const M68kAnalysisImage &image, const M68kAnalysisConfig &config, M68kFiniteAnalysisResult &result) {
  M68kFiniteAdapter adapter{image, config};
  const auto &states = result.solution.in_states;
  const auto facts = point_facts(adapter, config, result);
  const auto &used = config.contexts;
  auto &report = result.contexts;
  const auto rounds = report.rounds;
  report = M68kContextReport{};
  report.enabled = true;
  report.rounds = rounds;

  // Activations: every call-site context, and every merged callee (walked from its context-0 entry over intra-activation edges).
  std::map<std::uint64_t, Activation> activations;
  std::map<std::uint32_t, std::set<std::uint32_t>> contexts_of;  // callee entry -> call-site contexts
  for (const auto &[point, fact] : facts) {
    if (m68k_point_context(point) != 0U) activations[m68k_point_context(point)].points.push_back(point);
    for (const auto callee : fact.callees) {
      if (m68k_point_context(callee) != 0U) contexts_of[m68k_point_pc(callee)].insert(m68k_point_context(callee));
      else activations[activation_of(callee)];
    }
  }
  for (auto &[key, activation] : activations) {
    if ((key & merged_key) == 0U) continue;
    std::set<std::uint64_t> seen{m68k_analysis_point(0U, static_cast<std::uint32_t>(key))};
    std::vector<std::uint64_t> pending(seen.begin(), seen.end());
    while (!pending.empty()) {
      const auto point = pending.back();
      pending.pop_back();
      const auto found = facts.find(point);
      if (found == facts.end()) continue;
      for (const auto target : found->second.intra)
        if (seen.insert(target).second) pending.push_back(target);
    }
    activation.points.assign(seen.begin(), seen.end());
  }
  // Local facts.
  for (auto &[key, activation] : activations) {
    (void)key;
    bool unbalanced = false, unknown = false;
    for (const auto point : activation.points) {
      const auto found = facts.find(point);
      if (found == facts.end()) continue;
      const auto &fact = found->second;
      unbalanced = unbalanced || fact.unbalanced;
      unknown = unknown || fact.unknown_effect;
      for (const auto callee : fact.callees) activation.nested.insert(activation_of(callee));
    }
    if (unbalanced) activation.fail = Sub::stack_unbalanced;
    else if (unknown) activation.fail = Sub::none;
  }
  // Balance over the activation graph: strongly connected components (Tarjan, iterative), each emitted after the components it
  // reaches. A recursive activation (a cycle) is never proven: Unknown(context_bound).
  {
    std::map<std::uint64_t, std::size_t> index, low;
    std::set<std::uint64_t> on_stack;
    std::vector<std::uint64_t> stack;
    std::size_t counter = 0U;
    const auto finish_component = [&](std::uint64_t root) {
      std::vector<std::uint64_t> component;
      for (;;) {
        const auto node = stack.back();
        stack.pop_back();
        on_stack.erase(node);
        component.push_back(node);
        if (node == root) break;
      }
      const bool cyclic = component.size() > 1U || activations.at(root).nested.contains(root);
      for (const auto node : component) {
        auto &activation = activations.at(node);
        activation.recursive = cyclic;
        if (!activation.fail && cyclic) activation.fail = Sub::context_bound;
        if (!activation.fail) {
          for (const auto nested : activation.nested) {
            const auto &callee = activations.at(nested);
            if (callee.balanced) continue;
            const auto why = callee.fail.value_or(Sub::none);
            activation.fail = activation.fail ? std::max(*activation.fail, why) : why;
          }
        }
        activation.balanced = !activation.fail;
      }
    };
    for (const auto &[start, unused] : activations) {
      (void)unused;
      if (index.contains(start)) continue;
      // Frames of (node, iterator position into its nested set).
      std::vector<std::pair<std::uint64_t, std::set<std::uint64_t>::const_iterator>> frames;
      const auto enter = [&](std::uint64_t node) {
        index[node] = low[node] = counter++;
        stack.push_back(node);
        on_stack.insert(node);
        frames.emplace_back(node, activations.at(node).nested.begin());
      };
      enter(start);
      while (!frames.empty()) {
        auto &[node, it] = frames.back();
        const auto &nested = activations.at(node).nested;
        if (it != nested.end()) {
          const auto next = *it++;
          if (!index.contains(next)) {
            enter(next);
          } else if (on_stack.contains(next)) {
            low[node] = std::min(low[node], index[next]);
          }
          continue;
        }
        const auto done = node;
        frames.pop_back();
        if (!frames.empty()) low[frames.back().first] = std::min(low[frames.back().first], low[done]);
        if (low[done] == index[done]) finish_component(done);
      }
    }
  }

  ContextDerivation out;
  out.next.merged = used.merged;
  out.next.round_bound = used.round_bound;
  bool valid = true;
  for (const auto &[callee, contexts] : contexts_of) {
    report.max_contexts_per_callee = std::max(report.max_contexts_per_callee, contexts.size());
    if (contexts.size() > m68k_context_bound) {
      out.next.merged.insert(callee);  // merged into context 0 next round (ADR 0079 decision 6)
      valid = false;
    }
  }
  for (const auto &[key, activation] : activations) {
    ++report.activations;
    if (activation.balanced) ++report.balanced_activations;
    if (activation.recursive) ++report.recursive_activations;
    if ((key & merged_key) != 0U) continue;
    const auto context = static_cast<std::uint32_t>(key);
    ++report.contexts;
    std::optional<M68kAnalysisState> summary;
    if (activation.balanced) {
      for (const auto point : activation.points) {
        if (!facts.at(point).exit) continue;
        const auto exit = exit_state(states.at(point));
        summary = summary ? join(*summary, exit) : exit;
      }
    }
    const auto previous = used.summaries.find(context);
    if (!summary) {
      const auto sub = activation.balanced ? Sub::none : activation.fail.value_or(Sub::none);
      out.next.opaque.emplace(context, sub);
      ++report.unproven[sub];
      if (previous != used.summaries.end()) valid = false;  // a used summary is no longer proven
      continue;
    }
    ++report.summaries;
    if (previous == used.summaries.end()) {
      out.next.summaries.emplace(context, std::move(*summary));
      continue;
    }
    const auto derived = precision_view(*summary);
    const auto applied = precision_view(previous->second);
    if (!leq(derived, applied)) {
      valid = false;  // the used summary is below what its own solution derives (stale): grow it
      out.next.summaries.emplace(context, join(previous->second, *summary));
    } else {
      // Valid: the more precise derived summary is the next candidate (kept as is when only its Unknown labels differ).
      out.next.summaries.emplace(context, derived == applied ? previous->second : std::move(*summary));
    }
  }
  out.valid = valid;
  report.merged_callees = out.next.merged.size();
  // The continuations this round applied.
  for (const auto &[point, fact] : facts) {
    if (!fact.call) continue;
    const auto context = m68k_call_context(m68k_point_pc(point));
    if (fact.callees.empty()) ++report.opaque_continuations[Sub::none];
    else if (std::any_of(fact.callees.begin(), fact.callees.end(), [](std::uint64_t c) { return m68k_point_context(c) == 0U; }))
      ++report.opaque_continuations[Sub::context_bound];
    else if (used.summaries.contains(context)) ++report.summary_continuations;
    else {
      const auto opaque = used.opaque.find(context);
      ++report.opaque_continuations[opaque == used.opaque.end() ? Sub::none : opaque->second];
    }
  }
  return out;
}

// The callee entries with more than K distinct call sites in a context-free solution (the starting merges).
std::set<std::uint32_t> seed_merged(const M68kAnalysisImage &image, const M68kAnalysisConfig &config,
                                    const M68kFiniteAnalysisResult &result) {
  M68kFiniteAdapter adapter{image, config};
  const auto facts = point_facts(adapter, config, result);
  std::map<std::uint32_t, std::set<std::uint32_t>> sites;
  for (const auto &[point, fact] : facts)
    for (const auto callee : fact.callees) sites[m68k_point_pc(callee)].insert(m68k_point_pc(point));
  std::set<std::uint32_t> out;
  for (const auto &[callee, callers] : sites)
    if (callers.size() > m68k_context_bound) out.insert(callee);
  return out;
}

std::set<std::uint32_t> resolved_sites(const M68kFiniteAnalysisResult &result) {
  std::set<std::uint32_t> out;
  for (const auto &[pc, site] : result.pc_index_sites)
    if (site.outcome == M68kPcIndexOutcome::resolved) out.insert(pc);
  for (const auto &[pc, site] : result.address_sites)
    if (site.resolved) out.insert(pc);
  return out;
}

M68kFiniteAnalysisResult analyze_memory(const M68kAnalysisImage &image, const std::vector<std::uint32_t> &entries,
                                        M68kAnalysisConfig config, const analysis::Bounds &bounds);

// ADR 0079 decisions 6 and 9 (SEG-030-T005). The T004 memory result (contexts off) is the comparator and the starting point (its
// memory policy and the callees with more than K call sites). Each round solves under the current configuration and derives the
// summaries, merges and memory policy its own solution implies. A round is valid when every summary it used is above the derived
// one, no callee exceeds K and the derived memory policy is below the one it used: its configuration is then a post-fixed point,
// so its solution is sound. The next configuration takes the derived summaries (a used summary that was exceeded is joined with
// the derived one, never kept), the grown merges and the joined policy. The run stops when a valid round reproduces its own
// configuration; at R rounds it returns the latest valid round; with no valid round (or an incomplete solve and no valid round) the
// contexts domain is switched off and the T004 result is returned (reason recorded).
M68kFiniteAnalysisResult analyze_contexts(const M68kAnalysisImage &image, const std::vector<std::uint32_t> &entries,
                                          M68kAnalysisConfig config, const analysis::Bounds &bounds) {
  auto free_config = config;
  free_config.domains.contexts = false;
  auto memory = analyze_memory(image, entries, free_config, bounds);
  const auto finish_without = [&](UnknownReason reason, std::uint32_t rounds, std::size_t iterations) {
    memory.contexts = M68kContextReport{};
    memory.contexts.enabled = true;
    memory.contexts.reason = reason;
    memory.contexts.rounds = rounds;
    memory.contexts.total_iterations = iterations;
    return std::move(memory);
  };
  if (!memory.complete) return finish_without(memory.reason, 0U, 0U);
  if (!memory.memory.converged) return finish_without(UnknownReason::iteration_bound, 0U, 0U);
  const auto memory_rounds = memory.memory.rounds;
  config.domains.address = true;
  config.domains.memory = true;
  config.memory.policy = memory.memory.policy;
  const auto seeded = seed_merged(image, free_config, memory);
  config.contexts.merged.insert(seeded.begin(), seeded.end());
  const auto round_bound = std::min(config.contexts.round_bound, m68k_memory_round_bound);
  std::optional<M68kFiniteAnalysisResult> best;
  std::size_t iterations = 0U;
  bool converged = false;
  UnknownReason failure = UnknownReason::iteration_bound;
  std::uint32_t round = 1U;
  for (; round <= round_bound; ++round) {
    auto out = solve_pinned(image, entries, config, bounds);
    iterations += out.iterations;
    if (!out.complete) {
      failure = out.reason;
      break;
    }
    const auto policy = derive_memory_policy(image, config, out);
    out.contexts.rounds = round;
    const auto derivation = derive_contexts(image, config, out);
    const bool valid = derivation.valid && leq(policy, config.memory.policy);
    out.memory.rounds = memory_rounds + round;
    out.memory.policy = config.memory.policy;
    out.memory.converged = valid;
    auto next = config;
    next.memory.policy = join(config.memory.policy, policy);
    next.contexts = derivation.next;
    const bool fixed = next.contexts == config.contexts && next.memory.policy == config.memory.policy;
    if (valid) {
      out.contexts.validated = true;
      out.contexts.returned_round = round;
      best = std::move(out);
      if (fixed) {
        converged = true;
        break;
      }
    }
    config = std::move(next);
  }
  if (!best) return finish_without(failure, std::min(round, round_bound), iterations);
  auto out = std::move(*best);
  out.contexts.converged = converged;
  out.contexts.rounds = std::min(round, round_bound);
  out.contexts.total_iterations = iterations;
  out.contexts.discovered_without_contexts = memory.reached.size();
  const auto with = resolved_sites(out);
  const auto without = resolved_sites(memory);
  for (const auto pc : with) out.contexts.sites_resolved_only_with_contexts += without.contains(pc) ? 0U : 1U;
  for (const auto pc : without) out.contexts.sites_resolved_only_without_contexts += with.contains(pc) ? 0U : 1U;
  out.contexts.unresolved_sites = out.unresolved_computed.size();
  return out;
}

}  // namespace

M68kFiniteAnalysisResult analyze_m68k_finite_values(const M68kAnalysisImage &image, const std::vector<std::uint32_t> &entries,
                                                    M68kAnalysisConfig config, const analysis::Bounds &bounds) {
  if (config.domains.contexts) return analyze_contexts(image, entries, std::move(config), bounds);
  return analyze_memory(image, entries, std::move(config), bounds);
}

namespace {

M68kFiniteAnalysisResult analyze_memory(const M68kAnalysisImage &image, const std::vector<std::uint32_t> &entries,
                                        M68kAnalysisConfig config, const analysis::Bounds &bounds) {
  if (!config.domains.memory) return solve_pinned(image, entries, std::move(config), bounds);
  // ADR 0079 decision 9: monotone driver rounds over the memory policy, at most R rounds; the final round's derived policy must be
  // below the one it ran with (final validation). Non-convergence switches the memory domain off.
  config.domains.address = true;
  config.memory.policy.async_all = config.memory.policy.async_all || config.memory.interrupts;
  for (std::uint32_t round = 1U; round <= m68k_memory_round_bound; ++round) {
    auto out = solve_pinned(image, entries, config, bounds);
    if (!out.complete) {
      out.memory.enabled = true;
      out.memory.rounds = round;
      out.memory.policy = config.memory.policy;
      return out;
    }
    const auto derived = derive_memory_policy(image, config, out);
    out.memory.rounds = round;
    out.memory.policy = config.memory.policy;
    if (leq(derived, config.memory.policy)) {
      out.memory.converged = true;
      return out;
    }
    config.memory.policy = join(config.memory.policy, derived);
  }
  auto fallback = config;
  fallback.domains.memory = false;
  auto out = solve_pinned(image, entries, fallback, bounds);
  out.memory.enabled = true;
  out.memory.converged = false;
  out.memory.rounds = m68k_memory_round_bound;
  out.memory.policy = config.memory.policy;
  return out;
}

}  // namespace

std::vector<std::pair<std::uint64_t, const M68kAnalysisState *>> m68k_points_of(const M68kFiniteAnalysisResult &result,
                                                                                std::uint32_t pc) {
  std::vector<std::pair<std::uint64_t, const M68kAnalysisState *>> out;
  for (const auto &[point, state] : result.solution.in_states)
    if (m68k_point_pc(point) == (pc & bus_mask)) out.emplace_back(point, &state);
  return out;
}

analysis::FiniteValue m68k_query_data_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg,
                                               unsigned width) {
  return result.solution.query(pc & bus_mask, [&](const M68kAnalysisState &state) {
    return state.values.values[m68k_analysis_slot(reg, width)];
  });
}

M68kPointsTo m68k_query_address_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg) {
  if (!result.solution.complete) return M68kPointsTo::unknown(result.solution.reason);
  const auto found = result.solution.in_states.find(pc & bus_mask);
  if (found == result.solution.in_states.end()) return M68kPointsTo::bottom();
  return found->second.address[reg & 7U];
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
    const auto found = result.reached.find(m68k_point_pc(point));
    if (found == result.reached.end()) out << " undecodable";
    else out << " len=" << found->second;
    out << " flags=" << (state.flag_setter ? hex(*state.flag_setter) : std::string("-"));
    for (std::size_t slot = 0; slot < m68k_analysis_slot_count; ++slot)
      out << " d" << slot / 2U << (slot % 2U == 0U ? ".w=" : ".l=") << state.values.values[slot].describe()
          << (state.width_derived[slot] ? "~" : "");
    if (result.address_domain)
      for (unsigned reg = 0; reg < 8U; ++reg) out << " a" << reg << '=' << state.address[reg].describe();
    if (result.memory.enabled) out << ' ' << state.memory.describe();
    if (result.contexts.enabled) out << " delta=" << state.stack_delta.describe();
    out << '\n';
  }
  for (const auto &[pc, site] : result.pc_index_sites) {
    out << "site " << hex(pc) << (site.call ? " jsr " : " jmp ") << m68k_pc_index_outcome_name(site.outcome);
    if (site.outcome != M68kPcIndexOutcome::resolved) out << " reason=" << analysis::unknown_reason_name(site.reason);
    out << " odd=" << site.odd_targets_excluded << " targets=";
    for (const auto target : site.targets) out << hex(target) << ',';
    out << '\n';
  }
  for (const auto &[pc, site] : result.address_sites) {
    out << "address-site " << hex(pc) << ' ' << (site.resolved ? "resolved" : "unknown");
    if (!site.resolved)
      out << " reason=" << analysis::unknown_reason_name(site.reason) << '/' << m68k_analysis_sub_reason_name(site.sub);
    out << " odd=" << site.odd_targets_excluded << " targets=";
    for (const auto target : site.targets) out << hex(target) << ',';
    out << '\n';
  }
  for (const auto &[pc, reason] : result.unresolved_computed)
    out << "unresolved " << hex(pc) << ' ' << analysis::unknown_reason_name(reason) << '\n';
  if (result.memory.enabled) {
    const auto &memory = result.memory;
    out << "memory rounds=" << memory.rounds << " converged=" << (memory.converged ? 1 : 0)
        << " external=" << (memory.policy.external_writer ? 1 : 0) << " async_all=" << (memory.policy.async_all ? 1 : 0)
        << " async_ranges=" << memory.policy.async.size() << " release_stores=" << memory.release_stores
        << " unknown_target_stores=" << memory.unknown_target_stores << " handler_points=" << memory.handler_points
        << " max_cells=" << memory.max_cells << " precise_reads=" << memory.precise_reads << '\n';
  }
  if (result.contexts.enabled) {
    const auto &contexts = result.contexts;
    out << "contexts rounds=" << contexts.rounds << " returned=" << contexts.returned_round
        << " validated=" << (contexts.validated ? 1 : 0) << " converged=" << (contexts.converged ? 1 : 0)
        << " contexts=" << contexts.contexts << " merged=" << contexts.merged_callees
        << " activations=" << contexts.activations << " balanced=" << contexts.balanced_activations
        << " recursive=" << contexts.recursive_activations << " summaries=" << contexts.summaries
        << " summary_continuations=" << contexts.summary_continuations;
    for (const auto &[sub, n] : contexts.opaque_continuations) out << " opaque_" << m68k_analysis_sub_reason_name(sub) << '=' << n;
    out << '\n';
  }
  return out.str();
}

}  // namespace segarecomp
