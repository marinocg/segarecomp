# ADR 0085: SEG-037 Bounded Memory/Alias/Object Provenance for Selective Admission: STOP

- Status: Accepted (decision: **STOP SEG-037. Keep compact broad AOT (ADR 0083) as the sole production strategy. Research
  success: NO against the pre-registered SEG-037-T001 threshold. Production default change: NO (moot — no title reached a
  measurable benefit to apply ADR 0080 decision 11 to).**)
- Date: 2026-10-06
- Task: SEG-037-T001..T007 (one combined, report-only/design delivery; T003, T004, T005 cancelled by their own
  pre-registered activation gates and never ran).
- Related, unchanged: ADR 0079 (M68K analysis instantiation), ADR 0080 (hybrid admission; decision 11 is the unchanged
  production-default rule), ADR 0081 (SEG-034: one real defect fixed, everything else stopped), ADR 0082 (SEG-035: STOP,
  bounded immutable pointer-table authority), ADR 0083 (SEG-036: compact broad-AOT entry representation, the current sole
  production strategy), ADR 0084 (SEG-037-T001: the activation design this ADR closes out).

## Question

SEG-037 asked whether a bounded, sound, game-independent memory/alias/object-provenance model could remove enough of the
whole-image fallback triggers that collapse SEG-031 hybrid admission to broad AOT on every authorized title, so that hybrid
admission becomes materially selective (non-broad) on at least one real title. ADR 0084 (SEG-037-T001) answered the ten
activation questions, found the only real available lever was *not* a new region/alias abstraction (the stack is not a
separate region; a coarse region-only bucket is vacuous) but a *narrower, already-earned* possibility: targeted recovery of
the M68K address domain's existing bounded `M68kPointsTo` forms for stores that currently collapse to top, named against two
consumer classes (an unanalysed interrupt handler's MOVEM save/restore slot; a computed-RTS's relative-A7 return/caller
slot). ADR 0084 pre-registered the exact go/no-go gates for every later child and decided CONTINUE, narrowly, to SEG-037-T002.

## Outcome

### SEG-037-T001 (done): activation design and baseline

Regenerated the post-SEG-036 six-title `--hybrid-plan` baseline and confirmed it byte-identical to the established record
(Sonic 1 `U` 246,293 / `D` 1,276 / 13 whole-image triggers; Sonic 2 `U` 496,387 / `D` 335 / 5 triggers; Cool Spot `U` 498,276
/ `D` 6,907 / 22 triggers; OutRun/Streets of Rage/Golden Axe `broad_analysis_incomplete`, no credited verdict). Ran the
mandatory cheap ceiling/oracle gate using the existing SEG-034/035 private diagnostics, found a believable (not certain),
cross-title, material path specifically for the computed-return/relative-A7 class, and recorded CONTINUE with every later
threshold pre-registered in ADR 0084.

### SEG-037-T002 (done): hard stop, confirmed with direct trace evidence

Checked ADR 0084's decision 1 against both named consumers on all three titles with a measured instance (Sonic 1, Sonic 2,
Cool Spot) using the already-built `--trace-points`/`--domains all` private diagnostics. Every poisoning store on every title
traced to one of four genuinely-unconstrained-origin classes — never to a store whose own `LEA`/`ADDA`/`SUBA`/address-register-
`MOVE` chain was simply one step short of the existing lattice's bounded forms:

1. the unanalysed-handler root default (`Unknown(unknown_input)` from entry — no established value to chain from at all);
2. deliberate, sound cross-boundary propagation of that same unprovenness (`apply_resumptions`'s `resumption.unproven` join
   into every address register at every boundary where an unanalysed/dead handler could fire);
3. existing, named, non-raiseable resource bounds (Cool Spot's `context_bound` sites);
4. genuine multi-path/cross-domain exclusions that are not a missing arithmetic step: an ambiguous join of two real
   control-flow paths with different net stack effects, and a deliberate exclusion of status-register writes (and
   `LINK`/`UNLK`) from stack-delta tracking because an SR write can switch the active supervisor/user stack pointer — a real
   hardware fact, confirmed by direct inspection of a concrete register-save routine, not an omitted propagation step.

No production or test code changed. This is ADR 0084's own pre-registered hard-stop condition, met exactly, and is recorded
as an honest negative result (the same style as ADR 0082's inventory-zero STOP) rather than stretched into a partial win.

### SEG-037-T003, T004, T005 (cancelled): activation conditions unmet

Each task's own pre-registered activation gate required its predecessor to have produced a usable primitive. T002 produced
none, so T003 (connect T002's memory facts to finite-value analysis), T004 (build the activation-relative return-cell
identity T002 would have fed), and T005 (bounded record/object-field provenance, conditional on T002-T004 jointly) were each
cancelled with their specific unmet condition recorded in their own harness records. None ran any synthetic or production
work. `SEG-037-T006`'s `depends_on` edge was corrected to drop these three cancelled dependencies in the same coherent edit
(per the backlog schema's dependency rule), leaving only `SEG-037-T002`.

### SEG-037-T006 (done): fresh closure rerun, classification C

An independent, from-scratch rerun of `segarecomp-genesis-analysis-report --hybrid-plan` on all six authorized titles (fresh
process per title, Release build, same head as T001/T002) reproduced the baseline exactly on every field, with no drift of
any kind. Deterministic aggregate hashes (sha256 of each title's sanitized aggregate JSON) are recorded in the SEG-037-T006
harness record. Classification: **C**. The research-success threshold (>= 20% whole-image-trigger reduction, or
`hybrid_total/U` < 0.95, on at least one title) was not met on any of the three credited titles — the reduction is exactly
0% and the ratio exactly 1.000000 everywhere. Classification B (a nonzero reduction with one coherent remaining cause) does
not apply either, because nothing was removed to leave a remainder. No second-round refinement was performed; none is
permitted without a nonzero result to refine.

### SEG-037-T007 (this task): production economics, decision, and independent adversarial gate

**Production economics comparison: not applicable.** T007's own Scope runs the broad-vs-hybrid production comparison "if
T006 classified any title A (or produced a non-broad hybrid admission for any title)". T006 classified every title C with
zero reduction; no title produced a non-broad hybrid admission; there is nothing to compare. ADR 0080 decision 11's
multi-title material-benefit/runtime-regression gates are consequently not reached — not because they failed a measurement,
but because no title ever produced a measurable candidate to apply them to.

**Decision 1 — did SEG-037 research succeed?** **NO**, against the SEG-037-T001/ADR 0084 pre-registered threshold (a real,
non-degenerate reduction in whole-image fallback trigger count, or `hybrid_total/U` below 0.95, on at least one title, with
zero escapes and no title-specific logic). The threshold was fixed before T002 ran and was not moved after seeing T002's
result. Zero titles met it.

**Decision 2 — does the production default change?** **NO.** This is moot rather than a failed measurement: ADR 0080
decision 11's stricter rule (generated C >= -30% AND compile CPU >= -25% on at least two complete-oracle titles, zero
escapes, no regression beyond +15%) is a gate on a measured candidate; SEG-037 produced no candidate to measure. ADR 0080
decision 11 and ADR 0083's compact broad-AOT representation remain the unmodified, sole production strategy.

**Final classification: STOP SEG-037. Keep compact broad AOT (ADR 0083) in production.**

## Independent adversarial review

A fresh independent review was run over the final combined head (branch `task/seg-037-t001`, product PR #76, head
`4315f96`). Because the entire product diff across all seven checkpoints is exactly two file additions —
`docs/decisions/0084-activation-design-for-bounded-memory-alias-and-object-provenance.md` and this ADR — with zero lines of
production or test code changed by T002 through T006, the review's purpose narrowed from "falsify a new mechanism" (there
is none) to two sharper questions: (a) is ADR 0084's and T002's hard-stop reasoning actually sound, or does it conceal a
case that should have rebutted the hard-stop and been implemented; and (b) is the exact head clean, green, and mergeable.

**Verdict: PASS.**

- **Reasoning soundness.** The reviewer independently re-read the real source rather than trusting the ADR/record prose,
  and confirmed each of the four claimed mechanisms directly: (1) `address_value.hpp` really does define exactly
  `{image, mutable_ram, io_device}` with the stack explicitly documented as `mutable_ram`, not a separate region, so ADR
  0084's "Correction" rejecting a region-only bucket is factually grounded, not an invented simplification, and did not
  overcorrect — a region-only bucket genuinely adds nothing when there is only one trackable region kind; (2)
  `finite_adapter.cpp`'s `apply_resumptions` really does deliberately set every address register to
  `Unknown(unknown_input, interrupt_resumption_unproven)` on an unproven resumption, confirming the "deliberate
  cross-boundary propagation" mechanism is real engineering, not a missed propagation step; (3) `stack_delta_after`
  really does deliberately exclude `write_status_register`/`LINK`/`UNLK` from delta tracking, with a comment citing the
  real supervisor/user-stack-pointer hardware fact T002's record relies on; (4) Cool Spot's `context_bound` sites really
  do trace to the pre-existing, non-raiseable `m68k_context_bound` resource limit (ADR 0079 decision 9/11). Given these
  four origins are each a point with no established finite/points-to fact to chain from — not a value one arithmetic step
  short of one — the reviewer found no structural code path by which decision 1's four named forms could narrow any of
  them, and reported that negative finding plainly rather than manufacturing a defect to seem thorough.
- **PR/task hygiene.** Diff confirmed as exactly two new files, 394 insertions, 0 deletions; no restricted-path content
  (no ROM bytes/addresses/sha256/private trace dumps) anywhere in it; exactly one combined implementation issue's scope
  (SEG-037, pre-authorized as a combined T001-T007 delivery before any implementation began); ADR 0084/0085 mutually
  consistent with every SEG-037-T00x harness Evidence section. `agent_verify.py control --task SEG-037-T007`: PASS.
  `gh pr view 76`: `headRefOid` matches local `HEAD` exactly, `mergeable=MERGEABLE`, `mergeStateStatus=CLEAN`,
  correctly still `isDraft=true` pending this gate.
- **No defect found.** No regression fixture or mutant is required — there is no new mechanism to regress.

This independent PASS, together with T001/T002/T006's own evidence, is the basis for closing SEG-037-T007 and the SEG-037
milestone as done.

## Consequences

- No new abstraction was added to `libs/cpu/m68k/analysis`, `libs/analysis`, or the Genesis machine layer. The milestone's
  own simplicity rule ("if a proposed abstraction has no current consumer, do not build it") is satisfied by construction:
  every named consumer was checked and found to bottom out at a genuinely unconstrained origin, so no new primitive was
  warranted.
- Broad AOT (ADR 0083's compact direct-entry representation) remains the unconditional, sole correctness and production
  strategy for M68K/Genesis. SEG-031's hybrid containment/admission policy is unchanged; it has simply never been exercised
  non-broad on any authorized title through SEG-034, SEG-035, or SEG-037.
- SEG-037's negative result is itself durable evidence, not a gap: three independent efforts (SEG-034's local precision
  refinements, SEG-035's immutable-table-authority inventory, SEG-037's whole-program alias/provenance design) have now each
  traced the same dominant blocker family (unanalysed-interrupt-handler unprovenness, cascading into PC-indexed dispatch,
  computed-return, and object-dispatch sites) to the same root cause: the handler's own epistemic unprovenness, which no
  purely-memory-side technique (local store provenance, immutable-table authority, or whole-program alias exclusion) can
  remove, because the poisoning value's absence of information is real, not an analysis gap.
- Future size/scalability work on the M68K/Genesis target should return to representation/factoring (the ADR 0083/SEG-036
  direction, already measured to materially reduce generated-C and executable size without touching admission) rather than
  another selective-admission attempt, unless a genuinely new fact becomes available about resolving interrupt-handler
  analysis itself (which is an orthogonal, harder control-flow-recovery problem, not a memory/alias/provenance problem, and
  is explicitly out of SEG-037's scope).
- No further analysis refinement is proposed by this ADR. A later task may reopen interrupt-handler analysis specifically
  (a different, harder problem than SEG-037's memory-provenance scope) if new, generically-applicable evidence emerges; this
  ADR does not pre-authorize that as a bounded successor because the required evidence does not yet exist.
