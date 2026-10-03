#pragma once

// SEG-030-T003 (ADR 0079 decisions 4, 5, 10, 11; report-only): the CPU-owned M68K address domain.
//
// An address register holds a bounded points-to value: bottom, at most `m68k_points_to_bound` pairs (region, offset set), or
// Unknown with one generic reason plus one CPU sub-reason. A region is one contiguous extent of 32-bit register values whose
// 24-bit bus addresses fall inside one machine region (an immutable image, work RAM or the I/O/device window); the machine view
// supplies the extents (`M68kAnalysisImage::region_of`). The stack is not a region of its own: it is work RAM.
//
// An offset set is either an exact set of at most `m68k_exact_offset_bound` offsets or a stride/congruence {lo, stride, hi}.
// Offsets always lie in [0, size] of their region (one past the end is admitted so a pointer may step to a table end); any
// arithmetic that leaves that range is Unknown(region_exit), never a clamp. A join that keeps growing a strided set widens it
// after `m68k_strided_growth_bound` growths, and only up to the region extent (the congruence is kept): it never widens to
// certainty. A strided set is never enumerated into control targets.
//
// The generic core is unchanged: sub-reasons are CPU-owned and every Unknown also carries one of the six generic reasons.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "segarecomp/analysis/finite_value.hpp"

namespace segarecomp {

// ADR 0079 decision 10: CPU-owned sub-reasons (closed vocabulary; the numeric order is the deterministic join priority).
enum class M68kAnalysisSubReason : std::uint8_t {
  none,
  base_unknown,
  region_exit,
  set_bound,
  target_outside_image,
  width_only,
  store_poison,
  async_writer,
  initial_memory,
  external_writer,
  context_bound,
  stack_unbalanced,
  frame_unproven,
  interrupt_resumption,
  invalidated,
};
inline constexpr std::size_t m68k_analysis_sub_reason_count = 15U;
[[nodiscard]] const char *m68k_analysis_sub_reason_name(M68kAnalysisSubReason reason) noexcept;

// ADR 0079 decision 4: the CPU-owned region vocabulary (Unknown is the points-to top, not a region).
enum class M68kRegionKind : std::uint8_t { image, work_ram, io_device };
[[nodiscard]] const char *m68k_region_kind_name(M68kRegionKind kind) noexcept;

// One region extent on the 24-bit bus, as reported by the machine view.
struct M68kRegionExtent {
  M68kRegionKind kind{M68kRegionKind::image};
  std::uint32_t id{};    // image identity for `image`; 0 otherwise
  std::uint32_t base{};  // first bus address
  std::uint32_t size{};  // bytes (> 0; base + size <= 2^24)
  // SEG-030-T004: physical mirror period in bytes (0: none). Region offsets that are congruent modulo `mirror` name the same
  // physical byte (Genesis work RAM: 64 KiB mirrored over $E00000-$FFFFFF).
  std::uint32_t mirror{};
};

// A region as a range of 32-bit register values: `base` = (upper register byte << 24) | bus base. Two register values with
// different upper bytes are different regions (they are different pointers even though the bus ignores the upper byte).
struct M68kRegion {
  M68kRegionKind kind{M68kRegionKind::image};
  std::uint32_t id{};
  std::uint32_t base{};
  std::uint32_t size{};
  std::uint32_t mirror{};  // SEG-030-T004: see M68kRegionExtent::mirror
  friend auto operator<=>(const M68kRegion &, const M68kRegion &) = default;
  // The largest admitted offset: one past the end, unless that register value would wrap past 2^32.
  [[nodiscard]] constexpr std::uint32_t limit() const noexcept {
    const std::uint64_t room = UINT64_C(0xFFFFFFFF) - base;
    return room < size ? static_cast<std::uint32_t>(room) : size;
  }
};

// Resource constants (ADR 0079 decision 11 plus the T003 strided-growth bound).
inline constexpr std::size_t m68k_points_to_bound = 8U;
inline constexpr std::size_t m68k_exact_offset_bound = 64U;
inline constexpr std::uint32_t m68k_strided_growth_bound = 64U;
// Concrete enumerations (address computations, table reads) never exceed the generic finite-set bound.
inline constexpr std::size_t m68k_address_enumeration_bound = analysis::default_set_bound;

class M68kOffsetSet {
public:
  M68kOffsetSet() = default;  // empty (only meaningful inside a known pair)

  // Normalizes (sort, dedupe); more than the exact bound becomes the tight strided hull.
  [[nodiscard]] static M68kOffsetSet of(std::vector<std::uint32_t> offsets);
  // Normalized strided form (count <= exact bound becomes exact). Requires stride > 0, lo <= hi, (hi - lo) % stride == 0.
  [[nodiscard]] static M68kOffsetSet strided(std::uint32_t lo, std::uint32_t stride, std::uint32_t hi, std::uint32_t growth = 0U);

  [[nodiscard]] bool is_strided() const noexcept { return strided_; }
  [[nodiscard]] bool empty() const noexcept { return !strided_ && exact_.empty(); }
  [[nodiscard]] const std::vector<std::uint32_t> &exact() const noexcept { return exact_; }  // when !is_strided()
  [[nodiscard]] std::uint32_t lo() const noexcept;
  [[nodiscard]] std::uint32_t hi() const noexcept;
  [[nodiscard]] std::uint32_t stride() const noexcept;  // 0 for an exact singleton
  [[nodiscard]] std::uint32_t growth() const noexcept { return growth_; }
  [[nodiscard]] std::uint64_t count() const noexcept;
  [[nodiscard]] bool contains(std::uint32_t offset) const noexcept;

  // Least upper bound inside a region of `size` bytes (growth-bounded widening up to the extent, congruence kept).
  friend M68kOffsetSet join(const M68kOffsetSet &left, const M68kOffsetSet &right, std::uint32_t size);
  // Set inclusion (the growth annotation is not part of the meaning).
  friend bool leq(const M68kOffsetSet &left, const M68kOffsetSet &right);
  friend bool operator==(const M68kOffsetSet &, const M68kOffsetSet &) = default;

  // Every offset plus every delta of `deltas` (non-empty), or nullopt when any result leaves [0, limit].
  [[nodiscard]] std::optional<M68kOffsetSet> shifted(const std::vector<std::int64_t> &deltas, std::uint32_t limit) const;
  // The members inside [lo_bound, hi_bound] (possibly empty).
  [[nodiscard]] M68kOffsetSet restricted(std::uint32_t lo_bound, std::uint32_t hi_bound) const;

  [[nodiscard]] std::string describe() const;

private:
  bool strided_{};
  std::vector<std::uint32_t> exact_;  // sorted distinct
  std::uint32_t lo_{}, stride_{}, hi_{};
  std::uint32_t growth_{};
};

struct M68kPointsTo {
  enum class Kind : std::uint8_t { bottom, known, unknown };
  Kind kind{Kind::bottom};
  std::vector<std::pair<M68kRegion, M68kOffsetSet>> pairs;  // sorted by region, distinct regions, non-empty sets
  analysis::UnknownReason reason{analysis::UnknownReason::unknown_input};  // when unknown
  M68kAnalysisSubReason sub{M68kAnalysisSubReason::none};                   // when unknown
  // The set's extent is derived from a width-only (operand-width) index domain: sound, but not an explicit bound.
  bool width_derived{};

  [[nodiscard]] static M68kPointsTo bottom() { return {}; }
  [[nodiscard]] static M68kPointsTo unknown(analysis::UnknownReason reason, M68kAnalysisSubReason sub = M68kAnalysisSubReason::none);
  // Normalizes: drops empty sets, merges equal regions, more than the pair bound is Unknown(set_bound).
  [[nodiscard]] static M68kPointsTo of(std::vector<std::pair<M68kRegion, M68kOffsetSet>> pairs, bool width_derived = false);

  [[nodiscard]] bool is_bottom() const noexcept { return kind == Kind::bottom; }
  [[nodiscard]] bool is_known() const noexcept { return kind == Kind::known; }
  [[nodiscard]] bool is_unknown() const noexcept { return kind == Kind::unknown; }
  // True when every pair's offsets are exact.
  [[nodiscard]] bool is_exact() const noexcept;
  // The exact 32-bit register values (sorted distinct), when known and exact.
  [[nodiscard]] std::optional<std::vector<std::uint32_t>> values() const;

  friend M68kPointsTo join(const M68kPointsTo &left, const M68kPointsTo &right);
  friend bool leq(const M68kPointsTo &left, const M68kPointsTo &right);
  friend bool operator==(const M68kPointsTo &, const M68kPointsTo &) = default;

  [[nodiscard]] std::string describe() const;
};

// Every value plus every delta (32-bit register arithmetic). Unknown passes through; leaving a region is Unknown(region_exit).
[[nodiscard]] M68kPointsTo m68k_points_to_add(const M68kPointsTo &value, const std::vector<std::int64_t> &deltas);

}  // namespace segarecomp
