#pragma once

// SEG-029-T002 (ADR 0078): the baseline abstract value domain of the generic analysis core.
//
// A value is bottom (no value reaches: unreachable under the analysis premise), an exact finite set of 64-bit values whose size is
// bounded, or Unknown with a typed reason. Unknown is top and is always sound. Exceeding the set bound yields Unknown(set_bound),
// never a wider guess. There is no widening, no interval and no region in the baseline (staged capabilities, ADR 0078).
//
// This header is CPU-free: it names no instruction set, location or platform concept (forbidden-identifier test).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace segarecomp::analysis {

// Closed vocabulary. The numeric order is the deterministic join priority: joining two Unknowns keeps the smaller reason.
enum class UnknownReason : std::uint8_t {
  unknown_input,          // the value enters the analysed region with no information (an entry, an untracked location)
  unsupported_transfer,   // the adapter has no exact transfer for the operation
  non_immutable_read,     // a read whose bytes are not provably immutable image bytes
  set_bound,              // an exact set would exceed its size bound
  iteration_bound,        // the solver exhausted its iteration bound
  state_bound,            // the solver exhausted its program-point bound
};
inline constexpr std::size_t unknown_reason_count = 6U;

[[nodiscard]] inline const char *unknown_reason_name(UnknownReason reason) noexcept {
  switch (reason) {
  case UnknownReason::unknown_input: return "unknown_input";
  case UnknownReason::unsupported_transfer: return "unsupported_transfer";
  case UnknownReason::non_immutable_read: return "non_immutable_read";
  case UnknownReason::set_bound: return "set_bound";
  case UnknownReason::iteration_bound: return "iteration_bound";
  case UnknownReason::state_bound: return "state_bound";
  }
  return "invalid";
}

// Resource constant: the default (and maximum accepted) size of an exact finite set.
inline constexpr std::size_t default_set_bound = 4096U;

class FiniteValue {
public:
  enum class Kind : std::uint8_t { bottom, precise, unknown };

  FiniteValue() = default;  // bottom

  [[nodiscard]] static FiniteValue bottom() { return {}; }
  [[nodiscard]] static FiniteValue unknown(UnknownReason reason) {
    FiniteValue out;
    out.kind_ = Kind::unknown;
    out.reason_ = reason;
    return out;
  }
  [[nodiscard]] static FiniteValue constant(std::uint64_t value) { return of({value}); }
  // Normalizes (sort, dedupe). An empty set is bottom; more than `bound` values is Unknown(set_bound).
  [[nodiscard]] static FiniteValue of(std::vector<std::uint64_t> values, std::size_t bound = default_set_bound) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.empty()) return bottom();
    if (values.size() > std::min(bound, default_set_bound)) return unknown(UnknownReason::set_bound);
    FiniteValue out;
    out.kind_ = Kind::precise;
    out.values_ = std::move(values);
    return out;
  }

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_bottom() const noexcept { return kind_ == Kind::bottom; }
  [[nodiscard]] bool is_precise() const noexcept { return kind_ == Kind::precise; }
  [[nodiscard]] bool is_unknown() const noexcept { return kind_ == Kind::unknown; }
  // Valid only when unknown.
  [[nodiscard]] UnknownReason reason() const noexcept { return reason_; }
  // Sorted, distinct; empty unless precise.
  [[nodiscard]] const std::vector<std::uint64_t> &values() const noexcept { return values_; }

  // Least upper bound. Unknown absorbs (the smaller reason wins); a union over `bound` is Unknown(set_bound).
  [[nodiscard]] friend FiniteValue join(const FiniteValue &left, const FiniteValue &right,
                                        std::size_t bound = default_set_bound) {
    if (left.is_bottom()) return right;
    if (right.is_bottom()) return left;
    if (left.is_unknown() || right.is_unknown()) {
      if (!left.is_unknown()) return right;
      if (!right.is_unknown()) return left;
      return unknown(std::min(left.reason_, right.reason_));
    }
    std::vector<std::uint64_t> merged;
    merged.reserve(left.values_.size() + right.values_.size());
    std::set_union(left.values_.begin(), left.values_.end(), right.values_.begin(), right.values_.end(),
                   std::back_inserter(merged));
    return of(std::move(merged), bound);
  }

  // Partial order consistent with `join`: bottom <= set <= Unknown; sets by inclusion; Unknown(a) <= Unknown(b) iff b <= a.
  [[nodiscard]] friend bool leq(const FiniteValue &left, const FiniteValue &right) {
    if (left.is_bottom()) return true;
    if (right.is_bottom()) return false;
    if (right.is_unknown()) return !left.is_unknown() || right.reason_ <= left.reason_;
    if (left.is_unknown()) return false;
    return std::includes(right.values_.begin(), right.values_.end(), left.values_.begin(), left.values_.end());
  }

  friend bool operator==(const FiniteValue &, const FiniteValue &) = default;

  // Deterministic text: "bottom", "unknown(<reason>)" or "{0x..,0x..}".
  [[nodiscard]] std::string describe() const {
    if (is_bottom()) return "bottom";
    if (is_unknown()) return std::string("unknown(") + unknown_reason_name(reason_) + ")";
    static constexpr char digits[] = "0123456789abcdef";
    std::string out = "{";
    for (std::size_t i = 0; i < values_.size(); ++i) {
      if (i != 0U) out += ',';
      out += "0x";
      bool started = false;
      for (int shift = 60; shift >= 0; shift -= 4) {
        const auto digit = static_cast<unsigned>((values_[i] >> static_cast<unsigned>(shift)) & 0xFU);
        if (digit != 0U || started || shift == 0) {
          out += digits[digit];
          started = true;
        }
      }
    }
    return out + "}";
  }

  // Exact pointwise image of a precise set; bottom and Unknown pass through unchanged.
  template <typename F>
  [[nodiscard]] FiniteValue map(F &&function, std::size_t bound = default_set_bound) const {
    if (!is_precise()) return *this;
    std::vector<std::uint64_t> out;
    out.reserve(values_.size());
    for (const auto value : values_) out.push_back(function(value));
    return of(std::move(out), bound);
  }

private:
  Kind kind_{Kind::bottom};
  UnknownReason reason_{UnknownReason::unknown_input};
  std::vector<std::uint64_t> values_;
};

// A fixed number of abstract locations (the adapter decides what each index means), joined pointwise. A vector whose
// `reachable` flag is false is the bottom state.
template <std::size_t N>
struct ValueVector {
  bool reachable{};
  std::array<FiniteValue, N> values{};

  [[nodiscard]] static ValueVector all_unknown(UnknownReason reason) {
    ValueVector out;
    out.reachable = true;
    out.values.fill(FiniteValue::unknown(reason));
    return out;
  }
  [[nodiscard]] friend ValueVector join(const ValueVector &left, const ValueVector &right) {
    if (!left.reachable) return right;
    if (!right.reachable) return left;
    ValueVector out;
    out.reachable = true;
    for (std::size_t i = 0; i < N; ++i) out.values[i] = join(left.values[i], right.values[i]);
    return out;
  }
  [[nodiscard]] friend bool leq(const ValueVector &left, const ValueVector &right) {
    if (!left.reachable) return true;
    if (!right.reachable) return false;
    for (std::size_t i = 0; i < N; ++i)
      if (!leq(left.values[i], right.values[i])) return false;
    return true;
  }
  friend bool operator==(const ValueVector &, const ValueVector &) = default;
};

}  // namespace segarecomp::analysis
