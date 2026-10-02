#pragma once

// SEG-029-T002 (ADR 0078): the generic deterministic bounded fixed-point solver and the CPU-adapter seam.
//
// The solver never decodes or interprets an instruction. It sees opaque, totally ordered program points (a 64-bit key the adapter
// encodes, for example an image identity and an execution address), adapter-defined abstract states it can only join and compare,
// and the typed successor edges the adapter's transfer function returns for one point and one input state. A computed edge carries
// a target the adapter derived from a precise abstract value; a computed site whose targets are not precise is reported as
// unresolved with a typed reason and contributes no edge (the result is relative to the discovered edge set).
//
// Determinism: the worklist is the ordered set of pending points and always yields the smallest key; states are kept in ordered
// maps; there is no hash-order iteration and no clock. Bounds: every pop counts one iteration and every newly reached point counts
// one state; exhausting either stops the solver with Unknown(iteration_bound / state_bound) for every query, never partial truth.
//
// Adapter requirements (checked by the `Adapter` concept):
//   using State = ...;                                   // join(State, State) -> State, leq(State, State) -> bool (ADL)
//   TransferResult<State> transfer(std::uint64_t point, const State &in);
// `transfer` must be a pure function of (point, in), monotone in every non-computed edge. Computed edges may legitimately disappear
// as the input grows (a site that resolved becomes unresolved); the solver handles that by pinning the site and restarting
// (see `solve`), so no target keeps a state derived from a stale, narrower input.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "segarecomp/analysis/finite_value.hpp"

namespace segarecomp::analysis {

// Closed vocabulary, used only for scheduling-neutral reporting.
enum class EdgeKind : std::uint8_t { fallthrough, branch, call, return_edge, computed, exceptional };
inline constexpr std::size_t edge_kind_count = 6U;

template <typename State>
struct Edge {
  std::uint64_t target{};
  EdgeKind kind{EdgeKind::fallthrough};
  State state{};
};

template <typename State>
struct TransferResult {
  std::vector<Edge<State>> edges;
  // Set when the point is a computed-control site whose targets are not precise: no computed edge is produced.
  std::optional<UnknownReason> unresolved_computed;
};

// Resource constants (ADR 0078). A caller may lower them; the solver never raises them.
inline constexpr std::size_t default_max_iterations = 1'000'000U;
inline constexpr std::size_t default_max_points = 1U << 20U;

struct Bounds {
  std::size_t max_iterations{default_max_iterations};
  std::size_t max_points{default_max_points};
};

template <typename A>
concept Adapter = requires(A adapter, const typename A::State &state, std::uint64_t point) {
  typename A::State;
  { join(state, state) } -> std::convertible_to<typename A::State>;
  { leq(state, state) } -> std::convertible_to<bool>;
  { adapter.transfer(point, state) } -> std::convertible_to<TransferResult<typename A::State>>;
};

template <typename State>
struct Solution {
  bool complete{};                      // false: a bound was exhausted; every query is Unknown(reason)
  UnknownReason reason{UnknownReason::iteration_bound};
  std::size_t iterations{};
  std::map<std::uint64_t, State> in_states;                     // fixed-point input state per reached point
  std::map<std::uint64_t, UnknownReason> unresolved_computed;   // computed sites without precise targets
  std::map<std::uint64_t, std::set<std::uint64_t>> computed_targets;  // computed sites with precise targets
  // Sites pinned unresolved because a later, wider input no longer emitted a computed target an earlier transfer had emitted;
  // the solve restarted with their computed edges suppressed (see `solve`). Each is also in `unresolved_computed`.
  std::map<std::uint64_t, UnknownReason> pinned;
  std::size_t restarts{};

  [[nodiscard]] bool reached(std::uint64_t point) const { return complete && in_states.contains(point); }

  // A typed query: Unknown(reason) if the solver did not complete; bottom if the point was not reached under the premise;
  // otherwise `project(in_state)`.
  template <typename F>
  [[nodiscard]] FiniteValue query(std::uint64_t point, F &&project) const {
    if (!complete) return FiniteValue::unknown(reason);
    const auto found = in_states.find(point);
    if (found == in_states.end()) return FiniteValue::bottom();
    return project(found->second);
  }
};

template <Adapter A>
[[nodiscard]] Solution<typename A::State> solve(A &adapter, const std::vector<std::pair<std::uint64_t, typename A::State>> &entries,
                                                const Bounds &requested = {}) {
  using State = typename A::State;
  // A caller may lower a bound, never raise it (ADR 0078 decision 4).
  const Bounds bounds{std::min(requested.max_iterations, default_max_iterations),
                      std::min(requested.max_points, default_max_points)};
  Solution<State> out;
  // Computed edges are not monotone: a site resolved from a narrow input may become unresolved (or drop a target) once its input
  // grows, while the target it reached earlier keeps a state derived only from that earlier input. Such a stale state would be
  // partial truth. After a fixed point, every site whose final transfer does not still emit every computed target it emitted during
  // the run is pinned unresolved and the solve restarts from the entries with the pinned sites' computed edges suppressed. Each
  // restart pins at least one new site, and all runs share the iteration bound, so this terminates.
  std::map<std::uint64_t, UnknownReason> pinned;
  std::size_t restarts = 0U;
  for (;;) {
    const auto iterations = out.iterations;
    out = Solution<State>{};
    out.iterations = iterations;
    out.pinned = pinned;
    out.restarts = restarts;
    std::set<std::uint64_t> worklist;
    std::map<std::uint64_t, std::set<std::uint64_t>> emitted;
    const auto propagate = [&](std::uint64_t target, const State &state) -> bool {
      const auto found = out.in_states.find(target);
      if (found == out.in_states.end()) {
        if (out.in_states.size() >= bounds.max_points) return false;
        out.in_states.emplace(target, state);
        worklist.insert(target);
        return true;
      }
      if (leq(state, found->second)) return true;
      found->second = join(found->second, state);
      worklist.insert(target);
      return true;
    };
    for (const auto &[point, state] : entries) {
      if (!propagate(point, state)) {
        out.reason = UnknownReason::state_bound;
        return out;
      }
    }
    while (!worklist.empty()) {
      if (out.iterations >= bounds.max_iterations) {
        out.reason = UnknownReason::iteration_bound;
        return out;
      }
      ++out.iterations;
      const auto point = *worklist.begin();
      worklist.erase(worklist.begin());
      const State in = out.in_states.at(point);
      auto result = adapter.transfer(point, in);
      // A site's resolution is a function of its (monotonically growing) input: the latest transfer is authoritative.
      out.unresolved_computed.erase(point);
      out.computed_targets.erase(point);
      const auto pin = pinned.find(point);
      if (result.unresolved_computed) {
        out.unresolved_computed[point] = *result.unresolved_computed;
      } else if (pin != pinned.end()) {
        out.unresolved_computed[point] = pin->second;
      }
      for (const auto &edge : result.edges) {
        if (edge.kind == EdgeKind::computed) {
          if (pin != pinned.end()) continue;  // a pinned site contributes no computed edge
          out.computed_targets[point].insert(edge.target);
          emitted[point].insert(edge.target);
        }
        if (!propagate(edge.target, edge.state)) {
          out.reason = UnknownReason::state_bound;
          return out;
        }
      }
    }
    bool restart = false;
    for (const auto &[site, targets] : emitted) {
      const auto final_targets = out.computed_targets.find(site);
      const bool covered = final_targets != out.computed_targets.end() &&
                           std::includes(final_targets->second.begin(), final_targets->second.end(), targets.begin(), targets.end());
      if (covered) continue;
      const auto reason = out.unresolved_computed.find(site);
      pinned.emplace(site, reason != out.unresolved_computed.end() ? reason->second : UnknownReason::unsupported_transfer);
      restart = true;
    }
    if (!restart) break;
    ++restarts;
  }
  out.complete = true;
  return out;
}

}  // namespace segarecomp::analysis
