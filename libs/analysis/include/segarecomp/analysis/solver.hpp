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
// `transfer` must be a pure, monotone function of (point, in).

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
                                                const Bounds &bounds = {}) {
  using State = typename A::State;
  Solution<State> out;
  std::set<std::uint64_t> worklist;
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
    if (result.unresolved_computed) out.unresolved_computed[point] = *result.unresolved_computed;
    for (const auto &edge : result.edges) {
      if (edge.kind == EdgeKind::computed) out.computed_targets[point].insert(edge.target);
      if (!propagate(edge.target, edge.state)) {
        out.reason = UnknownReason::state_bound;
        return out;
      }
    }
  }
  out.complete = true;
  return out;
}

}  // namespace segarecomp::analysis
