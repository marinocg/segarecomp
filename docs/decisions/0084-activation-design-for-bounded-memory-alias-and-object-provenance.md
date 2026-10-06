# ADR 0084: Activation Design for Bounded Memory/Alias/Object Provenance (SEG-037): CONTINUE

- Status: Accepted (decision: **CONTINUE** to SEG-037-T002, narrowly scoped to the mechanisms identified below. No primitive is
  implemented by this ADR; it pre-registers the design and the thresholds every later SEG-037 child is bound by.)
- Date: 2026-10-06
- Task: SEG-037-T001 (first task of the SEG-037 combined delivery).
- Related, unchanged and not bypassed: ADR 0079 (M68K analysis instantiation: the address/memory/contexts/frames domains), ADR 0080
  (hybrid admission; decision 11 is the unchanged production-default rule; decision 12 names the generic precision-blocker classes),
  ADR 0081 (SEG-034: bounded precision refinements, all stopped except one defect fix), ADR 0082 (SEG-035: bounded immutable
  pointer-table authority, STOP), ADR 0078 (generic abstract-analysis core and CPU adapters: `libs/analysis`).

## Question

SEG-037 asks whether a bounded, sound, game-independent memory/alias/object-provenance model can remove enough of the whole-image
fallback triggers that currently collapse SEG-031 hybrid admission to broad AOT on every authorized title. SEG-037-T001 does not
build that model. It regenerates the baseline, runs a cheap optimistic ceiling experiment with the private diagnostics SEG-034/035
already built, and either stops the whole research track or pre-registers the activation design and every later threshold.

## 0. What already exists (read before proposing anything new)

The CPU-owned M68K address and memory domains (`libs/cpu/m68k/analysis`, SEG-030, ADR 0079) already implement most of the
progression the SEG-037 milestone record describes as a future staged design:

- **Region model**: `M68kRegionKind {image, mutable_ram, io_device}`, extents supplied by the machine view (Genesis-owned). This is
  "machine memory regions", already built.
- **Bounded pointer regions/offsets**: an address register holds a *bounded points-to value* — bottom, at most
  `m68k_points_to_bound` `(region, offset-set)` pairs, or Unknown with a reason. An offset set is an exact set (at most
  `m68k_exact_offset_bound` offsets) or a `{lo, stride, hi}` congruence, always clamped to `[0, size]` of its region. This is
  "fixed/provenance-derived abstract locations" plus "bounded pointer regions/offsets", already built.
- **Structural alias exclusion and strong/weak update**: `M68kCell {kind, id, offset, width}` plus `M68kCellValue {data, pointer}`.
  A store to one physical cell is a strong update; a store to any other *known* target is a weak update that only touches the
  cells its target set overlaps (`abstract_memory.hpp`: "a push at a known A7 offset or a store to a distinct exact field leaves
  every disjoint cell intact"). This is "sound may-alias / must-not-alias" and "strong or weak update", already built, for known
  targets.
- **Activation-relative stack deltas**: SEG-030-T005 already tracks the stack delta relative to the activation entry, not an
  absolute A7 (ADR 0081, SEG-034-T003 record: "the tracked deltas are already relative to the activation entry").
- **Finite mutable-cell values**: an `M68kCellValue` already carries a precise finite value domain (`analysis::FiniteValue`) when a
  cell is strongly known; this is "finite mutable-cell values", already built for *provably single-writer* cells.

**Correction (checked directly against `address_value.hpp` and `abstract_memory.cpp` before writing decision 1): the stack is
explicitly *not* a region of its own.** `address_value.hpp`: "A region is one contiguous extent of 32-bit register values whose
24-bit bus addresses fall inside one machine region... The stack is not a region of its own: it is mutable RAM." `M68kRegionKind`
has exactly three members (`image`, `mutable_ram`, `io_device`), and only `mutable_ram` is tracked as cells at all
(`m68k_memory_tracked`). This means a coarse `(region known, offset Unknown)` point is **not a useful new lattice point for any of
the named consumers**: every tracked cell already lives in the single `mutable_ram` region, so "region = mutable_ram, offset
Unknown" has *exactly* the same poisoning footprint as the existing top (`M68kPointsTo::unknown()`) — it excludes nothing a plain
Unknown target does not already exclude (both poison every `mutable_ram` cell, because there is only one region kind to poison).
A fake "stack region" distinct from `mutable_ram` would be unsound invention (the machine view defines regions, and the stack is a
real, already-decided `mutable_ram` fact, ADR 0079 decision 4) and is explicitly rejected. **Decision 1 below is revised to not
rely on any region-only bucket.**

**What the lattice actually already supports, and where the real lever is.** `m68k_memory_store` (`abstract_memory.cpp`) already
separates a target's exact-offset component, its strided/congruence component, and its "whole" (top) component, and only the
"whole" component triggers `store_poison`; an exact or strided (bounded congruence) component only touches the cells
`for_overlapping` reports. So a store whose address value is *already* a bounded `M68kPointsTo` (an exact offset set, or the sound
`{lo, stride, hi}` hull `M68kOffsetSet::strided` produces when an exact set would exceed `m68k_exact_offset_bound`) is **already**
excluded, by existing code, from every cell outside that bounded range — this is not new. The actual, narrow gap is: for the
specific stores that currently evaluate to the full top (`M68kPointsTo::unknown()`, not a bounded congruence) and are the ones
poisoning a named consumer cell (an interrupt handler's own save slot, or a computed RTS's relative-A7 return/caller slot), can
*additional, sound* M68K provenance propagation — using only the lattice types that already exist (no new region, no new offset
form) — recover a bounded (exact or strided) `M68kPointsTo` instead of top, wherever one is actually derivable from the store's
own arithmetic chain? There is already one concrete precedent that this pattern is real and not merely hypothetical: ADR 0081
decision 2 (`M68kFiniteAdapter::resolve_store_spill`) found exactly one store that was over-eagerly collapsing to a poisoning
"whole region" target when a bounded clip-plus-landing form was actually sound, and retaining that bound removed one real
whole-image trigger (14 -> 13 on Sonic 1). T002 generalizes the search for *that specific pattern* — an over-eager collapse to
top where the existing lattice's bounded forms were in fact derivable — to the named consumers below. It does **not** claim this
pattern recurs; whether it does is exactly T002's hard-stop question.

## 1. Smallest useful abstract-object/location model (what T002 implements)

T002 does **not** build a new region, a new offset-set form, or a new alias rule. It is a bounded, targeted propagation pass: for
each store instruction that is a measured contributor to a named consumer's poisoning (section 8) and whose address value
currently evaluates to `M68kPointsTo::unknown()`, trace its arithmetic chain (the same `LEA` / `ADDA` / `SUBA` / address-register
`MOVE` chain the address domain already understands) one or more steps further than today's propagation does, and keep the
*already-existing* bounded form (an exact offset set, or a `strided` congruence clamped to the region extent) wherever that chain
is actually sound and derivable — never inventing a bound that is not implied by the existing arithmetic facts, never collapsing
a genuinely unconstrained origin (a RAM cell with no established finite domain used directly as a pointer, structurally the same
"no finite value domain" class ADR 0081/0082 already found for the width-only object-id dispatch byte) into a fabricated bounded
form. Where the chain's origin is itself unconstrained, the result stays `M68kPointsTo::unknown()` and the store still poisons —
this is the honest, fail-closed outcome, and it is the T002 hard-stop trigger (see the pre-registered thresholds) when it is true
for every instance of a named consumer on every title.

## 2. Identity creation without game-specific knowledge

An abstract location may be created only from: (a) a statically known region/base supplied by the Genesis machine view
(`region_of`); (b) a pointer value produced by bounded M68K register/address arithmetic (`LEA`, `ADDA`/`SUBA` of an immediate or a
finite-set value, address-register `MOVE`) whose source was already region-known; (c) the activation-relative A7 base
(SEG-030-T005) plus a relative delta; (d) a finite points-to origin already proven by the existing address domain, including a
pointer value read from an `M68kCellValue.pointer`-tagged memory cell. No identity may be created from a label, a decompiled
symbol name, a disassembly comment, or any title-specific address/offset/table list. This is unchanged from the milestone record;
T001 adds no new origin, and (per the correction above) never a synthetic "stack region" distinct from `mutable_ram`.

## 3. Alias representation

Unchanged from the existing CPU-owned domain (`abstract_memory.hpp`, `address_value.hpp`): may-alias is "target region/offset sets
overlap"; must-not-alias is "target region/offset sets are disjoint, both known (exact or strided, same region or different
regions)"; `Unknown` is the top (no information, poisons every tracked cell). T002 adds no new point to this lattice — it only
widens, for specific named stores, how often a bounded (non-top) value is actually reached before falling back to Unknown, using
forms (exact set, strided congruence) that already exist and whose must-not-alias semantics are already sound and already
implemented in `for_overlapping`.

## 4. Strong vs. weak update rule

Unchanged (`abstract_memory.hpp`, already decided by SEG-030/ADR 0079): an exact single-cell target is a strong update; any other
known (exact-multi or strided) target is a weak update over every cell its target set overlaps (`for_overlapping`); a target that
leaves `[0, size]` of its region, or crosses a mirror boundary, drops the whole region; a target with no bounded form at all
(top) poisons every tracked (`mutable_ram`) cell (`store_poison`). T002 changes no rule here — it only changes how often a store
reaches the "known (exact-multi or strided)" case instead of the "top" case, for the named consumer stores.

## 5. External/Z80/DMA writer treatment

Unchanged, Genesis-machine-owned (ADR 0079 decision 7, `M68kMemoryPolicy`): `external_writer` makes every work-RAM read Unknown;
`async_all` / `async` ranges remove cells independent of any M68K provenance fact. SEG-037 adds no M68K-generic model of an
external writer; the machine supplies the policy exactly as today. A bounded (non-top) store recovered by T002 never narrows an
externally-owned cell's policy; the machine policy always takes precedence over any points-to refinement.

## 6. Activation-relative stack objects

Consumes, rather than replaces, the existing SEG-030-T005 activation-relative stack delta. A stack object is identified as
`(activation entry point, relative A7 delta at the identifying instruction, slot width)`. Two activation-relative stack objects of
different activations are never the same identity (no cross-activation confusion, decision 9's corpus must test this explicitly).
T004 (not T002) is the task that builds the symbolic relative-A7 return cell this enables; T002 only supplies, where derivable, a
bounded (exact or strided) `M68kPointsTo` for the specific stores that would otherwise poison that cell as `top`, so that the
already-existing `for_overlapping` exclusion (decision 3/4) can apply to them instead of `store_poison`.

## 7. Finite termination/resource bounds

No new bound is introduced. T002-T005 reuse the existing bounds unchanged: `m68k_points_to_bound`, `m68k_exact_offset_bound`,
`m68k_strided_growth_bound`, `m68k_memory_cell_bound`, the solver's `max_iterations` / `max_points` (ADR 0079 decision 9/11,
CLI `--max-iterations` / `--max-points`). Exceeding any bound yields `Unknown(set_bound)` / `Unknown(resource_bound)`, never a
silently narrower or wrong answer. No child may raise an existing bound to force convergence (milestone Non-goals); a child that
needs a materially larger bound to show any effect must say so and stop rather than widen it.

## 8. Named existing SEG-034 consumer sites

Every new primitive below must have a named consumer among the sites ADR 0081 and ADR 0080 decision 12 already enumerate. No
primitive without one of these is built:

| primitive | consumer site(s) | titles with a measured instance |
| --- | --- | --- |
| targeted bounded-provenance recovery (decision 1: recover an existing-lattice exact/strided `M68kPointsTo` instead of top, never a new region) | the MOVEM save/restore slot of an unanalysed interrupt handler; the symbolic relative-A7 return cell of a computed RTS | Sonic 1 (`interrupt_resumption` RTE 2, `stack_unbalanced` 5, `base_unknown` 2), Sonic 2 (`interrupt_resumption` 3, computed RTS 2), Cool Spot (`interrupt_resumption` 8, computed RTS 14) |
| activation-relative return cell (T004) | the relative-A7 idiom: a routine pops its own return address, builds or reuses the caller frame, and returns (`rts_computed/.../stack_unbalanced`) | Sonic 1, Sonic 2, Cool Spot (ADR 0081 SEG-034-T003: "the same skip idiom" on all three) |
| finite mutable-cell domain (T003) | a PC-indexed dispatch whose index register is a small mode/state counter, not a byte read from level/object data (`pc_index_explicit/.../interrupt_resumption_unproven`) | Sonic 1 only measured so far (the game-mode dispatch index lost across the first interrupt boundary); **no consumer observed yet on Sonic 2 / Cool Spot / OutRun / Streets of Rage / Golden Axe** — record this explicitly, do not invent one |
| bounded record/object field (T005, conditional) | the object-dispatcher RAM-indexed table (`A1 <- ROM table[object-id byte in RAM]; JSR (A1)`) | Sonic 1 (width-only 9 of 23 ceiling sites), structurally present wherever width-only PC-indexed dispatch dominates the ceiling; **this is the mechanism SEG-035/ADR 0082 already evaluated and stopped for its specific sub-problem (bounded immutable pointer-table authority) — T005 may only proceed if T002-T004 change what is known about the RAM array's *base/stride*, never its *value domain*, which stays a non-goal** |

No primitive is proposed with zero named consumers. The finite mutable-cell-domain primitive (T003) is the one with the
narrowest confirmed cross-title footprint; SEG-037-T003 must re-check Sonic 2/Cool Spot/OutRun/Streets of Rage/Golden Axe for an
equivalent small-mode-counter dispatch before claiming the primitive is exercised there, and must record "no consumer observed"
rather than skip the check if none is found.

## 9. Required synthetic soundness corpus (before any primitive is trusted on a real title)

Each primitive above needs project-authored fixtures and mutants covering, at minimum:

- a disjoint-unknown-base writer (an Unknown-target store whose provenance chain never touches the region under test: must not
  poison it);
- an overlapping/partial write (a store whose offset set partially overlaps the tracked cell: must weak-update, never strong);
- a join across two paths with different offset sets (must keep only the intersection, never grow silently);
- a loop that re-derives the same bounded (exact or strided) pointer every iteration (must reach a fixed point, never grow the
  offset set past `m68k_strided_growth_bound`);
- a call/return summary boundary (the callee's effect on the tracked region must be summarized, not re-walked, and must not leak
  an activation-relative identity across activations);
- an aliasing adversary: two address registers independently derived to the same region but provably disjoint offset ranges
  (must not merge into a false must-alias);
- cross-activation confusion: two nested or sibling activations of the same routine with the same relative-A7 delta (must be two
  distinct identities, never unified);
- external/asynchronous invalidation arriving between two uses of the same recovered bounded pointer (the machine policy must
  still win over the new recovery, per decision 5);
- a value-set that would exceed `m68k_exact_offset_bound` or `m68k_points_to_bound` (must cap to the existing stride/Unknown
  form, never silently truncate to a wrong exact set);
- an index that exceeds the domain a finite mutable-cell tracker has proven (must fail closed to Unknown, never wrap or clamp to
  an in-range guess).

No primitive may be exercised on a real ROM before every one of these fixtures exists and every associated mutant is killed
(the SEG-034/035 precedent: `analysis_m68k_precision_test`-style fixtures plus resolution-disabled / value-joined / landing-dropped
mutants).

## 10. Measured outcome that justifies the complexity

The same number used for the research-success threshold below (section "Pre-registered thresholds"), not a softer one: at least
one authorized title must show a **>= 20% reduction in its whole-image fallback trigger count**, or **`hybrid_total / U` strictly
below 0.95** (i.e., a real, non-degenerate hybrid plan, not a one-trigger-fewer still-broad result), with zero observed escapes
and no title-specific logic. This is deliberately independent of ADR 0080 decision 11's own, stricter production-default rule
(-30% generated C **and** -25% compile CPU on at least two complete-oracle titles); SEG-037 research can succeed under this
threshold without ever changing the production default.

## Ownership split (concrete paths)

```text
libs/analysis/include/segarecomp/analysis/{finite_value.hpp,solver.hpp}   generic finite-domain / fixed-point core (SEG-029)
libs/cpu/m68k/analysis/{address_value,abstract_memory,frames,finite_adapter}.{hpp,cpp}
    M68K addressing semantics, points-to regions/offsets, cell strong/weak update, A7/stack effects, MOVEM/LINK/UNLK/RTE semantics
      -> the targeted bounded-provenance recovery (decision 1) attaches here, as additional propagation through the existing
         `M68kPointsTo`/`M68kOffsetSet` chain, next to `M68kAnalysisSubReason::base_unknown` and `store_poison` -- never a new
         region kind, never a new offset-set form
platforms/genesis/machine/{frontend,hybrid_admission,reachability_challenger}.cpp
    bus/memory region extents, cartridge/RAM/device topology, external-writer policy (Z80/DMA), hybrid admission closure
platforms/genesis/analysis_report/src/hybrid_plan.cpp
    unchanged: consumes the CPU analysis; no new report-only logic is added by T001
```

No new top-level directory is created. T002's recovery pass is a CPU-owned (`libs/cpu/m68k/analysis`) widening of how often the
*existing* `M68kPointsTo`/`M68kOffsetSet`/`M68kCell` types reach a bounded value instead of top for the named stores, not a new
type and not a new generic abstraction in `libs/analysis`, because no second CPU adapter exists to justify genericity yet
(ADR 0078's own rule).

## Pre-registered thresholds (verbatim; not moved after seeing later results)

- **Research-success threshold (overall, independent of the production default).** SEG-037 research is successful if, on at least
  one authorized title, a primitive built under this ADR produces a plan with zero observed escapes, identical guest behaviour on
  the oracle workload, deterministic output, no title-specific logic, **and** either (a) the whole-image fallback trigger count
  drops by at least 20% from this ADR's confirmed baseline, or (b) `hybrid_total / U` on that title drops below 0.95. Falling short
  of this on every title is a negative result, honestly reported as such; it does not retroactively justify a smaller "progress"
  claim.
- **T002 hard-stop condition.** For each named consumer store (section 8) that currently evaluates to `M68kPointsTo::unknown()`
  (top), trace its arithmetic chain using only the existing address-domain rules (decision 2). T002 stops before T003/T004 if, on
  every title with a named consumer, every such store's chain bottoms out at a genuinely unconstrained origin — a value with no
  established finite or points-to domain at its source (the same "no finite value domain" class already found for the width-only
  object-id dispatch byte, ADR 0081/0082), so that no sound exact-or-strided `M68kPointsTo` is derivable and the only way to
  exclude the store from the consumer cell would be to *assume* (not prove) disjointness — i.e., the "named premise" SEG-034
  already recorded and declined to adopt as a trust decision. Needing a general heap/symbolic machinery (an unbounded number of
  distinct runtime object identities, or a theorem-prover-grade constraint system) to make the same proof is the same hard-stop.
  Finding, for at least one named consumer on at least one title, a store whose chain *is* already sound and derivable under the
  existing rules but is currently discarded to top (the same class of defect ADR 0081 decision 2 already fixed once) rebuts the
  hard-stop for that instance and T002 continues.
- **T004 stop condition.** T004 (activation-relative stack objects / computed-return integrity) stops if resolving the
  relative-A7 return cell for a real title's `stack_unbalanced` sites requires a general stack theorem prover (unbounded symbolic
  reasoning about arbitrary stack-pointer arithmetic sequences) rather than the bounded `(activation entry, relative delta, slot
  width)` identity of decision 6 plus whatever bounded-provenance recovery T002 actually delivered (never a fake region, never
  an assumed-disjoint premise).
- **T005 activation condition.** T005 (bounded record/object-field provenance) may start only if T002-T004 together have already
  produced, on at least one title, a *proven* bounded base and stride for a RAM record array consumed by a named whole-image
  trigger (section 8) — never merely "the index is read from RAM" and never a claim about the record's *field value domain*
  (which stays excluded; SEG-035/ADR 0082 already found no finite value-domain bound for the measured object-id field, and T005
  does not reopen that question). Absent that, T005 is cancelled with this condition recorded as unmet.
- **T006 A/B/C classification rule and single-refinement permission.** T006 classifies the fresh full-corpus closure measurement:
  **A** (selective) if the research-success threshold above is met on at least one title with zero escapes; **B** (near miss) if
  the whole-image trigger count drops but stays above the research-success threshold on every title, **and** the remaining
  triggers after the round are attributable to **one coherent generic cause** (the same named sub-reason, e.g. every remaining
  trigger is `base_unknown` from the same unresolved mechanism) **with a bounded correction already scoped** (a specific, named,
  small change to one existing function, not a new primitive) — in that case, and only that case, exactly one further bounded
  refinement round is permitted; **C** (explosion/STOP) otherwise, including when the closure does not reach a second round, when
  removing one blocker class exposes a strictly larger next-class trigger count (as ADR 0081 already measured for the interrupt
  class), or when the "one coherent cause" test fails because the remaining triggers span more than one sub-reason family.
- **T007 ADR-decision shape.** T007 records two separated answers, never conflated: (1) did SEG-037 research succeed against this
  ADR's research-success threshold (yes/no, per title); (2) does the production default change — only if ADR 0080 decision 11's
  unchanged, stricter multi-title rule (-30% generated C **and** -25% compile CPU on at least two complete-oracle titles, zero
  escapes, no regression beyond +15%) independently passes. A "yes" to (1) does not imply a "yes" to (2); SEG-037 may close as a
  successful research track with no production change.

## Consequences

- T002 is now authorized to proceed (see the harness Evidence record for the ceiling experiment that justifies CONTINUE rather than
  STOP) with the single, narrow primitive of decision 1, against the named consumers of section 8, gated by the synthetic corpus of
  section 9, and bound by the T002 hard-stop above.
- T005 is explicitly conditional and, on current evidence (SEG-035/ADR 0082), is more likely to be cancelled than activated: the
  dominant remaining class after interrupt/computed-return resolution is width-only RAM-indexed object dispatch, whose blocker is a
  value-domain fact (not an alias fact) that this track does not attempt to bound.
- If T002-T004 together remove only the computed-return and interrupt classes and expose the already-measured width-only
  object-dispatch class at the same or larger magnitude (as the uncredited ceiling already shows for the interrupt class alone),
  T006 must classify that as C, not a rounded-up B, per the rule above.
- **Correction recorded during drafting.** An earlier draft of decision 1 proposed a `(region known, offset Unknown)` lattice
  point as the T002 mechanism. Checking `address_value.hpp` directly showed this is vacuous: the stack is explicitly not a region
  of its own (it is `mutable_ram`, the only tracked region kind), so "region = mutable_ram, offset Unknown" poisons exactly the
  same cells as plain Unknown already does. Decision 1 is corrected to the actual available lever: targeted recovery of the
  bounded (exact or strided) `M68kPointsTo` forms the lattice already supports, for the specific stores that currently fall back
  to top. No region, alias rule, or update rule is new or weakened by this ADR.
