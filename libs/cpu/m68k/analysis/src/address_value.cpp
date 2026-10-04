// SEG-030-T003 (ADR 0079, report-only). See address_value.hpp.

#include "segarecomp/cpu/m68k/analysis/address_value.hpp"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <tuple>

namespace segarecomp {
namespace {

using analysis::UnknownReason;

std::string hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

// gcd of (member - lo) over the members of a set (0 for a singleton).
std::uint32_t member_gcd(const M68kOffsetSet &set) {
  if (set.is_strided()) return set.stride();
  std::uint32_t g = 0U;
  for (const auto value : set.exact()) g = std::gcd(g, value - set.lo());
  return g;
}

// Unknown(a) <= Unknown(b) iff b's (reason, sub) is not greater (the smaller pair is the higher, absorbing element).
bool unknown_leq(const M68kPointsTo &left, const M68kPointsTo &right) {
  return std::tie(right.reason, right.sub) <= std::tie(left.reason, left.sub);
}

}  // namespace

const char *m68k_analysis_sub_reason_name(M68kAnalysisSubReason reason) noexcept {
  switch (reason) {
  case M68kAnalysisSubReason::none: return "none";
  case M68kAnalysisSubReason::base_unknown: return "base_unknown";
  case M68kAnalysisSubReason::region_exit: return "region_exit";
  case M68kAnalysisSubReason::set_bound: return "set_bound";
  case M68kAnalysisSubReason::target_outside_image: return "target_outside_image";
  case M68kAnalysisSubReason::width_only: return "width_only";
  case M68kAnalysisSubReason::store_poison: return "store_poison";
  case M68kAnalysisSubReason::async_writer: return "async_writer";
  case M68kAnalysisSubReason::initial_memory: return "initial_memory";
  case M68kAnalysisSubReason::external_writer: return "external_writer";
  case M68kAnalysisSubReason::context_bound: return "context_bound";
  case M68kAnalysisSubReason::stack_unbalanced: return "stack_unbalanced";
  case M68kAnalysisSubReason::frame_unproven: return "frame_unproven";
  case M68kAnalysisSubReason::interrupt_resumption: return "interrupt_resumption";
  case M68kAnalysisSubReason::invalidated: return "invalidated";
  case M68kAnalysisSubReason::return_slot_rewritten: return "return_slot_rewritten";
  case M68kAnalysisSubReason::interrupt_resumption_unproven: return "interrupt_resumption_unproven";
  }
  return "invalid";
}

const char *m68k_region_kind_name(M68kRegionKind kind) noexcept {
  switch (kind) {
  case M68kRegionKind::image: return "image";
  case M68kRegionKind::mutable_ram: return "mutable_ram";
  case M68kRegionKind::io_device: return "io_device";
  }
  return "invalid";
}

// ---------------------------------------------------------------------------------------------------------------
// Offset sets.

M68kOffsetSet M68kOffsetSet::of(std::vector<std::uint32_t> offsets) {
  std::sort(offsets.begin(), offsets.end());
  offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
  if (offsets.size() <= m68k_exact_offset_bound) {
    M68kOffsetSet out;
    out.exact_ = std::move(offsets);
    return out;
  }
  std::uint32_t g = 0U;
  for (const auto value : offsets) g = std::gcd(g, value - offsets.front());
  return strided(offsets.front(), g, offsets.back());
}

M68kOffsetSet M68kOffsetSet::strided(std::uint32_t lo, std::uint32_t stride, std::uint32_t hi, std::uint32_t growth) {
  M68kOffsetSet out;
  if (stride == 0U || lo == hi) {
    out.exact_ = {lo};
    return out;
  }
  const std::uint64_t count = (static_cast<std::uint64_t>(hi) - lo) / stride + 1U;
  if (count <= m68k_exact_offset_bound) {
    for (std::uint64_t i = 0; i < count; ++i) out.exact_.push_back(static_cast<std::uint32_t>(lo + i * stride));
    return out;
  }
  out.strided_ = true;
  out.lo_ = lo;
  out.stride_ = stride;
  out.hi_ = hi;
  out.growth_ = growth;
  return out;
}

std::uint32_t M68kOffsetSet::lo() const noexcept { return strided_ ? lo_ : (exact_.empty() ? 0U : exact_.front()); }
std::uint32_t M68kOffsetSet::hi() const noexcept { return strided_ ? hi_ : (exact_.empty() ? 0U : exact_.back()); }
std::uint32_t M68kOffsetSet::stride() const noexcept { return strided_ ? stride_ : member_gcd(*this); }
std::uint64_t M68kOffsetSet::count() const noexcept {
  return strided_ ? (static_cast<std::uint64_t>(hi_) - lo_) / stride_ + 1U : exact_.size();
}

bool M68kOffsetSet::contains(std::uint32_t offset) const noexcept {
  if (!strided_) return std::binary_search(exact_.begin(), exact_.end(), offset);
  return offset >= lo_ && offset <= hi_ && (offset - lo_) % stride_ == 0U;
}

M68kOffsetSet join(const M68kOffsetSet &left, const M68kOffsetSet &right, std::uint32_t limit) {
  if (left.empty()) return right;
  if (right.empty()) return left;
  if (!left.strided_ && !right.strided_) {
    std::vector<std::uint32_t> merged;
    std::set_union(left.exact_.begin(), left.exact_.end(), right.exact_.begin(), right.exact_.end(), std::back_inserter(merged));
    if (merged.size() <= m68k_exact_offset_bound) {
      M68kOffsetSet out;
      out.exact_ = std::move(merged);
      return out;
    }
  }
  const auto lo = std::min(left.lo(), right.lo());
  const auto hi = std::max(left.hi(), right.hi());
  const auto g = std::gcd(std::gcd(member_gcd(left), member_gcd(right)), std::gcd(left.lo() - lo, right.lo() - lo));
  auto out = M68kOffsetSet::strided(lo, g, hi);
  // The growth annotation counts strict strided growth; past the bound the set widens to the region extent (congruence kept).
  if (leq(out, left) && left.strided_) out.growth_ = left.growth_;
  else if (leq(out, right) && right.strided_) out.growth_ = right.growth_;
  else out.growth_ = std::max(left.growth_, right.growth_) + 1U;
  if (out.strided_ && out.growth_ > m68k_strided_growth_bound) {
    // Widening to the extent is idempotent; the stride can still only shrink (finitely often).
    const auto first = out.lo_ % out.stride_;
    const auto last = limit >= first ? first + (limit - first) / out.stride_ * out.stride_ : first;
    out = M68kOffsetSet::strided(first, out.stride_, std::max(last, out.hi_), out.growth_);
  }
  return out;
}

bool leq(const M68kOffsetSet &left, const M68kOffsetSet &right) {
  if (left.empty()) return true;
  if (right.empty()) return false;
  if (!left.strided_) {
    return std::all_of(left.exact_.begin(), left.exact_.end(), [&](std::uint32_t value) { return right.contains(value); });
  }
  if (!right.strided_) return false;  // a normalized strided set has more members than any exact set
  return left.lo_ >= right.lo_ && left.hi_ <= right.hi_ && left.stride_ % right.stride_ == 0U &&
         (left.lo_ - right.lo_) % right.stride_ == 0U;
}

std::optional<M68kOffsetSet> M68kOffsetSet::shifted(const std::vector<std::int64_t> &deltas, std::uint32_t limit) const {
  if (deltas.empty() || empty()) return std::nullopt;
  const auto [dmin, dmax] = std::minmax_element(deltas.begin(), deltas.end());
  const std::int64_t new_lo = static_cast<std::int64_t>(lo()) + *dmin;
  const std::int64_t new_hi = static_cast<std::int64_t>(hi()) + *dmax;
  if (new_lo < 0 || new_hi > static_cast<std::int64_t>(limit)) return std::nullopt;  // leaves the region: never clamped
  if (!strided_ && exact_.size() * deltas.size() <= m68k_address_enumeration_bound) {
    std::vector<std::uint32_t> out;
    out.reserve(exact_.size() * deltas.size());
    for (const auto offset : exact_)
      for (const auto delta : deltas) out.push_back(static_cast<std::uint32_t>(static_cast<std::int64_t>(offset) + delta));
    auto result = of(std::move(out));
    result.growth_ = growth_;
    return result;
  }
  std::uint32_t g = member_gcd(*this);
  for (const auto delta : deltas) g = std::gcd(g, static_cast<std::uint32_t>(delta - *dmin));
  return strided(static_cast<std::uint32_t>(new_lo), g, static_cast<std::uint32_t>(new_hi), growth_);
}

M68kOffsetSet M68kOffsetSet::restricted(std::uint32_t lo_bound, std::uint32_t hi_bound) const {
  if (!strided_) {
    M68kOffsetSet out;
    for (const auto value : exact_)
      if (value >= lo_bound && value <= hi_bound) out.exact_.push_back(value);
    return out;
  }
  const auto start = std::max(lo_bound, lo_);
  const auto end = std::min(hi_bound, hi_);
  if (start > end) return {};
  const auto first = lo_ + (start - lo_ + stride_ - 1U) / stride_ * stride_;
  if (first > end || first < lo_) return {};
  const auto last = lo_ + (end - lo_) / stride_ * stride_;
  if (last < first) return {};
  return strided(first, stride_, last, growth_);
}

std::string M68kOffsetSet::describe() const {
  if (strided_) return "[" + hex(lo_) + ".." + hex(hi_) + " step " + hex(stride_) + "]";
  std::string out = "{";
  for (std::size_t i = 0; i < exact_.size(); ++i) out += (i == 0U ? "" : ",") + hex(exact_[i]);
  return out + "}";
}

// ---------------------------------------------------------------------------------------------------------------
// Points-to values.

M68kPointsTo M68kPointsTo::unknown(UnknownReason reason, M68kAnalysisSubReason sub) {
  M68kPointsTo out;
  out.kind = Kind::unknown;
  out.reason = reason;
  out.sub = sub;
  return out;
}

M68kPointsTo M68kPointsTo::of(std::vector<std::pair<M68kRegion, M68kOffsetSet>> pairs, bool width_derived) {
  std::stable_sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  M68kPointsTo out;
  for (auto &[region, offsets] : pairs) {
    if (offsets.empty()) continue;
    if (!out.pairs.empty() && out.pairs.back().first == region)
      out.pairs.back().second = join(out.pairs.back().second, offsets, region.limit());
    else out.pairs.emplace_back(region, std::move(offsets));
  }
  if (out.pairs.empty()) return bottom();
  if (out.pairs.size() > m68k_points_to_bound) return unknown(UnknownReason::set_bound, M68kAnalysisSubReason::set_bound);
  out.kind = Kind::known;
  out.width_derived = width_derived;
  return out;
}

bool M68kPointsTo::is_exact() const noexcept {
  return is_known() && std::none_of(pairs.begin(), pairs.end(), [](const auto &pair) { return pair.second.is_strided(); });
}

std::optional<std::vector<std::uint32_t>> M68kPointsTo::values() const {
  if (!is_exact()) return std::nullopt;
  std::vector<std::uint32_t> out;
  for (const auto &[region, offsets] : pairs)
    for (const auto offset : offsets.exact()) out.push_back(region.base + offset);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

M68kPointsTo join(const M68kPointsTo &left, const M68kPointsTo &right) {
  if (left.is_bottom()) return right;
  if (right.is_bottom()) return left;
  if (left.is_unknown() || right.is_unknown()) {
    if (!left.is_unknown()) return right;
    if (!right.is_unknown()) return left;
    return unknown_leq(left, right) ? right : left;  // the smaller (reason, sub) absorbs
  }
  auto pairs = left.pairs;
  pairs.insert(pairs.end(), right.pairs.begin(), right.pairs.end());
  return M68kPointsTo::of(std::move(pairs), left.width_derived || right.width_derived);
}

bool leq(const M68kPointsTo &left, const M68kPointsTo &right) {
  if (left.is_bottom()) return true;
  if (right.is_bottom()) return false;
  if (right.is_unknown()) return !left.is_unknown() || unknown_leq(left, right);
  if (left.is_unknown()) return false;
  if (left.width_derived && !right.width_derived) return false;
  for (const auto &[region, offsets] : left.pairs) {
    const auto found = std::find_if(right.pairs.begin(), right.pairs.end(), [&](const auto &pair) { return pair.first == region; });
    if (found == right.pairs.end() || !leq(offsets, found->second)) return false;
  }
  return true;
}

std::string M68kPointsTo::describe() const {
  if (is_bottom()) return "bottom";
  if (is_unknown())
    return std::string("unknown(") + analysis::unknown_reason_name(reason) + "/" + m68k_analysis_sub_reason_name(sub) + ")";
  std::string out = width_derived ? "~{" : "{";
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    const auto &[region, offsets] = pairs[i];
    out += (i == 0U ? "" : ";") + std::string(m68k_region_kind_name(region.kind)) + "#" + std::to_string(region.id) + "@" +
           hex(region.base) + "+" + offsets.describe();
  }
  return out + "}";
}

M68kPointsTo m68k_points_to_add(const M68kPointsTo &value, const std::vector<std::int64_t> &deltas) {
  if (!value.is_known()) return value;
  if (deltas.empty()) return M68kPointsTo::bottom();
  std::vector<std::pair<M68kRegion, M68kOffsetSet>> pairs;
  for (const auto &[region, offsets] : value.pairs) {
    auto moved = offsets.shifted(deltas, region.limit());
    if (!moved) return M68kPointsTo::unknown(UnknownReason::unsupported_transfer, M68kAnalysisSubReason::region_exit);
    pairs.emplace_back(region, std::move(*moved));
  }
  return M68kPointsTo::of(std::move(pairs), value.width_derived);
}

}  // namespace segarecomp
