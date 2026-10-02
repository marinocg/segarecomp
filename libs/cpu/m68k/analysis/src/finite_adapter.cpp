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
    const M68kRegion region{extent->kind, extent->id, (value & ~bus_mask) | extent->base, extent->size};
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
    if (src.mode == M68kEaMode::address_register && operation.size == M68kMemoryAccessWidth::long_word)
      destination(dst.reg, in.address[src.reg & 7U]);
    else destination(dst.reg, classify(image_, source_values(image_, in, src, operation.size)));
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
  // SEG-030-T003: LEA and PEA only compute an address (M68000PRM: no data register is written). The data owner reports them as
  // writing every Dn (their footprint is not claimed complete); with the address domain enabled the data registers are kept.
  const bool address_only = config_.domains.address && (operation.kind == M68kIrKind::load_effective_address ||
                                                        operation.kind == M68kIrKind::push_effective_address);
  for (unsigned reg = 0; reg < 8U && !address_only; ++reg) {
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
  if (config_.domains.address) transfer_address_registers(operation, in, out);

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
      if (config_.domains.address)
        for (unsigned reg = 0; reg < 8U; ++reg) (void)address_branch_filter(setter->operation, operation, taken, reg, edge.address[reg]);
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
  case M68kDynamicControlFamily::jump_address_indirect:
  case M68kDynamicControlFamily::call_address_indirect:
  case M68kDynamicControlFamily::jump_address_disp16:
  case M68kDynamicControlFamily::call_address_disp16:
  case M68kDynamicControlFamily::jump_address_index:
  case M68kDynamicControlFamily::call_address_index:
    if (config_.domains.address) {
      const auto report = evaluate_address_site(pc, operation, control.dynamic, in);
      if (report.resolved) {
        State edge = out;
        edge.flag_setter.reset();
        for (const auto target : report.targets) result.edges.push_back({target, EdgeKind::computed, edge});
      } else {
        result.unresolved_computed.emplace(report.reason);
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
    out.address_domain = config.domains.address;
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
      if (config.domains.address) {
        const auto family = m68k_control_successors(decoded->operation).dynamic;
        if (is_address_site_family(family)) {
          auto report = adapter.evaluate_address_site(pc, decoded->operation, family, state);
          if (!config.pinned_sites.contains(pc) && out.solution.pinned.contains(point)) invalidated.insert(pc);
          out.address_sites.emplace(pc, std::move(report));
          continue;
        }
      }
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
    const auto found = result.reached.find(static_cast<std::uint32_t>(point));
    if (found == result.reached.end()) out << " undecodable";
    else out << " len=" << found->second;
    out << " flags=" << (state.flag_setter ? hex(*state.flag_setter) : std::string("-"));
    for (std::size_t slot = 0; slot < m68k_analysis_slot_count; ++slot)
      out << " d" << slot / 2U << (slot % 2U == 0U ? ".w=" : ".l=") << state.values.values[slot].describe()
          << (state.width_derived[slot] ? "~" : "");
    if (result.address_domain)
      for (unsigned reg = 0; reg < 8U; ++reg) out << " a" << reg << '=' << state.address[reg].describe();
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
  return out.str();
}

}  // namespace segarecomp
