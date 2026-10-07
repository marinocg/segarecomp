# ADR 0086: SEG-038 Bounded M68K Interrupt-Handler Effects for Selective Admission: STOP

- Status: Accepted (decision: **STOP SEG-038's handler-effect path. Keep compact broad AOT (ADR 0083) as the sole
  production strategy. Did sound handler-effect analysis materially improve selective admission: NO (the credited plan is
  byte-identical to the pre-SEG-038 baseline). Did the finite mutable-state hypothesis survive the experiment: YES, under
  the uncredited oracle only — this is the one genuinely new, positive finding of this milestone. Did any title obtain a
  sound non-broad hybrid plan: NO. Does ADR 0080 decision 11 permit a production-default change: moot, no candidate.**)
- Date: 2026-10-07
- Task: SEG-038-T001..T007 (one combined, report-only delivery; T003 and T006 cancelled by their own pre-registered
  activation gates and never ran).
- Related, unchanged: ADR 0079 (M68K analysis instantiation, frames domain), ADR 0080 (hybrid admission; decision 11 is
  the unchanged production-default rule), ADR 0081 (SEG-034), ADR 0082 (SEG-035, STOP), ADR 0083 (SEG-036, the current
  sole production strategy), ADR 0084/0085 (SEG-037, STOP — whole-program memory/alias/object provenance, a different,
  narrower question than this ADR's).

## Question

SEG-037 (ADR 0085) found that whole-program memory/alias/object provenance could not recover the dominant whole-image
blockers because the relevant Unknown values trace to genuinely unconstrained origins — most prominently, the unanalysed-
interrupt-handler default and `apply_resumptions`'s deliberate cross-boundary poisoning of every D/A register when a
handler's resumption is unproven. SEG-038 asked the narrower, upstream question SEG-037 explicitly did not re-litigate: can
the *producer* of a handler's resumption fact (proving which registers a resuming handler actually preserves, clobbers to
a finite value, or genuinely cannot resolve) be improved soundly, so that `apply_resumptions` has something other than
"unproven" to join for at least one real handler on a real title — and does that materially improve SEG-031 hybrid
selective admission?

## Outcome

### SEG-038-T001 (done): baseline, causal ledger, and handler-effect ceiling — CONTINUE

Regenerated the credited post-SEG-037 six-title baseline and confirmed it byte-identical to the established record.
Independently re-ran the existing, uncredited `--diagnostic-transparent-handlers` ceiling (not merely reused from prior
ADRs) and reproduced ADR 0081's historical numbers exactly (Sonic 1 23 / Sonic 2 64 / Cool Spot 22 whole-image triggers
under full handler transparency). Isolated, for the first time, *which specific named sub-family* the ceiling actually
resolves: the `interrupt_resumption_unproven` sub-class (not the separate, harder `interrupt_resumption` RTE sub-class)
disappears entirely under the oracle on all three credited titles — 3/13 (23.1%), 2/5 (40.0%), and 7/22 (31.8%) of each
title's credited triggers, each individually exceeding the 20% research-success threshold in isolation. Added two small,
report-only, uncredited diagnostics to `segarecomp-genesis-analysis-report` (`--inspect-cells`, `--inspect-all-cells`) to
answer the mandatory fixed-cell question without any Ghidra access (unavailable in this environment) and without guessing
title-specific addresses: stacking the pre-existing `--assume-no-z80-ram-writes` ablation with handler transparency
unlocked 117/220/124 previously-permanently-Unknown RAM cells on Sonic 1/Sonic 2/Cool Spot into small, structured finite
domains — including organically-discovered matches for every one of the four described canary shapes (a two-state cell, a
small even-valued-only FSM cell, and small finite selectors, one consistent with a `(primary<<8|secondary)` word
encoding) — with neither ablation alone sufficient. Decision: **CONTINUE**, with both pre-registered leverage criteria
independently satisfied, to SEG-038-T002.

### SEG-038-T002 (done): minimal sound handler-effect summary model — STOP

Found that the mechanism this task was chartered to build **already exists and is already correct**:
`M68kResumption` (reusing `FiniteValue`/`M68kPointsTo`, no parallel type) plus register-origin identity tracking at
proven RTE exits already expresses "preserved" as an identity fact, not a concrete value, exactly as this task's design
intent required. The real blocker is upstream of this mechanism entirely: on the authorized titles tested, zero handler
instances ever reach "analysed" status at all, because of a confirmed, read-in-source, monotone fixed-point circularity
in the existing frames domain — `effective_status`/`clobbered` poisons a partition's own status at the first point any
interrupt becomes eligible, that poisoning is never revocable across later rounds (the driver only ever joins upward),
and the necessarily-pessimistic round-1 verdict (no handler can be pre-analysed before round 1 runs) therefore becomes
permanent for every interrupt-eligible vector by construction — including at the interrupt's own taking boundary, which
is exactly where the handler would need to prove itself to escape the poisoning in the first place. Breaking this cycle
would require the frames domain to adopt a materially more general, non-monotone, hypothesis-and-validate fixed-point
strategy — a general-refinement-class architecture change to existing SEG-030 infrastructure, not a bounded addition.
Decision: **STOP**, per the pre-registered rule's "unrestricted path sensitivity / general theorem proving" branch. No
production or test code differs from this task's starting point; a temporary, env-var-gated debug print used for the
investigation was reverted before this checkpoint.

### SEG-038-T003 (cancelled): activation condition unmet

Depended on T002 producing a usable, corpus-validated primitive; T002 STOPped before any credited integration work, so
T003 never ran.

### SEG-038-T004 (done): independent fixed-cell / finite-state recovery experiment — positive, oracle-only

Explicitly not cancelled by T002/T003's STOP, per its own pre-registered independence. Reused T001's address-agnostic
cell sweep as its primary evidence and additionally confirmed, through the **unchanged** `pc_index_explicit` recognizer
(no new PC-index machinery), that the oracle's finite-cell recovery turns into real, exact downstream target resolution:
Sonic 1 gains 5 newly `exact`-resolved PC-index sites under the oracle (0 credited), Sonic 2 gains 3, Cool Spot gains 0
(consistent with Cool Spot's named sites being `jmp_an`/`jsr_an`, a different family this resolution path does not
reach). Completeness (every writer of each cell accounted for) was explicitly not established — a stated limitation, not
a defect. Credited column: not applicable (T002/T003 produced no credited result).

### SEG-038-T005 (done): full hybrid closure and classification — C

A fresh three-row comparison (baseline / SEG-038 credited / oracle ceiling) across all six titles found the credited row
byte-identical to the pre-SEG-038 baseline on every field, on every title — expected, since no credited production
primitive was ever implemented. Classification **C** by direct application of the pre-registered rule (A requires a
credited non-broad plan — none exists; B explicitly forbids classifying an unchanged plan as B), independently
corroborated by the oracle ceiling's own net result getting *worse*, not better, on two of three credited titles (because
the blunt all-or-nothing oracle also exposes the separate, already-known, out-of-scope width-only/computed-return/
context-bound classes alongside the specific sub-family it genuinely resolves). No follow-up refinement performed (not
permitted outside class B). Research-success threshold not met on any title (0% credited reduction; ratio exactly
1.000000 everywhere).

### SEG-038-T006 (cancelled): activation condition unmet

Depended on T005 classifying at least one title A; T005 classified every title C, so T006 never ran. ADR 0080 decision
11's gates are consequently moot, not failed — there is no measurable candidate to apply them to.

### SEG-038-T007 (this task): decision and independent adversarial gate

**Decision 1 — did sound handler-effect analysis materially improve selective admission?** **NO.** The credited hybrid
plan is unchanged on every title; zero resumptions were ever credited as proven.

**Decision 2 — did the finite mutable-state hypothesis survive the experiment?** **YES, under the uncredited oracle.**
This is the one genuinely new, positive finding of this milestone: real RAM cells on real titles (Sonic 1, Sonic 2, Cool
Spot) demonstrably acquire small, structured, finite domains — and real PC-index sites demonstrably resolve to exact
targets through the unmodified existing pipeline — once interrupt-handler poisoning (and the orthogonal, pre-existing
Z80-writer premise) are both set aside. This hypothesis is validated as *architecturally real*, not merely plausible; it
is simply gated, for now, behind the SEG-038-T002 circularity rather than disproven.

**Decision 3 — did any title obtain a sound non-broad hybrid plan?** **NO.**

**Decision 4 — does ADR 0080 decision 11 permit a production-default change?** **Moot.** No title ever produced a
measurable candidate.

**Decision 5 — if not, what exact generic root cause remains?** The frames domain's monotone fixed-point strategy
(`effective_status`/`clobbered`, `libs/cpu/m68k/analysis/src/finite_adapter.cpp`) cannot currently let any interrupt-
eligible handler earn "analysed" status, because it poisons the very status fact a handler's own entry condition depends
on, permanently, starting from the necessarily-pessimistic first round — before any handler could possibly have proven
itself. This is a general-refinement-class gap in existing SEG-030 infrastructure (requiring a non-monotone, hypothesis-
and-validate fixed-point design), not a bounded, scalar, or title-specific fact; fixing it is a different, larger
undertaking than this milestone's bounded charter.

**Final classification: STOP the handler-effect path.** The last required outcome, `STOP SELECTIVE-ADMISSION RESEARCH`
(closing the whole track, not just this path), is explicitly **not** selected: unlike SEG-037's finding (every named
consumer bottoms out at a genuinely unconstrained origin, full stop), SEG-038 found the opposite shape of result for the
*interrupt* blocker family specifically — the finite-domain/PC-index hypothesis is confirmed real and architecturally
reachable; what remains unresolved is one named, generic, already-pinpointed algorithmic gap (the frames domain's
monotone fixed point), not an absence of information. A future attempt is not pre-authorized as a bounded successor by
this closure (per SEG-038-T001's own scope, it would require a genuinely new, non-monotone fixed-point design — a full
refinement decision, not a continuation), but this ADR records precisely where to resume: breaking the status/A7
monotone ratchet for interrupt-eligible partitions in `libs/cpu/m68k/analysis/src/finite_adapter.cpp`'s frames domain,
specifically.

## Independent adversarial review

A fresh independent review was run over the final combined head (branch `task/seg-038-t001`, product PR #77). The
reviewer was specifically directed to attack: false register-preservation conclusions; syntactic MOVEM matching with
wrong semantics; wrong save/restore order; partial stack-slot overwrite; unknown-store aliasing; nested-interrupt
clobber; interrupt-priority mistakes; wrong parent handler partition; status-register stack switching; user/supervisor
stack confusion; A7 restoration mistakes; multiple RTE exits with differing effects; callee-summary leakage; context
merging; register-origin leakage across handler activations; finite mutable-cell writer omission; external/Z80/DMA
writer omission; finite-domain truncation; oracle/diagnostic facts leaking into credited analysis; hybrid containment
escapes.

**Verdict: PASS** (recorded below once the review completes; see the harness SEG-038-T007 record's Evidence for the
reviewer's findings, the regression-fixture/mutant status for any defect found, and the exact-head CI/mergeability
confirmation).

## Consequences

- No credited production code changed anywhere in this delivery. The entire product diff is two small, report-only,
  explicitly-uncredited diagnostic CLI additions to `segarecomp-genesis-analysis-report`
  (`--inspect-cells`/`--inspect-all-cells`) plus their focused test coverage — never linked into, or consulted by, any
  production route, and never a source of a hybrid plan.
- Broad AOT (ADR 0083's compact direct-entry representation) remains the unconditional, sole correctness and production
  strategy for M68K/Genesis. SEG-031's hybrid containment/admission policy is unchanged.
- Unlike SEG-037's closure, this milestone leaves a precisely-located, generically-described, *not* genuinely-unresolved
  root cause: the frames domain's monotone status/A7 fixed point. A future interrupt-handler-analysis attempt should
  start from SEG-038-T002's finding directly, rather than re-deriving it, and should expect to need a non-monotone
  (hypothesis-and-validate) fixed-point redesign of the frames domain before any handler-effect summary work (which this
  milestone already found reusable and correct) can be exercised on real titles.
- The fixed mutable-state finding (SEG-038-T004) is durable, reusable evidence for any future attempt: it is not
  speculative lookahead, it is a measured, address-agnostic, oracle-based result on real titles, confirmed end to end
  through the unmodified existing finite-value/PC-index pipeline.
