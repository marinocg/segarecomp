# ADR-0026: An offline code-entry inventory partitions static validation into stitched independent units

- Status: Accepted
- Date: 2026-09-07
- Deciders: SEG-007-T180 (architecture-class task), consuming SEG-007-T179's
  ADR-0025 offline whole-ROM analysis groundwork and its
  `needs_full_refinement` handoff that the reset-entry walk recursively
  rediscovers downstream inventory code and exhausts its per-root discovery
  budget before the SEG-007-T178 `JMP (An)` frontier.
- Amends: ADR-0025 (adds the Phase 1 admission / Phase 2 stitch model on the
  canonical assisted route). Cross-references ADR-0013 (entry-rooted discovery
  prefix boundary) and ADR-0014 (entry-connected admission invariant): the
  per-root walk semantics, ceilings, and fail-closed cross-root aggregation are
  all preserved verbatim; only the set of addresses a given root recursively
  walks is narrowed. Weakens neither ADR-0009 Tier-1, ADR-0024/0025 Tier-2
  emitted-set dispatch, the A7/SP exclusion, nor the raw/unannotated route.

## Context

ADR-0025 made a deterministic offline Ghidra `code_entry_candidate` inventory an
additional per-root seed source. Each candidate was still handled as the start
of a full recursive reachable-region walk bounded by
`m68k_discovery_max_instructions`. For a broad, high-recall inventory of a real
commercial ROM the reset-entry root's own walk recursively rediscovers a large
fraction of the downstream inventory-covered code (branch/call/indirect targets
into already-proposed units), exhausting that one per-root budget with a fatal
`discovery_budget_exhausted` before reaching the pure `JMP (An)` / Nemesis-A3
frontier the ADR-0025 Tier-2 mechanism already handles. Raising any budget is a
forbidden non-goal; it would also not converge.

## Decision

### 1. The inventory PARTITIONS validation; it does not just seed it

An offline `code_entry_candidate` inventory is treated as a proposed partition
of the program's reachable code into bounded local units. Validation runs in two
phases inside `discover_m68k_general_startup`:

- **Phase 1 (admission).** Each candidate address `c` is walked as its own
  bounded local unit via `discover_m68k_static_graph(c, limits, environment,
  boundary = (all other candidate addresses) ∪ {reset entry})`. No Phase-1
  result is merged. `c` is *admitted* iff its entry decoded cleanly into a
  retained block, its walk hit no fatal (non-`discovery_prefix_boundary`)
  probe failure, and its walk was not truncated by a `discovery_budget_exhausted`
  cut. Non-admitted candidates are recorded only as normalized rejection metrics
  and never participate further (not as a seed, not in any boundary set).

- **Phase 2 (stitch + aggregate).** The reset entry (`seed_index == 0`), the
  IRQ6 autovector root, every admitted-unit root, and every
  `runtime_confirmed` fallback seed are each walked with
  `boundary = admitted_units \ {this root's own entry}`, and every result flows
  through the unchanged `merge_root_result`. The reset entry stays `seeds[0]`
  and remains the sole authority (with the IRQ6 root) for whole-program
  rejection; only its boundary set changed.

### 2. `independent_unit_boundaries` narrows recursion, never control flow

`discover_m68k_static_graph` gains an optional
`const std::set<std::uint32_t> &independent_unit_boundaries`. When the walk
reaches a boundary address `B` as a *statically-resolved control-transfer
destination* — an unconditional/conditional direct-branch taken target, a
JSR/BSR/foldable `JMP`/`JSR` callee, or an ADR-0009 indirect candidate — the
transfer is still validated, the `A -> B` edge and (for a call) the call frame
are still recorded, and `B` is still noted as a block entry; only the
`worklist_.push_back(B)` recursive body walk is elided, because `B`'s body is
charged to `B`'s own admitted unit.

Sequential fallthrough (`next_pc`) and call/branch continuation (`next_pc`) are
**never** guarded and are always enqueued: an inventory boundary must not invent
a control-flow boundary the MC68000 instruction stream does not contain. `RTS` /
`RTE` enqueue nothing, unchanged. An empty boundary set (every pre-T180 caller,
and every raw/unannotated recompilation) makes the pass byte-identical.

### 3. Overlap agreement and conflict are unchanged fail-closed aggregation

Two admitted units that decode overlapping code still go through
`decoded_matches` in `merge_root_result`: identical decodes deduplicate and are
counted as agreement; any decode / indirect-candidate-set / completion-RTS
disagreement raises `aggregation_conflict` and fails the whole build closed with
`startup_graph_mismatch`, exactly as before. ADR-0014 P1 entry-connectivity and
`partial_or_rejection`'s `retained_addresses == sibling_addresses` equality are
untouched — a stitched edge still records `B` as a retained block entry in the
aggregate, so the aggregate remains a connected static program.

### 4. One aggregated program, one emission

There is no parallel program representation. The aggregated
`FrontendAnalysis` feeds the existing
`validated_code_entry_candidate_roots -> emitted_block_entries ->
emitted_code_address_set` path for one C generation and one compile. ADR-0009
Tier-1 exact finite target sets remain preferred; ADR-0024/0025 Tier-2
`JMP/JSR (An)` and `pc_index8` membership dispatch still resolve against the
final validated `EmittedCodeAddressSet`.

### 5. Normalized instrumentation

`FrontendAnalysis::OfflineInventoryStitchMetrics` records numbers only (never a
raw address): offline candidate count; admitted unit count; rejected count by
normalized reason (`fatal_probe_failure` / `walk_truncated_budget_cut` /
`entry_undecodable` / `other`); stitched direct-edge count; overlapping-unit
agreement / conflict count; max and aggregate local discovery instructions and
blocks. The struct also declares `emitted_block_count` /
`emitted_code_address_count` fields, but this batch leaves them **declared, not
yet populated** (always 0) and does not print them — wiring them from the final
validated `emitted_block_entries` / `EmittedCodeAddressSet` is owned by the
`needs_full_refinement` continuation that settles the Phase-2 aggregate
representation. One normalized stderr line carrying the populated counters is
emitted whenever the inventory is non-empty; `tools/genesis_startup_bridge.py`
parses that line in-process from the emitter's stderr string (no artifact is
written into the compare-runs out-dir surface) and threads the same counters
into its `ONE_SHOT_SUMMARY` / `EPHEMERAL_FRONTIER` JSON.

### 6. Synthetic coverage split

The landed boundary-guard mechanism is covered by refinement-stable synthetic
tests for the direct-branch / direct-call / conditional-branch / sequential-
fallthrough stitch cases plus the false-positive-rejection and empty-inventory
no-op cases. The overlap-merge, cross-unit fail-closed, Tier-1 regression, and
Tier-2 dispatch cases are deferred to the `needs_full_refinement` continuation,
which may reshape Phase-2 aggregation and should own those assertions once its
representation is settled.

### 7. Admission closure under rejected partition boundaries

- **Amends:** this ADR's own §1 Phase-1 admission rule. Consuming task/evidence: SEG-007-T212's
  `needs_full_refinement` handoff (runtime `internal_dispatch_inconsistency` at a single
  admitted-but-never-completed target address), refined into this section by the
  `backlog/seg-007-refine` full-refinement pass; implemented by SEG-007-T213.

**Problem.** §1's Phase-1 admission walk for a candidate `c` uses
`boundary = (all OTHER proposed candidate addresses) ∪ {reset entry}` — the FULL initial candidate
set, evaluated in one single pass, before any candidate has been rejected. §1's Phase-2 stitch walk
for an admitted root instead uses `boundary = admitted_units \ {this root's own entry}` — the FINAL
admitted set, which by construction excludes every rejected candidate. These two boundary sets are
not guaranteed to agree: a candidate `c` can be admitted in Phase 1 only because a neighboring
candidate `b` acted as a local stitching stop during `c`'s walk, and `b` can subsequently be
rejected by Phase 1's own criteria (undecodable entry, fatal probe failure, budget-truncated walk).
When Phase 2 then walks `c`'s unit under the narrower `admitted_units` boundary (which no longer
contains `b`), the walk no longer stops at `b` and instead continues into whatever code follows it —
frequently invalid/junk decode territory — so `c`'s unit never completes as a retained block in the
representation the emitted set is built from, even though `c` is formally a member of
`admitted_units`. Phase-1 admission is therefore not closed under its own Phase-1 rejection result:
admission is not evaluated against the boundary set Phase 2 will actually use. This was confirmed
empirically by SEG-007-T212 for the runtime-selected `internal_dispatch_inconsistency` target and is
a structural admission-model gap, not a Sonic-specific defect and not a T185/ADR-0028 pruning or
emitted-set-traversal regression (the target was never a completed retained block entry in either
representation to begin with). A second, narrower instance of the same boundary-set-asymmetry family
exists independently of any rejection: §1's Phase-1 boundary always includes the reset entry for
every candidate, but §1's EXISTING Phase-2 boundary construction omits the reset entry entirely for
every non-reset root — see "Reset-entry boundary consistency" below.

**Decision: dependency-aware fixed-point admission.** Phase-1 admission is redefined as a
monotonically shrinking, deterministic fixed-point computation over the initial candidate set,
rather than one single pass:

```text
A0 = all proposed offline candidates

round n (n >= 0):
  validate every surviving c in An with:
      boundary = (An - {c}) + {reset entry}
  A(n+1) = An minus every c whose validation now fails
           (entry_undecodable | fatal_probe_failure | walk_truncated_budget_cut)

repeat until A(n+1) == An
final admitted_units = A* = the fixed point
```

The sequence is monotonically non-increasing (`A0 ⊇ A1 ⊇ A2 ⊇ ... ⊇ A*`): a round can only remove
candidates from the surviving set, never add one back. **A rejected candidate is never re-admitted
in a later round for any reason**, including to preserve another candidate's own validity; the
`proposed -> admitted` and `proposed/admitted -> rejected` transitions are the only ones this
computation performs, and `rejected -> admitted` never occurs within one fixed-point computation.
`A*` is exactly the greatest (largest) subset of `A0` that is simultaneously self-consistent: every
member of `A*` validates cleanly under a boundary containing only `A*`'s other members plus the
reset entry — the exact boundary shape Phase 2 will use. Phase 2 (§1's stitch/aggregate step) is
unchanged except that it now consumes this fixed-point `A*` in place of the single-pass
`admitted_units`; its own `boundary = admitted_units \ {this root's own entry}` boundary
construction, seed ordering, and `merge_root_result` consumption are untouched except as amended
below under "Reset-entry boundary consistency".

**Monotonic-rejection proof.** An `independent_unit_boundaries` member (this ADR's Phase-1/Phase-2
boundary set) only suppresses RECURSIVE BODY TRAVERSAL past an already-validated control-transfer
destination; it never invalidates, un-decodes, or removes an instruction, edge, or frame already
recorded on the walking candidate's own path before that boundary is reached. Removing a boundary
member therefore only ever EXPOSES additional recursive body instructions along the same
already-walked path — it can never retract, un-decode, or otherwise erase a decode/probe result the
walk had already produced before reaching that boundary. This makes each of the three admission
failure classes monotone with respect to boundary removal:

- `entry_undecodable` — decoding the candidate's own entry instruction does not consult
  `independent_unit_boundaries` at all; boundary composition is irrelevant to this failure.
- `fatal_probe_failure` — a non-prefix-boundary `discovery_budget_exhausted` (or any other fatal
  issue) is raised by flow the walk has ALREADY explored; removing a boundary can only let the walk
  explore MORE flow (past a stop it used to hit), never retract flow already explored and already
  found fatal.
- `walk_truncated_budget_cut` — removing a boundary can only ADD recursive body instructions to the
  walk's own per-root budget consumption (by letting it walk past a stop it used to hit), never
  reduce the instruction count already consumed; a walk that already exhausted its budget before any
  boundary was removed remains exhausted.

Therefore: shrinking the surviving candidate set (which, under both §1's original Phase-1 rule and
this §7 fixed point, can only shrink the boundary set, since a member's own entry is always excluded
from its own boundary) can turn a PREVIOUSLY-VALID candidate invalid (a stop it relied on
disappearing exposes it to failing flow beyond that stop), but can never turn an ALREADY-INVALID
candidate valid (no boundary removal retracts a decode/probe result or reduces consumed budget).
Admission validity is thus monotone non-increasing with respect to boundary-set shrinkage; a
candidate rejected at any round of the fixed point could not become valid again under a
further-shrunk boundary set, so the computation never needs, and never performs, a
`rejected -> admitted` transition. This is exactly why `A*` is the GREATEST fixed point reachable by
monotonic removal from `A0`, and why "never re-admit a rejected candidate" is a proven invariant of
the computation rather than merely an asserted policy. If implementation inspection of
`discover_m68k_static_graph` reveals any behavior violating this monotonicity property (a boundary
removal that somehow retracts an already-produced decode/probe result), implementation must stop and
correct the architecture rather than keep this claim.

**Dependency-aware revalidation (preferred over naive whole-set re-validation).** Recompute a
candidate's validation only when it can actually be affected: during each candidate's admission
probe, record which OTHER candidate addresses its own bounded local walk actually reached as a
validated control-transfer destination (a taken direct-branch target, a JSR/BSR/foldable-JMP callee,
or an ADR-0009 finite indirect candidate) and used as a boundary stop — i.e. the exact set of
boundary members `discover_m68k_static_graph` consulted while producing that candidate's own probe
result. This is `c`'s dependency set, built only from already-produced static-discovery facts (never
heuristic address proximity or ROM-specific knowledge). When a boundary candidate is removed in
round `n`, only candidates whose dependency set intersects the removed set are revalidated in round
`n+1`; a candidate whose local walk never reached the removed boundary as a control-transfer
destination is provably unaffected (its own probe result did not consult that boundary member) and
is not requeued. This bounds per-round cost to the affected subgraph rather than the whole candidate
population, and is traversal-order independent because it is derived solely from each candidate's
own already-computed, deterministic probe result. A simpler whole-set re-validation (recompute every
surviving candidate's probe every round) is semantically equivalent and acceptable as a first
bounded implementation or as a fallback if the dependency bookkeeping proves disproportionately
costly for the actual candidate population size; either produces the identical final `A*` because
both compute the same monotonic fixed point, only at different revalidation cost.

**Termination.** `A0` is finite (bounded by the offline inventory's finite candidate count) and each
round either removes at least one candidate or the computation stops; therefore the number of rounds
is bounded by `|A0|` and the computation always terminates in a finite, deterministic number of
rounds regardless of candidate proposal order. No new discovery/block/round/frontier-exit ceiling is
introduced or raised; this is a bound on ADMISSION rounds, an entirely different quantity from the
existing per-root instruction/block ceilings, which remain unchanged and are consulted unchanged
inside each round's per-candidate probe.

**Reset-entry boundary consistency (every post-admission walk, not only the final Phase-2 seed
loop).** §7's fixed point already includes the reset entry in every Phase-1 candidate probe's own
boundary (`boundary = (An - {c}) + {reset entry}`), but EVERY existing post-admission walk that reuses
this ADR-0026 4th-arg `independent_unit_boundaries` seam (`boundary = admitted_units;
boundary.insert(synthesized_control_roots...); boundary.erase(root_value)`, before this amendment)
omits the reset entry entirely for every non-reset root — the reset entry participates in a
non-reset candidate's Phase-1 admission boundary but silently disappears from that same candidate's
boundary at every later discovery phase that reuses this seam. This is another instance of the exact
boundary-set asymmetry this section exists to eliminate: a boundary necessary to establish a
candidate's final admission must not disappear from that same candidate's own post-admission
independent-unit walk, AT ANY PHASE, not only the final synchronous Phase-2 seed loop. This amendment
therefore generalizes the invariant: after fixed-point admission, EVERY static-discovery walk rooted
at an address other than reset that uses this boundary seam must treat reset as an
independently-owned boundary:

```text
independent_unit_boundaries(root R) =
    final_admitted_units (A*)
  ∪ synthesized_control_roots
  ∪ {reset entry}
  - {R}
```

(continuation roots continue to be supplied through their own existing, separate 5th-arg
`cont_boundary` seam, unchanged). For reset's OWN root walk, `R == {reset entry}`, so the `- {R}` term
removes it and naturally restores today's existing boundary (`A* ∪ synthesized_control_roots`) with
no `seed_index == 0` or equivalent special case needed — reset must never treat itself as a boundary,
at any phase. This construction applies identically at every current
`boundary = admitted_units; boundary.insert(synthesized_control_roots...)` site in
`discover_m68k_general_startup`: the ADR-0027/ADR-0028 continuation/resolved-control-root synthesis
pre-pass (each round's per-root `unit_boundary`), the normal synchronous Phase-2 seed walks, the IRQ6
autovector root walk (`irq6_boundary`), and the Tier-2 late-represented-indirect-target walk. This
does NOT require every walk to use literally identical boundary sets after ADR-0027/ADR-0028
synthesis grows `synthesized_control_roots`/`continuation_roots` mid-computation — each walk may still
validly see only the synthesized partitions discovered so far at its own point in the computation.
The invariant this closes is narrower and exact: no boundary member that was NECESSARY to establish a
candidate's final Phase-1 admission (namely, the reset entry, which every admission probe already
includes) may be absent from that same candidate's corresponding post-admission independent-unit
walk, at any phase. A candidate cannot pass admission, nor synthesize/complete a unit at any
intermediate discovery phase, merely because the reset entry was a Phase-1-only stopping boundary
that then silently disappeared later — including during the ADR-0027/ADR-0028 synthesis pre-pass or
the IRQ6 walk, not only the final Phase-2 seed loop.

**Interaction with other roots and mechanisms.** ADR-0027 continuation roots, ADR-0028 synthesized
resolved-control-target roots, the IRQ6 autovector root, and `runtime_confirmed` fallback seeds all
participate in Phase 2 and (for continuation/synthesized-control roots) the pre-pass, computed
strictly AFTER Phase-1 admission reaches its fixed point `A*`; none of them participates in or is
required by the admission fixed point itself. The reset entry's role changes only as described
immediately above (added to every non-reset root's boundary at every walk that reuses this seam); no
other existing boundary-set construction changes shape beyond that — they simply now consume the
corrected, closure-complete `admitted_units = A*` instead of the single-pass set.
`FrontendAnalysis::semantic_partition_boundary_addresses`
(ADR-0028 §8) continues to be built once discovery finishes, and continues to use only the final
fixed-point `admitted_units`; a candidate rejected at any round of the fixed point is excluded from
it, from every Phase-2 seed/boundary set, and from contributing any decoded instruction or block
identity to the final aggregate, by the same existing mechanism that already excludes a
Phase-1-rejected candidate today (§1's untouched "rejected candidates never participate further"
rule now additionally covers a candidate rejected in round `n > 0` of the fixed point, not only
round 0).

**Rejection-reason bookkeeping.** A candidate's recorded rejection reason is whichever of
`entry_undecodable` / `fatal_probe_failure` / `walk_truncated_budget_cut` its LAST (round-of-rejection)
probe produced; a candidate that was provisionally valid in an earlier round and only fails after a
dependency's removal is counted once, under its final-round reason, in the unchanged
`OfflineInventoryStitchMetrics` rejection categories — it is never double-counted across rounds.

**New normalized metrics (numbers only, no raw address, added to `OfflineInventoryStitchMetrics`):**
`admission_fixed_point_rounds` — the number of validation-pass rounds run over the
currently-scheduled/surviving candidate set until convergence, where one round is defined
consistently as one validation pass over that set: **exactly `0` when the initial candidate set is
empty** (there is no candidate to validate — a true zero-round no-op, matching the raw/no-inventory
fixture), and **`>= 1` whenever the initial candidate set is non-empty** (round 0's own single
validation pass always counts, even when it changes nothing).
`admission_revalidation_count` (number of per-candidate re-probes performed across all rounds after
round 0), `admission_dependency_edge_count` (size of the recorded candidate-to-boundary dependency
relation, 0 if the whole-set fallback is used instead of dependency tracking), and
`rejected_after_dependency_removal_count` (candidates admitted in round 0 but rejected in a later
round). `offline_candidate_count` and `admitted_unit_count` are unchanged in meaning —
`admitted_unit_count` now reports `|A*|` rather than `|A0|`'s single-pass admitted count.

**Preserved safety invariant.** Offline/Ghidra candidates remain untrusted proposals throughout.
This fixed point never promotes a rejected candidate to a semantic partition boundary, an emitted
block root, a C4 frontier, a hidden stitching sentinel, or a runtime-dispatchable address; it only
makes the ADMISSION decision for a surviving candidate self-consistent with the boundary set Phase 2
will actually use. A candidate that is rejected at fixed point but is separately, independently
reached through a validated first-party direct control edge (the exact shape SEG-007-T212 observed)
is handled entirely by EXISTING first-party discovery semantics after the fixed point — reset/
title-root recursive discovery, ADR-0027 continuation-root synthesis, or ADR-0028
resolved-control-target synthesis, each unchanged — never by re-admitting the rejected offline
proposal itself. Whether such an address should additionally receive some other authoritative
treatment BECAUSE it is validated-first-party-reachable (as distinct from being an offline proposal)
is a separate question this amendment does not decide; see this task's own Non-goals for its
explicit deferral.

**Rejected alternatives.**

- *Keep single-pass admission; make Phase 2 use the ORIGINAL full candidate set (including rejected
  candidates) as its boundary.* Rejected: this lets a candidate Phase 1 has already determined to be
  invalid (undecodable, fatal-probe-failing, or budget-truncated) continue to shape canonical
  program construction indefinitely, which is the exact untrusted-proposal violation §1 was written
  to prevent.
- *Promote a rejected candidate to a downstream semantic/stitching boundary fact merely because
  another unit's admission depended on it.* Rejected for the same reason: it would make a
  Phase-1-rejected, unvalidated proposal directly influence Phase-2 canonical construction,
  contradicting §1's existing untrusted-proposal model.
- *Reuse ADR-0028's synthesized-resolved-control-target retry machinery, applied to every admitted
  unit whose Phase-2 walk fails to complete, with the wider (pre-rejection) boundary.* Already
  attempted and empirically disproved by SEG-007-T212: it raised
  `synthesized_resolved_control_target_unit_count` from 4 to 402 while leaving every downstream
  metric (`retained_block_count_before/after_pruning`, `emitted_block_count`,
  `emitted_code_address_count`) and the runtime frontier completely unchanged, because the retried
  unit's local probe still only terminates cleanly by reintroducing a rejected candidate's address
  as an artificial local stop that Phase 2's own downstream completion sets (`block_entries` /
  `candidate_frontier_addresses` / `semantic_partition_boundary_addresses`) never recognize — a
  no-op, not a correction. Not re-attempted here.
- *Expand logical-table/descriptor coverage (ADR-0032/ADR-0033) for this target.* Rejected:
  SEG-007-T212 found no evidence of a computed/table-driven transfer at this edge; the source-side
  edge is a retained direct `BSR`, not a table dispatch.
- *Whole-set (non-dependency-aware) fixed-point re-validation as the FINAL implementation.* Not
  rejected outright — accepted as an equally-correct, simpler fallback if the dependency-tracking
  bookkeeping proves disproportionately costly relative to the actual candidate population; this
  task runs a bounded whole-set experiment first and adopts dependency-aware revalidation as the
  preferred final shape unless that experiment shows the whole-set cost is already acceptable and
  the dependency bookkeeping would add complexity without a measured benefit.

## Consequences

- SEG-007-T213 / §7: fixed-point admission closes both the rejected-candidate admission/Phase-2
  boundary-consistency gap and the reset-entry boundary asymmetry across every post-admission walk
  that reuses the ADR-0026 4th-arg `independent_unit_boundaries` seam (the ADR-0027/ADR-0028
  synthesis pre-pass, the normal synchronous Phase-2 seed walks, the IRQ6 autovector root walk, and
  the Tier-2 late-represented-indirect-target walk); the only boundary-construction change at any of
  these sites is adding the reset entry to every non-reset root's boundary (each root's own walk,
  seed ordering, and `merge_root_result` aggregation semantics are otherwise untouched), and the
  untrusted-proposal model ADR-0026 already established is not weakened.
- The reset-entry root's per-root budget now covers only its own unit plus
  non-partitioned fallthrough code, so a broad inventory no longer exhausts it
  before the ADR-0025 Tier-2 frontier.
- Discovery stays fully static and fail-closed: no interpreter, JIT, runtime
  opcode decode, budget increase, Phase-B expansion, runtime-confirmed seed, or
  Sonic-specific address.
- Phase 1 rejects a candidate only when its entry fails to decode, hits a fatal
  probe failure, or truncates its local walk on budget; such a candidate never
  becomes a Phase-2 seed and never contributes a boundary. A false positive that
  happens to decode into plausible-but-wrong instructions within budget IS
  admitted and merged in Phase 2. Its reachability is still gated: an aggregate
  block only becomes a real dispatch target through emitted-set membership, and
  the unchanged cross-root `decoded_matches` / per-root supersession guards fail
  the build closed on a genuine cross-root decode conflict. So a within-budget
  false positive can add unreachable decoded blocks but cannot silently poison a
  valid unit's emitted code.
- The model is necessary but, for an inventory whose control-transfer boundaries
  do not densely cover the reset-reachable region, not sufficient on its own: a
  fallthrough-connected instruction stream longer than the per-root ceiling and
  only sparsely broken by unconditional transfers into admitted units stays
  charged in full to the reset root. Fallthrough must never be stitched
  (§2), so clearing that residual requires a further refinement (whole-region
  ownership of a candidate-free linearly-reachable stretch, or an equivalent
  entry-connected representation change) tracked as SEG-007-T180's
  `needs_full_refinement` continuation.
