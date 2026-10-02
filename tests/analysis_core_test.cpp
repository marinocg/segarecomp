// SEG-029-T002 (ADR 0078): the generic analysis core over a synthetic, CPU-free toy program language (no instruction set).
// Each toy point carries one operation over two abstract locations and names its successors explicitly.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "segarecomp/analysis/finite_value.hpp"
#include "segarecomp/analysis/solver.hpp"

namespace {

using namespace segarecomp::analysis;

int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

enum class Op { set, add, copy, opaque, nop, jump_via, stop };
struct ToyPoint {
  Op op{Op::nop};
  unsigned loc{};
  std::uint64_t value{};
  unsigned source{};
  std::vector<std::uint64_t> next;  // static successors
};
using State = ValueVector<2>;

struct ToyAdapter {
  using State = ::State;
  std::map<std::uint64_t, ToyPoint> program;
  std::size_t transfers{};
  TransferResult<State> transfer(std::uint64_t point, const State &in) {
    ++transfers;
    TransferResult<State> out;
    const auto found = program.find(point);
    if (found == program.end()) return out;
    const auto &p = found->second;
    State s = in;
    switch (p.op) {
    case Op::set: s.values[p.loc] = FiniteValue::constant(p.value); break;
    case Op::add: s.values[p.loc] = s.values[p.loc].map([&](std::uint64_t v) { return (v + p.value) & 0xFFU; }); break;
    case Op::copy: s.values[p.loc] = s.values[p.source]; break;
    case Op::opaque: s.values[p.loc] = FiniteValue::unknown(UnknownReason::unsupported_transfer); break;
    case Op::nop: break;
    case Op::stop: return out;
    case Op::jump_via: {
      const auto &target = s.values[p.loc];
      if (target.is_precise()) {
        for (const auto t : target.values()) out.edges.push_back({t, EdgeKind::computed, s});
      } else if (target.is_unknown()) {
        out.unresolved_computed = target.reason();
      }
      return out;
    }
    }
    for (const auto n : p.next) out.edges.push_back({n, p.next.size() > 1U ? EdgeKind::branch : EdgeKind::fallthrough, s});
    return out;
  }
};

std::string dump(const Solution<State> &solution) {
  std::ostringstream out;
  out << "complete=" << solution.complete << " iterations=" << solution.iterations << '\n';
  for (const auto &[point, state] : solution.in_states) {
    out << point << ':';
    for (const auto &value : state.values) out << ' ' << value.describe();
    out << '\n';
  }
  for (const auto &[point, reason] : solution.unresolved_computed) out << "unresolved " << point << ' ' << unknown_reason_name(reason) << '\n';
  for (const auto &[point, targets] : solution.computed_targets) {
    out << "targets " << point;
    for (const auto t : targets) out << ' ' << t;
    out << '\n';
  }
  return out.str();
}

std::vector<std::pair<std::uint64_t, State>> entry(std::uint64_t point = 0U) {
  return {{point, State::all_unknown(UnknownReason::unknown_input)}};
}
FiniteValue loc_at(const Solution<State> &s, std::uint64_t point, unsigned loc) {
  return s.query(point, [&](const State &state) { return state.values[loc]; });
}

void domain_laws() {
  const auto a = FiniteValue::of({3, 1, 3});
  expect(a.values() == std::vector<std::uint64_t>{1, 3}, "DOM: normalized sorted distinct");
  expect(FiniteValue::of({}).is_bottom(), "DOM: empty set is bottom");
  const auto u = FiniteValue::unknown(UnknownReason::set_bound);
  expect(join(a, FiniteValue::bottom()) == a && join(FiniteValue::bottom(), a) == a, "DOM: bottom is the join identity");
  expect(join(a, u) == u && join(u, a) == u, "DOM: Unknown absorbs");
  expect(join(FiniteValue::unknown(UnknownReason::set_bound), FiniteValue::unknown(UnknownReason::unknown_input)).reason() ==
             UnknownReason::unknown_input,
         "DOM: Unknown reasons join deterministically (smallest)");
  expect(join(FiniteValue::of({1, 2}), FiniteValue::of({3}), 2).reason() == UnknownReason::set_bound,
         "DOM: a union over the bound is Unknown(set_bound), never a truncated set");
  expect(FiniteValue::of({1, 2, 3}, 2).is_unknown(), "DOM: of() over the bound is Unknown");
  std::vector<std::uint64_t> big(default_set_bound + 1U);
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = i;
  expect(FiniteValue::of(big, default_set_bound * 2U).reason() == UnknownReason::set_bound,
         "DOM: a caller can never raise the bound above the resource constant");
  // join is an upper bound, commutative, idempotent; leq is consistent with join.
  const std::vector<FiniteValue> samples{FiniteValue::bottom(), FiniteValue::constant(1), FiniteValue::of({1, 2}),
                                         FiniteValue::of({2, 9}), u, FiniteValue::unknown(UnknownReason::unknown_input)};
  for (const auto &x : samples)
    for (const auto &y : samples) {
      const auto j = join(x, y);
      expect(leq(x, j) && leq(y, j), "DOM: join is an upper bound of " + x.describe() + " and " + y.describe());
      expect(j == join(y, x), "DOM: join commutes");
      expect(leq(x, y) == (join(x, y) == y), "DOM: leq(x,y) iff join(x,y)==y for " + x.describe() + ", " + y.describe());
    }
  expect(join(a, a) == a, "DOM: join idempotent");
  expect(FiniteValue::of({0x10, 0xab}).describe() == "{0x10,0xab}" && FiniteValue::constant(0).describe() == "{0x0}",
         "DOM: deterministic description");
}

void straight_line_and_join() {
  ToyAdapter t;
  t.program[0] = {Op::set, 0, 4, 0, {1}};
  t.program[1] = {Op::nop, 0, 0, 0, {2, 3}};       // branch
  t.program[2] = {Op::add, 0, 1, 0, {4}};          // path A: 5
  t.program[3] = {Op::set, 0, 9, 0, {4}};          // path B: 9
  t.program[4] = {Op::stop, 0, 0, 0, {}};
  const auto s = solve(t, entry());
  expect(s.complete, "JOIN: solver completes");
  expect(loc_at(s, 4, 0) == FiniteValue::of({5, 9}), "JOIN: branch join is the exact union {5,9}: " + loc_at(s, 4, 0).describe());
  expect(loc_at(s, 4, 1).reason() == UnknownReason::unknown_input, "JOIN: untouched location stays Unknown(unknown_input)");
  expect(loc_at(s, 99, 0).is_bottom(), "JOIN: an unreached point is bottom, not a value");
}

void no_false_certainty_from_join() {
  ToyAdapter t;
  t.program[0] = {Op::nop, 0, 0, 0, {1, 2}};
  t.program[1] = {Op::set, 0, 4, 0, {3}};
  t.program[2] = {Op::opaque, 0, 0, 0, {3}};       // one unknown path
  t.program[3] = {Op::jump_via, 0, 0, 0, {}};
  t.program[4] = {Op::stop, 0, 0, 0, {}};
  const auto s = solve(t, entry());
  expect(loc_at(s, 3, 0).reason() == UnknownReason::unsupported_transfer,
         "UNK: a join with an Unknown path is Unknown, never the precise side");
  expect(s.unresolved_computed.contains(3) && !s.computed_targets.contains(3) && !s.in_states.contains(4),
         "UNK: an Unknown computed target yields no edge and a typed unresolved site");
}

void computed_edges_and_propagation() {
  ToyAdapter t;
  t.program[0] = {Op::nop, 0, 0, 0, {1, 2}};
  t.program[1] = {Op::set, 0, 10, 0, {3}};
  t.program[2] = {Op::set, 0, 20, 0, {3}};
  t.program[3] = {Op::copy, 1, 0, 0, {4}};
  t.program[4] = {Op::jump_via, 1, 0, 0, {}};
  t.program[10] = {Op::set, 0, 7, 0, {30}};
  t.program[20] = {Op::opaque, 0, 0, 0, {30}};
  t.program[30] = {Op::stop, 0, 0, 0, {}};
  const auto s = solve(t, entry());
  expect(s.computed_targets.contains(4) && s.computed_targets.at(4) == std::set<std::uint64_t>{10, 20},
         "COMP: a precise location gives exactly the computed targets");
  expect(loc_at(s, 30, 0).reason() == UnknownReason::unsupported_transfer, "COMP: Unknown propagates through computed edges");
}

void loop_termination() {
  ToyAdapter t;  // l0 = 0; loop: l0 += 1 (mod 256) -> back; the set grows to all 256 values, within the bound
  t.program[0] = {Op::set, 0, 0, 0, {1}};
  t.program[1] = {Op::add, 0, 1, 0, {1, 2}};
  t.program[2] = {Op::stop, 0, 0, 0, {}};
  const auto s = solve(t, entry());
  expect(s.complete && loc_at(s, 2, 0).values().size() == 256U, "TERM: a bounded loop reaches the exact 256-value fixed point");

  ToyAdapter wide;  // the same loop with a bounded set that cannot hold the fixed point: Unknown(set_bound), still terminates
  wide.program[0] = {Op::set, 0, 0, 0, {1}};
  wide.program[1] = {Op::add, 0, 3, 0, {1, 2}};
  wide.program[2] = {Op::stop, 0, 0, 0, {}};
  const auto w = solve(wide, entry());
  expect(w.complete && loc_at(w, 2, 0).values().size() == 256U, "TERM: odd stride covers all 256 residues");
}

void bound_exhaustion() {
  ToyAdapter t;
  t.program[0] = {Op::set, 0, 0, 0, {1}};
  t.program[1] = {Op::add, 0, 1, 0, {1, 2}};
  t.program[2] = {Op::stop, 0, 0, 0, {}};
  const auto s = solve(t, entry(), Bounds{10U, default_max_points});
  expect(!s.complete && s.reason == UnknownReason::iteration_bound, "BOUND: iteration bound exhaustion is typed");
  expect(loc_at(s, 1, 0).reason() == UnknownReason::iteration_bound && loc_at(s, 99, 0).is_unknown(),
         "BOUND: after exhaustion every query is Unknown (never partial truth, never bottom)");
  const auto p = solve(t, entry(), Bounds{default_max_iterations, 2U});
  expect(!p.complete && p.reason == UnknownReason::state_bound && loc_at(p, 0, 0).reason() == UnknownReason::state_bound,
         "BOUND: point bound exhaustion is typed Unknown(state_bound)");
}

void determinism() {
  const auto build = [] {
    ToyAdapter t;
    for (std::uint64_t i = 0; i < 64U; ++i) t.program[i] = {i % 3U == 0U ? Op::add : Op::nop, 0, i, 0, {(i * 7U + 3U) % 64U, (i + 1U) % 64U}};
    t.program[0].op = Op::set;
    return t;
  };
  auto a = build();
  auto b = build();
  const auto ra = dump(solve(a, entry()));
  const auto rb = dump(solve(b, entry()));
  // Insertion order of entries must not matter either.
  auto c = build();
  std::vector<std::pair<std::uint64_t, State>> entries{{5U, State::all_unknown(UnknownReason::unknown_input)},
                                                       {0U, State::all_unknown(UnknownReason::unknown_input)}};
  std::vector<std::pair<std::uint64_t, State>> reversed(entries.rbegin(), entries.rend());
  auto d = build();
  expect(ra == rb, "DET: repeated runs are byte-identical");
  expect(dump(solve(c, entries)) == dump(solve(d, reversed)), "DET: entry order does not change the result");
}

}  // namespace

int main() {
  domain_laws();
  straight_line_and_join();
  no_false_certainty_from_join();
  computed_edges_and_propagation();
  loop_termination();
  bound_exhaustion();
  determinism();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cout << "analysis_core_test: OK\n";
  return EXIT_SUCCESS;
}
