# ADR-0031: Late Tier-1 computed-JMP target representation and return-edge retention

- Status: Accepted
- Date: 2026-09-09
- Deciders: SEG-007-T190 (architecture-class task), consuming SEG-007-T189
  (ADR-0030) and its two post-completion diagnostic experiments, re-verified by
  direct source inspection of `m68k_prove_stitched_an_indirect_targets`
  (`src/machine/genesis/frontend.cpp`), `return_reachability_successors` /
  `m68k_reachable_return_edges` (`src/cpu/m68k/static_program.cpp`), and the
  discovery/merge machinery (`src/cpu/m68k/static_discovery.cpp`).
- Amends: ADR-0009 (computed indirect control-flow target resolution),
  ADR-0028 (ceiling-triggered resolved-control-target unit synthesis),
  ADR-0029 (canonical stitched control-edge relation consumption), ADR-0030
  (bounded call-context-sensitive An finite-register analysis). Every rule
  those ADRs state is unchanged: `m68k_canonical_control_adjacency`, ADR-0009's
  finite + unknown = unknown merge semantics, the A7/SP finite-proof exclusion,
  the 256-member target-set cap, emitted block identity (program-address only),
  and the `(caller, callee, continuation)` call-identity model
  (SEG-007-T151). This ADR adds one bounded, additive representation +
  return-edge closure at the existing post-stitch consumption boundary; it
  changes no decoder, lifter, or code generator.
- Related: ADR-0011 (bare-address worklist, non-recursive call/return),
  ADR-0025/ADR-0026/ADR-0027 (offline inventory partitioning and stitching).

## Context

ADR-0030's bounded call-context-sensitive mechanism now proves a non-empty
finite Tier-1 target candidate set for the real Sonic computed `JMP (An)`
frontier. Two coupled post-stitch seams then still block that proof from
reaching generated code, both exposed by the same executed Experiment-1
evidence:

- **(A) Late Tier-1 target representation.** The candidate-acceptance gate in
  `m68k_prove_stitched_an_indirect_targets` requires every candidate to already
  satisfy `merged_decoded.contains(candidate.value)` *and*
  `block_entries.contains(candidate.value)` before its `M68kIndirectTargetEaSet`
  is accepted. A late target whose *only* static path is through the exact
  computed `JMP` the proof itself resolves is not yet represented there on the
  unmodified canonical route, so it is silently rejected before Tier-1
  acceptance is reached. Experiment 1's manual offline candidate seeding
  bypassed exactly this gate by pre-representing the target as an ordinary
  external candidate; that it then worked through Tier-1 acceptance is evidence
  the gate is a genuine prerequisite, not that it can be skipped.
- **(B) Return-edge retention after acceptance.** `m68k_reachable_return_edges`
  -- the one post-stitch consumer ADR-0029 Decision 3 deliberately left outside
  the `m68k_canonical_control_adjacency` unification -- has no channel to follow
  an accepted Tier-1 target, and the aggregate reachable-RTS re-synthesis ran
  strictly *before* `m68k_prove_stitched_an_indirect_targets` in the same
  function. So even an accepted late target's reachable RTS never received a
  live `return_to_continuation` edge before ADR-0028's completed-prefix
  orphan-RTS retention pass pruned it, cascading backward into the newly
  represented target block itself.

## Decision

### 1. Stage A: satisfy the acceptance gate by representation, never relaxation

The `merged_decoded` / `block_entries` acceptance gate is unchanged and
unweakened. A sound Tier-1 candidate address that is not yet represented is fed
back through the EXISTING `discover_m68k_static_graph` admission probe and the
EXISTING `merge_root_result` merge, exactly as an ordinary admitted unit
(ADR-0025/0026) or a synthesized resolved-control-target unit (ADR-0028) is.
Admission criteria are identical: the entry must decode cleanly into a retained
block, the walk must hit no fatal (non-prefix-boundary) probe failure, and the
walk must not be truncated by a budget cut. A candidate that fails any of these
is not represented, exactly like any other rejected candidate today.

The Tier-1 proof itself is the sole source of a late candidate address. No
hint, external tool, Ghidra result, s1disasm ingestion, brute-force scan, or
manually persisted candidate seeds Stage A. Only a `JMP (An)` source that is
currently an unproven Tier-2 relation (`unproven_indirect_by_source`) and not
already a proven Tier-1 relation is eligible; `JSR (An)` needs candidate-
specific frames this pass does not create and stays fail-closed, unchanged.

### 2. Stage B: purely-additive Tier-1-aware reachable-return traversal

`m68k_reachable_return_edges` gains one optional, purely-additive input: the
set of already-accepted Tier-1 `M68kIndirectTargetEaSet` facts, keyed by exact
source instruction address, defaulting empty so every existing call site --
in particular the per-seed walk in `static_discovery.cpp` -- is byte-identical.

`return_reachability_successors`'s `jmp` case is extended: when the source
address is a key of that accepted set (mutually exclusive with the existing
`m68k_is_statically_foldable_control_ea` branch -- a source is only ever
unproven-then-proven, never both), the entry's already-validated candidate
addresses are added to the successor set. An `rts` reached only through such a
proven computed jump therefore yields its `return_to_continuation` edge under
the ORIGINAL enclosing `M68kStaticCall` `(caller, callee, continuation)`
identity of the direct call whose callee body reaches that computed jump
(SEG-007-T151 per-callee framing, unchanged). No call frame is ever fabricated
for the Tier-1 target itself.

### 3. Bounded monotone fixed point

Stage A is an ordinary discovery/stitching round, so a newly represented
candidate's body can contain its own computed control-transfer site that a
subsequent finite-`An` proof pass can newly resolve. The A/B closure is
therefore run as a bounded monotone fixed point over exactly
`{fresh finite-An proof over the current graph -> committed-relation
revalidation -> Stage A representation -> (only on a graph-stable round)
acceptance gate -> Stage B return-edge resynthesis -> retention}`.

- **Convergence condition.** A round that grows no representation, commits no
  new Tier-1 target, and adds no new decoded instruction / block entry / edge /
  frame terminates the fixed point.

#### 3a. Representation is monotone; Tier-1 finite-`An` facts are not

The represented program only ever grows: a decoded instruction, block entry,
edge, or frame that Stage A adds is never removed. That growth is monotone over
a finite lattice, which is what makes the closure terminate.

A Tier-1 finite-`An` fact is **not** monotone under that same graph growth. The
finite-register proof merges `finite + unknown = unknown` (ADR-0009) and unions
finite candidate sets across reaching paths. Representing a late target adds new
authoritative reaching paths -- the target body branching back to the
computed-`JMP` source or to one of its predecessors, a path that writes the
same `An` through an unknown-producing transfer, or simply a second finite
producer that becomes reachable only once an earlier target (or its committed
`indirect_branch` / Stage B edge) exists. Any of these can widen a previously
finite `An` state to unknown, or change its finite candidate set, at a
computed-control site whose Tier-1 relation an earlier round already proved.
A finite Tier-1 fact proved against one graph state therefore carries no
guarantee against a later, larger graph state.

#### 3b. Provisional proof vs. committed Tier-1 authority

Each round's finite-`An` proof is **provisional**. A Tier-1 relation is
**committed** (its `indirect_branch` edge added, its
`accepted_tier1_by_source` entry recorded, its Stage B reachable-return
traversal performed, its `reached_unresolved_direct_edge` issue retracted) only
on a round where Stage A added no new representation -- i.e. the graph was
identical across both that round's proof and its acceptance gate. A round that
grew representation commits nothing and simply re-runs the proof over the
expanded graph next round.

Every already-committed relation is re-proved and revalidated at the top of
every later round, before it is allowed to keep authorizing an edge, a Stage B
traversal, or retention. Revalidation requires the fresh proof over the current
(possibly further-expanded, including by the commit round's own edge additions)
graph to still yield, for that exact source, a finite candidate set equal to the
committed set. If a committed relation disappears, widens to unknown, or changes
to a different finite set, the whole build **fails closed** with a normalized
architecture diagnostic (`DirectFlowDiagnostic::startup_graph_mismatch`,
consistent with ADR-0028's fail-closed pattern). A stale target set is never
silently retained, and no larger non-monotone retraction/fixed-point machinery
is introduced -- fail-closed invalidation is the deliberate, preferred
conservative contract.
- **Iteration ceiling.** `kM68kLateIndirectClosureRoundCeiling = 8`. Each
  productive round either strictly grows `merged_decoded` / `block_entries` by
  at least one previously-unrepresented late Tier-1 target address, or (on a
  graph-stable round) strictly grows the monotonically increasing
  accepted-Tier-1 source set / edge set. All of these are monotone (never
  shrink) over a finite address space, so the closure is a monotone fixed point
  over a finite lattice and always terminates. Because commit is deferred to
  the first graph-stable round and one further round then revalidates the
  committed relations against the edge-expanded graph, a route that needs `k`
  levels of representation now converges in roughly `k + 2` rounds rather than
  `k + 1`; the ceiling of 8 still leaves generous headroom (the T189 Sonic
  frontier is single-level). The value 8 is chosen (not
  copied from an unrelated ceiling) as generous headroom over the deepest
  computed-dispatch-through-computed-dispatch nesting a Genesis startup route
  realistically exhibits: the T189 Sonic frontier is single-level, and classic
  jump-table dispatch rarely chains past 2-3 levels. It is independent of, and
  far smaller than, `m68k_fallthrough_continuation_unit_ceiling` (which bounds a
  different quantity: total synthesized partition units).
- **Determinism.** The proof's output is order-independent (ADR-0029/0030). The
  set of late candidates to represent each round is collected into a sorted
  `std::set` and probed in ascending address order; `merge_root_result` is
  order-independent given its existing dedup. The whole closure is a pure,
  deterministic function of the post-stitch graph.
- **Fail-closed on ceiling exhaustion.** If the ceiling is reached with a
  still-changing round, the whole build fails closed with a bounded diagnostic
  frontier (`DirectFlowDiagnostic::discovery_budget_exhausted`, consistent with
  ADR-0028's fail-closed reporting pattern). No sound candidate, accepted
  target, or synthesized edge is ever silently dropped.

The closure is instrumented (numbers only) via
`OfflineInventoryStitchMetrics::late_indirect_closure_rounds`,
`late_indirect_target_unit_count`, and `late_indirect_closure_converged`.
`late_indirect_closure_rounds` now counts representation rounds plus the
deferred graph-stable commit round plus the final revalidate-and-converge
round; `late_indirect_closure_converged` is `false` both on ceiling exhaustion
and on a fail-closed stale-relation invalidation.

### 3c. Soundness / convergence argument

1. **Termination.** The represented program (decoded instructions, block
   entries, edges, frames) grows monotonically over a finite address space, and
   the committed-Tier-1 source/edge sets grow monotonically; every productive
   round strictly increases one of them, so the fixed point converges (or hits
   the explicit ceiling and fails closed).
2. **Fresh proof.** Each round's finite-`An` proof is recomputed from scratch
   over the current complete graph; no proof result is cached across rounds.
3. **No stale authority.** A Tier-1 relation is committed only on a round where
   the graph was stable across its proof and its acceptance gate, and every
   committed relation is re-proved and revalidated against every later expanded
   graph before it keeps authorizing an edge / Stage B traversal / retention.
4. **Fail-closed retraction.** If a later expansion makes a committed relation
   disappear, widen to unknown, or change its finite set, the build fails
   closed (`startup_graph_mismatch`) rather than retaining or partially
   retracting it.

### 4. Ordering

The Tier-1-unaware baseline reachable-RTS completion still runs once, first,
unchanged. The fixed point then runs after it, inside
`discover_m68k_general_startup`, before the analysis is finalized and long
before ADR-0028's completed-prefix retention/pruning pass -- so an accepted
late target's `return_to_continuation` edge exists before any orphan-RTS
retention decision is made.

## Consequences

- A late computed-`JMP (An)` target proven only by the post-stitch
  finite-`An` proof is now represented, accepted through the unchanged gate,
  emitted, and its reachable RTS retains a live `return_to_continuation` edge
  to the original caller continuation.
- A genuinely unproven computed `JMP`, a candidate that fails the existing
  admission criteria, a closure that would need more than 8 rounds, or a
  committed Tier-1 relation later invalidated by graph expansion all behave
  exactly as before / fail the build closed -- never a fabricated target,
  weakened gate, silently dropped edge, or silently retained stale target set.
- No decoder, lifter, code generator, emitted-block identity, Tier-1/Tier-2
  dispatch representation, canonical edge authority, 256-member cap, A7/SP
  exclusion, or ADR-0009/0029/0030 merge-semantics change was made or is
  required.
- A future closure that needs depth > 8, or a `JSR (An)` late target requiring
  candidate-specific frames, is a SEPARATE, not-preselected future decision.
