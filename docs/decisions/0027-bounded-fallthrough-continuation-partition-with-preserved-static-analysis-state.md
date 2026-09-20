# ADR-0027: Bounded fallthrough-continuation partition with preserved static-analysis state

- Status: Accepted
- Date: 2026-09-07
- Deciders: SEG-007-T181 (architecture-class task), consuming SEG-007-T180's
  ADR-0026 offline-inventory partition/stitch model and its `needs_full_refinement`
  handoff that a reset-reachable region still contains long, legitimate,
  sequentially fallthrough-connected instruction stretches with no independently
  admitted inventory boundary inside them, so one root still owns
  `A -> next_pc -> next_pc -> ...` to the per-root instruction ceiling.
- Amends: ADR-0026 (adds a fourth partition mechanism alongside Phase-1
  admission, Phase-2 control-transfer stitching, and the IRQ6 asynchronous
  static hardware root). Cross-references ADR-0009 (finite-`An` Tier-1),
  ADR-0011/ADR-0014 (bare-address worklist, entry-connected admission),
  ADR-0024/ADR-0025 (Tier-2 emitted-set dispatch), ADR-0013 (entry-rooted
  discovery prefix boundary). Weakens none of them.

## Context

ADR-0026 §2 correctly forbids treating a plain `next_pc` step as a control-flow
boundary that could be guarded mid-walk. The residual canonical one-shot Sonic
build-time stop is a `discovery_budget_exhausted` on the reset root whose cause
is NOT recursive branch/call ownership (ADR-0026 bounded that) but a genuinely
linear stretch: the only live state across an ordinary `next_pc` step is the
decode position plus a monotonic accumulator set. The `M68kStaticGraphWalker`
worklist holds bare addresses (ADR-0011/ADR-0014 removed the
`(addr, stack_signature)` DFS); ADR-0009 finite-`An`/`Dn` facts come from
`analyze_finite_index_values`, a separate per-root whole-decode-map fixpoint
seeded at conservative bottom; call frames are call-site keyed and
`return_to_continuation` edges are a global post-merge pass; cross-root overlap
already deduplicates via `decoded_matches` / `m68k_merge_register_state` and
fails closed on conflict. The representation can therefore suspend at an
arbitrary `next_pc` step and resume soundly. The decision was YES; exactly one
architecture successor is created.

## Decision

### §1 Unit synthesis at the ceiling

Between Phase-1 admission and Phase-2 stitching, a continuation-root fixpoint
pre-pass (offline-inventory-aware only; inert with an empty inventory) re-drives
every root (reset entry, admitted units, continuation roots discovered so far,
runtime-confirmed seeds). When a root walk reaches `m68k_discovery_max_instructions`
at a plain `next_pc` step (sequential fallthrough or branch/call continuation)
whose continuation decoded cleanly as a prefix-boundary shape (no open control
target, empty unresolved reason), that continuation address is recorded as a
synthesized fallthrough-continuation unit root. It then participates in Phase 2
identically to an admitted unit: it is a Phase-2 seed, a member of the separate
5th-arg fallthrough-continuation boundary set for every other root, and its
facts flow through the unchanged `merge_root_result`.

### §2 Continuation-state contents

A continuation unit carries only: the continuation root address (the predecessor
instruction's `next_pc`); the recorded `M68kStaticEdgeKind::fallthrough_continuation`
edge; the obligation that the predecessor instruction decoded cleanly and is
non-terminal / a branch-or-call whose continuation is this `next_pc`. No abstract
register state, no call-frame stack, no per-path value facts are threaded. The
continuation unit's `analyze_finite_index_values` re-seeds at conservative bottom
(all-unknown); this is sound and fail-closed.

### §3 One representation / one program

The existing discovery worklist, static-program blocks/edges, boundary set, and
`merge_root_result` are reused. No parallel program representation, no parallel
interpreter state, no second static program. A continuation unit is an ordinary
bounded local unit distinguished only by its `fallthrough_continuation` in-edge.

### §4 Fixpoint re-drive + normalized count ceiling

The pre-pass repeats until a round discovers no new continuation root. Monotonic
growth bounded by `m68k_fallthrough_continuation_unit_ceiling` (64, a deliberate
fraction of `m68k_discovery_max_blocks`, never an instruction-budget raise)
guarantees termination. A region that would need more continuation units than the
ceiling fails the BUILD closed with a normalized `discovery_budget_exhausted`
diagnostic sourced at the reset entry -- no loop, no cap raise.

### §5 Preserved invariants

- ADR-0026 control-transfer stitching is unchanged; `fallthrough_continuation`
  is a distinct edge kind from a stitched control transfer. Both keep their
  target a retained aggregate block entry (ADR-0014 P1).
- `decoded_matches` / `m68k_merge_register_state` dedupe-vs-conflict is
  unchanged: identical overlapping decode deduplicates; a conflicting decode,
  indirect-candidate-set, completion-RTS, or cross-tier fact raises
  `startup_graph_mismatch` and fails the whole build closed.
- ADR-0025 Tier-1-over-Tier-2 cross-root resolution is unchanged.
- ADR-0009: a finite-`An` proof chain cut by a continuation boundary degrades
  the downstream indirect site to Tier-2 `JMP/JSR (An)` / `pc_index8`
  emitted-set membership dispatch, or is re-established locally within the
  continuation unit; it never yields an unsound Tier-1 set and never hard-fails
  the build. A3 / finite-`An` provenance is not reopened. A7/SP exclusions are
  unchanged.
- True fallthrough semantics: the continuation boundary introduces no
  runtime-visible branch/block-entry and no PC/CC/register/memory/call/return
  change. It is the address-order block split generated C already performs; no
  branch or jump is emitted at it.

### §6 Normalized instrumentation

`OfflineInventoryStitchMetrics` gains (numbers only):
`stitched_fallthrough_continuation_edge_count`, `fallthrough_continuation_unit_count`,
`continuation_synthesis_rounds`, `ceiling_trips_not_continuation_eligible`
(reset/IRQ6 prefix-boundary-shaped `discovery_budget_exhausted` issues whose
address is NOT a continuation root -- the genuine open-control-edge frontier
diagnostic, unchanged downstream). The deferred ADR-0026 §5
`emitted_block_count` / `emitted_code_address_count` are wired from the final
validated `EmittedCodeAddressSet` via a normalized
`segarecomp: offline inventory emission:` stderr line parsed by
`tools/genesis_startup_bridge.py`.

### §7 Synthetic coverage

Project-authored synthetic fixtures prove: linear stretch > ceiling partitions
into >=2 continuation units with the identical instruction set / block
boundaries modulo the split; no runtime-visible control change (identical
PC/CC/reg/mem, no emitted branch/jump); a ceiling trip at a genuine open control
edge still fails closed; a continuation boundary right after JSR/BSR preserves
`return_to_continuation`; a continuation unit overlapping a later admitted unit
dedupes via `decoded_matches` and fails closed on conflict; an ADR-0009
finite-`An` chain cut degrades to Tier-2 (never unsound Tier-1, never hard
fail); empty inventory / raw route byte-identical with zero continuation units;
a pathological all-linear region reaches the ceiling and fails closed with a
normalized diagnostic and no loop; Tier-2 `JMP (An)` dispatch succeeds when the
target is contributed by a continuation unit and fails closed when absent;
determinism (identical partition + identical generated C across two runs).

### §8 Fail-closed disposition

If the existing static-program representation fundamentally cannot carry a
fallthrough continuation without threading per-path state or inventing a runtime
control boundary, no cap is raised and no unsafe approximation is implemented:
stop with `needs_full_refinement` and record the exact representation invariant.
If one specific stretch cannot be partitioned generically, classify the generic
reason and fail closed -- no ROM-specific exception.

## Consequences

The offline-inventory-aware static-discovery partition model has four mechanisms.
The raw/unannotated recompilation route is byte-identical and never synthesizes a
continuation unit. A later genuine CPU / codegen / device / runtime semantic
frontier is the correct stopping point for this objective; a bounded successor is
created for that concrete frontier rather than broadening this task.

## Amendment (SEG-007-T219)

`m68k_fallthrough_continuation_unit_ceiling` raised `64U -> 128U`. SEG-007-T218's
corrected, committed-data-conflict-free 197-record `compat/genesis/` inventory
resolves genuinely more real indirect jumps, making previously-unreachable
straight-line code reachable; the §4 fixpoint pre-pass legitimately needs 128
synthesized units (114 fallthrough-continuation + 14 resolved-control-target) to
partition it, exceeding the original 64U count ceiling. This is the SAME generic,
image-independent doubling technique `m68k_discovery_max_instructions` /
`m68k_discovery_max_blocks` themselves use for their own ceiling history (see
their doc comments in `include/segarecomp/machine/genesis/frontend.hpp`) -- not a
per-ROM/per-route retune, which §4's own "ADR-0027 forbids retuning it by route
signature" text still governs. The untouched `m68k_discovery_max_instructions`
per-root ceiling is never approached at the new value (observed
`max_local_instr=249 < 256` at convergence); this amendment raises only the
COUNT ceiling on partitioned units, exactly as §4 describes, never an
instruction-budget raise. Confirmed deterministic (byte-identical across
repeated runs) and decisively distinguished from a genuine architecture gap by
an ephemeral, reverted-before-commit unlimited-count sanity check: with no count
ceiling, the fixpoint pre-pass still converges at exactly 128 units and the
opted-in canonical one-shot route advances past static discovery entirely to a
new, qualitatively different pipeline boundary (a CPU decode/lift/IR/C11
unsupported-instruction frontier) -- confirming 128U is sufficient and the
existing four-mechanism partition model fully expresses this shape with no
representation gap requiring a new architecture decision.

## Amendment (SEG-007-T223)

`m68k_fallthrough_continuation_unit_ceiling` is raised `128U -> 256U`. This is
the same generic, image-independent bounded doubling precedent as the prior
amendment: it changes only the shared count ceiling for synthesized
fallthrough-continuation and resolved-control-target partition units, never a
per-root instruction, block, seed, or round budget. Focused project-authored
coverage demonstrates deterministic acceptance just beyond the former ceiling
and a separate over-ceiling partition still fails closed with the normalized
`discovery_budget_exhausted` result. The correction therefore remains within
the existing four-mechanism representation and introduces neither a route
signature exception nor a new architecture decision.
