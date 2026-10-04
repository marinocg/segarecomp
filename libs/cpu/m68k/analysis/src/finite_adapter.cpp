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
  out.status = join(left.status, right.status);
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
  if (!leq(left.status, right.status)) return false;
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

void M68kFiniteAdapter::transfer_address_registers(const M68kIrOperation &operation, const State &in, State &out,
                                                   const M68kMemoryPolicy *policy) const {
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
      const auto read = read_memory_with(policy != nullptr ? *policy : config_.memory.policy, in, src, access_bytes(operation.size),
                                         &tracked);
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

M68kFiniteAdapter::M68kFiniteAdapter(const M68kAnalysisImage &image, M68kAnalysisConfig config)
    : image_(image), config_(std::move(config)) {
  if (!config_.domains.frames) return;
  for (const auto &[tag, policy] : config_.frames.policies) tag_policies_.emplace(tag, join(config_.memory.policy, policy));
  // The partition of the handlers without an analysed instance (never taken, or taken but unanalysed) has no eligibility, preemption
  // or resumption analysis: every cell is asynchronous there (the T004 consequence).
  auto every_cell = config_.memory.policy;
  every_cell.async_all = true;
  tag_policies_[m68k_dead_handler_tag] = every_cell;
  // The unknown-entry partition (handlers entered from a taking point whose state is not modelled): every cell is asynchronous too
  // (this subsumes any writer derived for it).
  tag_policies_[m68k_unknown_entry_tag] = every_cell;
}

M68kAnalysisState M68kFiniteAdapter::entry_state(bool continuation, std::uint32_t tag) const {
  auto state = State::all_unknown();
  if (config_.domains.memory) state.memory.absent = continuation ? Sub::store_poison : Sub::initial_memory;
  // SEG-030-T005 (B1): a root starts its activation at delta 0. A continuation built here (a resuming exception's or a pushed code
  // address's) is reached only after code the activation does not summarise (a handler ending in RTE, an RTS through a pushed
  // address) whose effect on A7 is never proven: its delta is Unknown, never bottom (bottom would join away and let an RTS on
  // that path pass as balanced). A call continuation instead receives the caller's delta from `continuation`, because an
  // unproven callee makes its callers unproven through the activation graph.
  if (config_.domains.contexts)
    state.stack_delta = continuation ? FiniteValue::unknown(UnknownReason::unsupported_transfer) : FiniteValue::of({0U});
  // SEG-030-T006: a root's status is Unknown unless it is the reset entry (root_state); an opaque continuation's status is its
  // partition's status bound (closure premise, ADR 0079 decision 8).
  if (config_.domains.frames) state.status = continuation ? status_bound(tag) : FiniteValue::unknown(UnknownReason::unknown_input);
  return state;
}

FiniteValue M68kFiniteAdapter::status_bound(std::uint32_t tag) const {
  // The dead-handler partition has no entry status (its roots start Unknown): its bound is Unknown.
  if (tag != 0U && !config_.frames.instances.contains(tag)) return FiniteValue::unknown(UnknownReason::unknown_input);
  const auto found = config_.frames.status_bounds.find(tag);
  return found == config_.frames.status_bounds.end() ? FiniteValue::bottom() : found->second;
}

M68kAnalysisState M68kFiniteAdapter::root_state(std::uint32_t tag, std::uint32_t pc) const {
  auto state = entry_state(false);
  if (!config_.domains.frames) return state;
  if (tag == 0U) {
    // M68000PRM reset: S = 1, T = 0, I = 7; SSP <- the long at vector 0 (the machine supplies it for a reset-entry program only).
    if (config_.frames.reset_entry && *config_.frames.reset_entry == (pc & bus_mask)) {
      state.status = FiniteValue::of({m68k_reset_status});
      if (config_.frames.reset_ssp) state.address[7] = classify(image_, constant(*config_.frames.reset_ssp));
    }
    return state;
  }
  const auto found = config_.frames.instances.find(tag);
  if (found == config_.frames.instances.end()) return state;  // the dead-handler partition: no frame fact
  state.status = found->second.status;
  state.address[7] = found->second.a7;
  return state;
}

FiniteValue M68kFiniteAdapter::effective_status(std::uint32_t tag, const State &in) const {
  if (!config_.domains.frames) return in.status;
  const auto found = config_.frames.clobbered.find(tag);
  if (found == config_.frames.clobbered.end()) return in.status;
  // A resuming child that may rewrite its saved SR can be taken at this boundary (an interrupt the boundary status admits): the
  // instruction then runs with an Unknown status.
  for (const auto level : found->second)
    if (level != 0U && m68k_interrupt_eligible(in.status, level == 8U ? std::nullopt : std::optional<unsigned>(level)))
      return FiniteValue::unknown(UnknownReason::unsupported_transfer);
  return in.status;
}

bool M68kFiniteAdapter::clobbered_after(std::uint32_t tag, const M68kIrOperation &operation, const FiniteValue &status) const {
  if (!config_.domains.frames) return false;
  const auto found = config_.frames.clobbered.find(tag);
  if (found == config_.frames.clobbered.end() || !found->second.contains(0U)) return false;
  // A resuming synchronous child (divide by zero, CHK, TRAPV) that may rewrite its saved SR resumes after this instruction.
  const auto raised = m68k_raised_vectors(&operation, status);
  return std::any_of(raised.begin(), raised.end(),
                     [](std::uint32_t v) { return m68k_vector_class(v) == M68kVectorClass::synchronous_resuming; });
}

M68kPointsTo M68kFiniteAdapter::frame_address(const FiniteValue &status, const State &in) const {
  // M68000PRM: an exception pushes its 6-byte group 1/2 frame below the SSP, which is A7 only in supervisor mode.
  if (!m68k_status_supervisor_proven(status)) return M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::frame_unproven);
  const auto &a7 = in.address[7];
  if (a7.is_unknown()) return M68kPointsTo::unknown(a7.reason, a7.sub == Sub::none ? Sub::base_unknown : a7.sub);
  return m68k_points_to_add(a7, {-static_cast<std::int64_t>(m68k_exception_frame_bytes)});
}

// SEG-030-T005: the generic reason of an opaque continuation's CPU sub-reason.
namespace {
UnknownReason continuation_reason(Sub sub) {
  switch (sub) {
  case Sub::context_bound: return UnknownReason::state_bound;
  case Sub::stack_unbalanced:
  case Sub::return_slot_rewritten: return UnknownReason::unsupported_transfer;
  default: return UnknownReason::unknown_input;
  }
}
}  // namespace

M68kAnalysisState M68kFiniteAdapter::opaque_continuation(Sub sub, std::uint32_t tag) const {
  if (sub == Sub::none) return entry_state(true, tag);
  auto state = State::all_unknown(continuation_reason(sub));
  state.address.fill(M68kPointsTo::unknown(continuation_reason(sub), sub));
  if (config_.domains.memory) state.memory.absent = sub;
  // SEG-030-T006: an opaque continuation's status is its partition's status bound (closure premise, ADR 0079 decision 8).
  if (config_.domains.frames) state.status = status_bound(tag);
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
  return read_memory_with(config_.memory.policy, in, ea, bytes, tracked);
}

M68kMemoryRead M68kFiniteAdapter::read_memory_operand(std::uint32_t tag, const State &in, const M68kEffectiveAddress &ea,
                                                      std::uint32_t bytes, bool *tracked) const {
  return read_memory_with(policy_for(tag), in, ea, bytes, tracked);
}

const M68kMemoryPolicy &M68kFiniteAdapter::policy_for(std::uint32_t tag) const {
  if (!config_.domains.frames) return config_.memory.policy;
  const auto found = tag_policies_.find(tag);
  return found == tag_policies_.end() ? config_.memory.policy : found->second;
}

M68kMemoryRead M68kFiniteAdapter::read_memory_with(const M68kMemoryPolicy &policy, const State &in, const M68kEffectiveAddress &ea,
                                                   std::uint32_t bytes, bool *tracked) const {
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
    if (tracked != nullptr) *tracked = true;  // the address may name mutable RAM
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
      auto read = m68k_memory_read(in.memory, pointer, bytes, policy);
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

std::optional<M68kCellValue> M68kFiniteAdapter::operand_value(const State &in, const M68kEffectiveAddress &ea, std::uint32_t bytes,
                                                              const M68kMemoryPolicy &policy) const {
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
  auto read = read_memory_with(policy, in, ea, bytes, nullptr);
  if (!read.known) return std::nullopt;
  return std::move(read.value);
}

std::vector<std::pair<M68kPointsTo, std::uint32_t>> M68kFiniteAdapter::memory_write_targets(const M68kIrOperation &operation,
                                                                                            const State &in) const {
  return memory_write_targets(operation, in, in.status);
}

std::vector<std::pair<M68kPointsTo, std::uint32_t>> M68kFiniteAdapter::memory_write_targets(const M68kIrOperation &operation,
                                                                                            const State &in,
                                                                                            const FiniteValue &status) const {
  std::vector<std::pair<M68kPointsTo, std::uint32_t>> out;
  const auto writes = m68k_memory_writes(operation);
  if (!writes.described) {
    out.emplace_back(M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::store_poison), 4U);
    return out;
  }
  for (const auto &write : writes.writes) {
    switch (write.target) {
    case M68kMemoryWrite::Target::unknown:
      // SEG-030-T006: the exception frame of the writer description (DIVx/CHK/TRAPV/TRAP/instruction exceptions and the interrupt
      // STOP waits for) is pushed below A7 when S = 1 is proven (for STOP: before and after its SR load), else Unknown.
      if (config_.domains.frames) {
        const auto frame_status =
            operation.kind == M68kIrKind::stop_until_interrupt && m68k_status_supervisor_proven(status)
                ? m68k_status_after(operation, status, std::nullopt)
                : status;
        auto target = frame_address(frame_status, in);
        if (target.is_unknown()) target = M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::store_poison);
        out.emplace_back(std::move(target), write.span);
        break;
      }
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

void M68kFiniteAdapter::transfer_memory(const M68kIrOperation &operation, std::uint32_t next, const State &in, State &out,
                                        const M68kMemoryPolicy &policy, const FiniteValue &status) const {
  const auto writes = m68k_memory_writes(operation);
  const auto targets = memory_write_targets(operation, in, status);
  if (!writes.described) {
    out.memory.poison_all();
    m68k_return_slots_unknown_store(out.memory);
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
    case M68kMemoryWrite::Value::source: value = operand_value(in, operation.source_ea, write.span, policy); break;
    }
    if (value && !value->is_pointer() && !value->data.is_precise()) value.reset();
    m68k_memory_store(out.memory, targets[i].first, targets[i].second, value, policy);
    // SEG-030-T008: every store is related to the recorded return slots (a call's push records one).
    m68k_return_slots_store(out.memory, targets[i].first, targets[i].second,
                            write.value == M68kMemoryWrite::Value::return_address ? std::optional<std::uint32_t>(next) : std::nullopt);
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
  const auto tag = m68k_context_tag(context);       // always 0 unless the frames domain is enabled
  const auto decoded = decode(pc);
  if (!decoded) return result;  // odd, unmapped or rejected: reached, no successor
  const auto &operation = decoded->operation;
  const auto next = (pc + decoded->length) & bus_mask;
  const auto &policy = policy_for(tag);
  // SEG-030-T006: the status this boundary runs with (Unknown after a clobbering child instance may have been taken).
  const auto status = effective_status(tag, in);

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
    auto read = read_memory_with(policy, in, operation.source_ea, bytes, &tracked);
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
  if (config_.domains.address) transfer_address_registers(operation, in, out, &policy);
  if (config_.domains.memory) transfer_memory(operation, next, in, out, policy, status);
  if (config_.domains.contexts) {
    out.stack_delta = stack_delta_after(operation, in);
    // The call frame (M68000PRM JSR/BSR/PEA: A7 - 4, then the push). The address domain alone leaves A7 Unknown there (T003, kept
    // byte-identical); the contexts domain tracks it so a callee is entered, and its summary returns, at an exact frame.
    if (operation.kind == M68kIrKind::call_general || operation.kind == M68kIrKind::bsr_call ||
        operation.kind == M68kIrKind::push_effective_address)
      out.address[7] = m68k_points_to_add(in.address[7], {-4});
  }
  if (config_.domains.frames) {
    // SEG-030-T006: the status after an SR writer (M68000PRM MOVE/ANDI/ORI/EORI to SR, STOP). The stack pointer the address and
    // contexts transfers dropped is kept when S = 1 is proven both before and after (no USP/SSP switch).
    out.status = status;
    if (m68k_status_register_writer(operation.kind)) {
      std::optional<std::vector<std::uint32_t>> source;
      if (operation.kind == M68kIrKind::write_status_register) {
        if (const auto value = operand_value(in, operation.source_ea, 2U, policy); value && value->data.is_precise()) {
          source.emplace();
          for (const auto v : value->data.values()) source->push_back(static_cast<std::uint32_t>(v & 0xFFFFU));
        }
      }
      out.status = m68k_status_after(operation, status, source);
      if (m68k_status_supervisor_proven(status) && m68k_status_supervisor_proven(out.status)) {
        const bool a7_update = is_auto_update(operation.source_ea) && operation.source_ea.reg == 7U;  // MOVE (A7)+,SR: not tracked
        out.address[7] = a7_update ? M68kPointsTo::unknown(UnknownReason::unsupported_transfer) : in.address[7];
        out.stack_delta = a7_update ? FiniteValue::unknown(UnknownReason::unsupported_transfer) : in.stack_delta;
      }
    }
    if (clobbered_after(tag, operation, status)) out.status = FiniteValue::unknown(UnknownReason::unsupported_transfer);
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
    // SEG-030-T006: a callee stays in the caller's partition.
    return m68k_analysis_point(m68k_tagged_context(tag, config_.contexts.merged.contains(target) ? 0U : m68k_call_context(pc)), target);
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
  case M68kDynamicControlFamily::return_from_subroutine:
  case M68kDynamicControlFamily::return_from_exception:
  case M68kDynamicControlFamily::return_restore_condition_codes:
    // SEG-030-T006: an RTE/RTR, or an RTS away from the entry stack delta, whose frame or return cells were written precisely by
    // analysed code is a computed jump in the same activation (frames domain). SEG-030-T008: an RTS at the entry delta whose return
    // cell holds a precise set with a value no call pushed is a computed return to the values its callers' continuations do not
    // already reach (memory domain). Every other return is modelled through continuations.
    if (config_.domains.memory) {
      std::optional<M68kReturnSiteReport> report;
      if (const auto slot = classify_return_slot(point, operation, in)) {
        if (slot->kind == M68kReturnSlotClass::computed) {
          report.emplace();
          report->resolved = true;
          report->targets = slot->fresh;
        }
      } else {
        report = evaluate_return_site(point, operation, in);
      }
      if (report && report->resolved) {
        const auto popped = operation.kind == M68kIrKind::return_from_subroutine ? 4 : static_cast<std::int64_t>(m68k_exception_frame_bytes);
        for (const auto target : report->targets) {
          State edge = out;
          edge.flag_setter.reset();
          edge.address[7] = m68k_points_to_add(in.address[7], {popped});
          edge.stack_delta = in.stack_delta.map([&](std::uint64_t v) { return (v + static_cast<std::uint64_t>(popped)) & UINT64_C(0xFFFFFFFF); });
          if (operation.kind == M68kIrKind::return_from_exception) {
            // The SR restored from the proven frame: a user-mode SR switches A7 to the untracked USP.
            edge.status = report->restored_status;
            if (!m68k_status_supervisor_proven(edge.status)) {
              edge.address[7] = M68kPointsTo::unknown(UnknownReason::unsupported_transfer, Sub::frame_unproven);
              edge.stack_delta = FiniteValue::unknown(UnknownReason::unsupported_transfer);
            }
          }
          computed.push_back({m68k_analysis_point(context, target), EdgeKind::computed, std::move(edge)});
        }
      }
    }
    break;
  case M68kDynamicControlFamily::none: break;
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
      if (config_.call_continuations)
        result.edges.push_back({stacked_point, EdgeKind::return_edge, continuation(pc, callees, in, tag)});
      break;
    }
    if (config_.call_continuations) result.edges.push_back({stacked, EdgeKind::return_edge, entry_state(true, tag)});
    break;
  case M68kStackedContinuationKind::exception_continuation:
    if (config_.exception_continuations) result.edges.push_back({stacked_point, EdgeKind::exceptional, entry_state(true, tag)});
    break;
  case M68kStackedContinuationKind::pushed_code_address:
    if (config_.pushed_code_continuations) result.edges.push_back({stacked_point, EdgeKind::return_edge, entry_state(true, tag)});
    break;
  case M68kStackedContinuationKind::none: break;
  }
  for (auto &edge : computed) result.edges.push_back(std::move(edge));
  return result;
}

// SEG-030-T006: RTE (M68000PRM: SR <- (SP), PC <- (SP + 2), SP + 6; privileged), RTR (CCR <- (SP), PC <- (SP + 2), SP + 6) and an
// RTS away from the activation's entry stack delta (PC <- (SP), SP + 4) read their frame or return address from abstract-memory cells
// only: a cell exists only when analysed code wrote it precisely (never a hardware exception frame, never an asynchronous cell), so
// a resolved site is a code-built frame. Every other one is Unknown with its reason.
// SEG-030-T008 (ADR 0079 decision 8): an RTS at its activation's entry stack delta pops the return cell (A7).L (M68000PRM RTS: PC <-
// (SP), SP + 4). It is a normal return only when that cell provably holds a return address a call pushed into it, or when the
// recorded slot cannot have been written by any store; a precise cell with another value is a computed return to that set; a slot
// a known-target store may have rewritten is Unknown(return_slot_rewritten). What remains (Unknown-target stores, opaque callee
// effects, asynchronous and external writers, an untracked A7) is the named return-slot integrity premise, reported per site.
std::optional<M68kReturnSlotOutcome> M68kFiniteAdapter::classify_return_slot(std::uint64_t point, const M68kIrOperation &operation,
                                                                            const State &in) const {
  if (!config_.domains.memory) return std::nullopt;
  if (m68k_control_successors(operation).dynamic != M68kDynamicControlFamily::return_from_subroutine) return std::nullopt;
  if (config_.domains.contexts && !(in.stack_delta == FiniteValue::of({0U}))) return std::nullopt;
  const auto pc = m68k_point_pc(point);
  const auto tag = m68k_point_tag(point);
  M68kReturnSlotOutcome out;
  const auto unknown = [&](UnknownReason reason, Sub sub) {
    out.kind = M68kReturnSlotClass::unknown;
    out.reason = reason;
    out.sub = sub;
    out.targets.clear();
    out.fresh.clear();
    return out;
  };
  const auto premise = [&](M68kReturnSlotPremise cause) {
    out.kind = M68kReturnSlotClass::premise;
    out.premise = cause;
    return out;
  };
  if (config_.pinned_sites.contains(pc)) return unknown(UnknownReason::unsupported_transfer, Sub::invalidated);
  const auto &a7 = in.address[7];
  if (!a7.is_known()) return premise(M68kReturnSlotPremise::slot_untracked);
  // The recorded slot at every exact A7 cell (recorded only when every cell is).
  bool recorded = a7.is_exact();
  bool rewritten = false, unknown_store = false;
  FiniteValue pushed;
  for (const auto &[region, offsets] : a7.pairs) {
    if (!recorded) break;
    if (!m68k_memory_tracked(region.kind)) {
      recorded = false;
      break;
    }
    for (const auto offset : offsets.exact()) {
      const auto at = region.mirror != 0U ? offset % region.mirror : offset;
      const auto found = in.memory.slots.find(M68kCell{region.kind, region.id, at, 4U});
      if (found == in.memory.slots.end()) {
        recorded = false;
        break;
      }
      pushed = join(pushed, found->second.pushed);
      rewritten = rewritten || found->second.rewritten;
      unknown_store = unknown_store || found->second.unknown_store;
    }
  }
  if (recorded && !pushed.is_precise()) recorded = false;
  M68kEffectiveAddress ea{};
  ea.mode = M68kEaMode::address_indirect;
  ea.reg = 7U;
  const auto read = read_memory_with(policy_for(tag), in, ea, 4U, nullptr);
  std::optional<std::vector<std::uint32_t>> values;
  if (read.known) {
    if (read.value.is_pointer()) {
      if (auto exact = read.value.pointer.values(); exact && !read.value.pointer.width_derived) values = std::move(*exact);
    } else if (read.value.data.is_precise() && !read.value.width_derived) {
      values.emplace();
      for (const auto v : read.value.data.values()) values->push_back(static_cast<std::uint32_t>(v));
    }
  }
  if (values) {
    std::set<std::uint32_t> expected;
    if (recorded)
      for (const auto v : pushed.values()) expected.insert(static_cast<std::uint32_t>(v) & bus_mask);
    if (std::all_of(values->begin(), values->end(), [&](std::uint32_t v) { return expected.contains(v & bus_mask); })) return out;
    std::set<std::uint32_t> exact;
    for (const auto value : *values) {
      const auto target = value & bus_mask;
      if ((target & 1U) != 0U) {
        ++out.odd_targets_excluded;  // an odd PC raises an address error: never a normal target
        continue;
      }
      if (!image_.mapped(target)) return unknown(UnknownReason::non_immutable_read, Sub::target_outside_image);
      exact.insert(target);
    }
    if (exact.empty()) return unknown(UnknownReason::unsupported_transfer, Sub::none);
    out.kind = M68kReturnSlotClass::computed;
    out.targets.assign(exact.begin(), exact.end());
    for (const auto target : exact)
      if (!expected.contains(target)) out.fresh.push_back(target);
    return out;
  }
  // Partially known (a strided or width-only value), or a known-target store may have written the slot: never the premise.
  if (read.known || (recorded && rewritten)) return unknown(UnknownReason::unsupported_transfer, Sub::return_slot_rewritten);
  if (recorded && unknown_store) return premise(M68kReturnSlotPremise::unknown_target_store);
  if (read.sub == Sub::async_writer) return premise(M68kReturnSlotPremise::async_writer);
  if (read.sub == Sub::external_writer) return premise(M68kReturnSlotPremise::external_writer);
  // Every store since the push was related to the recorded slot and none may write it: the cell still holds the pushed address.
  if (recorded) return out;
  switch (in.memory.absent) {
  case Sub::store_poison: return premise(M68kReturnSlotPremise::unknown_target_store);
  case Sub::none:
  case Sub::initial_memory: return unknown(UnknownReason::unknown_input, Sub::initial_memory);  // no call pushed this slot
  case Sub::set_bound: return unknown(UnknownReason::state_bound, Sub::set_bound);
  default: return premise(M68kReturnSlotPremise::opaque_callee);  // an opaque continuation's memory (context_bound, ...)
  }
}

std::optional<M68kReturnSiteReport> M68kFiniteAdapter::evaluate_return_site(std::uint64_t point, const M68kIrOperation &operation,
                                                                            const State &in) const {
  if (!config_.domains.memory) return std::nullopt;
  const auto family = m68k_control_successors(operation).dynamic;
  const bool rts = family == M68kDynamicControlFamily::return_from_subroutine;
  const bool rte = family == M68kDynamicControlFamily::return_from_exception;
  if (!rts && !rte && family != M68kDynamicControlFamily::return_restore_condition_codes) return std::nullopt;
  // SEG-030-T008: an RTS at the entry delta is a site only when its return slot makes it computed or Unknown.
  if (const auto slot = classify_return_slot(point, operation, in)) {
    if (slot->kind == M68kReturnSlotClass::normal || slot->kind == M68kReturnSlotClass::premise) return std::nullopt;
    M68kReturnSiteReport report;
    report.family = family;
    report.resolved = slot->kind == M68kReturnSlotClass::computed;
    report.targets = slot->targets;
    report.odd_targets_excluded = slot->odd_targets_excluded;
    report.reason = slot->reason;
    report.sub = slot->sub;
    return report;
  }
  if (!config_.domains.frames) {
    // SEG-030-T008: without the frames domain an RTS away from the entry stack delta (a push window, an unbalanced pop) is never
    // resolved: a typed Unknown site (its activation is unproven, stack_unbalanced).
    if (!rts || !config_.domains.contexts) return std::nullopt;
    M68kReturnSiteReport report;
    report.family = family;
    report.reason = UnknownReason::unsupported_transfer;
    report.sub = config_.pinned_sites.contains(m68k_point_pc(point)) ? Sub::invalidated : Sub::stack_unbalanced;
    return report;
  }
  const auto pc = m68k_point_pc(point);
  const auto tag = m68k_point_tag(point);
  M68kReturnSiteReport out;
  out.family = family;
  const auto finish = [&](UnknownReason reason, Sub sub) {
    out.resolved = false;
    out.reason = reason;
    // An RTE of a handler partition returns through the hardware frame of an asynchronous or exception entry: never a normal
    // resumption the analysis can name.
    out.sub = rte && tag != 0U ? Sub::interrupt_resumption : sub;
    out.targets.clear();
    out.restored_status = FiniteValue::bottom();
    return out;
  };
  if (rts) {
    // An RTS at the entry delta is an ordinary return (call continuations); only one away from it is a computed jump.
    if (in.stack_delta == FiniteValue::of({0U})) return std::nullopt;
    if (!in.stack_delta.is_precise() ||
        std::find(in.stack_delta.values().begin(), in.stack_delta.values().end(), 0U) != in.stack_delta.values().end())
      return finish(UnknownReason::unsupported_transfer, Sub::stack_unbalanced);
  }
  if (config_.pinned_sites.contains(pc)) return finish(UnknownReason::unsupported_transfer, Sub::invalidated);
  const auto status = effective_status(tag, in);
  if (rte && !m68k_status_supervisor_proven(status)) return finish(UnknownReason::unsupported_transfer, Sub::frame_unproven);
  const auto &policy = policy_for(tag);
  const auto read_cells = [&](std::int16_t displacement, std::uint32_t bytes) {
    M68kEffectiveAddress ea{};
    ea.mode = displacement == 0 ? M68kEaMode::address_indirect : M68kEaMode::address_disp16;
    ea.reg = 7U;
    ea.displacement = displacement;
    return read_memory_with(policy, in, ea, bytes, nullptr);
  };
  const auto unproven = [&](const M68kMemoryRead &read) {
    return finish(read.reason, read.sub == Sub::initial_memory || read.sub == Sub::none ? Sub::frame_unproven : read.sub);
  };
  const auto precise = [](const M68kMemoryRead &read, std::uint32_t bytes) -> std::optional<std::vector<std::uint32_t>> {
    if (!read.known) return std::nullopt;
    std::vector<std::uint32_t> values;
    if (read.value.is_pointer()) {
      const auto exact = read.value.pointer.values();
      if (!exact || read.value.pointer.width_derived) return std::nullopt;
      values = *exact;
    } else {
      if (!read.value.data.is_precise() || read.value.width_derived) return std::nullopt;
      for (const auto v : read.value.data.values()) values.push_back(static_cast<std::uint32_t>(v));
    }
    if (bytes == 2U)
      for (auto &v : values) v &= 0xFFFFU;
    return values;
  };
  const auto target_read = read_cells(rts ? 0 : 2, 4U);
  const auto targets = precise(target_read, 4U);
  if (!target_read.known) return unproven(target_read);
  if (!targets) return finish(UnknownReason::unsupported_transfer, Sub::frame_unproven);
  if (rte) {
    const auto sr_read = read_cells(0, 2U);
    const auto sr = precise(sr_read, 2U);
    if (!sr_read.known) return unproven(sr_read);
    if (!sr) return finish(UnknownReason::unsupported_transfer, Sub::frame_unproven);
    std::vector<std::uint64_t> restored;
    for (const auto v : *sr) restored.push_back(m68k_status_of_sr(v));
    out.restored_status = FiniteValue::of(std::move(restored));
  }
  std::set<std::uint32_t> exact;
  for (const auto value : *targets) {
    const auto target = value & bus_mask;
    if ((target & 1U) != 0U) {
      ++out.odd_targets_excluded;  // an odd PC raises an address error: never a normal target
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

// SEG-030-T005: the continuation state of the call at `pc` whose callees in this transfer are `callees`.
M68kAnalysisState M68kFiniteAdapter::continuation(std::uint32_t pc, const std::vector<std::uint32_t> &callees, const State &in,
                                                  std::uint32_t tag) const {
  State state;
  const auto &contexts = config_.contexts;
  const auto context = m68k_tagged_context(tag, m68k_call_context(pc));
  if (callees.empty()) {
    state = opaque_continuation(Sub::none, tag);  // an unresolved callee: unknown effect
  } else if (std::any_of(callees.begin(), callees.end(), [&](std::uint32_t callee) { return contexts.merged.contains(callee); })) {
    state = opaque_continuation(Sub::context_bound, tag);
    // SEG-030-T006 (frames domain only): every callee a balanced merged callee: the caller's A7 and the callees' exit statuses.
    if (config_.domains.frames) {
      FiniteValue exits;
      const bool proven = std::all_of(callees.begin(), callees.end(), [&](std::uint32_t callee) {
        const auto found = contexts.merged_exits.find(m68k_tagged_context(tag, callee));
        if (!contexts.merged.contains(callee) || found == contexts.merged_exits.end()) return false;
        exits = join(exits, found->second);
        return true;
      });
      if (proven) {
        state.address[7] = in.address[7];
        state.status = exits;
      }
    }
  } else if (const auto summary = contexts.summaries.find(context); summary != contexts.summaries.end()) {
    state = summary->second;
    // SEG-030-T006 (frames domain only): a summary exists only for a balanced activation (every exit is an RTS at stack delta {0}),
    // so every invocation returns at its own entry A7 + 4, which is this caller's A7 at the call. The summary's own A7 is the join
    // of the exits over every invocation of the context (a nested callee's context is shared by every invocation of its outer
    // callee), so it is replaced by the caller-relative value, as for a balanced merged callee. The summary's other facts (registers,
    // A0-A6, memory) are joins over every invocation's exits, sound for each one, and are kept as they are.
    if (config_.domains.frames) state.address[7] = in.address[7];
  } else {
    const auto opaque = contexts.opaque.find(context);
    state = opaque_continuation(opaque == contexts.opaque.end() ? Sub::none : opaque->second, tag);
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
bool site_resolved(const M68kReturnSiteReport &site) { return site.resolved; }

// SEG-030-T006: the tags of the writer-only (uncredited) instances.
std::set<std::uint32_t> m68k_writer_only_tags(const M68kFrameConfig &frames) {
  std::set<std::uint32_t> out;
  for (const auto &[tag, instance] : frames.instances)
    if (!instance.credited) out.insert(tag);
  return out;
}

// SEG-030-T006: the distinct handler PCs of the delivered vectors, in order.
std::vector<std::uint32_t> handler_pcs(const M68kFrameConfig &frames) {
  std::set<std::uint32_t> out;
  for (const auto &vector : frames.vectors) out.insert(vector.handler & bus_mask);
  return {out.begin(), out.end()};
}

// SEG-030-T006: the seeds of every partition: the main entries in tag 0, every live instance at its handler, every handler PC also
// entered with an Unknown entry in the unknown-entry partition (SEG-030-T008 correction), and every other handler PC without a live
// credited instance in the dead-handler partition (kept so that the discovery roots are unchanged; a writer-only instance never
// stands in for a delivered handler's root).
std::vector<std::pair<std::uint64_t, M68kAnalysisState>> frame_seeds(const M68kFiniteAdapter &adapter, const M68kAnalysisConfig &config,
                                                                    const std::vector<std::uint32_t> &entries) {
  std::vector<std::pair<std::uint64_t, M68kAnalysisState>> seeds;
  const auto handlers = handler_pcs(config.frames);
  std::set<std::uint32_t> live;
  for (const auto &[tag, instance] : config.frames.instances) {
    seeds.emplace_back(m68k_analysis_point(m68k_tagged_context(tag, 0U), instance.handler), adapter.root_state(tag, instance.handler));
    if (instance.credited) live.insert(instance.handler);
  }
  for (const auto entry : entries) {
    const auto pc = entry & bus_mask;
    const bool handler = std::binary_search(handlers.begin(), handlers.end(), pc);
    if (!handler || config.frames.main_entries.contains(pc)) seeds.emplace_back(pc, adapter.root_state(0U, pc));
    if (handler && !live.contains(pc) && !config.frames.unknown_entries.contains(pc))
      seeds.emplace_back(m68k_analysis_point(m68k_tagged_context(m68k_dead_handler_tag, 0U), pc),
                         adapter.root_state(m68k_dead_handler_tag, pc));
  }
  for (const auto pc : config.frames.unknown_entries)
    seeds.emplace_back(m68k_analysis_point(m68k_tagged_context(m68k_unknown_entry_tag, 0U), pc),
                       adapter.root_state(m68k_unknown_entry_tag, pc));
  std::sort(seeds.begin(), seeds.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  return seeds;
}

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
    if (config.domains.frames) seeds = frame_seeds(adapter, config, entries);
    else
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
    // SEG-030-T006: the points of a writer-only instance are analysed (asynchronous writers) but never credited.
    out.writer_only_tags = m68k_writer_only_tags(config.frames);
    const auto credited = [&](std::uint64_t point) { return !out.writer_only_tags.contains(m68k_point_tag(point)); };
    for (const auto &[point, reason] : out.solution.unresolved_computed)
      if (credited(point)) out.unresolved_computed.emplace(m68k_point_pc(point), reason);
    std::set<std::uint32_t> invalidated;
    struct SlotSite {
      M68kReturnSlotPremise cause{M68kReturnSlotPremise::none};
      bool computed{};
      bool unknown{};
    };
    std::map<std::uint32_t, SlotSite> slot_sites;
    for (const auto &[point, state] : out.solution.in_states) {
      if (!credited(point)) continue;
      const auto pc = m68k_point_pc(point);
      const auto decoded = adapter.decode(pc);
      if (!decoded) {
        out.undecodable.insert(pc);
        continue;
      }
      out.reached.emplace(pc, decoded->length);
      if (config.domains.memory) {
        // SEG-030-T008: the return-slot class of every RTS at the entry delta (a pinned one whose computed targets were lost is
        // invalidated even when its final class is normal).
        if (const auto slot = adapter.classify_return_slot(point, decoded->operation, state)) {
          if (!config.pinned_sites.contains(pc) && out.solution.pinned.contains(point)) invalidated.insert(pc);
          auto &site = slot_sites[pc];
          if (slot->kind == M68kReturnSlotClass::premise && site.cause == M68kReturnSlotPremise::none) site.cause = slot->premise;
          site.computed = site.computed || slot->kind == M68kReturnSlotClass::computed;
          site.unknown = site.unknown || slot->kind == M68kReturnSlotClass::unknown;
        }
        if (auto report = adapter.evaluate_return_site(point, decoded->operation, state)) {
          if (!config.pinned_sites.contains(pc) && out.solution.pinned.contains(point)) invalidated.insert(pc);
          merge_site(out.return_sites, pc, std::move(*report));
          continue;
        }
      }
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
    if (config.domains.memory) {
      auto &slots = out.return_slots;
      slots.enabled = true;
      for (const auto &[pc, site] : slot_sites) {
        ++slots.sites;
        if (site.cause != M68kReturnSlotPremise::none) {
          ++slots.premise_sites;
          slots.premise_pcs.insert(pc);
          ++slots.premise_by_cause[site.cause];
        }
        if (site.unknown) ++slots.unknown_sites;
        else if (site.computed) ++slots.computed_sites;
        else if (site.cause == M68kReturnSlotPremise::none) ++slots.normal_sites;
      }
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

// SEG-030-T010: the bus ranges a store of `span` bytes at known `targets` may touch, clipped to `ranges` and merged into `out`.
// Returns false when the store touches no observed range.
bool observe_store(const M68kPointsTo &targets, std::uint32_t span, const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges,
                   std::vector<std::pair<std::uint32_t, std::uint32_t>> &out) {
  bool touched = false;
  for (const auto &[region, offsets] : targets.pairs) {
    const std::uint64_t lo = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.lo();
    const std::uint64_t hi = static_cast<std::uint64_t>(region.base & bus_mask) + offsets.hi() + span;
    for (const auto &[first, last] : ranges) {
      if (!(lo < last && first < hi)) continue;
      touched = true;
      out.emplace_back(static_cast<std::uint32_t>(std::max<std::uint64_t>(lo, first)),
                       static_cast<std::uint32_t>(std::min<std::uint64_t>(hi, last)));
    }
  }
  return touched;
}

void merge_ranges(std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges) {
  std::sort(ranges.begin(), ranges.end());
  std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;
  for (const auto &range : ranges) {
    if (!merged.empty() && range.first <= merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
    else merged.push_back(range);
  }
  ranges = std::move(merged);
}

// The policy the solution itself requires (ADR 0079 decisions 7 and 9) plus the memory statistics of the run.
M68kMemoryPolicy derive_memory_policy(const M68kAnalysisImage &image, const M68kAnalysisConfig &config, M68kFiniteAnalysisResult &result) {
  M68kMemoryPolicy derived;
  // SEG-030-T006: with the frames domain the asynchronous writers are per partition (derive_frames); the global policy carries only
  // the external writer.
  const bool frames = config.domains.frames;
  derived.async_all = config.memory.interrupts && !frames;
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
    if (!frames && states.contains(root & bus_mask) && handler.insert(root & bus_mask).second) pending.push_back(root & bus_mask);
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
    const auto tag = m68k_point_tag(point);
    bool stores = false;
    for (const auto &[targets, span] : adapter.memory_write_targets(operation, state, adapter.effective_status(tag, state))) {
      if (targets.is_bottom()) continue;
      stores = true;
      if (!targets.is_known()) ++report.unknown_target_stores;
      if (!config.memory.observed_store_ranges.empty()) {
        if (!targets.is_known()) ++report.observed_unknown_target_stores;
        else if (observe_store(targets, span, config.memory.observed_store_ranges, report.observed_store_ranges))
          ++report.observed_known_stores;
      }
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
    // Memory-source operands that may read mutable RAM: precise or Unknown by generic reason x CPU sub-reason. A writer-only instance's
    // stores count above (they are real writes), its reads are never credited.
    const auto instance = config.frames.instances.find(tag);
    const bool credited = instance == config.frames.instances.end() || instance->second.credited;
    if (credited && m68k_memory_mode(operation.source_ea.mode) && operation.kind != M68kIrKind::load_effective_address &&
        operation.kind != M68kIrKind::push_effective_address && operation.kind != M68kIrKind::jump_general &&
        operation.kind != M68kIrKind::call_general) {
      bool tracked = false;
      const auto read = adapter.read_memory_operand(tag, state, operation.source_ea, access_bytes(operation.size), &tracked);
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
  merge_ranges(report.observed_store_ranges);
  report.release_store = release;
  // SEG-030-T010: a credited platform bound replaces the blanket rule (every cell) by the bounded ranges.
  if (release && !config.memory.assume_no_external_writer) {
    if (!config.memory.external_writer_bound) derived.external_writer = true;
    else
      for (const auto &range : *config.memory.external_writer_bound) derived.add_async(range);
  }
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
  bool slot_unproven{};   // SEG-030-T008: RTS at the entry stack delta whose return slot is computed or Unknown
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
  // SEG-030-T006: a merged callee is one activation per partition (the tag is the context's top byte; 0 without frames).
  return m68k_context_site(context) == 0U ? merged_key | (static_cast<std::uint64_t>(m68k_context_tag(context)) << 24U) |
                                                m68k_point_pc(callee_point)
                                          : context;
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
    // SEG-030-T006 (frames domain only): an instruction that always raises only non-resuming synchronous vectors (ILLEGAL, line
    // 1010/1111) ends its path. Its handler never resumes into this activation (m68k_vector_class, ADR 0079 decision 7: a handler
    // RTE is interrupt_resumption, never an analysed edge), so it is not an unknown effect. A TRAP/TRAPV whose continuation is not
    // modelled, an unresolved computed site and an undecodable point stay unknown effects.
    bool non_resuming_raise = false;
    if (control.always_raises_exception && config.domains.frames) {
      const auto raised = m68k_raised_vectors(&decoded->operation, state.status);
      non_resuming_raise = !raised.empty() && std::all_of(raised.begin(), raised.end(), [](std::uint32_t vector) {
        return m68k_vector_class(vector) == M68kVectorClass::synchronous;
      });
    }
    // A pushed code address is not an escape by itself: a return through it is an RTS away from the entry stack delta.
    if ((control.always_raises_exception && !non_resuming_raise) || result.solution.unresolved_computed.contains(point) ||
        (control.stacked == M68kStackedContinuationKind::exception_continuation && !config.exception_continuations))
      facts.unknown_effect = true;
    const auto edges = adapter.transfer(point, state).edges;
    // SEG-030-T006: a return resolved from a code-built frame is a computed jump inside the activation (frames domain only).
    const bool resolved_return = std::any_of(edges.begin(), edges.end(), [](const auto &edge) { return edge.kind == EdgeKind::computed; });
    // SEG-030-T008: an RTS at the entry delta is an exit only when its return slot is normal or under the named premise.
    if (const auto slot = adapter.classify_return_slot(point, decoded->operation, state)) {
      if (slot->kind == M68kReturnSlotClass::normal || slot->kind == M68kReturnSlotClass::premise) facts.exit = true;
      else facts.slot_unproven = true;
    } else if (control.dynamic == M68kDynamicControlFamily::return_from_subroutine && !resolved_return) {
      if (state.stack_delta == zero) facts.exit = true;
      else facts.unbalanced = true;
    }
    if ((control.dynamic == M68kDynamicControlFamily::return_from_exception ||
         control.dynamic == M68kDynamicControlFamily::return_restore_condition_codes) &&
        !resolved_return)
      facts.unbalanced = true;
    // A pinned computed site contributes no computed edge to the solution (solver pin-and-restart): its transfer's computed edges
    // are not part of the final edge set (their targets may have no state), and the site is already an unknown effect.
    const bool pinned = result.solution.unresolved_computed.contains(point);
    for (const auto &edge : edges) {
      if (pinned && edge.kind == EdgeKind::computed) continue;
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
  if (out.status.is_unknown()) out.status = FiniteValue::unknown(UnknownReason::unknown_input);
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
    if (m68k_context_site(m68k_point_context(point)) != 0U) activations[m68k_point_context(point)].points.push_back(point);
    for (const auto callee : fact.callees) {
      if (m68k_context_site(m68k_point_context(callee)) != 0U) contexts_of[m68k_point_pc(callee)].insert(m68k_point_context(callee));
      else activations[activation_of(callee)];
    }
  }
  for (auto &[key, activation] : activations) {
    if ((key & merged_key) == 0U) continue;
    const auto low = static_cast<std::uint32_t>(key);
    std::set<std::uint64_t> seen{m68k_analysis_point(m68k_tagged_context(m68k_context_tag(low), 0U), m68k_context_site(low))};
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
    bool unbalanced = false, unknown = false, slot = false;
    for (const auto point : activation.points) {
      const auto found = facts.find(point);
      if (found == facts.end()) continue;
      const auto &fact = found->second;
      unbalanced = unbalanced || fact.unbalanced;
      slot = slot || fact.slot_unproven;
      unknown = unknown || fact.unknown_effect;
      for (const auto callee : fact.callees) activation.nested.insert(activation_of(callee));
    }
    if (unbalanced) activation.fail = Sub::stack_unbalanced;
    else if (slot) activation.fail = Sub::return_slot_rewritten;
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
    if ((key & merged_key) != 0U) {
      // SEG-030-T006 (frames domain only): a balanced merged callee returns at its caller's A7, with a status among its exits'. The
      // claim is validated like a summary (a used claim must still hold, and its status must cover the derived one).
      if (config.domains.frames) {
        const auto low = static_cast<std::uint32_t>(key);
        const auto previous = used.merged_exits.find(low);
        if (activation.balanced) {
          FiniteValue exits;
          for (const auto point : activation.points) {
            const auto found = facts.find(point);
            if (found != facts.end() && found->second.exit) exits = join(exits, states.at(point).status);
          }
          if (previous != used.merged_exits.end() && !leq(exits, previous->second)) {
            valid = false;
            out.next.merged_exits[low] = join(previous->second, exits);
          } else {
            out.next.merged_exits[low] = exits;
          }
        } else if (previous != used.merged_exits.end()) {
          valid = false;  // a used claim no longer holds
        }
      }
      continue;
    }
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
    const auto context = m68k_tagged_context(m68k_point_tag(point), m68k_call_context(m68k_point_pc(point)));
    if (fact.callees.empty()) ++report.opaque_continuations[Sub::none];
    else if (std::any_of(fact.callees.begin(), fact.callees.end(),
                         [](std::uint64_t c) { return m68k_context_site(m68k_point_context(c)) == 0U; }))
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

// ---------------------------------------------------------------------------------------------------------------
// SEG-030-T006: handler instances, per-partition asynchronous writers and frame integrity (ADR 0079 decisions 7 and 9).

// Relative byte intervals [lo, hi) (offsets from a handler instance's entry A7, the address of its saved SR), sorted and merged; more
// than the exact bound collapses to the hull. `unknown`: some write's position relative to the entry is not known.
struct RelativeWrites {
  bool unknown{};
  std::vector<std::pair<std::int64_t, std::int64_t>> intervals;

  void add(std::int64_t lo, std::int64_t hi) {
    if (unknown || lo >= hi) return;
    intervals.emplace_back(lo, hi);
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> merged;
    for (const auto &next : intervals) {
      if (!merged.empty() && next.first <= merged.back().second) merged.back().second = std::max(merged.back().second, next.second);
      else merged.push_back(next);
    }
    if (merged.size() > m68k_exact_offset_bound) merged = {{merged.front().first, merged.back().second}};
    intervals = std::move(merged);
  }
  void add_shifted(const RelativeWrites &other, std::int64_t shift) {
    if (other.unknown) unknown = true;
    if (unknown) {
      intervals.clear();
      return;
    }
    for (const auto &[lo, hi] : other.intervals) add(lo + shift, hi + shift);
  }
  [[nodiscard]] bool overlaps(std::int64_t lo, std::int64_t hi) const {
    if (unknown) return true;
    return std::any_of(intervals.begin(), intervals.end(), [&](const auto &interval) { return interval.first < hi && lo < interval.second; });
  }
};

std::int64_t signed32(std::uint64_t value) { return static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value))); }

// { a + b } modulo 2^32 over two precise sets, bounded by the exact offset bound (else Unknown).
FiniteValue add_sets(const FiniteValue &left, const FiniteValue &right) {
  if (left.is_bottom() || right.is_bottom()) return FiniteValue::bottom();
  if (!left.is_precise() || !right.is_precise()) return FiniteValue::unknown(UnknownReason::unsupported_transfer);
  if (left.values().size() * right.values().size() > m68k_address_enumeration_bound) return FiniteValue::unknown(UnknownReason::set_bound);
  std::vector<std::uint64_t> out;
  for (const auto a : left.values())
    for (const auto b : right.values()) out.push_back((a + b) & UINT64_C(0xFFFFFFFF));
  return FiniteValue::of(std::move(out), m68k_exact_offset_bound);
}

// The offsets, relative to A7 before the instruction, of the `index`th memory write of `operation` when it is A7-relative (a push,
// an exception frame, or (A7), (A7)+, -(A7), d16(A7) destinations), else nullopt.
std::optional<std::int64_t> a7_relative_offset(const M68kIrOperation &operation, const M68kMemoryWrite &write) {
  switch (write.target) {
  case M68kMemoryWrite::Target::push: return -static_cast<std::int64_t>(write.span);
  case M68kMemoryWrite::Target::unknown: return -static_cast<std::int64_t>(m68k_exception_frame_bytes);
  case M68kMemoryWrite::Target::destination: break;
  }
  const auto &destination = operation.destination_ea;
  if (destination.reg != 7U) return std::nullopt;
  std::int64_t base = 0;
  const auto &source = operation.source_ea;
  if (operation.kind != M68kIrKind::movem_transfer && is_auto_update(source) && source.reg == 7U) {
    const auto step = static_cast<std::int64_t>(auto_update_step(7U, operation.size));
    base = source.mode == M68kEaMode::address_postinc ? step : -step;
  }
  switch (destination.mode) {
  case M68kEaMode::address_indirect:
  case M68kEaMode::address_postinc: return base;
  case M68kEaMode::address_predec:
    return base - static_cast<std::int64_t>(operation.kind == M68kIrKind::movem_transfer ? write.span
                                                                                         : auto_update_step(7U, operation.size));
  case M68kEaMode::address_disp16: return base + destination.displacement;
  default: return std::nullopt;
  }
}

// Why a handler taken from a partition is not analysed as an instance of its own; its consequences then fall on the parent.
enum class Unanalysed : std::uint8_t {
  none,
  unmodelled_parent,    // taken inside the unknown-entry partition (its state is not modelled)
  nested,               // the handler is already on the parent's chain: unbounded nesting
  depth_bound,          // the parent's chain is at the instance depth bound
  entry_unknown,        // the frame address (its entry A7) is Unknown
};
const char *unanalysed_name(Unanalysed why) {
  switch (why) {
  case Unanalysed::none: return "none";
  case Unanalysed::unmodelled_parent: return "unmodelled_parent";
  case Unanalysed::nested: return "nested";
  case Unanalysed::depth_bound: return "depth_bound";
  case Unanalysed::entry_unknown: return "entry_unknown";
  }
  return "invalid";
}

// SEG-030-T008 correction: a non-resuming instance is an analysed partition like any other; every one of its interrupt-eligible or
// raising boundaries enters its children as instances (their frame address is that partition's own A7), never not at all.
Unanalysed instance_admission(const M68kFrameConfig &frames, std::uint32_t handler, std::uint32_t parent_tag) {
  if (parent_tag == 0U) return Unanalysed::none;
  if (!frames.instances.contains(parent_tag)) return Unanalysed::unmodelled_parent;
  std::uint32_t depth = 0U;
  for (auto tag = parent_tag; tag != 0U;) {
    const auto found = frames.instances.find(tag);
    if (found == frames.instances.end()) return Unanalysed::depth_bound;
    if (found->second.handler == handler) return Unanalysed::nested;
    if (++depth >= m68k_instance_depth_bound) return Unanalysed::depth_bound;
    tag = found->second.parent;
  }
  return Unanalysed::none;
}

// The clobber levels of a set of vectors (ADR 0079 decision 7): 1-7 for an autovector, 8 for an interrupt of unknown level, 0 for a
// resuming synchronous vector. A non-resuming synchronous vector never resumes into its parent.
std::set<std::uint32_t> clobber_levels(const std::set<std::uint32_t> &vectors) {
  std::set<std::uint32_t> out;
  for (const auto vector : vectors) {
    const auto cls = m68k_vector_class(vector);
    if (cls == M68kVectorClass::synchronous) continue;
    const auto level = m68k_interrupt_level(vector);
    out.insert(cls == M68kVectorClass::interrupt ? (level ? *level : 8U) : 0U);
  }
  return out;
}

struct FrameDerivation {
  M68kFrameConfig next;  // the used configuration joined with what this solution derives
  bool valid{};          // everything this solution derives is below the used configuration (a post-fixed point)
  bool failed{};         // the instance tag bound was exceeded (the frames domain fails closed)
  std::set<std::uint32_t> writer_tags;  // analysed partitions that are asynchronous writers of their parent
};

// One handler (taken from one parent partition) contribution of this round.
struct Contribution {
  FiniteValue status;
  M68kPointsTo a7;
  std::set<std::uint32_t> vectors;
  bool resuming{}, interrupt{};
  bool credited{};  // some delivered vector enters it from a credited partition
  std::vector<M68kPointsTo> interrupt_frames;   // the frame address at every parent point where an interrupt vector enters it
  std::vector<std::uint64_t> taking_points;     // parent points where a resuming vector enters it
};

// The A7 of every point of partition `tag` relative to the entry A7 of the partition's handler instance (the saved SR), from the
// activation structure: the root activation starts at 0, a callee activation at the call point's relative A7 - 4.
std::map<std::uint64_t, FiniteValue> relative_a7(M68kFiniteAdapter &adapter, const M68kFiniteAnalysisResult &result, std::uint32_t tag,
                                                 std::uint32_t handler) {
  const auto &states = result.solution.in_states;
  const std::uint64_t root_key = ~UINT64_C(0);
  std::map<std::uint64_t, std::vector<std::uint64_t>> intra, callees;
  std::vector<std::uint64_t> points;
  for (const auto &[point, state] : states) {
    if (m68k_point_tag(point) != tag) continue;
    points.push_back(point);
    const auto decoded = adapter.decode(m68k_point_pc(point));
    if (!decoded) continue;
    const bool call = m68k_control_successors(decoded->operation).stacked == M68kStackedContinuationKind::call_continuation;
    for (const auto &edge : adapter.transfer(point, state).edges) {
      if (call && (edge.kind == EdgeKind::call || edge.kind == EdgeKind::computed)) callees[point].push_back(edge.target);
      else intra[point].push_back(edge.target);
    }
  }
  // Membership of the context-site-0 activations (the root and every merged callee): walks over intra edges.
  std::map<std::uint64_t, std::set<std::uint64_t>> member;  // point -> activation keys
  const auto walk = [&](std::uint64_t start, std::uint64_t key) {
    std::vector<std::uint64_t> pending{start};
    std::set<std::uint64_t> seen{start};
    while (!pending.empty()) {
      const auto point = pending.back();
      pending.pop_back();
      if (!states.contains(point)) continue;
      member[point].insert(key);
      for (const auto target : intra[point])
        if (seen.insert(target).second) pending.push_back(target);
    }
  };
  walk(m68k_analysis_point(m68k_tagged_context(tag, 0U), handler), root_key);
  std::set<std::uint64_t> merged_entries;
  for (const auto &[point, targets] : callees)
    for (const auto target : targets)
      if (m68k_context_site(m68k_point_context(target)) == 0U) merged_entries.insert(target);
  for (const auto entry : merged_entries) walk(entry, merged_key | entry);
  for (const auto point : points) {
    const auto context = m68k_point_context(point);
    if (m68k_context_site(context) != 0U) member[point].insert(context);
  }
  std::map<std::uint64_t, FiniteValue> entry{{root_key, FiniteValue::of({0U})}};
  std::map<std::uint64_t, FiniteValue> rel;
  for (std::uint32_t pass = 0U; pass < 4U * m68k_exact_offset_bound; ++pass) {
    bool changed = false;
    for (const auto point : points) {
      FiniteValue value;
      for (const auto key : member[point]) {
        const auto found = entry.find(key);
        if (found != entry.end()) value = join(value, add_sets(found->second, states.at(point).stack_delta), m68k_exact_offset_bound);
      }
      if (rel[point] != value) {
        rel[point] = value;
        changed = true;
      }
    }
    for (const auto &[point, targets] : callees) {
      const auto at = add_sets(rel[point], FiniteValue::of({UINT64_C(0xFFFFFFFC)}));
      for (const auto target : targets) {
        const auto key = m68k_context_site(m68k_point_context(target)) == 0U ? merged_key | target : m68k_point_context(target);
        auto &current = entry[key];
        const auto joined = join(current, at, m68k_exact_offset_bound);
        if (joined != current) {
          current = joined;
          changed = true;
        }
      }
    }
    if (!changed) return rel;
  }
  for (auto &[point, value] : rel) value = FiniteValue::unknown(UnknownReason::iteration_bound);
  return rel;
}

using StatusBounds = std::map<std::uint32_t, FiniteValue>;

// The per-partition status bounds the solution implies (M68kFrameConfig::status_bounds). For each live partition (the main flow and
// every analysed instance): its entry statuses (the main roots, or the instance's entry), every SR writer's result and every proven
// RTE's restored status in that partition. A partition whose clobber levels (M68kFrameConfig::clobbered) can be taken under that
// join (a level eligible under it, or a resuming synchronous child: level 0) has an Unknown bound: its boundaries may then run with
// an Unknown status. Otherwise no clobber fires there, and every status of the partition is one of the joined statuses.
StatusBounds derive_status_bounds(const M68kAnalysisImage &image, const M68kAnalysisConfig &config,
                                  const M68kFiniteAnalysisResult &result, const std::vector<std::uint32_t> &entries) {
  M68kFiniteAdapter adapter{image, config};
  const auto &frames = config.frames;
  StatusBounds out;
  const auto handlers = handler_pcs(frames);
  auto &main = out[0U];
  for (const auto entry : entries) {
    const auto pc = entry & bus_mask;
    if (!std::binary_search(handlers.begin(), handlers.end(), pc) || frames.main_entries.contains(pc))
      main = join(main, adapter.root_state(0U, pc).status);
  }
  for (const auto &[tag, instance] : frames.instances) out[tag] = join(out[tag], instance.status);
  for (const auto &[point, state] : result.solution.in_states) {
    const auto tag = m68k_point_tag(point);
    if (tag != 0U && !frames.instances.contains(tag)) continue;
    const auto decoded = adapter.decode(m68k_point_pc(point));
    if (!decoded) continue;
    const auto family = m68k_control_successors(decoded->operation).dynamic;
    if (!m68k_status_register_writer(decoded->operation.kind) && family != M68kDynamicControlFamily::return_from_exception) continue;
    auto &bound = out[tag];
    for (const auto &edge : adapter.transfer(point, state).edges) bound = join(bound, edge.state.status);
  }
  for (auto &[tag, bound] : out) {
    const auto found = frames.clobbered.find(tag);
    if (found == frames.clobbered.end()) continue;
    const bool taken = std::any_of(found->second.begin(), found->second.end(), [&](std::uint32_t level) {
      return level == 0U || m68k_interrupt_eligible(bound, level == 8U ? std::nullopt : std::optional<unsigned>(level));
    });
    if (taken) bound = FiniteValue::unknown(UnknownReason::unsupported_transfer);
  }
  return out;
}

StatusBounds join(const StatusBounds &left, const StatusBounds &right) {
  auto out = left;
  for (const auto &[tag, bound] : right) out[tag] = join(out[tag], bound);
  return out;
}

bool leq(const StatusBounds &left, const StatusBounds &right) {
  return std::all_of(left.begin(), left.end(), [&](const auto &entry) {
    const auto found = right.find(entry.first);
    return leq(entry.second, found == right.end() ? FiniteValue::bottom() : found->second);
  });
}

FrameDerivation derive_frames(const M68kAnalysisImage &image, const M68kAnalysisConfig &config, M68kFiniteAnalysisResult &result) {
  M68kFiniteAdapter adapter{image, config};
  const auto &states = result.solution.in_states;
  const auto &used = config.frames;
  FrameDerivation out;
  out.next = used;
  auto &report = result.frames;
  const bool validated = report.validated;
  report = M68kFrameReport{};
  report.enabled = true;
  report.validated = validated;
  report.handler_vectors = used.vectors.size();
  report.reset_state = used.reset_entry.has_value();

  // 1. Contributions: every boundary of a live partition (main, an analysed instance, or the unknown-entry partition) where a vector
  //    can be taken. The dead-handler partition holds only handlers that no modelled boundary takes: its boundaries never run.
  std::map<std::pair<std::uint32_t, std::uint32_t>, Contribution> contributions;  // (handler, parent tag)
  std::map<std::uint32_t, std::vector<std::uint64_t>> partition_points;
  report.potential_interrupt_vectors = used.potential_interrupts.size();
  // Every source: the delivered vectors, then the potential (installed, undelivered) interrupts, which enter writer-only instances.
  std::vector<std::pair<M68kHandlerVector, bool>> sources;  // (vector, delivered)
  for (const auto &vector : used.vectors) sources.emplace_back(vector, true);
  for (const auto &vector : used.potential_interrupts)
    if (m68k_vector_class(vector.vector) == M68kVectorClass::interrupt) sources.emplace_back(vector, false);
  for (const auto &[point, state] : states) {
    const auto tag = m68k_point_tag(point);
    const bool unmodelled = tag == m68k_unknown_entry_tag;
    if (tag != 0U && !unmodelled && !used.instances.contains(tag)) continue;  // the dead-handler partition is never a parent
    partition_points[tag].push_back(point);
    // A writer-only partition is analysed exactly like a credited one, but its points are counted apart; the unknown-entry
    // partition's points are takers but are not counted.
    const bool parent_credited = tag == 0U || unmodelled || used.instances.at(tag).credited;
    if (!parent_credited) ++report.writer_only_points;
    std::size_t ignored = 0U;
    const auto count = [&](std::size_t &counter) -> std::size_t & { return parent_credited && !unmodelled ? counter : ignored; };
    ++count(report.points);
    const auto decoded = adapter.decode(m68k_point_pc(point));
    const M68kIrOperation *operation = decoded ? &decoded->operation : nullptr;
    // An interrupt is taken at the boundary (its status); a synchronous exception while the instruction runs (after any clobbering
    // interrupt at the boundary).
    const auto &boundary = state.status;
    const auto status = adapter.effective_status(tag, state);
    if (status.is_unknown()) ++count(report.status_unknown);
    if (m68k_status_supervisor_proven(status)) ++count(report.supervisor_proven);
    const auto boundary_frame = adapter.frame_address(boundary, state);
    const auto running_frame = adapter.frame_address(status, state);
    const auto raised = m68k_raised_vectors(operation, status);
    bool eligible = false, raising = false, potential_eligible = false;
    bool unknown_frame = false;
    for (const auto &[vector, delivered] : sources) {
      const auto cls = m68k_vector_class(vector.vector);
      const bool interrupt = cls == M68kVectorClass::interrupt;
      const bool takes = interrupt ? m68k_interrupt_eligible(boundary, m68k_interrupt_level(vector.vector))
                                   : std::binary_search(raised.begin(), raised.end(), vector.vector);
      if (!takes) continue;
      (interrupt ? eligible : raising) = true;
      if (!delivered) potential_eligible = true;
      const auto &frame = interrupt ? boundary_frame : running_frame;
      unknown_frame = unknown_frame || frame.is_unknown();
      auto &contribution = contributions[{vector.handler & bus_mask, tag}];
      contribution.status = join(contribution.status, m68k_handler_entry_status(vector.vector, interrupt ? boundary : status));
      contribution.a7 = join(contribution.a7, frame);
      contribution.vectors.insert(vector.vector);
      contribution.interrupt = contribution.interrupt || interrupt;
      contribution.resuming = contribution.resuming || cls != M68kVectorClass::synchronous;
      contribution.credited = contribution.credited || (delivered && parent_credited);
      if (interrupt) contribution.interrupt_frames.push_back(frame);
      if (cls != M68kVectorClass::synchronous) contribution.taking_points.push_back(point);
    }
    if (potential_eligible) ++count(report.potential_eligible);
    if (eligible) ++count(report.interrupt_eligible);
    else if (!status.is_unknown()) ++count(report.interrupt_masked);
    if (raising) ++count(report.raising_points);
    if (unknown_frame && parent_credited && !unmodelled) {
      if (!m68k_status_supervisor_proven(eligible ? boundary : status)) ++report.frame_unproven_supervisor;
      else {
        ++report.frame_unknown_a7;
        const auto &a7 = state.address[7];
        ++report.frame_a7_unknown_by_reason[std::string(analysis::unknown_reason_name(a7.reason)) + "/" +
                                            m68k_analysis_sub_reason_name(a7.sub)];
      }
    }
  }

  // 2. Instances. An admitted contribution with a known entry A7 is an instance (the used ones keep their tags; a new one gets the
  //    next tag); any other is unanalysed: a credited one enters its handler with an Unknown entry (the unknown-entry partition), and a
  //    resuming unanalysed child makes its parent's writers and status Unknown.
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> tag_of;
  std::uint32_t next_tag = std::max(used.next_tag, 1U);
  for (const auto &[tag, instance] : used.instances) {
    tag_of[{instance.handler, instance.parent}] = tag;
    next_tag = std::max(next_tag, tag + 1U);
  }
  bool valid = true;
  std::map<std::uint32_t, M68kMemoryPolicy> async;               // derived per-partition asynchronous writers
  std::map<std::uint32_t, std::set<std::uint32_t>> clobbered;    // derived clobber levels per partition
  std::set<std::uint32_t> unknown_entries;                        // derived handlers entered with an Unknown entry
  // A handler entered only by potential interrupts (or from a writer-only partition) is counted under `<cause>/writer_only`.
  const auto unanalysed = [&](std::uint32_t handler, std::uint32_t parent, const std::set<std::uint32_t> &vectors, bool resuming,
                              bool credited, Unanalysed why) {
    ++report.unanalysed[std::string(unanalysed_name(why)) + (credited ? "" : "/writer_only")];
    if (credited) unknown_entries.insert(handler);
    if (!resuming) return;
    async[parent].async_all = true;
    const auto levels = clobber_levels(vectors);
    clobbered[parent].insert(levels.begin(), levels.end());
  };
  for (auto &[key, contribution] : contributions) {
    auto why = instance_admission(used, key.first, key.second);
    if (why == Unanalysed::none && contribution.a7.is_unknown()) why = Unanalysed::entry_unknown;
    auto found = tag_of.find(key);
    if (why != Unanalysed::none) {
      unanalysed(key.first, key.second, contribution.vectors, contribution.resuming, contribution.credited, why);
      if (found != tag_of.end() && used.instances.contains(found->second)) {
        valid = false;  // an analysed instance whose entry is no longer known
        out.next.instances.erase(found->second);
      }
      continue;
    }
    if (found == tag_of.end()) {
      if (next_tag > m68k_max_instance_tag) {
        out.failed = true;
        return out;
      }
      found = tag_of.emplace(key, next_tag++).first;
    }
    M68kHandlerInstance derived{};
    derived.handler = key.first;
    derived.parent = key.second;
    derived.status = contribution.status;
    derived.a7 = contribution.a7;
    derived.vectors = contribution.vectors;
    derived.resuming = contribution.resuming;
    derived.interrupt = contribution.interrupt;
    derived.credited = contribution.credited;
    const auto previous = used.instances.find(found->second);
    if (previous == used.instances.end()) {
      valid = false;  // a new live instance: not yet analysed
      out.next.instances[found->second] = derived;
      continue;
    }
    auto &instance = out.next.instances[found->second];
    const bool below = leq(derived.status, instance.status) && leq(derived.a7, instance.a7) &&
                       std::includes(instance.vectors.begin(), instance.vectors.end(), derived.vectors.begin(), derived.vectors.end()) &&
                       (!derived.resuming || instance.resuming) && (!derived.interrupt || instance.interrupt) &&
                       (!derived.credited || instance.credited);
    if (below) continue;
    valid = false;
    const bool grew = !leq(derived.a7, instance.a7);
    instance.status = join(instance.status, derived.status);
    instance.a7 = join(instance.a7, derived.a7);
    instance.vectors.insert(derived.vectors.begin(), derived.vectors.end());
    instance.resuming = instance.resuming || derived.resuming;
    instance.interrupt = instance.interrupt || derived.interrupt;
    instance.credited = instance.credited || derived.credited;
    if (grew && ++instance.growth > m68k_instance_growth_bound)
      instance.a7 = M68kPointsTo::unknown(UnknownReason::iteration_bound, Sub::frame_unproven);  // widening
  }
  // An instance whose entry A7 became Unknown is no longer analysed (its consequences fall on its parent); neither is any
  // instance below it.
  for (bool removed = true; removed;) {
    removed = false;
    for (auto it = out.next.instances.begin(); it != out.next.instances.end();) {
      const auto &instance = it->second;
      const bool orphan = instance.parent != 0U && !out.next.instances.contains(instance.parent);
      if (!instance.a7.is_unknown() && !orphan) {
        ++it;
        continue;
      }
      if (instance.credited) unknown_entries.insert(instance.handler);
      if (instance.resuming && !orphan) {
        out.next.policies[instance.parent].async_all = true;
        const auto levels = clobber_levels(instance.vectors);
        out.next.clobbered[instance.parent].insert(levels.begin(), levels.end());
      }
      it = out.next.instances.erase(it);
      removed = true;
    }
  }

  // 3. The writes of every analysed instance partition: absolute ranges (stores plus its interrupt frames into its parent), its
  //    A7-relative writes (relative to its entry, the saved SR) and the writes whose position relative to the entry is not known.
  std::map<std::uint32_t, M68kMemoryPolicy> writes;
  std::map<std::uint32_t, RelativeWrites> relative;
  std::map<std::uint32_t, M68kMemoryPolicy> absolute;
  std::map<std::uint32_t, std::map<std::uint64_t, FiniteValue>> rel_a7;
  for (const auto &[tag, instance] : used.instances) {
    auto &own = writes[tag];
    auto &rel_writes = relative[tag];
    auto &abs_writes = absolute[tag];
    rel_a7[tag] = relative_a7(adapter, result, tag, instance.handler);
    const auto &rel = rel_a7[tag];
    for (const auto point : partition_points[tag]) {
      const auto &state = states.at(point);
      const auto decoded = adapter.decode(m68k_point_pc(point));
      if (!decoded) continue;
      const auto &operation = decoded->operation;
      const auto status = adapter.effective_status(tag, state);
      const auto description = m68k_memory_writes(operation);
      const auto targets = adapter.memory_write_targets(operation, state, status);
      for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto &[target, span] = targets[i];
        if (target.is_bottom()) continue;
        const auto touched = m68k_memory_touched(target, span);
        if (!touched) {
          own.async_all = true;
          abs_writes.async_all = true;
          if (instance.resuming) ++report.unknown_target_writer_stores;
        } else {
          for (const auto &range : *touched) own.add_async(range);
        }
        const auto offset = description.described && i < description.writes.size()
                                ? a7_relative_offset(operation, description.writes[i])
                                : std::nullopt;
        const auto found = rel.find(point);
        if (offset && found != rel.end() && found->second.is_precise()) {
          for (const auto r : found->second.values()) rel_writes.add(signed32(r) + *offset, signed32(r) + *offset + span);
        } else if (touched) {
          for (const auto &range : *touched) abs_writes.add_async(range);
        }
      }
    }
    const auto contribution = contributions.find({instance.handler, instance.parent});
    if (contribution == contributions.end()) continue;
    for (const auto &frame : contribution->second.interrupt_frames) {
      const auto touched = m68k_memory_touched(frame, m68k_exception_frame_bytes);
      if (!touched) own.async_all = true;
      else
        for (const auto &range : *touched) own.add_async(range);
    }
  }

  // 4. Per-partition asynchronous writers: a partition receives the writes (stores and interrupt frames) of every analysed resuming
  //    child instance and, transitively, that child's own asynchronous writers (they persist when the child resumes into it).
  const auto analysed_resuming = [&](std::uint32_t tag) {
    const auto found = out.next.instances.find(tag);
    return used.instances.contains(tag) && found != out.next.instances.end() && found->second.resuming;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto &[tag, instance] : used.instances) {
      if (!analysed_resuming(tag)) continue;
      const auto contributed = join(writes[tag], async[tag]);
      auto &target = async[instance.parent];
      if (leq(contributed, target)) continue;
      target = join(target, contributed);
      changed = true;
    }
  }
  for (const auto &[tag, instance] : used.instances)
    if (analysed_resuming(tag)) out.writer_tags.insert(tag);

  // 5. Frame integrity: a resuming instance whose own writes, or those of its resuming descendants, may reach its saved SR word makes
  //    the status of its parent Unknown after every boundary where it can be taken (the analysis never assumes the SR an RTE
  //    restores).
  std::map<std::uint32_t, RelativeWrites> subtree;
  std::map<std::uint32_t, M68kMemoryPolicy> subtree_absolute;
  for (const auto &[tag, instance] : used.instances) {
    subtree[tag] = relative[tag];
    subtree_absolute[tag] = join(absolute[tag], async[tag].async_all ? async[tag] : M68kMemoryPolicy{});
  }
  // Children have larger tags than their parents: one pass in decreasing tag order.
  for (auto it = used.instances.rbegin(); it != used.instances.rend(); ++it) {
    const auto tag = it->first;
    const auto parent = it->second.parent;
    if (!analysed_resuming(tag) || parent == 0U || !used.instances.contains(parent)) continue;
    subtree_absolute[parent] = join(subtree_absolute[parent], subtree_absolute[tag]);
    const auto contribution = contributions.find({it->second.handler, parent});
    if (contribution == contributions.end()) continue;
    const auto &rel = rel_a7[parent];
    for (const auto point : contribution->second.taking_points) {
      const auto at = rel.find(point);
      if (at == rel.end() || !at->second.is_precise()) {
        subtree[parent].add_shifted(RelativeWrites{true, {}}, 0);
        continue;
      }
      for (const auto r : at->second.values()) {
        // The child's entry (its saved SR) is 6 bytes below the parent's A7 at the taking boundary.
        RelativeWrites child = subtree[tag];
        if (it->second.interrupt) child.add(0, static_cast<std::int64_t>(m68k_exception_frame_bytes));
        subtree[parent].add_shifted(child, signed32(r) - static_cast<std::int64_t>(m68k_exception_frame_bytes));
      }
    }
  }
  for (const auto &[tag, instance] : used.instances) {
    if (!analysed_resuming(tag)) continue;
    bool intact = !subtree[tag].overlaps(0, 2);
    if (intact) {
      const auto &abs_writes = subtree_absolute[tag];
      const auto sr = m68k_memory_touched(instance.a7, 2U);
      if (!sr || abs_writes.async_all) intact = !abs_writes.async_all && abs_writes.async.empty();
      else
        for (const auto &range : *sr) intact = intact && !abs_writes.asynchronous(M68kCell{range.kind, range.id, range.lo, range.hi - range.lo});
    }
    if (intact) continue;
    ++report.frame_integrity_failures;
    const auto levels = clobber_levels(instance.vectors);
    clobbered[instance.parent].insert(levels.begin(), levels.end());
  }

  // 6. The next configuration (join-only) and the final validation of this round.
  for (const auto &[tag, policy] : async) {
    const auto previous = used.policies.find(tag);
    if (previous == used.policies.end() ? !(policy == M68kMemoryPolicy{}) : !leq(policy, previous->second)) valid = false;
    out.next.policies[tag] = join(out.next.policies[tag], policy);
  }
  for (const auto &[tag, levels] : clobbered) {
    const auto previous = used.clobbered.find(tag);
    if (previous == used.clobbered.end() ? !levels.empty()
                                         : !std::includes(previous->second.begin(), previous->second.end(), levels.begin(), levels.end()))
      valid = false;
    out.next.clobbered[tag].insert(levels.begin(), levels.end());
  }
  if (!std::includes(used.unknown_entries.begin(), used.unknown_entries.end(), unknown_entries.begin(), unknown_entries.end()))
    valid = false;
  out.next.unknown_entries.insert(unknown_entries.begin(), unknown_entries.end());
  out.next.next_tag = next_tag;
  out.valid = valid;

  // Report (counts only).
  std::set<std::uint32_t> live_handlers;
  for (const auto &[tag, instance] : used.instances) {
    ++report.instances;
    if (instance.credited) live_handlers.insert(instance.handler);
    else ++report.writer_only_instances;
    if (instance.interrupt) ++report.interrupt_instances;
    else if (instance.resuming) ++report.resuming_instances;
    else ++report.synchronous_instances;
  }
  for (const auto handler : handler_pcs(used)) report.dead_handlers += live_handlers.contains(handler) ? 0U : 1U;
  report.unknown_entry_handlers = used.unknown_entries.size();
  report.clobbered_partitions = used.clobbered.size();
  if (const auto main = used.policies.find(0U); main != used.policies.end()) {
    report.main_async_all = main->second.async_all;
    report.main_async_ranges = main->second.async.size();
    for (const auto &range : main->second.async) report.main_async_bytes += range.hi - range.lo;
  }
  return out;
}

std::set<std::uint32_t> resolved_sites(const M68kFiniteAnalysisResult &result) {
  std::set<std::uint32_t> out;
  for (const auto &[pc, site] : result.pc_index_sites)
    if (site.outcome == M68kPcIndexOutcome::resolved) out.insert(pc);
  for (const auto &[pc, site] : result.address_sites)
    if (site.resolved) out.insert(pc);
  for (const auto &[pc, site] : result.return_sites)
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
  const bool frames = config.domains.frames;
  const auto original = config;
  auto free_config = config;
  free_config.domains.contexts = false;
  free_config.domains.frames = false;
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
  if (frames) {
    // SEG-030-T006: the asynchronous writers are per partition and grow from nothing (the T004 policy, every cell asynchronous
    // under the untracked mask, is only the fallback); the instances grow from none.
    config.memory.interrupts = false;
    config.memory.policy = M68kMemoryPolicy{};
    config.frames.instances.clear();
    config.frames.policies.clear();
    config.frames.clobbered.clear();
    config.frames.unknown_entries.clear();
  }
  // SEG-030-T006: when the frames domain cannot return a validated round, the T005 contexts result is returned (reason recorded).
  const auto without_frames = [&](UnknownReason reason, const std::string &failure, std::uint32_t rounds, std::size_t total) {
    auto fallback = original;
    fallback.domains.frames = false;
    auto out = analyze_contexts(image, entries, fallback, bounds);
    out.frames = M68kFrameReport{};
    out.frames.enabled = true;
    out.frames.reason = reason;
    out.frames.failure = failure + " after " + std::to_string(rounds) + " rounds, " + std::to_string(total) + " iterations";
    return out;
  };
  const auto seeded = seed_merged(image, free_config, memory);
  config.contexts.merged.insert(seeded.begin(), seeded.end());
  const auto round_bound = std::min(config.contexts.round_bound, m68k_memory_round_bound);
  std::optional<M68kFiniteAnalysisResult> best;
  std::size_t iterations = 0U;
  bool converged = false;
  UnknownReason failure = UnknownReason::iteration_bound;
  // SEG-030-T006 warm start: with the frames configuration held at nothing (no handler instance, no asynchronous writer, which is
  // never returned), the contexts rounds first settle the callee summaries, so that the frames configuration then grows from nothing
  // over precise summaries (a least fixed point) instead of from the opaque continuations of the first rounds.
  std::uint32_t warm_rounds = 0U;
  while (frames && warm_rounds < round_bound) {
    ++warm_rounds;
    auto out = solve_pinned(image, entries, config, bounds);
    iterations += out.iterations;
    if (!out.complete) return without_frames(out.reason, "warm_solve_bound", warm_rounds, iterations);
    const auto policy = derive_memory_policy(image, config, out);
    out.contexts.rounds = warm_rounds;
    const auto derivation = derive_contexts(image, config, out);
    auto next = config;
    next.memory.policy = join(config.memory.policy, policy);
    next.contexts = derivation.next;
    next.frames.status_bounds = join(config.frames.status_bounds, derive_status_bounds(image, config, out, entries));
    const bool fixed = derivation.valid && next.contexts == config.contexts && next.memory.policy == config.memory.policy &&
                       next.frames.status_bounds == config.frames.status_bounds;
    config = std::move(next);
    if (fixed) break;
  }
  std::uint32_t round = 1U;
  M68kFrameReport first_round;
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
    FrameDerivation frame_derivation;
    if (frames) {
      frame_derivation = derive_frames(image, config, out);
      if (frame_derivation.failed) return without_frames(UnknownReason::state_bound, "instance_bound", round, iterations);
      const auto bounds = derive_status_bounds(image, config, out, entries);
      if (!leq(bounds, config.frames.status_bounds)) frame_derivation.valid = false;
      frame_derivation.next.status_bounds = join(config.frames.status_bounds, bounds);
      std::size_t writer_points = 0U;
      for (const auto &[point, state] : out.solution.in_states) {
        (void)state;
        if (frame_derivation.writer_tags.contains(m68k_point_tag(point))) ++writer_points;
      }
      out.memory.handler_points = writer_points;
    }
    const bool valid = derivation.valid && leq(policy, config.memory.policy) && (!frames || frame_derivation.valid);
    out.memory.rounds = memory_rounds + round;
    out.memory.policy = config.memory.policy;
    out.memory.converged = valid;
    if (frames) {
      out.frames.validated = valid;
      // The reported memory policy is the main flow's (the global external writer plus the main partition's asynchronous writers).
      if (const auto main = config.frames.policies.find(0U); main != config.frames.policies.end())
        out.memory.policy = join(out.memory.policy, main->second);
      // The first frames round's boundary counts, before any clobbered status propagates (diagnosis of the first loss).
      if (round == 1U) first_round = out.frames;
      out.frames.first_round_points = first_round.points;
      out.frames.first_round_status_unknown = first_round.status_unknown;
      out.frames.first_round_interrupt_eligible = first_round.interrupt_eligible;
      out.frames.first_round_interrupt_masked = first_round.interrupt_masked;
      out.frames.first_round_frame_unknown_a7 = first_round.frame_unknown_a7;
      out.frames.first_round_frame_unproven_supervisor = first_round.frame_unproven_supervisor;
      out.frames.first_round_a7_unknown_by_reason = first_round.frame_a7_unknown_by_reason;
    }
    auto next = config;
    next.memory.policy = join(config.memory.policy, policy);
    next.contexts = derivation.next;
    // SEG-030-T006: the frames configuration grows only from a round whose contexts configuration is settled (its summaries are
    // those of the current partitions), so a new handler instance is never judged on the opaque continuations of its first round.
    const bool settled = derivation.valid && next.contexts == config.contexts && next.memory.policy == config.memory.policy;
    if (frames && settled) next.frames = frame_derivation.next;
    const bool fixed = next.contexts == config.contexts && next.memory.policy == config.memory.policy && next.frames == config.frames;
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
  if (!best) {
    if (frames) return without_frames(failure, "no_validated_round", std::min(round, round_bound), iterations);
    return finish_without(failure, std::min(round, round_bound), iterations);
  }
  auto out = std::move(*best);
  if (frames) {
    out.frames.warm_rounds = warm_rounds;
    out.frames.frame_rounds = std::min(round, round_bound);
  }
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
  // SEG-030-T006: the frames domain implies the contexts domain (handler partitions are contexts), which implies memory and address.
  if (config.domains.frames) config.domains.contexts = true;
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

// Both queries join over every point of the PC (every call context and handler partition): reading one context's point alone would
// present a context-restricted fact as the fact of the instruction. Without contexts the PC is its only point.
analysis::FiniteValue m68k_query_data_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg,
                                               unsigned width) {
  if (!result.solution.complete) return FiniteValue::unknown(result.solution.reason);
  FiniteValue out = FiniteValue::bottom();
  for (const auto &[point, state] : m68k_points_of(result, pc)) {
    (void)point;
    out = join(out, state->values.values[m68k_analysis_slot(reg, width)]);
  }
  return out;
}

M68kPointsTo m68k_query_address_register(const M68kFiniteAnalysisResult &result, std::uint32_t pc, unsigned reg) {
  if (!result.solution.complete) return M68kPointsTo::unknown(result.solution.reason);
  auto out = M68kPointsTo::bottom();
  for (const auto &[point, state] : m68k_points_of(result, pc)) {
    (void)point;
    out = join(out, state->address[reg & 7U]);
  }
  return out;
}

const char *m68k_return_slot_premise_name(M68kReturnSlotPremise premise) noexcept {
  switch (premise) {
  case M68kReturnSlotPremise::none: return "none";
  case M68kReturnSlotPremise::slot_untracked: return "slot_untracked";
  case M68kReturnSlotPremise::unknown_target_store: return "unknown_target_store";
  case M68kReturnSlotPremise::opaque_callee: return "opaque_callee";
  case M68kReturnSlotPremise::async_writer: return "async_writer";
  case M68kReturnSlotPremise::external_writer: return "external_writer";
  }
  return "invalid";
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
    if (result.frames.enabled) out << " status=" << state.status.describe();
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
  for (const auto &[pc, site] : result.return_sites) {
    out << "return-site " << hex(pc) << ' ' << m68k_dynamic_control_family_name(site.family) << ' '
        << (site.resolved ? "resolved" : "unknown");
    if (!site.resolved)
      out << " reason=" << analysis::unknown_reason_name(site.reason) << '/' << m68k_analysis_sub_reason_name(site.sub);
    out << " targets=";
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
  if (result.return_slots.enabled) {
    const auto &slots = result.return_slots;
    out << "return-slots sites=" << slots.sites << " normal=" << slots.normal_sites << " premise=" << slots.premise_sites
        << " computed=" << slots.computed_sites << " unknown=" << slots.unknown_sites;
    for (const auto &[cause, n] : slots.premise_by_cause) out << " premise_" << m68k_return_slot_premise_name(cause) << '=' << n;
    out << '\n';
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
  if (result.frames.enabled) {
    const auto &frames = result.frames;
    out << "frames validated=" << (frames.validated ? 1 : 0) << " warm=" << frames.warm_rounds << " rounds=" << frames.frame_rounds
        << " instances=" << frames.instances << " interrupt=" << frames.interrupt_instances
        << " resuming=" << frames.resuming_instances << " synchronous=" << frames.synchronous_instances
        << " writer_only=" << frames.writer_only_instances << " writer_only_points=" << frames.writer_only_points
        << " dead=" << frames.dead_handlers << " unknown_entry=" << frames.unknown_entry_handlers << " integrity_failures=" << frames.frame_integrity_failures
        << " clobbered=" << frames.clobbered_partitions << " eligible=" << frames.interrupt_eligible
        << " masked=" << frames.interrupt_masked << " main_async_all=" << (frames.main_async_all ? 1 : 0)
        << " main_async_bytes=" << frames.main_async_bytes;
    for (const auto &[name, n] : frames.unanalysed) out << " unanalysed_" << name << '=' << n;
    if (!frames.validated) out << " failure=" << frames.failure;
    out << '\n';
  }
  return out.str();
}

}  // namespace segarecomp
