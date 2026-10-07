# ADR 0087: SEG-039 Bounded Hypothesis-and-Validate Fixed Point for M68K Interrupt Frame Instances: STOP

- Status: Accepted (decision: **STOP FRAME HYPOTHESIS PATH**. Did the frames bootstrap circularity have
  a bounded solution: NO, not with a local per-candidate scheme — the blocking mechanism is a single,
  shared, cross-candidate, partition-wide status fact, not a per-handler self-contamination. Did any
  real interrupt handler become analysed under a new mechanism: N/A, no production mechanism was
  implemented (correctly gated off by the pre-registered T001 stop rule). Did `D`/hybrid improve: N/A,
  unchanged — no production code changed. Is selective admission still supported by the evidence: YES,
  unchanged from ADR 0086 — the finite mutable-state hypothesis (SEG-038-T004) remains real and
  reachable; what is now additionally known is exactly *why* a bounded local mechanism cannot reach it.)
- Date: 2026-10-07
- Task: SEG-039-T001, SEG-039-T007 (one combined, report-only delivery; T002-T006 cancelled by T001's
  own pre-registered activation gate and never ran).
- Related, unchanged: ADR 0079 (M68K analysis instantiation, frames domain), ADR 0080 (hybrid
  admission; decision 11 unchanged, moot here — no candidate), ADR 0083 (SEG-036, the current sole
  production strategy), ADR 0086 (SEG-038, STOP handler-effect path — the direct predecessor this ADR
  continues from and refines).

## Question

ADR 0086 (SEG-038-T002) found that the existing, already-sound handler-effect proof machinery
(`M68kResumption`, register-origin identity, `FiniteValue`, `M68kPointsTo`) is never exercised on real
titles because zero real interrupt-handler instances ever reach "analysed" status, due to a monotone
fixed-point circularity in the frames domain: `effective_status`/`clobbered` poisons a partition's
status the first round any interrupt becomes eligible, permanently. SEG-039 asked the direct follow-up
question ADR 0086 explicitly deferred: can that circularity be broken by a small, bounded,
deterministic, fail-closed hypothesis-and-validate mechanism — propose one candidate handler instance,
solve, validate against its own derived effects, commit-or-reject — without "assuming handlers preserve
registers," deleting the `clobbered` fact, or weakening `effective_status()`?

## Outcome

### SEG-039-T001 (done): formalize the circularity and test the minimum synthetic breaker — STOP

Reread the current `effective_status()`/`clobbered`/`frame_address()`/outer-driver-join code
(`libs/cpu/m68k/analysis/src/finite_adapter.cpp`, `frames.cpp`) and found ADR 0086's phrasing of the
trigger condition imprecise: a plain, single-vector, non-self-nesting handler genuinely reaches
"analysed" status and gets a proven resumption today (confirmed via the pre-existing
`handler_preserves_registers()` test, which already passes on unmodified `main`). The precise trigger
requires at least one resuming contribution to become genuinely unanalysable — the cleanest synthetic
case being a handler that lowers its own interrupt mask, enabling self-nesting, which the analysis
correctly classifies `nested`/`depth_bound` and conservatively marks `async_all`. Built three new
synthetic, project-authored MC68000 test fixtures
(`tests/analysis_m68k_frames_circularity_test.cpp`): a clean single-handler case (does *not* reproduce
the bug, refining ADR 0086), a self-nesting case (reproduces it), and — the key new finding — a case
with two *independent* vectors sharing one partition, where the self-nesting defect in one vector's
handler collaterally and permanently blocks an otherwise perfectly provable, unrelated handler at a
different vector from ever reaching "analysed" status.

Built a test-only "propose one handler instance, solve, validate, commit-or-reject" breaker (reusing
the unchanged production engine restricted to a narrowed candidate configuration, never editing
`effective_status`/`apply_resumptions`/`clobbered`, never linked into production) and ran it against
all ten pre-registered synthetic mutation classes (perfect save/restore; clobbered register; wrong
restore slot; corrupted saved SR; failed A7 restore; one corrupt RTE exit among several; unknown
aliasing write into the save area; nested higher-priority interrupt; non-RTE exit; resource-bound
exhaustion). **All ten passed** with the semantically correct verdict, including a deliberate negative
control proving that dropping a genuinely co-eligible nesting source from the candidate's scope would
be the unsound shortcut that makes mutation 8 falsely pass.

**Decision: STOP.** The bounded per-candidate scheme soundly handles every hazard local to a single
candidate's own partition — but the collateral-damage fixture showed the actual real-title-blocking
fact (`clobbered[0]`/`state.status`) is a single, shared, cross-candidate, partition-wide value that is
never revocable once poisoned by *any* unrelated sibling handler's own defect. No per-candidate
propose/solve/validate loop — however carefully scoped — can restore a different, otherwise
perfectly-provable sibling handler once that shared fact is corrupted, without either (a) a genuinely
non-monotone, cross-candidate fixed point that can retract `clobbered`/status per-hypothesis (exactly
the remedy ADR 0086 already named), or (b) splitting the single combined supervisor-mode/
interrupt-mask `status` domain so an unrelated handler's mask-corruption cannot also destroy the
separately-provable supervisor-mode fact `frame_address()` needs — a foundational representation change
touching every program state, not a bounded per-candidate addition. Both are general-refinement-class
architecture changes per the milestone's own pre-registered stop rule, not a bounded mechanism. No
production code changed; the only diff is the new test file and its CMake registration.

### SEG-039-T002 (cancelled): activation condition unmet

Depended on T001 recording CONTINUE with a bounded candidate-validation formulation that both proves
the good synthetic case and rejects every broken variant. T001's own ten required mutations all passed
in isolation, but the milestone's actual target — unblocking real handlers on real titles sharing a
partition with any unanalysable sibling — was shown unreachable by the bounded local scheme in the same
task, so T001 recorded STOP rather than CONTINUE. T002 never ran.

### SEG-039-T003 (cancelled): activation condition unmet

Depended on T002 producing an accepted design. T002 was cancelled transitively; T003 never ran.

### SEG-039-T004 (cancelled): activation condition unmet

Depended on T003 producing a mechanism that passed the synthetic corpus and could be run on real
titles. T003 was cancelled transitively; T004 never ran. No real-title measurement was performed in
this milestone — the synthetic/architectural finding in T001 was sufficient to determine the bounded
mechanism cannot reach the milestone's goal, before any real-title cost was spent.

### SEG-039-T005 (cancelled): activation condition unmet

Depended on T004 validating at least one real handler instance. T004 was cancelled transitively; T005
never ran. The credited `--hybrid-plan` baseline (Sonic 1 `D=1,276`/13 triggers, Sonic 2 `D=335`/5
triggers, Cool Spot `D=6,907`/22 triggers, all `hybrid_total/U = 1.000000`) is therefore unchanged from
the post-SEG-038 state recorded in ADR 0086; it was not necessary to reproduce it again in this
milestone, since no production code path that could affect it was touched.

### SEG-039-T006 (cancelled): activation condition unmet

Depended on T005 classifying at least one title A, or an otherwise-existing non-broad credited plan.
Neither condition was ever reached; T006 never ran. ADR 0080 decision 11 is consequently moot (no
candidate), not failed. No successor milestone registration (the Z80 writer-authority candidate,
SEG-040) was performed by T006, deferred to this task's own final disposition below.

### SEG-039-T007 (this task): decision and independent adversarial gate

**Decision 1 — did the frames bootstrap circularity have a bounded solution?** **NO**, not with a
local, per-candidate hypothesis-and-validate scheme. The circularity's real blocking mechanism —
discovered precisely by this milestone, refining ADR 0086 — is a *shared, cross-candidate* fact
(`clobbered[0]`/`state.status`), not a per-handler self-contamination; fixing it requires a
general-refinement-class architecture change (a non-monotone, cross-candidate-retractable fixed point,
or a foundational split of the status representation), not a bounded mechanism.

**Decision 2 — did any real interrupt handler become analysed?** **N/A.** No production mechanism was
implemented; the pre-registered T001 stop rule correctly prevented any real-title work from being
attempted on a mechanism already shown structurally insufficient for the milestone's actual goal.

**Decision 3 — did any `M68kResumption` become newly proven?** **N/A**, same reason.

**Decision 4 — did `interrupt_resumption_unproven` decrease?** **NO** — unchanged from the ADR 0086
baseline; no production code changed.

**Decision 5 — did `D`/exact target recovery improve?** **NO** — unchanged; no production code
changed.

**Decision 6 — did hybrid become non-broad?** **NO** — unchanged; every credited title remains
`broad_whole_image`/`broad_analysis_incomplete` exactly as under ADR 0086.

**Decision 7 — is the next dominant blocker the known Z80 writer authority or something else?** **Something
else, more precisely than before.** Independently of the already-known Z80 blanket-writer wall
(SEG-030-T010), this milestone identifies the frames domain's **shared, cross-candidate, partition-wide
status fact** as the precise, generic, already-pinpointed remaining obstacle to any interrupt-handler
analysis improvement — more specific than ADR 0086's "monotone status fixed point" framing, which this
ADR narrows to the cross-candidate sharing property specifically (a single-candidate monotone fixed
point, by itself, is not the obstacle — the earlier good-case fixture proves a plain handler already
gets analysed today).

**Decision 8 — is selective admission still supported by the evidence?** **YES**, unchanged from ADR
0086: the finite mutable-state/exact-PC-index hypothesis (SEG-038-T004) remains real, measured, and
reachable in principle; this milestone did not weaken that finding, it only confirmed more precisely
why a bounded local mechanism cannot reach it from the interrupt-handler side.

**Final classification: STOP FRAME HYPOTHESIS PATH.** `STOP SELECTIVE-ADMISSION RESEARCH` is explicitly
**not** selected: the remaining obstacle is a precisely-named, generic, already-located architectural
property (the frames domain's shared cross-candidate status fact), not an absence of information or a
disproof of the underlying finite-state hypothesis. A future attempt is not pre-authorized as a bounded
successor by this closure — per T001's own finding, it would require a genuinely non-monotone,
cross-candidate-retractable fixed point or a foundational status-representation split, both
full-refinement-class architecture decisions — but this ADR records precisely where to resume:
`libs/cpu/m68k/analysis/src/finite_adapter.cpp`'s frames domain, specifically the sharing of
`clobbered[tag]`/`state.status` across every handler instance of partition `tag`, not merely its
monotonicity within one candidate.

Separately, the already-known Z80 blanket-writer wall (SEG-030-T010) remains unresolved and orthogonal.
This milestone's evidence does not newly justify registering that successor beyond what ADR 0086 already
established (it was already known before SEG-039 began, and SEG-039's STOP does not change its status);
no SEG-040 registration is performed here. A future milestone may still register it independently of this
one's outcome.

## Independent adversarial review

A fresh independent review was run over the final combined head (branch `task/seg-039-t001`, product PR
#78). The reviewer was specifically directed to attack: a candidate proving itself circularly; child
post-resumption effect contaminating its own entry proof; wrong interrupt-taking boundary; interrupt
mask/priority error; nested higher-priority interrupt; candidate order dependence; candidate accepted
despite one unknown exit; candidate accepted despite one non-RTE exit; saved SR corruption; saved PC
corruption; wrong exception-frame layout; wrong A7; supervisor/user stack switch; partial MOVEM
restoration; wrong MOVEM ordering; callee clobber hidden by origin identity; unknown memory alias over
save slot; recursive handler/exception interaction; candidate facts escaping failed validation; rejected
candidate accidentally retained in a later round; state/resource bound producing an optimistic rather
than `Unknown` result; diagnostic/oracle fact leaking into the credited path.

**Verdict: PASS.** The reviewer independently re-derived the shared-partition-poisoning mechanism from
the unchanged production code (not merely trusting this ADR's prose), confirmed via `FRAMES_DEBUG=1`
instrumentation that `clobbered`/`async` are keyed by *parent tag* and that an outer, otherwise-
provable contribution at `parent_tag==0` is always admissible until *any* sibling's defect poisons it
— and confirmed that combining co-resident vectors into one candidate (the natural "overlooked
alternative" to probe) does not escape this, because the poisoning is a property of the shared,
tag-0-keyed `clobbered` map inside the unchanged engine, not of candidate scoping; the plain
multi-vector fixture already exercises exactly that combined scope and still fails identically. The
reviewer independently verified `apply_resumptions()` never touches `edge.status` (directly refuting
the originally-hypothesized self-contamination shape, confirming the ADR's narrower shared-fact
finding instead), confirmed the SR interrupt-mask encoding (`$2000` = S=1, mask=0, re-opening every
level), hand-decoded the trickiest mutation encodings against the production decoder, and confirmed
zero bytes differ under `libs/`/`platforms/`/`apps/`.

One genuine defect was found and fixed, entirely confined to the new test file: mutation 6's
"corrupted RTE exit" fixture was mis-encoded (a `BEQ.S` guarding branch's PC-relative target landed on
the following instruction's extension word rather than its opcode) and, once that was fixed, its
intended "corruption" (`ADDQ.L #2,2(A7)` on the stacked return PC) turned out not to be corruption at
all — `finite_adapter.cpp`'s `frame_pc_identity`/`frame_pc_fact` tracking deliberately and soundly
treats an immediate ADDQ/SUBQ adjustment of the stacked PC as an exact-offset resumption. The fixture
was corrected to use a genuine corruption (storing an Unknown-valued register onto the stacked PC
slot) with two new regression assertions pinning the branch target and decode-agreement so the defect
cannot silently recur. This is a test-fixture-local correction, not a change to the shared-partition-
poisoning finding (independently re-derived by the reviewer from unchanged production code), and does
not alter the STOP classification.

A fresh full gate was run against the exact corrected head (product branch `task/seg-039-t001`,
commit `87b8342`): **100% tests passed, 0 tests failed out of 321** (`agent_verify.py full`, no
authorized local ROM available in this environment, so the Sonic differential-ROM leg was omitted).
The reviewer agreed the STOP classification (not CONTINUE, not the stronger `STOP
SELECTIVE-ADMISSION RESEARCH`) is correctly justified by the evidence.

## Consequences

- No credited production code changed anywhere in this delivery. The entire product diff is one new,
  test-only synthetic fixture/breaker file (`tests/analysis_m68k_frames_circularity_test.cpp`) plus its
  CMake registration, and this ADR — never linked into, or consulted by, any production route, and
  never a source of a hybrid plan.
- Broad AOT (ADR 0083's compact direct-entry representation) remains the unconditional, sole
  correctness and production strategy for M68K/Genesis. SEG-031's hybrid containment/admission policy
  is unchanged.
- Unlike SEG-038's closure (which located the obstacle as "the frames domain's monotone status fixed
  point" in general), this milestone narrows the obstacle precisely: it is the **sharing** of the
  status/clobbered fact across every handler instance of one partition, not merely the fact's
  monotonicity within a single candidate's own analysis. A future attempt should start from this
  narrower finding directly, and should expect to need either a cross-candidate-retractable fixed point
  or a status-representation split before any bounded per-candidate hypothesis-and-validate mechanism
  (which this milestone already proved sound for every *locally*-scoped hazard, including self-nesting
  in isolation) can be composed into something that survives an unrelated sibling handler's own defect.
- The SEG-038-T004 fixed mutable-state finding remains durable, unchanged, reusable evidence for any
  future attempt.
