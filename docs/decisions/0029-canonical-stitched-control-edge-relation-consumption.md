# ADR-0029: Canonical validated stitched control-edge relation is the sole post-stitch control-transfer reachability authority

- Status: Accepted
- Date: 2026-09-08
- Deciders: SEG-007-T188 (architecture-class task), consuming SEG-007-T187's
  final corrected evidence and its
  `EXPERIMENT_RESTORES_REACHABILITY_BUT_CASE_C_REMAINS` shadow experiment, which
  selected "parity fix first; context sensitivity explicitly NOT preselected".
- Amends: ADR-0009 (computed indirect control-flow target resolution). The
  ADR-0009 fixed-point merge semantics (commutative, idempotent, monotone toward
  `unknown`, finite + unknown = unknown), the 256-member finite-set cap, and the
  A7 finite-proof exclusion are all unchanged.
- Related: ADR-0011 (bare-address worklist, non-recursive call/return),
  ADR-0026 (offline inventory partitions into stitched units),
  ADR-0027 (fallthrough-continuation partition),
  ADR-0028 (ceiling-triggered resolved-control-target unit synthesis).
  Weakens none of them.

## Context

Two post-stitch consumers independently reconstructed a partial control-flow
graph from a per-instruction successor helper (`m68k_value_flow_successors`):

1. `m68k_select_stitched_analysis_roots` (SEG-007-T186 root selection over the
   validated stitched value-flow graph); and
2. the ADR-0009 finite forward register-value fixed point
   (`M68kStaticGraphWalker::analyze_finite_register_values` /
   `analyze_finite_index_values`).

That helper only re-derived control transfers it could recompute locally from a
single decoded instruction: direct branches, foldable `JMP`/`JSR` targets,
`BSR` displacements. It emitted **nothing** for a non-foldable computed control
EA. Meanwhile canonical generation/stitching already statically proves and
represents finite `pc_index8` indexed PC-relative and pure `(An)`
register-indirect control target sets as ordinary `M68kStaticEdge`s
(`indirect_branch` / `indirect_call`, one per proven candidate), aggregated as
`merged_edges` -> `FrontendAnalysis::static_edges`, with the multi-candidate set
recovered from the source instruction's own retained `M68kIndirectTargetEaSet`.

The gap (`canonical_stitched_cfg_vs_value_flow_successor_parity_gap`): a proven
finite indexed/`(An)` indirect-control edge was canonical yet invisible to the
two consumers, so code reachable **only** through such an edge received no
reaching register state and could not itself be analyzed or covered by root
selection.

## Decision

1. **Single authority.** The aggregated `std::vector<M68kStaticEdge>` relation
   (plus each source instruction's retained `M68kIndirectTargetEaSet`) is the
   sole post-stitch control-transfer reachability authority. Both consumers bind
   to it through one shared adjacency builder,
   `m68k_canonical_control_adjacency(entries, canonical_edges)`.

2. **`m68k_value_flow_successors` is demoted** to
   `m68k_decode_local_flow_successors`: decode-local `next_pc`-class successors
   only (sequential fallthrough, the not-taken side of a conditional branch, and
   a call site's own post-call continuation). It emits no taken-branch, no
   `jmp`/`jsr` target, no callee-entry, and no indirect-target successor. The
   canonical relation carries **no plain sequential fallthrough edge**, so this
   helper still supplies every `next_pc` successor; the canonical edges supply
   only non-fallthrough transfers. `m68k_compute_callee_register_footprint`
   consumes the same shared adjacency, so its callee-body walk still traverses
   taken branches and resolved indirect transfers exactly as before.

3. **Edge-kind classification.**

   | canonical edge kind          | adjacency contribution                                                                 |
   |------------------------------|----------------------------------------------------------------------------------------|
   | `direct_branch`              | plain successor(target) — value propagation                                             |
   | `indirect_branch`            | plain successor(target) — value propagation                                             |
   | `direct_call`                | plain successor(callee) + decode-local `force_unknown` continuation, `callee_entry` filled from this edge for the bounded callee-write-footprint proof |
   | `indirect_call`              | plain successor(callee) + decode-local `force_unknown` continuation (`callee_entry` unfilled: multiple callees -> whole-state-unknown continuation) |
   | `fallthrough`               | plain successor(target) — value propagation (conditional not-taken block split)          |
   | `fallthrough_continuation`  | plain successor(target) — structural, value propagation                                 |
   | `return_to_continuation`    | **omitted** — structural return topology is owned by `synthesize_return_edges` and the runtime return-membership check; including it here would churn the ADR-0026/T186 root-selection SCC topology with no finite-value benefit |

   Result lists are deterministically sorted and de-duplicated on the
   `(address, force_unknown, callee_entry)` triple.

4. **`force_unknown` / callee-write-footprint conservatism is preserved
   verbatim.** The post-call continuation is still the decode-local
   `force_unknown` successor; a finite An/Dn set survives it only when the
   bounded callee register-write footprint proves the callee never writes that
   register (A7 never preserved). An unknown or multi-target callee keeps the
   whole-state-unknown continuation.

5. **Intra-walk `pc_index8` proof consumes the walk's in-progress `edges_`.**
   `edges_` grows monotonically during a walk (append-only). Every instruction
   on a direct path to an index producer is walked — and its taken-branch /
   call / stitched edges appended — before the indirect control site that
   consumes the proof is processed. A later edge append can only add successors,
   which is monotone toward `unknown` in the ADR-0009 lattice; it never
   retroactively strengthens an earlier finite proof. The authoritative
   canonical frontend run
   (`m68k_prove_stitched_an_indirect_targets(merged_decoded, analysis_roots,
   merged_edges, ...)`) re-runs the analysis over the complete aggregated
   relation.

6. **Authoritative chain.** proposal (Ghidra candidate / hint / runtime
   observation) -> independent validation or static proof -> canonical stitched
   `M68kStaticEdge` -> root / reachability / value-flow consumer. A raw
   candidate, an emitted address, an external hint, or a runtime-observed target
   never becomes an authoritative control edge on its own; admission of a unit
   does not make its body authoritative for reachability.

## Consequences

- A proven finite `pc_index8` / `(An)` indirect-control edge is now visible to
  root selection and the ADR-0009 fixed point; code reachable only through it
  can be proven, and it is covered by an authoritative component instead of
  spawning an artificial unknown root.
- No new edge kind, no decoder change, no ceiling raise, no context sensitivity,
  no candidate-root suppression, no ADR-0009 merge-semantics change.
- If a genuine finite authoritative caller context and a genuine unknown
  authoritative caller context meet at shared code, the join legitimately stays
  `unknown` (case C). That is not failure; bounded call-site/context-sensitive
  ADR-0009 remains a separate future decision.
