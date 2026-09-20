# ADR 0019: Finite-Region Cursor Progress — Generalizing Output-Write Progress to Statically-Certified Read Progress

- Status: Accepted
- Date: 2026-09-02
- Relates to: ADR 0017 (bounded generated data-transform progress proof — the
  original output-write mechanism), ADR 0018 (scope identity as a partition key,
  writer-local certification — this ADR generalizes its grouping key and access
  family, keeps its soundness argument), ADR 0007 (generated-runtime loop-progress
  watchdog, no-progress window `W`, single never-replenished global
  `progress_credit`), ADR 0016 (bounded static finite-loop progress proof — the
  `M68K_LOOP_PROOF_MAX_*` constants; unrelated, unchanged), ADR 0006 (generic
  immutable/generated cartridge-data region ownership — the source of the new
  admissible READ region), ADR 0013 (four-seed / four-round Phase-B discovery
  bounds).
- Does not amend, and does not relitigate: ADR 0007's no-progress window `W`, its
  `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` global never-renewable ceiling, or the
  `instruction_budget_exhausted` fail-closed stop; ADR 0016's admitted
  finite-control-loop family or the numeric values of its resource caps; ADR
  0013's four-seed / four-round Phase-B bounds; ADR 0006's cartridge-data
  ownership model (`MappingClaim` / `M68kOwnedCartridgeRegionFact` proof
  obligations, immutability, mapping/bounds re-verification); ADR 0002's
  static-dispatch boundary; the wire report schema; the stop / diagnostic enums;
  `STOP_DIAGNOSTIC_PAIRS`. ADR 0018's soundness argument (Cases A-C, the
  finite-region low-water ratchet, the finite never-replenished shared global
  credit) is preserved and extended, not replaced.

## Context

SEG-007-T156 implemented ADR 0018 end to end (full gate 85/85; independent
adversarial PASS) and re-executed the authorized pinned Sonic Phase-B route.
Writer-local output-WRITE data-transform progress notes are now emitted and
credited in generated-native execution (round-4 generated C now contains
`genesis_note_data_progress` calls for multiple proof groups). Despite that, the
round-4 `instruction_budget_exhausted` watchdog terminal was **byte-identically
unchanged**: same dispatch step count, same final program counter, with and
without ADR 0018. T156's own normalized diagnosis: the decompressor's inner
counter loops are already progress-credited through the ADR 0016 loop-progress
family, and the standing blocker is the T154-classified **outer** decompressor
loop — a multi-block reducible natural loop whose back-edge control value is
re-loaded from memory each traversal (non-monotonic, no statically-fixed trip
bound) — which no existing progress family owns. T156 recorded
`needs_full_refinement` because closing that gap looked, from output-write
evidence alone, like it might require generic multi-instruction / data-flow
decompressor-termination analysis, which this project's non-goals explicitly and
correctly forbid (no abstract interpreter, no generic value/data-flow analysis,
no decompressor-format semantics).

### Operator-supplied throwaway diagnostic experiment (evidence only, non-durable)

After T156's implementation was complete but before it merged, the operator ran a
disposable, fully-reverted architecture experiment in an isolated worktree based
on the (then unmerged) `task/seg-007-t156` branch. It was never committed,
pushed, or turned into a PR/ADR/backlog record and is not discoverable from
repository history. It is recorded here strictly as **non-durable diagnostic
evidence** motivating this decision, not as executed repository evidence, and no
raw address/opcode/byte/offset from it is reproduced anywhere in this ADR.

**Phase 1 (inspection of the final blind no-progress window).** Of the final 128
dispatches immediately preceding the T156-unchanged `instruction_budget_exhausted`
stop: 38 had an existing loop-progress note, 0 had an ADR-0017/0018 output-write
data-progress note, and 45 had a successful routed READ from an
ADR-0006-owned cartridge region, with the cartridge-read cursor strictly
monotonically increasing and a maximum gap of 2 dispatches between successful
reads. Normalized classification: `FINAL_WINDOW_CONTAINS_DENSE_FINITE_INPUT_
CURSOR_PROGRESS` — the exact blind interval that defeats the current mechanisms
contains regular, successful consumption of a finite, statically-owned, immutable
cartridge-data region, i.e. the same *shape* of finite-region cursor progress
ADR 0017/0018 already recognize on the output side, just on the input side.

**Phase 2 (disposable experiment).** A minimal mechanism was added, in the
disposable worktree only, recognizing source-side auto-updating `MOVE` reads
where: the routed READ succeeded; the source effective address was decoded
`(An)+` / `-(An)`; the access lay wholly inside an ADR-0006-owned
`M68kOwnedCartridgeRegionFact` region; the fixed decoded step was known
(including the MC68000 A7-byte step-by-2 rule); the architectural cursor `An`
committed exactly once, after successful access, equal to the pre-access cursor
adjusted by exactly the decoded step; and the standard live region / no-wrap /
exact-commit guards held. It converted the observed cursor into a
direction-normalized `remaining_bound` (postincrement: `region_end -
committed_cursor`; predecrement: `committed_cursor - region_begin`), reusing the
existing ADR-0007-style historical low-water ratchet and finite never-replenished
global credit semantics unchanged.

**Experimental result.** Two complete pinned Sonic Phase-B executions were
deterministic (byte-identical generated round-4 C, byte-identical sanitized
output). 3777 experimental read-progress observations were produced, 3775
credited. Execution advanced substantially beyond the T156 baseline (318766 total
dispatches before the new stop); the `instruction_budget_exhausted` terminal
disappeared; a genuinely new, unrelated runtime-selected frontier was reached
instead: `c4_lowering_gap` with `family = clr_auto_update`. This was not another
watchdog terminal, not a device frontier, not an ADR-0013 four-round/four-seed
saturation terminal, and not decompressor-specific. Normalized verdicts:
`EXPERIMENT_SUPPORTS_FINITE_REGION_READ_PROGRESS` and
`EXPERIMENT_ADVANCED_TO_CLR_AUTO_UPDATE`.

Independently, direct inspection of `src/codegen/c11/frontend.cpp`
(`classify_m68k_c4_gap_shapes`) confirms `GENESIS_C4_LOWERING_DIMENSIONS_
CLR_AUTO_UPDATE` is already a named, pre-existing `requires_architecture_decision`
gap class in the shipped C4 dispatcher, alongside the already-implemented
`ADD_AUTO_UPDATE`/`MOVEA_AUTO_UPDATE`/etc. siblings that were each closed by their
own prior task using the same deferred-address-register-commit pattern. The
experiment's result is therefore consistent with, not merely asserted by, durable
repository evidence: the next lowering gap the pipeline would name for an
auto-updating `write_clr` destination is exactly the one the experiment reports.

### Interpretation

This experiment falsifies the stronger hypothesis, implicit in T156's
`needs_full_refinement` disposition, that the Sonic route necessarily requires
generic multi-instruction / data-flow decompressor-termination analysis. A much
smaller generic invariant sufficed: **successful monotonic consumption of a
finite, statically-owned cartridge-data region** — the mirror image, on the READ
side, of what ADR 0017/0018 already certify on the WRITE side. This is not a
decompressor-format recognizer: it makes no claim about what the bytes mean, only
that a bounded, statically-owned window of memory is being consumed
monotonically and without wraparound.

### Addendum: single-data-slot validation experiment (evidence only, non-durable,
    added after this decision's initial drafting, before refinement PR merge)

The Phase-2 experiment above used a *separate* experimental READ low-water slot
alongside the existing ADR-0017/0018 WRITE `data_slot_*` state, for isolation
only. This ADR's own Decision below instead proposes that production READ and
WRITE progress share the single existing `GenesisDataProgressNote` / active
`data_slot_proof_id` / `data_slot_low_water` mechanism. That specific
runtime-state assumption was not covered by the Phase-2 experiment and is
therefore recorded, tested, and confirmed separately here.

A second disposable experiment, run in an isolated worktree with no
commit/push/PR/backlog/ADR change and fully reverted afterward — not
discoverable from repository history, recorded strictly as non-durable
operator-supplied diagnostic evidence, not as executed repository evidence —
fed valid finite-region READ observations through the existing single
production mechanism instead of a second slot: the existing single
`GenesisDataProgressNote`, the existing single active `data_slot_proof_id` /
`data_slot_low_water` state, distinct deterministic proof identities per
stream, the existing shared never-replenished global progress credit, and no
second READ slot. Same-dispatch READ/WRITE collisions were resolved
conservatively by letting the existing WRITE observation win (the READ
observation for that exact dispatch is hidden/uncredited), so the already-landed
T156 WRITE-progress behavior was never weakened to obtain the result.

Result: the pinned Sonic Phase-B route deterministically (two independent
generate/compile/run cycles, byte-identical) advanced past the T156
`instruction_budget_exhausted` baseline and reached the same later
runtime-selected frontier as the Phase-2 experiment: `c4_lowering_gap` /
`family = clr_auto_update`. Normalized counters: eligible READ observations
6057; READ observations surviving same-dispatch arbitration 5298; eligible
WRITE observations 32766; WRITE observations surviving arbitration 32766
(100%, confirming WRITE-wins arbitration held); same-dispatch READ
observations hidden by WRITE arbitration 759; active data-stream identity
(`proof_id`) replacements 7; strict READ low-water credits 5293; strict WRITE
low-water credits 32763; maximum accepted data-progress gap after the former
T156 terminal 28 dispatches; generated-native execution advanced 22519
dispatches beyond the former T156 terminal before selecting `clr_auto_update`.
Verdict: `EXPERIMENT_SUPPORTS_SINGLE_DATA_SLOT_ADR0019`.

Interpretation: the single active data-progress slot does not materially
thrash between interleaved READ/WRITE streams on the selected route (only 7
identity replacements across more than 38,000 total eligible observations);
same-dispatch WRITE-first preservation does not suppress the READ progress
needed to cross the former watchdog (post-baseline max accepted-progress gap
of 28 dispatches remains dense relative to `W`); separate persistent READ and
WRITE watchdog slots are **not** required by the executed evidence. This
addendum confirms, and does not change, the one-data-slot production
architecture already selected in the Decision below — it is evidence for the
existing choice, not a motivation for a new slot, a per-stream runtime map,
larger watchdog limits, or any other architecture change.

## The two things this ADR must generalize

ADR 0017/0018, as implemented, name their proof/progress-stream key only by
`scope_identity + cursor_An` (`src/cpu/m68k/static_loop_proof.cpp`,
`m68k_prove_generated_data_progress`, the `by_cursor` grouping map). When
advancers sharing one `scope_identity + cursor_An` disagree on `direction` or
`step`, the **entire group is rejected outright** (`continue` in the grouping
loop) rather than being partitioned into distinct compatible sub-groups. This is
already a latent defect independent of adding READ progress: two independently
valid, compatible writer/reader shapes that happen to reuse the same `An` in one
SCC (for example a postincrement pass and an unrelated predecrement pass on the
same scratch pointer register in one scope) currently suppress each other's
progress entirely instead of forming two independent streams. `proof.region` is
also currently a single hardcoded `M68kDataProgressRegion::synthetic_work_ram`
field, not itself part of the grouping/identity key, which is adequate only
because there has so far been exactly one region kind.

This ADR therefore:

1. Generalizes the ADR-0017/0018 proof/progress-stream identity key from
   `scope_identity + cursor_An` to `scope_identity + cursor_An + direction +
   fixed_step + access_direction + region_identity`, so that independent valid
   progress shapes sharing a scope and cursor register become **distinct
   compatible groups** instead of collapsing to one all-or-nothing group that a
   single disagreeing advancer can suppress entirely. `access_direction`
   (read vs. write) and `region_identity` (which owned region — synthetic work
   RAM, or which specific ADR-0006 owned cartridge region) become first-class
   parts of that key, not incidental payload fields.
2. Adds a second admissible access family alongside the existing output-WRITE
   family: statically-certified finite-region cursor **READ** progress from an
   ADR-0006-owned immutable cartridge region.

Both families share exactly one proof/progress-stream shape
(`scope_identity + cursor_An + direction + fixed_step + access_direction +
region_identity` → `remaining_bound`), one historical low-water ratchet per
slot, and one finite never-replenished global `progress_credit` pool — this ADR
adds a second certified access family to an existing generic mechanism; it does
not create a second mechanism.

## Explicitly forbidden

Decompressor-format semantics; sentinel recognition; routine recognition; generic
pointer/value interpretation; abstract interpretation; runtime opcode
fetch/decode; a JIT; a second dispatcher; another decompressor-specific semantic
owner; restoring whole-SCC purity (ADR 0018 remains correct — see its Cases A-C,
extended below to cover reads); reusing ADR 0016 loop-shape caps for this
mechanism; altering `W` or the global progress-credit ceiling
(`GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`); altering ADR 0013's four-round /
four-seed Phase-B bounds; altering ADR 0006's cartridge-data ownership model
(only *consuming* its existing `M68kOwnedCartridgeRegionFact` / `MappingClaim`
proof, never re-deriving or weakening it).

## Preferred production architecture

Conceptual pipeline (applies uniformly to both WRITE and READ progress families):

```
generated access operation
  -> writer/reader-local static certification (per-operation, unchanged shape
     from ADR 0017 Decision step 4, extended per family below)
  -> successful routed access (genesis_route_access; WRITE or READ)
  -> exact decoded auto-update commit (single An commit, direction-correct step,
     including the A7-byte step-by-2 rule)
  -> finite owned region live guard (synthetic_work_ram bounds for WRITE;
     revalidated ADR-0006 owned-region bounds for READ)
  -> direction-normalized remaining_bound
     (postincrement: region_end - committed_cursor;
      predecrement:  committed_cursor - region_begin)
  -> existing historical low-water ratchet (per proof_id slot)
  -> existing shared never-replenished global progress_credit
```

## READ progress certification requirements (all required, statically certified,
   never assumed)

Instrumentation of a candidate READ advancer is valid only when **all** hold:

1. The routed access direction is READ and the routed READ succeeds
   (`genesis_route_access` read success, exactly like every other read the
   pipeline already routes).
2. The source effective address is decoded `(An)+` or `-(An)`, with a known
   fixed decoded step (including the MC68000 A7-byte step-by-2 rule).
3. The complete relevant CPU-owned effect footprint for that exact candidate
   operation is known (the `M68kOperationEffect` `address_register_write_mask`
   auto-update entry is present and complete for that exact writer, exactly as
   ADR 0017/0018 already require on the WRITE side). An operation with an
   incomplete/unknown footprint is rejected as a member; it is never
   instrumented and never counted against any other candidate.
4. The actual access extent lies wholly inside one retained, revalidated
   ADR-0006 `M68kOwnedCartridgeRegionFact` region (the same
   mapping/bounds-containment proof obligation ADR 0006 already defines and the
   pipeline already trusts for its ROM-read success path); the region is
   immutable and statically owned by construction of that fact.
5. No unsigned wrap occurs on the computed cursor.
6. The architectural cursor commit occurs exactly once, after successful
   access, equal to the pre-access cursor adjusted by exactly the decoded step
   (the same single-commit deferred-address-register-commit discipline the
   codebase already uses for every other postincrement/predecrement lowering).

Failed reads emit no note and commit no partial cursor. Only a strictly smaller
`remaining_bound` for the same progress-stream identity (per the generalized key
above) receives credit; equal or larger is establish/replace or no-progress,
exactly as ADR 0018 already specifies for WRITE progress. Existing output-WRITE
behavior, its runtime ABI, and its watchdog `data_slot` accounting are otherwise
unchanged by this ADR; the implementation may reuse or add a progress-stream slot
identity per the generalized key, but must not introduce a second watchdog credit
pool, a second `W`, or any accounting path that lets one dispatch consume more
than the existing single shared credit unit across all progress families
combined.

## Soundness argument (extends ADR 0018 Cases A-C to reads; unchanged premises)

The question is the same one ADR 0018 already answered for WRITE progress,
restated for READ progress: can an uninstrumented modification of the cursor
`An`, or any sequence of read observations, violate ADR 0007 boundedness, given
the unchanged runtime protections (finite admissible region; note only after
routed-access success and the single `An` commit; live bounds/no-wrap/exact-commit
guards; one `data_slot_low_water` per active `proof_id`; credit only for a
strictly-smaller `remaining_bound`; one shared, never-replenished global
`progress_credit`; unchanged `W` and `instruction_budget_exhausted` stop)?

- **Rewind / reset (uninstrumented or instrumented-but-non-advancing).** The
  low-water ratchet is monotone non-increasing by construction: any observation
  whose `remaining_bound` is not strictly smaller than the current low water is
  no-progress. A rewound or reset cursor can only ever produce an equal or larger
  `remaining_bound` relative to the true minimum already seen, so it is never
  credited. Identical to ADR 0018 Case A; unaffected by adding a read family.
- **Forward mutation (instrumented or an uninstrumented intervening operation).**
  For one active `proof_id` slot instance, `remaining_bound` is confined to
  `[0, region_size]`, and every credited step strictly lowers the ratchet toward
  0, so credited steps for one slot instance are bounded by `region_size / step`,
  a finite constant per region. Every credited step, across every progress
  family sharing the mechanism, also permanently consumes one unit of the single
  never-replenished global `progress_credit`; the total number of credited steps
  over an entire run is therefore bounded by that fixed global constant, after
  which no further step is credited, `steps_since_progress` reaches `W`, and the
  run fails closed. Identical to ADR 0018 Case B.
- **Region escape / wrap.** The live guard requires the access to lie wholly
  inside the retained, revalidated owned region and to produce no unsigned wrap;
  either failure means no note and no partial commit is exposed to the progress
  mechanism at all — the same as ADR 0006's own fail-closed containment
  discipline for every other owned-region access.
- **Two owned regions never share one stream.** `region_identity` is now part of
  the progress-stream identity key (this ADR's generalization), so two distinct
  ADR-0006 regions can never be conflated into one `proof_id`/low-water slot even
  if a candidate cursor register happens to alias between them across disjoint
  scopes; that is exactly the scope-identity locality guarantee ADR 0018 already
  established for WRITE progress, now also keyed by region and access direction.
- **Same SCC/cursor but different direction/step no longer suppresses all
  progress.** Because the identity key now includes `direction` and
  `fixed_step`, two structurally distinct, individually-compatible advancer
  groups sharing one `scope_identity + cursor_An` form two independent proof
  groups rather than one group that a single disagreeing advancer voids
  entirely. Each group is independently subject to the same Cases above.
- **Finite shared global credit is unchanged and remains the sole termination
  guarantee**, decremented by at most one unit per dispatch across all note
  families combined (deterministic ordering unchanged), never replenished.

No concrete counterexample requires more than the finite-region low-water
ratchet per slot plus the finite never-replenished global credit, for either
access direction. Adding a second, symmetric access family does not weaken this
argument; it reuses the same finite-region ratchet-plus-credit termination proof
per family, and the shared credit pool remains the single cross-family
termination bound.

## Decision

1. Generalize the ADR-0017/0018 data-transform progress proof/progress-stream
   identity from `scope_identity + cursor_An` to `scope_identity + cursor_An +
   direction + fixed_step + access_direction + region_identity`. Within one
   derived scope and cursor register, advancers that disagree on direction, step,
   access direction, or region form **distinct** compatible groups (each
   independently certified and, if valid, independently proved), instead of one
   group where any disagreement voids all progress for that cursor.
2. Add a second admissible access family: successful, statically-certified
   finite-region cursor READ progress from an ADR-0006-owned immutable cartridge
   region, per the READ certification requirements above. The existing
   output-WRITE family (`GENESIS_REGION_SYNTHETIC_WORK_RAM`) is preserved
   unchanged; this is an additive family sharing the same mechanism, not a
   replacement.
3. Both families share exactly one progress-stream shape (identity key above →
   direction-normalized `remaining_bound`), the existing historical low-water
   ratchet semantics, and the existing single, finite, never-replenished global
   `progress_credit` pool (at most one credit unit consumed per dispatch across
   all families combined, deterministic ordering preserved).
4. Resource bounds remain aggregate-derived exactly as ADR 0018 established: no
   ADR-0016 `M68K_LOOP_PROOF_MAX_*` constant is repurposed as an ADR-0018/0019
   capacity; work remains proportional to the retained aggregate
   (`blocks + edges + operations`) with the generalized key adding only a
   constant-factor widening of the grouping map, not a new unbounded dimension.
5. `m68k_prove_finite_loop_progress` (ADR 0016), `W`, the global credit ceiling,
   the wire schema, the stop/diagnostic enums, `STOP_DIAGNOSTIC_PAIRS`, ADR 0006's
   ownership/immutability model, and ADR 0013's four-seed/four-round bound are
   all left completely unchanged.

## Consequences

- The Sonic round-4 route's dense final-window finite cartridge-region READ
  cursor consumption (Phase 1 of the operator experiment) becomes eligible for
  the same kind of finite, bounded, terminating progress credit the WRITE side
  already receives, without any decompressor-format-specific reasoning.
- A genuinely non-advancing transform, a reset/rewound cursor (either
  direction), a region escape, a wrap, repeated same-offset accesses, and
  unbounded re-entry all still stop deterministically at the unchanged
  `instruction_budget_exhausted` stop, bounded by the unchanged
  never-replenished global `progress_credit`.
- Two independent compatible progress shapes sharing one scope and cursor
  register (differing only in direction/step/access-direction/region) no longer
  suppress each other; this closes a latent defect in the shipped ADR-0018
  grouping, not only enables READ progress.
- If, after the implementation task certifies READ progress and re-executes the
  Sonic Phase-B route, the actual runtime-selected terminal changes to a
  different frontier (for example the pre-existing named `c4_lowering_gap`
  `family = clr_auto_update` C4 dispatcher gap the operator experiment and the
  shipped `classify_m68k_c4_gap_shapes` code both already name), that frontier is
  handled by its own existing semantic owner (`write_clr` CPU/CCR semantics,
  reusing the established deferred-address-register-commit pattern) within the
  same implementation task if and only if it is exactly that named, bounded,
  already-precedented gap; any other unpredicted frontier is recorded honestly
  as this task's own outcome per normal continuation-disposition rules, not
  speculatively pre-implemented.
- A later task wanting yet another progress family (a computed-index scatter
  cursor, a non-`An` cursor, a third admissible region kind) must still emit its
  own CPU/codegen-proven typed note through the same seam and existing semantic
  owner, keyed consistently with this ADR's generalized identity, or record why
  that is unsound; it must not reintroduce a fixed total cutoff, a runtime
  opcode decoder, or program-wide register grouping.
