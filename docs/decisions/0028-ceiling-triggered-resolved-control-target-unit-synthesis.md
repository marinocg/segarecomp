# ADR-0028: Ceiling-triggered resolved-control-target unit synthesis, and the scalable multi-unit partial-program representation invariant

- Status: Accepted (synthesis mechanism §1-§7 and scalable-representation
  correction §8/§8.1/§8.2 landed; §9 authoritative exact direct-control target
  closure drafted by `backlog/seg-007-refine` and tightened across two
  further refinement passes after independent review, a bounded local
  measurement, and a corrected two-stage pipeline/ceiling-scope review,
  reserved for implementation by SEG-007-T214)
- Date: 2026-09-07 (§9 drafted 2026-09-12, tightened 2026-09-12, pipeline/
  ceiling corrected 2026-09-12)
- Deciders: SEG-007-T182 (architecture-class task, §1-§7), consuming
  SEG-007-T181's ADR-0027 fallthrough-continuation partition and its
  `needs_full_refinement` handoff that the residual reset-root per-root
  instruction-ceiling trip lands at a statically resolved+validated direct
  control-transfer destination, not a pure `next_pc` step. SEG-007-T183
  (architecture-class task, §8), consuming SEG-007-T182's own
  `needs_full_refinement` handoff of the scalable multi-unit partial-program
  representation contract. §9 drafted by `backlog/seg-007-refine`, consuming
  SEG-007-T213's `needs_full_refinement` handoff (a retained emitted exact
  direct-control edge whose target is confirmed admitted, cleanly decoded,
  and merged into the aggregate, yet ends with no emitted block, no explicit
  frontier, and no other supported dispatch representation — proven generic
  by a 149-of-2475 unrepresented-target audit over the canonical Sonic
  build), then tightened after a bounded local measurement established the
  `0x00005ebe` cause as `never_became_block_entry` (§9.2, not a
  completed-prefix erase-pass prune) and independent review corrected the
  structural-vs-actual-frontier equivalence, the materialize-then-reprune
  handling, and the offline-inventory gating of the invariant.
- Amends: ADR-0026 (adds a fifth partition mechanism alongside Phase-1 admission,
  Phase-2 control-transfer stitching, the IRQ6 asynchronous static hardware root,
  and ADR-0027 fallthrough continuation). Cross-references ADR-0009 (finite-`An`
  Tier-1), ADR-0011/ADR-0014 (bare-address worklist, entry-connected admission),
  ADR-0024/ADR-0025 (Tier-2 emitted-set dispatch). Weakens none of them.

## Context

ADR-0027 deliberately restricts ceiling-triggered continuation synthesis to
plain `next_pc` steps, because only those carry nothing but decode position plus
a monotonic accumulator set. The canonical one-shot Sonic build still stopped at
build-time `discovery_budget_exhausted`, and diagnosis (fresh headless Ghidra
analysis + heterogeneous hints composition + the assisted one-shot route)
confirmed the residual trip lands on the FIRST instruction of a
statically-resolved+validated direct control-transfer destination reached through
`enqueue_control_target` (a taken direct-branch target / JSR/BSR/foldable
JMP-JSR callee / ADR-0009 finite indirect candidate) that begins a large
candidate-free region still charged to the reset root's budget.

A statically resolved+validated control-transfer target is strictly stronger
evidence than an offline Ghidra proposal: segarecomp itself established that
executable control flow reaches that exact address, and the transfer was fully
validated (mapped, aligned, in-bounds, cleanly decoded, non-mid-instruction).
The A -> X edge and (for a call) the call frame are already recorded before the
body walk is scheduled.

## Decision

### §1 Synthesized resolved-control-target unit eligibility

`M68kStaticGraphWalker` records every address it enqueues via
`enqueue_control_target` in `control_target_reached_`. On a per-root
instruction-ceiling trip whose ADR-0013 §2 side-effect-free probe decodes
cleanly as a prefix-boundary shape (no open control target, empty
`unresolved_reason`), if the trip address is a member of
`control_target_reached_` and NOT a member of `next_pc_reached_`, it is appended
to the new `M68kStaticDiscoveryResult::resolved_control_target_frontier`.
Continuation classification (ADR-0027) takes precedence when an address is both.
A ceiling trip at a genuinely unresolved / undecodable / unmapped / misaligned
open control edge is in neither set and keeps its existing fatal diagnostic
verbatim. Odd/misaligned/unmapped/mid-instruction targets are rejected by the
unchanged `validate_branch_target` / `admit_target` path before they can ever be
recorded.

### §2 Preserved call/return semantics

A synthesized unit that is a call target keeps the existing `direct_call` /
`indirect_call` edge, the call frame, the caller continuation, and the global
`return_to_continuation` reconstruction. The transfer is not reduced to a jump
and the caller is not required to recursively own the callee body: only the
recursive body walk of X is partitioned into X's own independently-validated
unit (identical to ADR-0026 §2 boundary handling).

### §3 One representation / one program

The frontend Phase-2 continuation-root FIXPOINT PRE-PASS (ADR-0027 §4) is
extended: each round also collects `resolved_control_target_frontier` addresses
into `synthesized_control_roots`. Those roots are unioned into the EXISTING
4th-arg `independent_unit_boundaries` set (ADR-0026 §2) for every subsequent
pre-pass round, every Phase-2 seed walk, and the IRQ6 autovector root walk, and
are appended to the Phase-2 seed list after the admitted units and continuation
roots. They flow through the unchanged `merge_root_result`, the unchanged
`decoded_matches` / `m68k_merge_register_state` dedupe-vs-conflict path, and the
unchanged Tier-1/Tier-2 dispatch. No parallel program representation, no parallel
interpreter state. Being reset/IRQ6-reachable through a validated control edge,
a synthesized resolved-control-target root IS authoritative for a whole-program
stop (like a continuation root, unlike a false-positive offline candidate).

### §4 Shared count ceiling — no budget raise

`m68k_fallthrough_continuation_unit_ceiling` is reinterpreted as one shared
COUNT ceiling over all synthesized partition units (continuation +
resolved-control-target). Its numeric value is unchanged (64). This is not an
instruction/block/seed/round/frontier-exit budget raise: every synthesized unit
is still independently walked under the unchanged per-root
`m68k_discovery_max_instructions` ceiling. A region needing more synthesized
units than the ceiling fails the BUILD closed with a normalized
`discovery_budget_exhausted` sourced at the reset entry — no loop, no cap raise.

### §5 Normalized instrumentation

`OfflineInventoryStitchMetrics` gains
`synthesized_resolved_control_target_unit_count` (numbers only), printed in the
existing `segarecomp: offline inventory stitch:` stderr line and parsed
generically by `tools/genesis_startup_bridge.py`. `ceiling_trips_not_continuation_eligible`
now also excludes synthesized resolved-control-target roots.

### §6 Preserved invariants

- The raw/unannotated recompilation route is byte-identical: the mechanism is
  gated on a non-empty offline inventory, and `control_target_reached_` recording
  is inert without a ceiling trip. `enqueue_control_target`'s stitch fast-path is
  unchanged.
- ADR-0026 control-transfer stitching, ADR-0027 `fallthrough_continuation` edges,
  ADR-0009 Tier-1, ADR-0024/0025 Tier-2 emitted-set dispatch, the A7 exclusion,
  and the `startup_graph_mismatch` conflict path are untouched.
- Candidate rejection observability (ADR-0026 §5) and the
  unsupported-semantic-frontier representation are unchanged.

## Scalable multi-unit partial-program representation invariant

After §1-§4, the canonical one-shot Sonic route passes the SEG-007-T181
`discovery_budget_exhausted` wall and reaches the experimentally predicted
`JSR (d8,PC,Xn)` PC-relative indexed indirect-call frontier
(`reached_unresolved_direct_edge`).
The large stitched multi-unit static prefix (~6e2 reconstructed blocks over
~1.6e4 aggregate blocks / ~3e4 aggregate instructions and ~8e3 stitched edges)
is retained through two layered representation corrections:

### §7 Cross-unit convergence-point block splitting (landed, SEG-007-T182)

`discover_m68k_general_startup` promotes to a block leader, before
`build_analysis` runs, every decoded instruction that is either (a) the
straight-line fallthrough successor of two or more distinct decoded instructions,
or (b) simultaneously a straight-line fallthrough successor and a direct
control-transfer edge target. Independently-walked stitched units (offline
admitted, synthesized resolved-control-target, synthesized fallthrough-
continuation, reset / IRQ6 roots) that converge onto a common straight-line tail
no single unit registered as a leader would otherwise reconstruct two retained
blocks sharing every instruction from the convergence address on, and
`runtime_frontier_eligible`'s per-block retained-instruction uniqueness guard
would reject the whole prefix. The split set is derived only from the
already-deterministic merged decode map (`decode_order`) and merged edge list, so
it is traversal-order independent. Identical overlapping decode is still
deduplicated as agreement by `merge_root_result` / `decoded_matches`; a genuine
decode / edge / target-set / completion conflict still sets `aggregation_conflict`
-> `startup_graph_mismatch`. No discovery / block / seed / round /
`m68k_discovery_max_frontier_exits` ceiling is consulted or raised.

Result: `build_analysis` reconstructs a ~594-block / ~1575-instruction prefix
whose retained blocks partition the instruction space with zero shared
instruction address (verified: `dup_instr = 0` over 595 blocks). The raw /
empty-inventory route stays byte-identical — on a single-unit walk no address has
two straight-line fallthrough predecessors, and a branch/loop target that is also
a fallthrough target is already a leader, so the promoted set is empty.

### §8 Large multi-unit prefix retention vs bounded frontier reporting

With the uniqueness guard satisfied the prefix still does not promote to an
emittable `FrontendPartialProgram`. `build_analysis(completed_blocks_only=true,
exclude_frontier_instruction=true)` iteratively erases every reconstructed block
that has an outgoing edge whose target is neither a retained entry nor a member
of the small `candidate_frontier_addresses` set (the single primary frontier plus
best-effort siblings). A large stitched multi-unit graph legitimately contains
hundreds of partition-boundary edges — `direct_branch` / `direct_call` /
`return_to_continuation` edges to addresses that are validated and decoded in the
aggregate graph but are not part of the one connected completed prefix. Each such
edge erases its source block, the erase cascades along the straight-line spine,
and the reset ingress block itself is removed (`ingress_retained = 0`), so
`runtime_frontier_eligible` fails at its `retained_entries` / ingress and
reachability checks. Independently, `recompiler_sort_dedup_and_bound_frontiers`
fails closed (returns nothing, forcing a plain rejection) once the boundary count
exceeds `m68k_discovery_max_frontier_exits`, rather than deterministically
truncating diagnostic reporting while retaining the executable obligations.

This is the exact fail-closed architecture fork SEG-007-T182's disposition
anticipated: promoting a large stitched program fundamentally requires a new
partial-program representation contract that (1) separates unbounded semantic
partition-boundary retention from bounded diagnostic frontier *reporting*; (2)
lets `build_analysis` / `runtime_frontier_eligible` retain a block whose outgoing
edge targets any validated partition boundary, not only the small primary/sibling
set; (3) changes `recompiler_sort_dedup_and_bound_frontiers` from fail-closed to
deterministic-truncate-with-residual-obligation; and (4) gives C4 a fail-closed
emission contract for an edge into a partition boundary that is neither emitted
nor in the Tier-2 emitted set. That spans the ADR-0013 / ADR-0026 emission
contracts and the meaning of `m68k_discovery_max_frontier_exits`.

**Implemented by SEG-007-T183.** `FrontendAnalysis::semantic_partition_boundary_addresses`
is a new address-only set — the union of `admitted_units`, `continuation_roots`,
`synthesized_control_roots`, `validated_code_entry_candidate_roots`,
`irq6_handler_entry`, and every Tier-1 finite indirect-target candidate — built
once discovery finishes, from already-validated facts only (no second parallel
static program). `build_analysis`'s completed-blocks-only erase pass and
`runtime_frontier_eligible`'s per-edge validity check both additionally accept a
target that is a member of this set, alongside the existing retained-entry /
`candidate_frontier_addresses` allowances. `recompiler_sort_dedup_and_project_frontiers`
(`include/segarecomp/recompiler/frontend.hpp`) replaces
`recompiler_sort_dedup_and_bound_frontiers` at this call site: it returns the
complete, untruncated, deduplicated obligation set (what C4 emission actually
iterates) alongside a deterministic bounded projection and residual count for
diagnostics; `m68k_discovery_max_frontier_exits` becomes a diagnostic-reporting
bound only and is not raised. C4's early edge-validity gate
(`src/codegen/c11/frontend.cpp`) additionally accepts a
`semantic_partition_boundary_addresses` member at that one structural
pre-check; the real fail-closed decision remains the later per-terminal /
per-indirect-candidate checks gating on the final `EmittedCodeAddressSet` /
frontier-stop names, so a partition-boundary target absent from both still
fails closed through the existing normalized
`genesis_internal_dispatch_inconsistency_stop` runtime sentinel — never a
silent drop or a fabricated success.

Making retention this permissive exposed three narrower, pre-existing
representation gaps that a small single-unit prefix never exercised, all
corrected inside `build_analysis` (not inside ADR-0027/ADR-0028's own
synthesis algorithms, which are unchanged):

- A `return_to_continuation` edge is sourced at the callee's own RTS, which
  can stay retained independently of whether the specific call's own CALLER
  instruction is retained. Such an orphaned return edge is now dropped
  (mirroring the pre-existing `fallthrough_continuation` dangling-target
  guard) rather than letting `runtime_frontier_eligible`'s `has_frame` check
  reject the whole promotion over a call identity that was never admitted.
- An RTS-terminal block with zero surviving (non-orphaned) return edges has
  no static fact describing where it would transfer control; it is pruned
  during the same fixpoint rather than reaching C4's per-block completeness
  check as an unrepresentable `return_count == 0` shape.
- `build_analysis` now also runs an ADR-0014 M1b-mirroring breadth-first
  reachability pass, recomputed each fixpoint round over the current block
  set, and prunes a block that is edge-safe but reachable only through
  another block whose own predecessor chain was separately erased (a
  self-consistent but disconnected island) — the identical relation
  `runtime_frontier_eligible`'s own walk already required, now enforced
  before that later check instead of only diagnosed by it.
- A synthesized `fallthrough_continuation` edge (ADR-0027) can coincide with
  the exact same (source, target) transition an ordinary
  `fallthrough`/`direct_branch` edge, or `build_analysis`'s own block-entry-
  merge fallthrough edge, already describes. The redundant synthesized copy
  is dropped as a pure dedup (never a new admission rule) before it can
  trip the existing "valid only on a non-terminal or call terminal" C4
  completeness check.

Verified against the canonical one-shot Sonic route: the aggregate reaches
`build_analysis` block-reconstruction (~3.6e3 candidate blocks), all four
corrections above converge to ~3.3e3 retained/reachable blocks with ingress
retained, `runtime_frontier_eligible` promotes the prefix
(9 total semantic frontier obligations, well under the unchanged 64-entry
diagnostic bound, `residual_frontier_obligation_count = 0`), and C4 emission
proceeds through ~3.25e3 emitted blocks. The route's next stop is a distinct,
narrower Tier-1 (ADR-0009) aggregation question — a computed-control-transfer
terminal classified by addressing-mode inspection alone reaching C4's
per-block validation without a corresponding `M68kIndirectTargetEaSet` fact —
explicitly out of this task's Tier-1/Tier-2-touching scope; see the owning
task record's Evidence for the exact normalized frontier and successor
disposition.

### §8.1 build_analysis straight-line termination / C4 straight-line-fallthrough acceptance consistency (landed, SEG-007-T185)

§8's separation of unbounded semantic partition-boundary retention from bounded
diagnostic frontier reporting was applied to the completed-prefix erase pass's
edge-safety check, `runtime_frontier_eligible`'s per-edge validity check, and
C4's early structural edge-target gate. It was **not** applied to two sibling
decision sites in the same invariant, exposed only once §8 let the large
stitched prefix reach C4 per-terminal validation:

- `build_analysis`'s own per-entry straight-line reconstruction loop decides
  whether to terminate a straight-line run and emit a representable
  `fallthrough` edge to the next address using only `block_entries` membership
  plus the bounded diagnostic `candidate_frontier_addresses` set. A validated
  semantic partition boundary reachable only as a straight-line fallthrough
  successor (not registered as a block entry, not carrying a live Tier-1/Tier-2
  discovery-issue fact) was absorbed past instead of being treated as a clean
  block terminal.
- C4's per-terminal `is_straight_line_fallthrough` acceptance (the check
  immediately before the "C4 block lacks terminal control transfer" sentinel)
  accepted a straight-line `fallthrough`/`fallthrough_continuation` terminal
  only when its target was a member of the final emitted block set or a
  represented frontier-stop address — not when it was a validated semantic
  partition boundary, even though C4's own structural edge-target gate one pass
  earlier already accepts exactly that target class.

The net effect at large multi-unit scale: a retained straight-line block whose
fallthrough successor is a partition-boundary entry that is itself not retained
(its own body never completes) and not in the bounded diagnostic frontier list
reached C4 and was rejected whole-program for lacking a terminal control
transfer.

Once that separation reached the C4 per-terminal path deterministically, it
also exposed two further pre-existing C4-representation gaps for **already
decoder/lifter/discovery-supported** control-transfer shapes that a small
single-unit prefix never reached:

- A retained block whose terminal is a direct (statically-folded
  absolute.w / absolute.l / d16(PC)) `JMP` producing a single `direct_branch`
  static edge. The edge kind, the `jump_general` IR/effect
  (`effect.pc == direct_target`), and the target block-entry are all
  pre-existing, already-validated facts; C4's per-terminal control-transfer
  classification recognized only `general_branch` / `dbcc_loop`, so such a
  block hit the "lacks terminal control transfer" sentinel.
- The per-candidate acceptance check for a Tier-1 finite indirect-target set
  accepted a candidate only when it was a retained emitted block or a
  represented frontier stop -- not when it was a validated semantic partition
  boundary, even though every Tier-1 candidate is by construction a member of
  that set and §8 had already widened the ordinary static-edge-target
  acceptance (the rule this check's own comment claims parity with) to accept
  exactly that class.

**Correction (SEG-007-T185).** One shared predicate
`is_semantic_partition_boundary_address(analysis, address)`
(`include/segarecomp/machine/genesis/frontend.hpp`) is now consulted
identically by: `build_analysis`'s straight-line block termination, its
completed-prefix erase-pass edge-safety check, C4's per-terminal
straight-line-fallthrough acceptance, and C4's Tier-1 per-indirect-candidate
acceptance. `build_analysis` terminates a
straight-line run and emits the same representable `fallthrough` edge kind it
already emits at a block-entry merge when the ordinary PC advance lands on a
boundary member (unconditionally — boundary retention is independent of the
`exclude_frontier_instruction` diagnostic projection). C4 accepts that same
boundary target at the per-terminal check. No new edge kind, frontier class, or
parallel static program; no admitted address is added by the predicate; every
existing fail-closed check is unchanged — a straight-line successor genuinely
absent from the retained-block set, the frontier-stop set, and the boundary set
still hits the unchanged "lacks terminal control transfer" sentinel, and a
boundary target present in neither the emitted block set nor a frontier stop
still fails closed at runtime through the existing
`genesis_internal_dispatch_inconsistency_stop`. The `build_analysis`
straight-line termination site is made consistent through the same shared
predicate even though every current boundary-set constituent reachable as a
straight-line fallthrough successor is already a registered block entry, so a
future boundary-set constituent that is not independently rooted cannot
reintroduce the gap.

The direct-`JMP` terminal is classified and lowered exactly like an
unconditional `general_branch`: one `direct_branch` edge, no fallthrough,
`emit_m68k_operation_c` (already) assigns `runtime->pc` from the folded
`effect.direct_target`, and the unchanged block-tail `GENESIS_CONTINUE_AT_PC`
reaches the successor through `genesis_dispatch`. No new edge kind, no new
control-transfer mechanism, no new CPU addressing-mode/instruction form -- the
structural edge-target gate that rejects a genuinely unrepresentable target is
unchanged. The Tier-1 per-candidate check gains the same
`is_semantic_partition_boundary_address` clause the ordinary-edge rule already
carries; the emitted runtime membership guard still admits only ADR-0009's
proven finite set, so no address the finite-set proof did not establish as
legal is accepted.

Verified against the canonical one-shot Sonic route after SEG-007-T184's
Tier-1/Tier-2 aggregation fix: discovery-stage metrics unchanged
(admitted-unit and semantic-partition-boundary counts, retained-block counts
before/after pruning, `ingress_retained`, `residual_frontier_obligation_count`,
and the unchanged 64-entry diagnostic bound all identical to T184); C4 emission
now completes (~3.25e3 emitted blocks / code addresses, no rejection
sentinel), the generated C compiles as strict C11, and generated-native
execution runs and advances to a genuinely later **runtime-selected**
execution frontier (a static-discovery instruction-budget exhaustion observed
at a runtime PC) -- a different subsystem from this boundary-retention /
C4-representation family. No discovery / block / root / round / frontier
capacity ceiling is raised; `m68k_discovery_max_frontier_exits` is unchanged;
no runtime-confirmed compilation seed, checkpoint, or Phase-B promotion is
introduced. The raw/unannotated (`--external-hints`-free) route is unaffected:
`semantic_partition_boundary_addresses` is empty there, and no
previously-emitting raw-route program regresses.

### §8.2 Direct-control target preservation across pruned continuation units (landed, SEG-007-T185, 2nd reopening)

§8/§8.1's boundary-retention separation let the large stitched prefix reach C4
emission and generated-native execution. Execution then advanced past the
checksum loop (ADR-0016) to a runtime PC that a generated direct `BSR`/`JSR`
transferred to: an RTS-terminal callee entry reached from tens of retained call
sites that was **decoded, registered as a block entry, reachable, and RTS-live**,
yet absent from the emitted dispatch set
(`genesis_internal_dispatch_inconsistency_stop`).

Root cause: the completed-prefix erase pass and `runtime_frontier_eligible`'s
per-edge target invariant treat a `return_to_continuation` edge like any other
edge — its continuation target must remain a retained entry / represented
frontier / semantic-partition boundary. When a single shared caller's
continuation unit was independently pruned (its own outgoing edge unsafe), the
still-dangling `return_to_continuation` edge from the shared callee's RTS to that
now-unretained continuation forced the erase pass to drop the **callee** block to
keep the final edge set consistent — cascading one pruned continuation through
the shared callee into a program-wide collapse of that whole call tree, taking
every *other* caller's call site down with it. The earlier fa27eecc sub-fix
(erase-pass reachability guard + C4 emitted-set seeding from direct-control edge
targets) could not reach this: `blocks.contains(T)` was already false by C4 time
because the callee was erased upstream in `build_analysis`.

**Correction (SEG-007-T185, 2nd reopening).** A `return_to_continuation` edge
whose continuation is not representable (not a retained entry, not a bounded
diagnostic frontier address, not a semantic-partition boundary) is a **dead
return sub-path**, not an unsafe outgoing edge and not an orphan-RTS trigger:

- The final `discovered_edges` rebuild drops it, mirroring the existing
  `fallthrough_continuation` retained-target guard and the adjacent
  `!source_retained(edge.call->caller)` drop exactly. `prefix.static_edges` then
  never presents `runtime_frontier_eligible` with a dangling return target.
- The completed-prefix erase pass's outgoing-edge-safety check no longer counts
  such an edge as unsafe, so a shared callee retaining **at least one**
  representable return path survives for all its other callers.
- The erase pass's orphan-RTS `has_live_return_edge` gate gains the same
  continuation-representable clause, so a callee with **zero** representable
  return paths is still pruned early and consistently (never reaches C4 as an
  RTS-terminal block with no return edge).

All three sites share the same representability test already used by §8's
ordinary-edge rule (`retained entry ∪ candidate_frontier_addresses ∪
is_semantic_partition_boundary_address`). No new edge kind, frontier class,
admitted address, block kind, or parallel "direct-call roots" program; no
call→jump lowering; no capacity ceiling raised. Gated to the multi-unit offline
route (`!semantic_partition_boundary_addresses.empty()`) — the raw single-unit
route never prunes continuation units and stays byte-identical (raw route SHA-256
`e836455233a4319353ab1d9cf24bf0acedf4d2f616869cf3e9e5b44909f20f9f`, unchanged).

This completes the boundary-retention-vs-representation invariant for
direct-control targets: once segarecomp has independently validated an exact
direct executable destination, that destination's executability does not depend
on any other caller's continuation surviving, nor on Ghidra independently
proposing the same address. Every such validated direct target resolves to
exactly one of: an emitted/retained block entry; a valid mid-block non-dispatch
address; a supported Tier-2 path; or an explicit represented semantic frontier —
never silently absent.

Verified against the canonical one-shot Sonic route: `emitted_block_count`
3294 → 3311, `retained_block_count_after_pruning` 3332 → 3338; ADR-0016
checksum-loop proof still present and the checksum loop still passes;
`generation_attempts = 1`, `compile_attempts = 1`,
`runtime_confirmed_seed_count = 0`; no checkpoint, no Phase-B, no ceiling raised.
Generated-native execution now passes the interior-address
`internal_dispatch_inconsistency` stop and advances to a genuinely different
subsystem frontier — a runtime-selected **indirect/computed control transfer**
(`JMP (An)`) whose Tier-2 computed target is not emitted
(`unresolved_indirect_target` / `tier2_computed_target_not_emitted`).

### §8.3 Semantic boundary membership is not executable representation (landed, SEG-007-T225)

**Follow-on correction; historical record preserved.** SEG-007-T183 and
SEG-007-T185 correctly established that
`FrontendAnalysis::semantic_partition_boundary_addresses` is sufficient for
static graph retention, partition/block termination, static-edge validity, and
bounded diagnostic projection. Their C4 acceptance behavior nevertheless
treated bare boundary membership as if it could also stand in for generated
representation. T225 demonstrated the incomplete contract: generated execution
could assign such an unrepresented target to PC and arrive at
`genesis_internal_dispatch_inconsistency_stop`.

`semantic_partition_boundary_addresses` proves only that an address is a
validated static partition/reachability boundary. It does **not** by itself
prove that generated C has a legal dispatch representation. For C4 emission,
every potentially dispatched target must resolve to exactly one established
representation:

1. an existing retained block, which has a generated block body and dispatcher
   arm;
2. an explicit typed frontier stop already represented in the generated
   program; or
3. another already-established dispatch mechanism whose contract explicitly
   owns that target.

A bare semantic-partition-boundary member is not a fourth representation
class. A boundary that is also an existing retained C4 block is already fully
represented; T225 registers that existing block in the existing emitted
dispatcher when entry-disconnected reachability had omitted its arm. This is
neither new code discovery nor a fabricated body.

When a C4 static edge or finite indirect-target candidate reaches a boundary
with none of these representations, generation rejects fail-closed. Generated
C must not knowingly assign that address to PC and rely on
`genesis_internal_dispatch_inconsistency_stop` later. The sentinel remains a
defensive runtime invariant for unexpected internal inconsistencies, but is no
longer the intended normal representation of a known statically unrepresented
semantic boundary.

This **supersedes only the corresponding T183/T185 C4 acceptance behavior** in
§8 and §8.1: the older structural/per-terminal acceptance and runtime-sentinel
outcome are retained above as historical evidence, not current policy. It does
not undo the static-analysis rationale for boundary membership in graph
retention, partition/block termination, static-edge validity, or bounded
diagnostic projection. Static reachability validity and generated
dispatchability are related but distinct contracts.

The remaining ownership question is deliberately not decided here: a validated,
reachable/obligated boundary with neither a retained generated block nor a typed
frontier-stop representation requires full refinement to choose a compatible
one-program/static-recompilation mechanism.

### §9 Authoritative exact direct-control target closure (drafted, `backlog/seg-007-refine`, consuming SEG-007-T213's `needs_full_refinement` handoff; implementation reserved for successor SEG-007-T214)

§8/§8.1/§8.2 established that `semantic_partition_boundary_addresses`
membership is sufficient for a target to pass the completed-prefix erase
pass's and C4's *structural* edge-target gates, and that a block whose own
ENTRY is the target of a retained direct-control edge (`direct_call`/BSR,
`direct_branch`, or a live `return_to_continuation`) is protected from the
erase pass's **reachability**-based prune (§8.2's `direct_control_target_entries`
guard). SEG-007-T212/T213 (full diagnostic history in their own records, not
repeated here) proved this protection is incomplete: the runtime-selected
`internal_dispatch_inconsistency` frontier at a retained direct-BSR target
(three independent exact edges, confirmed reached by an admitted offline unit
whose own independent Phase-2 root walk completes cleanly and merges into the
decoded/block-entry aggregate without conflict, per T213's Evidence) still
ends emission with **zero** emitted block, **zero** frontier-stop
representation, and **zero** other supported dispatch representation. A
bounded audit of the full generated program (T213 Evidence) found this is
generic, not target-specific: of `2475` unique exact direct-control targets,
`149` are unrepresented — no emitted block, no explicit frontier, no other
supported dispatch.

**Measured root cause for the disclosed target (bounded local
instrumentation, `backlog/seg-007-refine` second tightening pass).** A
temporary, fully-reverted stderr diagnostic against the completed-prefix
erase pass (`src/machine/genesis/frontend.cpp`) measured, for the
already-disclosed target: it is present in the merged decode map (not a
validation failure), it is correctly collected by
`direct_control_target_entries` (§8.2's reachability-prune guard is
working as intended), and it was **never a block entry at any point** —
not before the erase pass's fixpoint begins, not after. None of the erase
pass's three sibling prune conditions (orphan-RTS, unproven-computed-
control, outgoing-edge-safety) ever evaluated it, because a block whose
entry equals this address never existed in `prefix.static_blocks` in the
first place. The classification is `never_became_block_entry`: §9.2
(block-entry promotion/splitting), not a materialize-then-reprune cycle,
is the mechanism that resolves this specific target. §9 nonetheless also
documents the materialize-then-reprune case (§9.1, §9.5) for the distinct
sub-population of the 149 unrepresented targets that DO already have a
block entry and are pruned for a legitimate, unrelated reason — both
cases are real and this closure mechanism must handle both, but only the
first is what fixes the disclosed address.

Reading `build_analysis`'s completed-prefix erase pass against this
evidence localizes the exact gap two ways: (a) the §7 cross-unit
convergence-point leader-promotion predicate only promotes an address to
a block entry when it is ALSO the straight-line fallthrough successor of
some decoded instruction; a target reached ONLY via an exact
direct-control edge from an outside unit, with no adjacent fallthrough
predecessor, is never considered by that predicate at all — this is the
`never_became_block_entry` gap §9.2 closes; and (b) independently, even a
target that DOES successfully become a block entry (whether via ordinary
per-root discovery or via §9.2's promotion) can still be pruned by one of
the erase pass's three OTHER conditions, which are not aware of
direct-control target ownership and can erase such a block outright with
no fallback representation ever created — this is the
materialize-then-reprune gap §9.1/§9.5 close. Both gaps produce the same
observable defect: **a retained emitted exact edge whose target has no
representation of any kind.**

This is the same trust/target-preservation family §8.2 already established
(admitted-unit or Tier-1-candidate status is untrusted-until-corroborated;
retained first-party exact control flow is authoritative), extended one
layer further: §8.2 preserved a boundary-member TARGET from erasure by
UNREACHABILITY once it already had a block entry; §9 additionally
guarantees every obligated target REACHES a block entry in the first
place (§9.2), and that if it is subsequently and legitimately pruned for
an unrelated body reason, the target address does not silently vanish
from the final program — it must receive an ACTUAL emitted typed
frontier-stop representation, never merely a structural analysis-time
fact (§9.6).

**This is a direct extension of ADR-0028, not a new ADR.** §1-§8.2 already
define the authoritative evidence class (a validated first-party exact
direct-control edge), the existing bounded materialization mechanism
(`M68kStaticGraphWalker`'s per-root walk, reused unchanged by §1's
resolved-control-target synthesis), the boundary-membership vocabulary
(`semantic_partition_boundary_addresses`), the existing typed frontier
representation (`UnresolvedFrontier`/`GenesisFrontierClass`), and the
existing fail-closed runtime sentinel
(`genesis_internal_dispatch_inconsistency_stop`). §9 closes the remaining
seam: every obligated target must reach a block entry or an actual typed
frontier, never merely pass a structural gate that a later prune (or a
promotion predicate that never even considers it) can silently defeat.

#### §9.1 Authoritative closure obligation

**What evidence creates the obligation.** A retained exact direct-control edge
`S -> T` (`direct_branch`, `direct_call`/BSR, or an unconditional direct
`JMP`/`JSR` folded to an absolute/PC-relative-displacement target — the same
edge kinds §8.2's `direct_control_target_entries` already collects) whose
source instruction `S` is a member of the FINAL retained instruction set (the
erase-pass fixpoint's last round) creates a closure obligation on `T`. This is
independent of `T`'s own offline-candidate identity (present, admitted, or
rejected) — exactly ADR-0026 §7's preserved invariant that authoritative
reachability promotion is never granted merely because an address coincides
with an offline proposal, restated here for the exact-edge evidence class:
authoritative status comes from the retained edge, never from candidate
identity. **This obligation is unconditional and is NOT gated on offline
-inventory/`semantic_partition_boundary_addresses` being non-empty** — see
§9.7 for the corrected raw-route invariant.

**When the obligation is evaluated.** After the INITIAL, ordinary
completed-prefix erase-pass fixpoint (§8/§8.2's `do { ... } while (changed)`
loop, run exactly once, unchanged from today's pre-existing behavior)
reaches its own fixed point — i.e. once no further block is removed for
reachability, orphan-RTS, unproven-computed-control, or outgoing-edge-safety
reasons under the CURRENT (pre-closure) retained set. This is deliberately
the same point §8.2 already finalizes `direct_control_target_entries` and
rebuilds `prefix.static_edges`: closure operates on that INITIAL erase
pass's stable output, not on an intermediate round.

**Two-stage pipeline (this section corrects an earlier draft that
described closure as merely "re-running the erase-pass fixpoint," which
cannot actually change which addresses are block leaders — leader
promotion, per §9.2, must run BEFORE `build_analysis` reconstructs
blocks, exactly where §7's existing predicate already runs, per
ADR-0028's existing ordering; promoting `T` into the leader set without
then reconstructing `build_analysis` would never produce an actual block
entry at `T`):

```text
merged decoded/edge facts
       |
initial build_analysis + erase fixpoint   (unchanged from today)
       |
identify exact edges whose SOURCES survived
       |
authoritative obligations T
       |
for each unsatisfied T:
  - already decoded, non-entry -> add T to the leader set (§9.2)
    [no new M68kStaticGraphWalker root; no synthesized-unit-ceiling
     consumption -- §9.3]
  - no decoded facts at all -> walk T with M68kStaticGraphWalker (§1)
    [consumes the shared synthesized-unit ceiling -- §9.3]
  - cannot be validated -> typed-frontier fallback immediately
       |
REBUILD build_analysis once, from the augmented leader set / merged units
       |
erase fixpoint again, once, over the rebuilt block set
       |
for each T: executable block entry, OR actual typed frontier
  (materialize-then-reprune case), OR fail closed
```

**What representation satisfies it (see §9.6 for the exact vocabulary).**
For every obligated `T`, exactly one of:

- `T` is the entry of a block in the FINAL retained/emitted set (ordinary
  case — already true for most boundary members; no action needed), OR
- `T` has an ACTUAL emitted typed frontier-stop representation
  (`UnresolvedFrontier` with a valid `GenesisFrontierClass` and diagnostic
  provenance — NOT merely structural `candidate_frontier_addresses` set
  membership, which is an intermediate analysis-time fact, never itself a
  final representation; §9.6), OR
- `T` is covered by another already-documented supported dispatch
  representation (currently none beyond the two above for this edge
  family; reserved for a future dispatch class without redefining this
  obligation), OR
- `T` is closed by the bounded materialization/promotion procedure below
  (§9.2), which itself resolves to one of the above.

**What bounded mechanism attempts materialization.** For an obligated `T`
not yet satisfying the above: if `T` is already present in the merged
decode map (`decoded_by_address`) but never became a block entry, attempt
§9.2's block-entry promotion/splitting FIRST — this is the measured
primary case for the disclosed `0x00005ebe` target and is expected to be
the common case, since a target reached only via an exact direct-control
edge (no adjacent fallthrough predecessor) is exactly what §7's existing
leader-promotion predicate never considers. Otherwise, reuse
`M68kStaticGraphWalker` exactly as §1 already does for resolved-control-
target synthesis: walk `T` as an independent unit against the current
`independent_unit_boundaries` (the same 4th-arg boundary construction
§1/§3 already assemble, unioned with `synthesized_control_roots` and the
final admitted set — no new boundary rule). Validation, alignment,
mapping, mid-instruction rejection, and decode-conflict checks are the
SAME unchanged checks every other root walk already uses
(`validate_branch_target`, `admit_target`,
`merge_root_result`/`decoded_matches`/`aggregation_conflict`).

- If promotion (§9.2) adds `T` to the leader set, OR a fresh walk of `T`
  produces a well-formed unit (no `aggregation_conflict`, clean decode),
  `T` is queued into the augmented leader set / merged-units input that
  feeds the SINGLE downstream rebuild: a fresh-walk unit is merged exactly
  like §1's synthesized resolved-control-target unit — added to
  `synthesized_control_roots`, unioned into
  `semantic_partition_boundary_addresses`, its blocks appended to
  `decoded_by_address`/`decode_order`/`prefix.static_edges` through the
  UNCHANGED merge path; a promotion is added to the block-leader set only
  (no new decode, no new edges — §9.2).
- **After EVERY obligated `T` has been processed this way** (not per-`T`):
  `build_analysis` is RECONSTRUCTED ONCE from the augmented leader set and
  merged decoded-and-edge facts, and the completed-prefix erase-pass
  fixpoint is then run ONCE MORE over that rebuilt block set (bounded — see
  §9.3). This single rebuild-and-reprune cycle is what actually produces a
  block entry starting at a promoted `T` — a leader-set change alone,
  without reconstructing `build_analysis`, would never do so. For each `T`
  (§9.3 — closure-processed at most once, no retry loop):
  - if `T` survives as a retained block entry after this rebuild-and
    -reprune cycle, the executable representation satisfies closure;
  - if `T` is pruned (again, for a target that already had a block entry
    before promotion/materialization, or for the first time, for a target
    that has one only now) by one of the erase pass's existing, unrelated,
    legitimate prune conditions (reachability no longer applies once `T`
    is a `direct_control_target_entries` member, but orphan-RTS,
    unproven-computed-control, or outgoing-edge-safety still can), this is
    the materialize-then-reprune case: construct the ACTUAL typed frontier
    representation described below, never attempt a second rebuild cycle
    for that `T`.
- If `T` cannot be validated in the first place (unmapped, misaligned,
  mid-instruction, or a genuine `aggregation_conflict` against
  already-decoded state), materialization fails safely BEFORE the rebuild,
  immediately producing the same typed-frontier case for that `T` (it is
  never queued into the rebuild input).

**What happens on safe materialization failure or a legitimate
re-prune.** `T` receives an ACTUAL emitted typed frontier-stop
representation: a real `UnresolvedFrontier` record with a valid
`GenesisFrontierClass` (reusing the existing `known_but_unemitted_target`
class, or another already-documented class if more precise for the
measured cause) and valid diagnostic provenance, constructed through the
existing frontier-emission machinery — never merely inserting `T` into
the structural `candidate_frontier_addresses` set, which gates only
analysis-time edge-safety checks and is never itself a final dispatch
representation (§9.6). This keeps `S`'s edge structurally valid without
fabricating an executable body for a target that cannot be, or is no
longer, validly executable.

**What causes fail-closed rejection.** If `T` can be neither
materialized/promoted (§9.2) nor given an actual typed frontier
representation (the frontier machinery itself cannot construct a valid
record — e.g. the diagnostic-bound accounting in §9.3 is exhausted, or no
valid `GenesisFrontierClass`/provenance can be established for `T`),
generation fails closed with a normalized
`authoritative_exact_target_closure_exhausted` diagnostic (no ROM address,
no Sonic-specific identifier) — never a silently emitted dangling edge.

#### §9.2 Block-entry promotion / splitting for an already-decoded, non-entry target

**This is the PRIMARY closure mechanism, confirmed by direct measurement
against the disclosed `0x00005ebe` target** (see the root-cause paragraph
above): the address is decoded but was never a block entry, because §7's
existing leader-promotion predicate only promotes an address that is ALSO
the straight-line fallthrough successor of some decoded instruction — a
target reached ONLY by an exact direct-control edge from an outside unit,
with no adjacent fallthrough predecessor, is never considered by that
predicate at all.

When `T` is already present in `decoded_by_address` (another unit's walk
passed through `T`, whether as a mid-sequence instruction or as an
admitted unit's own root that simply never got promoted to a leader),
closure MUST NOT re-decode or duplicate `T`'s instruction: this is exactly
the convergence case §7's cross-unit block-splitting already solves for
straight-line fallthrough convergence, generalized here to a target
reached by an exact direct-control edge with no fallthrough predecessor
requirement.

- If `T` already satisfies §7's existing leader-promotion rule
  (fallthrough successor of ≥2 distinct predecessors, or simultaneously a
  fallthrough successor and a control-transfer target) it is ALREADY a
  leader and therefore already a block entry — no new action, this is the
  pre-existing case §7 covers.
- §9 extends §7's leader-promotion predicate with one additional
  membership test, evaluated at the same point (before `build_analysis`
  runs, over the same deterministic merged `decode_order`/edge-list, so it
  stays traversal-order independent): an address that is the target of an
  obligated exact direct-control edge (§9.1) is ALSO promoted to a block
  leader, WITHOUT requiring any fallthrough predecessor at all (this is
  the corrected, strictly weaker precondition than §7's own rule — §7
  requires a fallthrough predecessor as a prerequisite before even
  checking control-edge-target membership; §9's extension drops that
  prerequisite for an obligated exact direct-control target). `build_
  analysis` then reconstructs `T`'s existing decoded instructions as a NEW
  block starting at `T` and SPLITS the containing unit's prior block at
  that boundary if `T` falls inside one (identical splitting mechanism to
  §7 — a leader-set membership change, not a new splitting algorithm); if
  `T` is itself an admitted unit's own root address that was decoded but
  never promoted, this degenerates to simply adding `T` to the leader set
  with no split needed.
  No duplicate ownership: the split block and the original block partition
  the SAME already-decoded instruction range with zero shared addresses,
  exactly as §7's `dup_instr = 0` invariant already verifies.
- This promotion is gated identically to §7/§8 (empty on the raw
  single-unit route — no boundary-obligated target exists without offline
  hints, because the raw route's entry-rooted walk already retains every
  direct-control target it reaches; §9.7).

#### §9.3 Bounded deterministic termination

Closure operates over a monotonically growing PROCESSED-target set
(`authoritative_exact_targets_processed`), seeded from every obligated `T`
present at the erase pass's fixpoint. Each candidate target is closure-processed
AT MOST ONCE — promotion/materialization is attempted once, the erase pass
is re-run once more to observe whether `T` survives, and the outcome
(executable or typed frontier) is then final for `T`; there is no retry
loop that rematerializes the same target after a re-prune:

- Promoting/materializing `T` (§9.1/§9.2) can expose new exact
  direct-control edges from `T`'s own newly-merged instructions; only
  NEWLY-discovered, not-yet-processed targets are appended to the
  obligation set (identical accumulation pattern to §1/§3's
  `synthesized_control_roots` fixed point already uses for
  ceiling-triggered synthesis).
- `T`'s own per-root walk (when a fresh walk, not a promotion, is needed)
  remains bounded by the UNCHANGED per-root
  `m68k_discovery_max_instructions` ceiling (§1's existing constraint,
  unchanged, not raised).
- **Ceiling scope, corrected.** ONLY a FRESHLY-WALKED closure unit (§9.1's
  "otherwise" branch — `T` has no decoded facts at all and requires a new
  `M68kStaticGraphWalker` root) is an ADDITIVE CONSUMER of the EXISTING
  shared synthesized-unit COUNT ceiling `m68k_fallthrough_continuation_
  unit_ceiling` (§4's pre-existing shared budget over continuation +
  resolved-control-target + freshly-walked §9 closure units) — this is
  not a separately-numbered "closure round" ceiling, and the numeric
  value is unchanged (not raised). A §9.2 leader-set PROMOTION of an
  already-decoded target consumes NO synthesized-unit-ceiling budget: it
  invokes no `M68kStaticGraphWalker` root, synthesizes no new
  independently-discovered unit, and only changes which already-decoded
  address is a block leader. Charging a promotion against the shared
  ceiling would risk an artificial `authoritative_exact_target_closure_
  exhausted` failure if many of the ROM's unrepresented-target population
  share `0x00005ebe`'s `never_became_block_entry` shape (T213's audit
  found 149 such candidates in one build), even though none of them need
  a fresh discovery root. Exhausting the shared ceiling via freshly-walked
  units is the one path to the
  `authoritative_exact_target_closure_exhausted` fail-closed diagnostic in
  §9.1 that is bounded by this ceiling; a promotion-only closure population
  can never trigger it via ceiling exhaustion.
- `authoritative_exact_closure_rounds` (Scope item 7) is a
  DIAGNOSTIC-ONLY metric describing whether closure needed its one
  rebuild-and-reprune cycle; it is never itself a cap or bound.

Termination follows from: the mapped cartridge image is finite; canonical
target addresses are therefore finite; each target is processed at most
once via exactly one rebuild-and-reprune cycle (this section); every
per-root walk closure invokes is itself already independently bounded
(§1's ceiling, unchanged); the shared synthesized-unit count ceiling
bounds the total number of FRESHLY-WALKED closure units; and the
promoted-leader population is separately, trivially bounded by the finite
obligation set itself (a target can be promoted at most once, and the
obligation set is finite by construction).

#### §9.4 Final generation-time invariant (backstop verifier)

Independent of where closure lands in the pipeline, a final check before
native emission is accepted verifies: for every retained exact
direct-control edge `S -> T` whose source `S` is selected for native
emission, `T` is (1) a member of the final `EmittedCodeAddressSet`
(executable block entry), (2) the subject of an ACTUAL emitted typed
frontier-stop representation with valid diagnostic provenance (NOT merely
structural `candidate_frontier_addresses` / `semantic_partition_boundary_
addresses` membership or bare decoded presence — §9.6), or (3) covered by
another documented supported-dispatch representation. If none of the
three holds, generation is REJECTED before native output is produced — a
build-time diagnostic, not a runtime sentinel;
`genesis_internal_dispatch_inconsistency_stop` remains the UNCHANGED
runtime backstop for any edge shape §9 does not cover (e.g. an
indirect/computed transfer, already out of this amendment's scope).
§9.1-§9.3 are the primary repair mechanism; §9.4 is strictly a backstop
that must never itself need to fire once §9.1-§9.3 are correctly
implemented.

#### §9.5 Preserved invariants (restated for this evidence class)

No runtime opcode decoding, no interpreter/JIT fallback, no Sonic-specific
target whitelist or hardcoded address, no promotion of a rejected offline
candidate merely because Ghidra proposed it (ADR-0026 §7 unchanged), no
logical-table/descriptor change (ADR-0032/ADR-0033 unchanged), no capacity
ceiling widened as the fix, no speculative executable block for an
undecodable/misaligned/conflicting target (§9.1's failure path always falls
back to an actual typed frontier, never to a fabricated block). Authoritative
direct reachability proves only that `T` NEEDS a representation; it never
bypasses mapping validation, instruction alignment, decode validity,
provenance consistency, or CFG consistency — a target those checks reject
receives an actual typed frontier, not an executable body.

#### §9.6 Final-representation vocabulary (structural facts vs. actual representations)

This section resolves an overstatement in this ADR section's original
draft, corrected after independent review: structural set membership is
NOT itself a final dispatch representation. The closure obligation (§9.1)
and the final verifier (§9.4) are satisfied ONLY by one of:

1. a final emitted executable block entry (the target is a member of the
   `EmittedCodeAddressSet` C4 actually emits code for);
2. an actual emitted typed frontier-stop representation — a real
   `UnresolvedFrontier` record (`include/segarecomp/machine/genesis/
   frontend.hpp`) with a valid `GenesisFrontierClass` and diagnostic
   provenance, constructed through the existing frontier-emission
   machinery;
3. another explicitly documented supported-dispatch representation.

They are NEVER satisfied by, alone:

- `candidate_frontier_addresses` set membership (this is a bounded,
  internal, analysis-time structural fact that gates the completed-prefix
  erase pass's and C4's structural edge-safety checks — it does not, by
  itself, cause any `UnresolvedFrontier` record to be emitted for that
  address, and prior to this amendment nothing guaranteed one existed);
- `semantic_partition_boundary_addresses` membership (an analysis-time
  boundary-construction fact, not an emission fact);
- bare presence in `decoded_by_address` (decoding is a precondition for
  representation, not itself a representation).

`candidate_frontier_addresses`/`semantic_partition_boundary_addresses`
remain exactly as before for their existing internal structural-gating
purposes; §9 only forbids treating that structural membership as
satisfying the closure obligation or the final verifier.

#### §9.7 Raw/no-inventory route invariant (corrected)

The authoritative closure obligation (§9.1) and the §9.4 final verifier
are conceptually active on EVERY route, including the raw/unannotated
single-unit route with no offline/Ghidra inventory — authoritative status
comes from the retained first-party exact edge, never from offline-
candidate identity or inventory presence (§9.1, restating ADR-0026 §7's
preserved invariant for this evidence class). The obligation extraction,
the closure loop, and the final verifier must NOT be implemented as gated
on offline-inventory/`semantic_partition_boundary_addresses` being
non-empty.

On the raw route, the pre-existing entry-rooted walk already retains
every direct-control target it reaches (this is an existing, unchanged
property of that route, not a new claim). Consequently every retained
exact target on that route is ALREADY represented (case 1 of §9.6) before
closure runs, the obligation set that actually needs materialization is
empty, and closure performs zero materializations — the byte-identical
result on the raw route is a MEASURED CONSEQUENCE of nothing needing
materialization, not a coded precondition that disables the invariant
itself. If a raw-route build were ever to contain a dangling retained
exact target (not currently observed, and not expected given the
entry-rooted walk's existing property), the SAME closure/frontier/
fail-closed invariant would apply exactly as on the offline-inventory
route.

## Consequences

The offline-inventory-aware static-discovery partition model has five
mechanisms. Resolved-control-target unit synthesis (§1-§6) plus cross-unit
convergence-point block splitting (§7) remove the T181 reset-root ceiling wall
and the per-block uniqueness collapse deterministically and without any budget
raise. §8, landed by SEG-007-T183, separates semantic partition-boundary
retention from bounded frontier reporting and corrects the three narrower
retention/reachability gaps that separation exposed, letting the large
stitched prefix reach C4 emission. §8.1, landed by SEG-007-T185, extends the
same boundary-membership predicate to the remaining C4/`build_analysis`
decision sites that still consulted only the narrower `block_entries` /
`candidate_frontier_addresses` / retained-block sets, and classifies/lowers a
direct absolute `JMP` block terminal like an unconditional `general_branch`.
SEG-007-T184 separately resolved the Tier-1/Tier-2 `M68kIndirectTargetEaSet`
aggregation-consistency question. §8.2, landed by SEG-007-T185's 2nd reopening,
preserves a validated direct-control callee across the independent pruning of any
one caller's continuation unit by treating an unrepresentable
`return_to_continuation` continuation as a dead return sub-path (dropped from the
edge set) rather than as a reason to erase the shared callee. With those in place
the canonical one-shot Sonic route emits strict-C11 generated C, compiles it, and
runs generated-native past the interior-address dispatch stop to a genuinely
different subsystem frontier: a runtime-selected indirect/computed control
transfer (`JMP (An)`) whose Tier-2 computed target is not emitted.

§9, implemented by SEG-007-T214, closes the remaining exact-direct-control
representation gap that §8.2 left open: a 149-of-2475 unrepresented-target
audit against the canonical Sonic build proved a retained exact direct edge's
target could still end emission with zero block, zero frontier, and zero
other dispatch representation. Bounded local measurement classified the
disclosed target's cause as `never_became_block_entry` — §7's existing
leader-promotion predicate never considers a target reached only by an exact
direct-control edge with no adjacent fallthrough predecessor — resolved by
§9.2's extended leader-promotion predicate, applied before a single
`build_analysis` rebuild and a single erase-pass re-run over the augmented
leader set. §9.1/§9.5 separately preserve the materialize-then-reprune case
for a target that reaches a block entry but is legitimately pruned again for
an unrelated body reason, and §9.6 corrects an earlier draft's overstatement
by fixing the three-way final-representation vocabulary: only a final
emitted executable block entry, an actual emitted typed frontier-stop
record, or another documented supported-dispatch representation satisfies
the obligation — never bare structural `candidate_frontier_addresses` /
`semantic_partition_boundary_addresses` membership or decoded-instruction
presence alone. §9.4's final generation-time verifier is the backstop that
enforces this invariant across every route, including the raw/no-inventory
route (§9.7), independent of offline-inventory state.
