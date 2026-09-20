#pragma once

// CPU-neutral compilation-plan contracts (no CPU ISA types).  Scenario profile, ingress,
// device classification, and frontier taxonomy deliberately live in the
// machine compatibility layer, not here.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace segarecomp {

// Generic ownership container.  It deliberately does not select a CPU,
// machine, frontier taxonomy, or runtime state representation.
template <typename AcceptedProgram, typename Frontier>
struct RecompilerPartialProgram {
  AcceptedProgram accepted_prefix;
  std::vector<Frontier> frontiers;
};

namespace detail {
// Shared sort/dedup body for both `recompiler_sort_dedup_and_bound_frontiers`
// and `recompiler_sort_dedup_and_project_frontiers` below. `frontier_sort_key`
// is caller-supplied so this stays generic over `Frontier`'s shape; it
// touches no CPU/machine-specific field of `Frontier` itself.
template <typename Frontier, typename KeyFn>
[[nodiscard]] std::vector<Frontier> recompiler_sort_dedup_frontiers(std::vector<Frontier> frontiers,
                                                                     KeyFn frontier_sort_key) {
  std::sort(frontiers.begin(), frontiers.end(), [&](const Frontier &left, const Frontier &right) {
    return frontier_sort_key(left) < frontier_sort_key(right);
  });
  frontiers.erase(std::unique(frontiers.begin(), frontiers.end(),
                               [&](const Frontier &left, const Frontier &right) {
                                 return frontier_sort_key(left) == frontier_sort_key(right);
                               }),
                   frontiers.end());
  return frontiers;
}
} // namespace detail

// Orders, deduplicates, and budget-bounds an already-classified,
// already-eligible frontier list. This function never classifies a
// frontier, never decides runtime eligibility, and never decides
// promotion -- callers must have already produced `frontiers` entries that
// are each individually eligible for promotion. `frontier_sort_key` is
// caller-supplied so this function stays generic over `Frontier`'s shape;
// it touches no CPU/machine-specific field of `Frontier` itself. Returns
// `std::nullopt` when the deduplicated count exceeds `max_frontier_exits`,
// leaving the "too many exits" policy decision (e.g. reject vs. some other
// fallback) to the caller.
template <typename Frontier, typename KeyFn>
[[nodiscard]] std::optional<std::vector<Frontier>> recompiler_sort_dedup_and_bound_frontiers(
    std::vector<Frontier> frontiers, std::uint32_t max_frontier_exits, KeyFn frontier_sort_key) {
  auto deduped = detail::recompiler_sort_dedup_frontiers(std::move(frontiers), frontier_sort_key);
  if (deduped.size() > max_frontier_exits) return std::nullopt;
  return deduped;
}

// SEG-007-T183: the complete, untruncated deduplicated semantic frontier
// obligation set (`complete`) alongside a deterministic bounded diagnostic
// projection of it (`bounded`, at most `max_frontier_exits` entries in the
// same canonical sort order) and the count of obligations left out of that
// bounded projection (`residual_count`). Unlike
// `recompiler_sort_dedup_and_bound_frontiers`, exceeding `max_frontier_exits`
// never truncates or rejects `complete` -- it only bounds the
// diagnostic-report-sized `bounded` subset. `complete` is what a caller must
// still represent/dispatch; `bounded`/`residual_count` are reporting-only.
template <typename Frontier>
struct RecompilerFrontierProjection {
  std::vector<Frontier> complete;
  std::vector<Frontier> bounded;
  std::uint32_t residual_count{};
};

template <typename Frontier, typename KeyFn>
[[nodiscard]] RecompilerFrontierProjection<Frontier> recompiler_sort_dedup_and_project_frontiers(
    std::vector<Frontier> frontiers, std::uint32_t max_frontier_exits, KeyFn frontier_sort_key) {
  auto deduped = detail::recompiler_sort_dedup_frontiers(std::move(frontiers), frontier_sort_key);
  RecompilerFrontierProjection<Frontier> projection;
  const auto bounded_count = std::min<std::size_t>(deduped.size(), max_frontier_exits);
  projection.bounded.assign(deduped.begin(), deduped.begin() + static_cast<std::ptrdiff_t>(bounded_count));
  projection.residual_count = static_cast<std::uint32_t>(deduped.size() - bounded_count);
  projection.complete = std::move(deduped);
  return projection;
}

} // namespace segarecomp
