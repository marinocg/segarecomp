#include "segarecomp/cpu/z80/analysis/adapter.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace segarecomp::cpu::z80::analysis {
namespace {

namespace core = segarecomp::analysis;
using core::FiniteValue;
using core::UnknownReason;

constexpr std::uint64_t kAddressMask = 0xFFFFU;

// Location index = the Reg enumerator (A B C D E H L BC DE HL SP IX IY).
std::size_t index(Reg reg) { return static_cast<std::size_t>(reg); }
bool is_half(Reg reg) { return reg >= Reg::b && reg <= Reg::l; }
bool is_high(Reg reg) { return reg == Reg::b || reg == Reg::d || reg == Reg::h; }
Reg pair_of(Reg half) { return half <= Reg::c ? Reg::bc : half <= Reg::e ? Reg::de : Reg::hl; }
Reg other_half(Reg half) {
  const auto i = static_cast<std::uint8_t>(half);
  return static_cast<Reg>(is_high(half) ? i + 1U : i - 1U);
}
bool has_halves(Reg reg) { return reg == Reg::bc || reg == Reg::de || reg == Reg::hl; }
Reg high_of(Reg pair) { return pair == Reg::bc ? Reg::b : pair == Reg::de ? Reg::d : Reg::h; }
std::uint64_t mask_of(Reg reg) { return is_wide(reg) ? 0xFFFFU : 0xFFU; }
std::uint64_t extract(std::uint64_t pair, bool high) { return high ? (pair >> 8U) & 0xFFU : pair & 0xFFU; }
std::uint64_t insert(std::uint64_t pair, bool high, std::uint64_t byte) {
  return high ? ((byte & 0xFFU) << 8U) | (pair & 0xFFU) : (pair & 0xFF00U) | (byte & 0xFFU);
}

// Combine two values exactly over the cross product; Unknown absorbs (the smaller reason wins, as in join).
template <typename F>
FiniteValue product(const FiniteValue& left, const FiniteValue& right, F&& function, std::size_t bound) {
  if (left.is_bottom() || right.is_bottom()) return FiniteValue::bottom();
  if (left.is_unknown() && right.is_unknown()) return FiniteValue::unknown(std::min(left.reason(), right.reason()));
  if (left.is_unknown()) return left;
  if (right.is_unknown()) return right;
  if (left.values().size() * right.values().size() > std::min(bound, core::default_set_bound))
    return FiniteValue::unknown(UnknownReason::set_bound);
  std::vector<std::uint64_t> out;
  out.reserve(left.values().size() * right.values().size());
  for (const auto l : left.values())
    for (const auto r : right.values()) out.push_back(function(l, r));
  return FiniteValue::of(std::move(out), bound);
}

// Write `value` to `target`, keeping a pair and its halves consistent: writing a pair sets both halves to its exact projections;
// writing a half combines the new byte set with the pair's previous values when those are precise (keeping the per-value other
// half), otherwise with the other half's location (the non-relational product; Unknown(set_bound) past the bound).
void assign(State& out, Reg target, const FiniteValue& value, std::size_t bound) {
  out.values[index(target)] = value;
  if (has_halves(target)) {
    const Reg high = high_of(target);
    out.values[index(high)] = value.map([](std::uint64_t p) { return extract(p, true); }, bound);
    out.values[index(other_half(high))] = value.map([](std::uint64_t p) { return extract(p, false); }, bound);
    return;
  }
  if (!is_half(target)) return;
  const bool high = is_high(target);
  FiniteValue& pair = out.values[index(pair_of(target))];
  if (pair.is_precise()) {
    pair = product(pair, value, [&](std::uint64_t p, std::uint64_t byte) { return insert(p, high, byte); }, bound);
    return;
  }
  pair = product(out.values[index(other_half(target))], value,
                 [&](std::uint64_t other, std::uint64_t byte) { return insert(other << (high ? 0U : 8U), high, byte); },
                 bound);
}

class Evaluator {
 public:
  Evaluator(const ImageView& image, std::size_t bound, const State& in) : image_(image), bound_(bound), in_(in) {}

  FiniteValue address(const AddressExpr& expr) const {
    if (expr.kind == AddressExpr::Kind::absolute) return FiniteValue::constant(expr.absolute);
    const auto offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(expr.offset));
    return Adapter::read(in_, expr.base).map([&](std::uint64_t v) { return (v + offset) & kAddressMask; }, bound_);
  }

  // Exact only when every byte of every possible address is an immutable image byte.
  FiniteValue load(const AddressExpr& expr, std::uint8_t width) const {
    const FiniteValue addresses = address(expr);
    if (addresses.is_bottom()) return addresses;
    if (!addresses.is_precise()) return FiniteValue::unknown(UnknownReason::non_immutable_read);
    std::vector<std::uint64_t> out;
    for (const auto a : addresses.values()) {
      std::uint64_t value = 0;
      for (std::uint8_t i = 0; i < width; ++i) {
        const auto at = static_cast<std::uint16_t>((a + i) & kAddressMask);
        if (!image_.contains(at)) return FiniteValue::unknown(UnknownReason::non_immutable_read);
        value |= static_cast<std::uint64_t>(image_.byte(at)) << (8U * i);
      }
      out.push_back(value);
    }
    return FiniteValue::of(std::move(out), bound_);
  }

  FiniteValue value(const ValueExpr& expr, std::uint64_t mask) const {
    switch (expr.kind) {
      case ValueExpr::Kind::constant: return FiniteValue::constant(expr.constant & mask);
      case ValueExpr::Kind::copy: return Adapter::read(in_, expr.source);
      case ValueExpr::Kind::add_constant: {
        const auto delta = static_cast<std::uint64_t>(static_cast<std::int64_t>(expr.delta));
        return Adapter::read(in_, expr.source).map([&](std::uint64_t v) { return (v + delta) & mask; }, bound_);
      }
      case ValueExpr::Kind::load: return load(expr.address, expr.width);
      case ValueExpr::Kind::opaque: break;
    }
    return FiniteValue::unknown(UnknownReason::unsupported_transfer);
  }

  // A precise store address set that touches the immutable range contradicts the caller's immutability premise.
  bool contradicts_premise(const MemoryStore& store) const {
    const FiniteValue addresses = address(store.address);
    if (!addresses.is_precise()) return false;
    for (const auto a : addresses.values())
      for (std::uint8_t i = 0; i < store.width; ++i)
        if (image_.contains(static_cast<std::uint16_t>((a + i) & kAddressMask))) return true;
    return false;
  }

  void apply(State& out, const RegisterWrite& write) const {
    const Reg target = write.target;
    const ValueExpr& v = write.value;
    const FiniteValue written = value(v, mask_of(target));
    if (!is_half(target)) {
      assign(out, target, written, bound_);
      return;
    }
    const FiniteValue& pair = out.values[index(pair_of(target))];
    const bool same_pair = (v.kind == ValueExpr::Kind::copy || v.kind == ValueExpr::Kind::add_constant) &&
                           is_half(v.source) && pair_of(v.source) == pair_of(target);
    if (pair.is_precise() && (v.kind == ValueExpr::Kind::constant || same_pair)) {
      // Exact pointwise update of the pair: each pair value keeps its own other half.
      const auto delta = static_cast<std::uint64_t>(static_cast<std::int64_t>(v.delta));
      const FiniteValue updated = pair.map(
          [&](std::uint64_t p) {
            std::uint64_t byte = v.constant;
            if (same_pair)
              byte = extract(p, is_high(v.source)) + (v.kind == ValueExpr::Kind::add_constant ? delta : 0U);
            return insert(p, is_high(target), byte);
          },
          bound_);
      out.values[index(target)] = written;
      out.values[index(pair_of(target))] = updated;
      return;
    }
    assign(out, target, written, bound_);
  }

 private:
  const ImageView& image_;
  std::size_t bound_;
  const State& in_;
};

core::TransferResult<State> unresolved(UnknownReason reason) {
  core::TransferResult<State> out;
  out.unresolved_computed = reason;
  return out;
}

void hex(std::string& out, std::uint64_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof buffer, "0x%04llx", static_cast<unsigned long long>(value));
  out += buffer;
}

}  // namespace

ImageView::ImageView(std::uint16_t base, std::span<const std::uint8_t> bytes, std::uint32_t image_id)
    : base_(base), bytes_(bytes), image_id_(image_id) {
  if (static_cast<std::size_t>(base) + bytes.size() > 0x10000U)
    throw std::invalid_argument("ImageView: image extends past the 16-bit logical address space");
}

bool ImageView::contains(std::uint16_t address) const noexcept {
  return address >= base_ && static_cast<std::size_t>(address - base_) < bytes_.size();
}

std::uint8_t ImageView::byte(std::uint16_t address) const noexcept {
  return contains(address) ? bytes_[static_cast<std::size_t>(address - base_)] : std::uint8_t{0};
}

FetchedByte ImageView::fetch(std::uint16_t address) const {
  FetchedByte out;
  if (!contains(address)) return out;  // non_code
  out.kind = FetchKind::byte;
  out.value = byte(address);
  out.image_id = image_id_;
  out.image_offset = static_cast<std::uint32_t>(address - base_);
  return out;
}

State Adapter::entry_state() { return State::all_unknown(UnknownReason::unknown_input); }

FiniteValue Adapter::read(const State& state, Reg reg) { return state.values[index(reg)]; }

State Adapter::with(const State& state, Reg reg, const FiniteValue& value) const {
  State out = state;
  out.reachable = true;
  assign(out, reg, value, set_bound_);
  return out;
}

core::TransferResult<State> Adapter::transfer(std::uint64_t point, const State& in) const {
  if (!in.reachable) return {};
  if (point > kAddressMask) return unresolved(UnknownReason::unsupported_transfer);
  const StartClassification start = decode_at(image_, static_cast<std::uint16_t>(point));
  if (start.kind != StartKind::decoded)
    return unresolved(start.kind == StartKind::mutable_code ? UnknownReason::non_immutable_read
                                                            : UnknownReason::unsupported_transfer);
  const Z80Effect effect = project_effect(start.instruction);
  if (!effect.supported) return unresolved(UnknownReason::unsupported_transfer);

  const Evaluator eval(image_, set_bound_, in);
  if (effect.store && eval.contradicts_premise(*effect.store)) return unresolved(UnknownReason::unsupported_transfer);
  State out = in;
  for (const auto& write : effect.writes) eval.apply(out, write);

  core::TransferResult<State> result;
  using core::EdgeKind;
  switch (effect.control) {
    case ControlKind::fallthrough: result.edges.push_back({effect.fallthrough, EdgeKind::fallthrough, out}); break;
    case ControlKind::jump: result.edges.push_back({effect.target, EdgeKind::branch, out}); break;
    case ControlKind::conditional_jump:
      result.edges.push_back({effect.target, EdgeKind::branch, out});
      result.edges.push_back({effect.fallthrough, EdgeKind::fallthrough, out});
      break;
    case ControlKind::call:
      result.edges.push_back({effect.target, EdgeKind::call, out});
      result.edges.push_back(
          {effect.fallthrough, EdgeKind::return_edge, State::all_unknown(UnknownReason::unsupported_transfer)});
      break;
    case ControlKind::return_: break;
    case ControlKind::conditional_return: result.edges.push_back({effect.fallthrough, EdgeKind::fallthrough, out}); break;
    case ControlKind::computed: {
      const FiniteValue targets = read(in, effect.computed_base);
      if (targets.is_unknown()) {
        result.unresolved_computed = targets.reason();
      } else {
        for (const auto t : targets.values()) result.edges.push_back({t, EdgeKind::computed, out});
      }
      break;
    }
  }
  return result;
}

std::string describe(const core::Solution<State>& solution) {
  std::string out;
  if (!solution.complete) {
    out += "incomplete reason=";
    out += core::unknown_reason_name(solution.reason);
    out += '\n';
    return out;
  }
  out += "complete iterations=" + std::to_string(solution.iterations) + '\n';
  for (const auto& [point, state] : solution.in_states) {
    out += "point ";
    hex(out, point);
    out += ':';
    for (std::size_t i = 0; i < kLocationCount; ++i) {
      out += ' ';
      out += reg_name(static_cast<Reg>(i));
      out += '=';
      out += state.reachable ? state.values[i].describe() : std::string("bottom");
    }
    out += '\n';
  }
  for (const auto& [point, targets] : solution.computed_targets) {
    out += "computed ";
    hex(out, point);
    out += ':';
    for (const auto t : targets) {
      out += ' ';
      hex(out, t);
    }
    out += '\n';
  }
  for (const auto& [point, reason] : solution.unresolved_computed) {
    out += "unresolved ";
    hex(out, point);
    out += ": ";
    out += core::unknown_reason_name(reason);
    out += '\n';
  }
  return out;
}

}  // namespace segarecomp::cpu::z80::analysis
