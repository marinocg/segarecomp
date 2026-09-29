// SEG-026-T002 (experiment, report-only). See finite_register_values.hpp.

#include "segarecomp/cpu/m68k/finite_register_values.hpp"

#include <algorithm>

#include "segarecomp/cpu/m68k/effects.hpp"

namespace segarecomp {
namespace {

std::uint32_t width_mask(unsigned width) { return width >= 32U ? UINT32_MAX : (UINT32_C(1) << width) - 1U; }

unsigned size_bits(M68kMemoryAccessWidth size) {
  return size == M68kMemoryAccessWidth::byte ? 8U : size == M68kMemoryAccessWidth::word ? 16U : 32U;
}

std::uint32_t sign_extend(std::uint32_t value, unsigned bits) {
  if (bits >= 32U) return value;
  const auto sign = UINT32_C(1) << (bits - 1U);
  value &= (UINT32_C(1) << bits) - 1U;
  return (value ^ sign) - sign;
}

bool is_data_register(const M68kEffectiveAddress &ea, unsigned reg) {
  return ea.mode == M68kEaMode::data_register && ea.reg == reg;
}

// Pointwise image of `input` under `f`; Unknown stays Unknown.
template <typename F>
M68kFiniteValues map_values(const M68kFiniteValues &input, unsigned width, F f) {
  if (!input.known) return M68kFiniteValues::unknown();
  std::vector<std::uint32_t> out;
  out.reserve(input.values.size());
  for (const auto value : input.values) out.push_back(f(value));
  auto result = M68kFiniteValues::of(std::move(out), width);
  result.width_derived = result.known && input.width_derived;
  return result;
}

// An explicit narrowing operation (mask, right shift, guard) clears `width_derived` only when it cuts the maximum.
void narrow(const M68kFiniteValues &before, M68kFiniteValues &after) {
  if (!after.known) return;
  after.width_derived = before.width_derived && !(before.known && !before.values.empty() && !after.values.empty() &&
                                                  after.values.back() < before.values.back());
}

// Cross product image; bails to Unknown before exceeding the limit.
template <typename F>
M68kFiniteValues combine(const M68kFiniteValues &left, const M68kFiniteValues &right, unsigned width, F f) {
  if (!left.known || !right.known) return M68kFiniteValues::unknown();
  if (left.values.size() * right.values.size() > m68k_finite_values_limit * 16U) return M68kFiniteValues::unknown();
  std::vector<std::uint32_t> out;
  for (const auto a : left.values)
    for (const auto b : right.values) out.push_back(f(a, b));
  auto result = M68kFiniteValues::of(std::move(out), width);
  result.width_derived = result.known && (left.width_derived || right.width_derived);
  return result;
}

struct Source {
  M68kFiniteValues values;  // low `bits` of the source operand (width = query width)
  std::uint32_t proof{};
  bool same_register{};     // the source is the destination register itself (pointwise, not a cross product)
};

// The source operand of an ALU/MOVE into Dreg, restricted to its low `bits`.
Source read_source(const M68kEffectiveAddress &ea, unsigned reg, unsigned bits, unsigned width,
                   M68kFiniteValueInputs &inputs, M68kFiniteTransfer &transfer) {
  const auto mask = width_mask(bits);
  Source out{};
  switch (ea.mode) {
  case M68kEaMode::immediate:
    out.values = M68kFiniteValues::of({ea.immediate_value & mask}, width);
    out.proof = m68k_finite_proof::constant;
    return out;
  case M68kEaMode::data_register:
    if (ea.reg == reg) {
      out.same_register = true;
      return out;
    }
    out.values = map_values(inputs.data_register_before(ea.reg, width), width, [&](std::uint32_t v) { return v & mask; });
    out.proof = m68k_finite_proof::register_copy;
    return out;
  case M68kEaMode::pc_index8: {
    if (ea.index_is_address) return out;  // An index: not tracked
    const unsigned index_width = ea.index_is_long ? 32U : 16U;
    const auto index = inputs.data_register_before(ea.index_reg, index_width);
    if (!index.known) return out;
    std::vector<std::uint32_t> values;
    for (const auto value : index.values) {
      const auto address = m68k_pc_index_address(ea, value);
      if (bits > 8U && (address & 1U) != 0U) {
        ++transfer.misaligned_reads_excluded;
        continue;
      }
      const auto read = inputs.immutable_read(address, bits / 8U);
      ++transfer.table_reads;
      if (!read) {
        transfer.immutable_read_failed = true;
        return Source{};
      }
      values.push_back(*read & mask);
    }
    out.values = M68kFiniteValues::of(std::move(values), width);
    out.values.width_derived = out.values.known && index.width_derived;  // entries selected by a width-only index
    out.proof = m68k_finite_proof::immutable_load;
    return out;
  }
  case M68kEaMode::pc_disp16:
  case M68kEaMode::absolute_word:
  case M68kEaMode::absolute_long: {
    const auto address = ea.absolute_address & UINT32_C(0x00FFFFFF);
    if (bits > 8U && (address & 1U) != 0U) return out;
    if (const auto read = inputs.immutable_read(address, bits / 8U)) {
      ++transfer.table_reads;
      out.values = M68kFiniteValues::of({*read & mask}, width);
      out.proof = m68k_finite_proof::immutable_load;
      return out;
    }
    break;  // mutable memory: fall through to the byte rule below
  }
  default: break;
  }
  const bool memory = ea.mode == M68kEaMode::address_indirect || ea.mode == M68kEaMode::address_postinc ||
                      ea.mode == M68kEaMode::address_predec || ea.mode == M68kEaMode::address_disp16 ||
                      ea.mode == M68kEaMode::address_index8 || ea.mode == M68kEaMode::absolute_word ||
                      ea.mode == M68kEaMode::absolute_long || ea.mode == M68kEaMode::pc_disp16;
  if (memory && bits == 8U) {
    std::vector<std::uint32_t> all(256U);
    for (std::uint32_t v = 0; v < 256U; ++v) all[v] = v;
    out.values = M68kFiniteValues::of(std::move(all), width);
    out.values.width_derived = true;
    out.proof = m68k_finite_proof::byte_load;
  }
  return out;
}

}  // namespace

M68kFiniteValues M68kFiniteValues::of(std::vector<std::uint32_t> values, unsigned width) {
  const auto mask = width_mask(width);
  for (auto &value : values) value &= mask;
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  if (values.size() > m68k_finite_values_limit) return unknown();
  return M68kFiniteValues{true, std::move(values)};
}

M68kFiniteValues m68k_finite_union(const M68kFiniteValues &left, const M68kFiniteValues &right, unsigned width) {
  if (!left.known || !right.known) return M68kFiniteValues::unknown();
  auto values = left.values;
  values.insert(values.end(), right.values.begin(), right.values.end());
  auto result = M68kFiniteValues::of(std::move(values), width);
  result.width_derived = result.known && (left.width_derived || right.width_derived);
  return result;
}

const char *m68k_finite_proof::name(std::uint32_t bit_index) noexcept {
  switch (bit_index) {
  case 0: return "constant";
  case 1: return "mask";
  case 2: return "byte_load";
  case 3: return "immutable_load";
  case 4: return "shift";
  case 5: return "add_sub";
  case 6: return "sign_extend";
  case 7: return "guard";
  case 8: return "register_copy";
  case 9: return "logical";
  case 10: return "call_edge";
  case 11: return "dynamic_edge";
  default: return "unknown";
  }
}

std::uint32_t m68k_pc_index_address(const M68kEffectiveAddress &ea, std::uint32_t index) noexcept {
  const auto offset = ea.index_is_long ? index : sign_extend(index, 16U);
  return (ea.pc_base_address + static_cast<std::uint32_t>(static_cast<std::int32_t>(ea.displacement)) + offset) &
         UINT32_C(0x00FFFFFF);
}

M68kFiniteTransfer m68k_finite_register_after(const M68kIrOperation &operation, unsigned reg, unsigned width,
                                              M68kFiniteValueInputs &inputs) {
  M68kFiniteTransfer transfer{};
  if (operation.kind == M68kIrKind::write_moveq) {
    // `m68k_operation_effect` deliberately keeps its legacy D0-only MOVEQ write (see effects.cpp); the decoded
    // destination field is the architectural register MOVEQ writes (all 32 bits).
    if (static_cast<unsigned>(operation.destination) != reg) return transfer;
    transfer.writes = true;
    transfer.values = M68kFiniteValues::of({sign_extend(static_cast<std::uint8_t>(operation.operand), 8U)}, width);
    transfer.proof = m68k_finite_proof::constant;
    return transfer;
  }
  switch (operation.kind) {
  // Control transfers and NOP write no data register (M68000PRM: JMP/Bcc/BRA/NOP touch only the PC; JSR/BSR only
  // the PC, SP and the pushed return address). The effect owner does not claim a complete footprint for every
  // one of them, so this fact is stated here rather than inferred from an absent footprint.
  case M68kIrKind::jump_general:
  case M68kIrKind::call_general:
  case M68kIrKind::bsr_call:
  case M68kIrKind::general_branch:
  case M68kIrKind::branch_ne_short:
  case M68kIrKind::branch_always_short:
  case M68kIrKind::no_operation: return transfer;
  default: break;
  }
  const auto effect = m68k_operation_effect(operation);
  if (effect.register_write_footprint_complete && ((effect.data_register_write_mask >> reg) & 1U) == 0U) return transfer;
  transfer.writes = true;  // Unknown unless a supported exact form below says otherwise
  if (!effect.register_write_footprint_complete) return transfer;

  const auto wmask = width_mask(width);
  const unsigned bits = std::min(size_bits(operation.size), width);  // low slice the operation defines
  const auto smask = width_mask(bits);
  const auto merge = [&](std::uint32_t old, std::uint32_t low) { return ((old & ~smask) | (low & smask)) & wmask; };
  const auto old_values = [&]() {
    if (bits >= width) return M68kFiniteValues::of({0U}, width);  // fully overwritten: `old` is irrelevant
    return inputs.data_register_before(reg, width);
  };
  const bool destination_is_reg = is_data_register(operation.destination_ea, reg);

  switch (operation.kind) {
  case M68kIrKind::write_clr:
    if (!destination_is_reg) return transfer;
    transfer.values = map_values(old_values(), width, [&](std::uint32_t old) { return merge(old, 0U); });
    transfer.proof = m68k_finite_proof::constant;
    return transfer;
  case M68kIrKind::write_move: {
    if (!destination_is_reg) return transfer;
    const auto source = read_source(operation.source_ea, reg, bits, width, inputs, transfer);
    if (transfer.immutable_read_failed) return transfer;
    if (source.same_register) {
      transfer.values = inputs.data_register_before(reg, width);
      return transfer;
    }
    transfer.values = combine(old_values(), source.values, width, merge);
    transfer.proof = source.proof;
    return transfer;
  }
  case M68kIrKind::logical_and:
  case M68kIrKind::logical_and_immediate:
  case M68kIrKind::logical_or:
  case M68kIrKind::logical_or_immediate:
  case M68kIrKind::exclusive_or:
  case M68kIrKind::exclusive_or_immediate:
  case M68kIrKind::add:
  case M68kIrKind::add_immediate:
  case M68kIrKind::add_quick:
  case M68kIrKind::subtract:
  case M68kIrKind::subtract_immediate:
  case M68kIrKind::subtract_quick: {
    if (!destination_is_reg) return transfer;
    const auto op = [&](std::uint32_t a, std::uint32_t b) -> std::uint32_t {
      switch (operation.kind) {
      case M68kIrKind::logical_and:
      case M68kIrKind::logical_and_immediate: return merge(a, a & b);
      case M68kIrKind::logical_or:
      case M68kIrKind::logical_or_immediate: return merge(a, a | b);
      case M68kIrKind::exclusive_or:
      case M68kIrKind::exclusive_or_immediate: return merge(a, a ^ b);
      case M68kIrKind::add:
      case M68kIrKind::add_immediate:
      case M68kIrKind::add_quick: return merge(a, a + b);
      default: return merge(a, a - b);
      }
    };
    const auto proof = operation.kind == M68kIrKind::logical_and || operation.kind == M68kIrKind::logical_and_immediate
                           ? m68k_finite_proof::mask
                       : operation.kind == M68kIrKind::logical_or || operation.kind == M68kIrKind::logical_or_immediate ||
                               operation.kind == M68kIrKind::exclusive_or ||
                               operation.kind == M68kIrKind::exclusive_or_immediate
                           ? m68k_finite_proof::logical
                           : m68k_finite_proof::add_sub;
    const auto source = read_source(operation.source_ea, reg, bits, width, inputs, transfer);
    if (transfer.immutable_read_failed) return transfer;
    const auto old = inputs.data_register_before(reg, width);
    const bool and_kind = operation.kind == M68kIrKind::logical_and || operation.kind == M68kIrKind::logical_and_immediate;
    if (and_kind && !old.known && bits >= width && source.values.known && source.values.values.size() == 1U) {
      // KnownMask: an unknown register ANDed with one constant covering the whole tracked width takes exactly the
      // submasks of that constant (2^popcount values; beyond the limit it stays Unknown).
      const auto constant = source.values.values.front() & wmask;
      if ((UINT64_C(1) << __builtin_popcount(constant)) > m68k_finite_values_limit) return transfer;
      std::vector<std::uint32_t> submasks;
      for (std::uint32_t sub = constant;; sub = (sub - 1U) & constant) {
        submasks.push_back(sub);
        if (sub == 0U) break;
      }
      transfer.values = M68kFiniteValues::of(std::move(submasks), width);
    } else if (source.same_register) {
      transfer.values = map_values(old, width, [&](std::uint32_t a) { return op(a, a); });
    } else {
      transfer.values = combine(old, source.values, width, op);
      transfer.proof = source.proof & ~m68k_finite_proof::constant;
      if (and_kind) narrow(old, transfer.values);
    }
    transfer.proof |= proof;
    return transfer;
  }
  case M68kIrKind::shift_rotate_register: {
    if (!destination_is_reg || operation.source_ea.mode != M68kEaMode::immediate) return transfer;
    const unsigned count = operation.source_ea.immediate_value;
    const unsigned op_bits = size_bits(operation.size);
    const auto kind = operation.shift_rotate_kind;
    const bool left = kind == M68kShiftRotateKind::lsl || kind == M68kShiftRotateKind::asl;
    const bool right = kind == M68kShiftRotateKind::lsr || kind == M68kShiftRotateKind::asr;
    // A left shift's low `width` bits depend only on the low `width` input bits; a right shift needs the whole
    // operation width.
    if (!left && !(right && op_bits <= width)) return transfer;
    const auto opmask = width_mask(op_bits);
    const auto old = inputs.data_register_before(reg, width);
    transfer.values = map_values(old, width, [&](std::uint32_t a) {
      std::uint32_t result;
      if (left) result = count >= 32U ? 0U : (a << count);
      else if (kind == M68kShiftRotateKind::lsr) result = count >= 32U ? 0U : ((a & opmask) >> count);
      else result = static_cast<std::uint32_t>(static_cast<std::int32_t>(sign_extend(a & opmask, op_bits)) >>
                                                (count >= 31U ? 31U : count));
      return op_bits >= width ? result & wmask : ((a & ~opmask) | (result & opmask)) & wmask;
    });
    if (right) narrow(old, transfer.values);
    transfer.proof = m68k_finite_proof::shift;
    return transfer;
  }
  case M68kIrKind::sign_extend_word: {  // EXT.W: byte -> word
    if (!destination_is_reg) return transfer;
    const auto old = inputs.data_register_before(reg, width);
    transfer.values = map_values(old, width, [&](std::uint32_t a) {
      return ((a & ~UINT32_C(0xFFFF)) | (sign_extend(a, 8U) & 0xFFFFU)) & wmask;
    });
    transfer.proof = m68k_finite_proof::sign_extend;
    return transfer;
  }
  case M68kIrKind::sign_extend_long: {  // EXT.L: word -> long (the low word is unchanged)
    if (!destination_is_reg) return transfer;
    const auto old = inputs.data_register_before(reg, width);
    transfer.values = map_values(old, width, [&](std::uint32_t a) { return sign_extend(a, 16U) & wmask; });
    transfer.proof = m68k_finite_proof::sign_extend;
    return transfer;
  }
  default: return transfer;
  }
}

bool m68k_finite_branch_filter(const M68kIrOperation &flag_setter, const M68kIrOperation &branch, bool taken,
                               unsigned reg, unsigned width, M68kFiniteValues &values) {
  if (!values.known || branch.kind != M68kIrKind::general_branch || branch.condition == M68kCondition::always ||
      branch.condition == M68kCondition::never)
    return false;
  const unsigned bits = size_bits(flag_setter.size);
  if (bits > width) return false;
  std::optional<std::uint32_t> immediate;
  bool test = false;
  if ((flag_setter.kind == M68kIrKind::compare_immediate || flag_setter.kind == M68kIrKind::compare) &&
      is_data_register(flag_setter.destination_ea, reg) && flag_setter.source_ea.mode == M68kEaMode::immediate) {
    immediate = flag_setter.source_ea.immediate_value;
  } else if (flag_setter.kind == M68kIrKind::test_operand && is_data_register(flag_setter.source_ea, reg)) {
    test = true;
  } else {
    return false;
  }
  const auto mask = width_mask(bits);
  std::vector<std::uint32_t> kept;
  for (const auto value : values.values) {
    std::uint16_t ccr = 0U;
    if (test) {
      const auto v = value & mask;
      if (v == 0U) ccr |= 0x4U;
      if (((v >> (bits - 1U)) & 1U) != 0U) ccr |= 0x8U;
    } else {
      const auto r = m68k_evaluate_subtraction(*immediate, value, flag_setter.size);
      ccr = static_cast<std::uint16_t>((r.negative ? 0x8U : 0U) | (r.zero ? 0x4U : 0U) | (r.overflow ? 0x2U : 0U) |
                                       (r.carry ? 0x1U : 0U));
    }
    if (M68kConditionSpecification::evaluate(branch.condition, ccr) == taken) kept.push_back(value);
  }
  auto filtered = M68kFiniteValues::of(std::move(kept), width);
  narrow(values, filtered);
  values = std::move(filtered);
  return true;
}

}  // namespace segarecomp
