# ADR 0017: Bounded Generated Data-Transform Progress Proof

- Status: Accepted
- Date: 2026-09-02
- Relates to: ADR 0007 (generated-runtime loop-progress watchdog and the
  `genesis_note_loop_backedge` seam), ADR 0016 (bounded static finite-loop
  progress proof), ADR 0009 (bounded retained static-analysis facts consumed by
  guarded C11 lowering), and ADR 0006 (generic immutable cartridge-data region
  ownership).
- Does not amend, and does not relitigate: ADR 0002's static-dispatch boundary
  (no target-byte fetch, no decoder, no interpreter, no JIT, no second
  dispatcher), the wire report schema, the stop/diagnostic enums,
  `STOP_DIAGNOSTIC_PAIRS`, ADR 0007's no-progress window `W`, its
  `GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT` global never-renewable ceiling, ADR
  0016's admitted finite-control-loop family or its resource caps, ADR 0006's
  cartridge-data ownership model, or ADR 0013's four-seed / four-round Phase-B
  bounds.

## Context

Generated-native Sonic Phase-B execution (`rounds = 4`, `seed_count = 4`,
byte-identical sanitized reports, adversarial PASS through SEG-007-T153) reaches
ADR 0007 `instruction_budget_exhausted` inside a real multi-block, data-dependent
decompression / bitstream-traversal loop. In normalized classes: the outer
back-edge control value is re-loaded from memory each traversal (bound-write
reload), is non-monotonic, and has no statically fixed trip bound; nested finite
counter loops are re-initialised and re-entered on every outer pass. It is not a
device / VDP / timing / controller-I/O wait loop. ADR 0006 cartridge-data
ownership works here (the loop successfully consumes immutable cartridge bytes)
and ADR 0016 works here (it correctly declines a loop it cannot prove). The loop
body performs proven monotonic post-increment byte writes into the bounded
64 KiB Genesis work-RAM region (`runtime/genesis/runtime.h` `work_ram[65536]`).

SEG-007-T147 was the first watchdog-family frontier on this route (resolved by
ADR 0016); this is the second. Per the anti-churn rule, a third one-loop
exception is not taken. The generic gap is: ADR 0007 has no way to recognise
sound, finite, non-renewable progress in a data-transform loop whose control-flow
trip count is data-dependent and unprovable under ADR 0016, but whose generated
CPU/static/codegen facts prove a cursor advancing monotonically through a finite
statically owned memory region. Neither ADR 0016 (a control-loop proof) nor ADR
0006 (data ownership, not progress) should be widened to cover it, and an
emitter-local recognizer of an instruction sequence, routine name, address, or
byte pattern is unsound for the same reason ADR 0016 gives.

This ADR is deliberately scoped to a **progress-accounting** mechanism. It does
**not** prove that any decompression format, compressed stream, or algorithm
terminates. It grants bounded credit for demonstrable forward motion through a
finite region and otherwise lets ADR 0007 fail closed exactly as today.

## Decision

Adopt a bounded static **data-transform progress proof** family, structurally
parallel to ADR 0016. CPU/static analysis owns a typed, provenance-keyed
`M68kGeneratedDataProgressProof`; C11 lowering consumes only the retained typed
fact and emits a progress note through the existing opcode-agnostic
`genesis_note_loop_backedge` seam (extended with a distinct typed note kind, see
below); ADR 0007's watchdog consumes that note through a deterministic
low-water / remaining-region rule analogous to its existing finite-loop slot,
under the unchanged global never-renewable credit ceiling.

### The retained static fact (`M68kGeneratedDataProgressProof`)

CPU/static analysis produces, at a bounded fixed point over the already-decoded
static CFG under the ADR 0016 resource caps (reused verbatim; not re-tuned), a
typed fact establishing **operation-level semantic facts only**, containing at
minimum:

- **cursor identity**: the architectural address register `An` that holds the
  output cursor;
- **compatible advancing-operation set**: a bounded deterministic collection of
  one or more decoded operations, each with its own exact
  provenance/source-address and IR identity, each of whose CPU-owned
  `M68kOperationEffect` register-write footprint is complete
  (`register_write_footprint_complete == true`, with the `An` auto-update in
  `address_register_write_mask`). Public Sonic decompression routines contain
  several distinct output-write sites (including separate `(a1)+` writers)
  advancing one destination cursor, so a single-static-writer requirement would
  be unsound; the set is bounded by the already-accepted ADR 0016
  covered-instruction / analysis-resource caps, with no new image-tuned capacity.
  Every member must share: the same `An` cursor; the same admitted direction; the
  same fixed decoded step; the generated-routed-write property; the same
  admissible region.
- **direction and step**: `postincrement` or `predecrement`, with a fixed nonzero
  step equal to the decoded access width (1/2/4), including the MC68000 A7-byte
  step-by-2 rule; identical across all members.
- **routed-write identity**: every member is a generated **routed write**
  (`genesis_route_access`) whose successful completion is a precondition of that
  member's cursor commit -- a failed/`GENESIS_STOP` access reaches the note
  emission point on no path.
- **admissible region**: the only initially admissible progress region is the
  statically known finite Genesis synthetic work-RAM region
  (`GENESIS_REGION_SYNTHETIC_WORK_RAM`, `work_ram[65536]`). ADR 0006
  build-time-owned immutable cartridge regions and any MMIO/device region are not
  admissible.
- **fail-closed**: a write to the proof cursor `An` that is neither an admitted
  compatible advancing writer nor a harmless effect proven not to alter that `An`
  invalidates the proof; a member with a different direction, a different or
  non-fixed step, or incomplete CPU effect metadata not completed under the
  effect-metadata completion rule below invalidates the proof; calls, unsupported
  operations, ambiguous CFG, or cap exhaustion produce no fact.
- **shared proof_id**: all compatible writer sites in one proof emit the SAME
  deterministic `proof_id` so the runtime low-water rule observes one continuous
  stream (`remaining_bound` strictly decreasing across writer A, writer B, writer
  A, ...). Per-writer `proof_id` assignment is forbidden -- alternating sites
  would repeatedly replace the runtime slot and could starve a continuously
  advancing cursor of progress credit.

The proof does **not** attempt to establish the concrete run-time start/end
address or range of the dynamic `(An)+` / `-(An)` cursor. `M68kStaticMemoryFact`
is retained only for statically-foldable effective addresses; dynamic
auto-updating accesses intentionally have no translation-time concrete-address or
region fact and route through `genesis_route_access`. `M68kOperationEffect`
proves register *effects*, not the *value* or *range* of `An`. Establishing that
range statically would require a bounded typed pointer/range fact and producer
that do not exist; this ADR deliberately does not require or authorize one, and
generic pointer/range/data-flow analysis remains excluded. Actual membership of
the live dynamic cursor in the work-RAM region is checked by generated C at
execution time (see "Codegen" below). Signed/mixed-width cursor arithmetic or a
runtime-variable step still fail with no proof.

If, in `write_move` or another auto-updating writer family, the qualifying
operation has already-truthful `data_register_write_mask` /
`address_register_write_mask` values but its `M68kIrKind` is not yet in the
`register_write_footprint_complete` set, the implementation task audits the
CPU-owned effect representation and adds that IR kind to the completeness
declaration (with focused synthetic and, where appropriate, Musashi differential
validation). That metadata completion is implementation work required by this
ADR; it is not a new IR kind and not analyzer-local MOVE/EA semantics.

### Retained-fact lifecycle across the multi-seed Phase-B aggregate

The proof consumed by C4 is based on the **final validated cross-seed aggregate**
program, never blindly unioned from independently incomplete per-seed proofs.
`M68kGeneratedDataProgressProof` is produced or revalidated after the cross-seed
decoded / IR / block / edge / frame aggregate is formed, retained on the final
`FrontendAnalysis` / `FrontendPartialProgram.accepted_prefix` representation
through an explicit typed field, and independently revalidated by C4 -- each
compatible advancing-operation member too -- against that same retained aggregate
before note emission. Complete proof identity / dedup key = the ordered
canonicalized full set of advancing-operation identities together with the common
cursor / direction / step / region properties; exact duplicate proofs deduplicate
deterministically by that key; inconsistent facts fail closed. This preserves the
SEG-007-T151 cross-seed completeness discipline (no per-seed visibility loss).

### Representation of postincrement / predecrement

The proof records the direction as an enum and the step as the fixed decoded
width, shared by all compatible members. `postincrement` means the routed write
happens at the pre-update `An` and the commit stores `An + step`; `predecrement`
means the commit stores `An - step` and the routed write happens at the
post-update `An`. Each member mirrors the existing SEG-007-T145 add-family
deferred single-address-register-commit path; the proof reuses that decoded EA
auto-update fact rather than modelling it a second time. When a proof has several
compatible members, each writer site independently performs this
write-then-deferred-commit and then emits its own guarded note with the shared
`proof_id`.

### Codegen (guarded note emission)

`runtime/genesis/runtime.h` adds a distinct typed note carrying a
**direction-independent decreasing bound**:

```
GenesisDataProgressNote { uint8_t present; uint32_t proof_id; uint32_t remaining_bound; }
```

as a further trailing zero-initialised `GenesisRuntime` field (every existing
`GenesisRuntime runtime = {0}` construction leaves it inert), plus
`void genesis_note_data_progress(GenesisRuntime *runtime, uint32_t proof_id, uint32_t remaining_bound);`
(NULL-safe, stores `{1, proof_id, remaining_bound}` and nothing else). The
runtime does not own the static proof or its direction and never infers direction
from `proof_id`; generated C converts the proof direction into one quantity that
always strictly **decreases** on genuine progress:

- post-increment: `remaining_bound = region_end - committed_cursor`;
- pre-decrement:  `remaining_bound = committed_cursor - region_begin`;

computed with widened arithmetic under the live region/no-wrap guards.

Only the general-startup runtime block emitter (the sole path that already sets
`M68kMemoryEmissionContext::loop_progress_object`) emits the call. It is emitted
**only after** the routed write has completed successfully (it is textually
dominated by the routed-access success continuation and by every `GENESIS_STOP`
early return) and **only after** the deferred `An` cursor commit statement. All
of these live-state conditions must hold or no note is emitted: the actual routed
write address and width lie entirely within the work-RAM region; the pre-access
and committed cursor values both satisfy the work-RAM bounds and the no-wrap
guard; `remaining_bound` is representable in `uint32_t`; and the committed cursor
equals the pre-access cursor adjusted by the decoded auto-update direction/step.
A failed or non-work-RAM access emits no note. This is a generated-C live-state
guard consuming a typed build-time CPU semantic fact -- not runtime opcode
inference and not a new pointer-analysis architecture. When `loop_progress_object`
is empty the emitted text is byte-for-byte unchanged from today. Deterministic
strict-C11 output is preserved.

### Watchdog semantics (`genesis_runtime_drive`, ADR 0007 extension)

`genesis_runtime_drive` gains one additional deterministic local slot for data
progress, structurally identical to the existing loop-progress slot:
`data_slot_active`, `data_slot_proof_id`, and a single `data_slot_low_water`
holding the smallest `remaining_bound` seen for the active proof. There is **no**
direction-dependent high-water/low-water branch -- the direction is already
normalised away in generated C. Per dispatch step, after clearing both note
`present` flags and dispatching:

- a first observation of a data note, or a note whose `proof_id` does **not**
  match the active slot, **establishes / replaces** the slot
  (`data_slot_low_water = remaining_bound`) and is **not** progress;
- a note whose `proof_id` matches the slot and whose `remaining_bound` is
  **strictly smaller** than `data_slot_low_water` lowers the low-water mark,
  consumes exactly one unit of the shared global `progress_credit` if it is
  nonzero, and counts as progress;
- an equal or larger `remaining_bound` (same offset rewritten, cursor reset,
  cursor rewound, re-entry) is **not** progress;
- `present == 0` is not progress and does not clear the slot.

`loop_progress` and `data_progress` keep distinct typed notes and distinct
low-water slots but share the one unchanged never-replenished
`progress_credit = GENESIS_WATCHDOG_MAX_PROGRESS_CREDIT`. If a single dispatch
step validly produces strict progress in **both** families, the driver consumes
**at most one** unit of shared global credit for that dispatch and resets
`steps_since_progress` **once**, under a deterministic family processing order
(loop-progress evaluated before data-progress). No dispatch can consume two
credit units. The `steps_since_progress` / `W` mechanism and the
`instruction_budget_exhausted` fail-closed stop are unchanged, so no combination
of loop notes and data notes can make a run unbounded.

## Why the proof is not renewable without bound

- **Cursor reset / rewind inside an infinite outer loop.** The data slot keeps
  its `proof_id` and its `data_slot_low_water`; a reset or rewound cursor
  produces a `remaining_bound` that is not strictly smaller than the low-water
  mark, so it is never credited, `steps_since_progress` reaches `W`, and the run
  fails closed.
- **Same output byte rewritten repeatedly.** `remaining_bound` does not strictly
  decrease; no credit; fail closed at `W`.
- **Cursor leaves the region / wrap.** The generated-C live region/no-wrap guard
  fails, so no note is emitted at all; fail closed at `W`.
- **Two data-transform loops alternated by an infinite outer loop.** Slot
  replacement lets each re-credit, but every credited step decrements the global
  never-replenished `progress_credit` (shared with loop-progress); once it hits
  0 no step is credited and the run fails closed.
- **Genuine forward progress through the finite region.** Bounded by
  `region_size / step` strict advances per instance before the cursor exits the
  region and stops producing notes -- a finite quantity -- and additionally
  bounded by the global credit ceiling.
- **Ordinary reads / unrelated RAM writes / cartridge reads.** No proof, no
  note; they never count as progress.

## Soundness properties (all preserved)

1. Progress is proven by build-time CPU/static/codegen facts
   (`M68kGeneratedDataProgressProof`, CPU-owned `M68kOperationEffect` footprint,
   decoded EA auto-update, static region proof); the runtime never infers it
   from opcode / PC / routine name / byte pattern / title-specific address.
2. `genesis_note_data_progress` is textually dominated by the routed-access
   success continuation and every `GENESIS_STOP` early return; a failed routed
   access is credited nothing.
3. The proof binds a finite statically proven region; the initial admissible set
   is exactly `GENESIS_REGION_SYNTHETIC_WORK_RAM` (`work_ram[65536]`).
4. The cursor moves strictly monotonically in one admitted direction
   (`postincrement` increasing or `predecrement` decreasing) with a fixed
   nonzero width step; non-strict motion is not progress.
5. Reinit / rewind / repetition / wrap / region escape / ambiguous alias /
   unknown write footprint / an unproven or incompatible writer of the proof `An`
   all either produce no proof or produce a note whose `remaining_bound` does not
   strictly decrease (or no note at all, when a generated-C live guard fails), and
   therefore yield no progress.
6. Ordinary ROM reads and ordinary RAM writes carry no proof and are never
   credited; only the bounded set of proven compatible advancing cursor writers,
   each at its own provenance, is.
7. Runtime accounting is opcode-agnostic and direction-agnostic: the watchdog
   reads only `{present, proof_id, remaining_bound}` and checks for a strictly
   smaller `remaining_bound` under a matching `proof_id`.
8. ADR 0007's global `progress_credit` ceiling is unchanged, never replenished,
   and shared by both note families; no sequence of certificates makes execution
   unbounded.
9. The mechanism never proves the decompression format / compressed stream /
   algorithm terminates; it credits demonstrated finite forward motion only.
10. No interpreter, JIT, runtime opcode fetch/decode, or second dispatcher is
    added; the generic driver consumes only the emitted note.
11. No new Sonic-specific fused `M68kIrKind` and no decompressor semantic owner
    is introduced; the proof is generic over any postincrement/predecrement
    cursor written by a bounded set of compatible routed-write sites into a finite
    owned region (decompression, block copies, fills, table expansion, similar
    transforms) and never counts arbitrary writes.

## Concrete answers

- **Static fact proving cursor + region**: `M68kGeneratedDataProgressProof` as
  above -- one `An` cursor, one admitted direction, one fixed width step, a bounded
  deterministic set of one or more compatible generated-routed-write advancing
  operations (each with a complete CPU register-write footprint and its own
  provenance/IR identity), and the statically known finite work-RAM region as the
  only admissible target. It does **not** prove the concrete dynamic cursor
  address/range; live membership is a generated-C runtime guard.
- **Reused CPU-owned effect facts**: `M68kOperationEffect`
  `register_write_footprint_complete`, `data_register_write_mask`,
  `address_register_write_mask`, the decoded EA postincrement/predecrement
  auto-update, and `affects_condition_codes` (all in
  `include/segarecomp/cpu/m68k/effects.hpp` / `src/cpu/m68k/effects.cpp`); the
  bounded fixed-point walk and caps from `src/cpu/m68k/static_loop_proof.cpp`.
  Minimal extension: a new producer function
  (`m68k_prove_generated_data_progress`), the typed fact struct, an explicit
  typed retained field on `FrontendAnalysis` / `FrontendPartialProgram.accepted_prefix`,
  and -- where the qualifying writer's `M68kIrKind` (e.g. `write_move`) has
  truthful register-write masks but is not yet declared
  `register_write_footprint_complete` -- adding that existing IR kind to the
  completeness declaration. No new pointer/range fact or producer.
- **Postincrement / predecrement**: direction enum + fixed decoded width step;
  reuses the SEG-007-T145 deferred single-`An`-commit lowering path (predecrement
  before / postincrement after the routed access; commit exactly once, strictly
  after every routed access; A7-byte step-by-2). Generated C then normalises the
  direction into the single always-decreasing `remaining_bound`.
- **When generated C emits the note**: at each compatible writer site, strictly
  after that site's routed-write success continuation and strictly after that
  site's `An` commit statement, guarded on live work-RAM cursor state, through
  `genesis_note_data_progress` with the shared `proof_id` at the
  `genesis_note_loop_backedge` seam (`src/codegen/c11/m68k.cpp`).
- **Cursor reset / rewind**: produces a `remaining_bound` that is not strictly
  smaller -> no progress -> fail closed at `W`.
- **Re-entry vs strict progress**: `data_slot_low_water` only moves down on a
  strictly-smaller `remaining_bound` with the matching `proof_id`; re-entry
  re-supplies a bound equal to or above the low-water mark.
- **Wrap prevention**: generated-C live guard requires the routed write extent
  and the pre-access and committed cursor to stay within `[region_begin,
  region_end]` with no unsigned wrap, and `remaining_bound` to be representable
  in `uint32_t`; failure emits no note.
- **Remaining-bound quantity consumed by the runtime**: the direction-normalised
  `remaining_bound` (`region_end - cursor` for post-increment, `cursor -
  region_begin` for pre-decrement); the runtime keeps only the smallest value
  seen and credits a strict decrease.
- **One mechanism or two**: one accounting mechanism (shared `W`,
  `steps_since_progress`, and the single never-replenished global
  `progress_credit`, with at most one credit unit per dispatch across both
  families); two distinct typed notes with distinct guards and distinct
  low-water slots.
- **Initially admissible regions**: only `GENESIS_REGION_SYNTHETIC_WORK_RAM`.
- **Build-time caps**: the ADR 0016 constants verbatim
  (`M68K_LOOP_PROOF_MAX_*`); no Sonic tuning; cap exhaustion -> no fact.
- **Adversarial cases that must fail closed**: failed routed write; cursor
  reset; cursor rewind; wrap / region escape; repeated same-offset write;
  ambiguous/unknown cursor write footprint; an unproven or incompatible writer of
  the proof `An` (different direction/step, incomplete footprint, non-routed);
  unrelated ordinary RAM writes; ROM reads only; unbounded re-entry (global
  ceiling); existing ADR 0016 finite-loop behaviour unchanged. Also required: a
  positive compatible-multi-writer case (two alternating compatible writer
  instructions on one `An`, same `proof_id`, one strictly-decreasing
  `remaining_bound` stream, execution beyond the no-progress window) that fails
  closed once one writer is given a different direction/step or an unproven `An`
  write is introduced.
- **Typed fact shape**: `M68kGeneratedDataProgressProof` holds a bounded
  deterministic collection of advancing-operation identities plus the common
  cursor / direction / step / region properties (not exactly one advancing
  operation). Complete proof identity / dedup key = the ordered canonicalized full
  identity set plus those common properties.
- **ABI / struct additions**: `GenesisDataProgressNote { present, proof_id,
  remaining_bound }` field on `GenesisRuntime`, `genesis_note_data_progress`, one
  added local slot group (`data_slot_active`, `data_slot_proof_id`,
  `data_slot_low_water`) in `genesis_runtime_drive`, and one explicit typed
  retained-proof field on `FrontendAnalysis` / `FrontendPartialProgram.accepted_prefix`.
  No wire schema, stop, or diagnostic enum change.
- **CPU effect-metadata completion**: if the qualifying advancing operation's
  `M68kIrKind` has truthful register-write masks but is not yet
  `register_write_footprint_complete`, the implementation task adds it to that
  declaration with focused positive/adversarial synthetic and, where
  appropriate, Musashi differential validation -- implementation work required by
  this ADR, not another refinement, and not analyzer-local MOVE/EA semantics.
- **Cross-seed lifecycle**: the proof (and each compatible member) is
  produced/revalidated against the final validated cross-seed aggregate, retained
  through the explicit typed field, and revalidated by C4 against that aggregate
  before emission; exact duplicates deduplicate by the ordered-identity-set dedup
  key; inconsistent facts fail closed.
- **Genericity**: the proof describes a monotonic width-stepped cursor written by
  a bounded set of compatible routed-write sites into a finite owned region; it is
  blind to what the bytes mean, so decompression, memcpy-style copies, fills, and
  table expansion all qualify identically, and no path credits a write that is not
  one of the proven compatible advancing cursor writers.

## Consequences

- A generated data-transform loop with a data-dependent, statically unprovable
  control trip count now runs to completion through static emitted dispatch when
  it demonstrably advances a proven cursor through finite work RAM, by consuming
  bounded shared progress credit; the generic runtime still decodes no target
  opcode.
- A non-advancing transform, a reset/rewound cursor, a region escape, and
  unbounded re-entry all still stop deterministically at the unchanged
  `instruction_budget_exhausted` / `instruction_budget_exhausted` pair.
- ADR 0007's window `W`, its global never-renewable ceiling, ADR 0016's family
  and caps, ADR 0006's cartridge-data model, and ADR 0013's four-seed /
  four-round bound are all untouched.
- A later task wanting a different progress family (a computed-index scatter
  cursor, a second admissible region, a non-`An` cursor) must emit its own
  CPU/codegen-proven typed note through the same seam and existing semantic
  owner, or record why that is unsound; it must not reintroduce a fixed total
  cutoff or a runtime opcode decoder.
- If, after this mechanism lands and the Sonic Phase-B route is re-executed, the
  next terminal is a fresh `discovery_prefix_boundary` that ADR 0013 cannot
  promote because the four-seed / four-round bound is exhausted, that is a
  separate executed Phase-B scalability frontier requiring its own full
  refinement (monotonic accumulated discovered-program state, not more retained
  seeds); it is explicitly not a `4 -> 8` retune inside the implementation task.
