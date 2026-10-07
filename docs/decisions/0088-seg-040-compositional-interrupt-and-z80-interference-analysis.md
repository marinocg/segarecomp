# ADR 0088: SEG-040 Compositional Interrupt and Z80 Interference Analysis for Selective M68K AOT

- Status: Accepted (decision: **ADOPT Z80 INTERFERENCE REFINEMENT ONLY**. Shared interrupt-state
  over-poisoning was NOT reduced (the M68K track STOPped before any mechanism was implemented: a
  second, independent, hardware-correct frame-integrity hazard entangles with the shared-fact
  collateral-poisoning mechanism at the governing fixture's own priority ordering, so no bounded
  per-candidate/per-child mechanism can resolve it without weakening `apply_resumptions()`). Z80
  analysis DID produce a sound, bounded, independently-tested M68K write-set derivation mechanism,
  which is adopted as the new production default because it strictly improves correctness/precision
  over the previous hardcoded inputs with zero regressions — but it did not bound any real title's
  write set measured in this milestone, so hybrid remains broad everywhere. Selective-admission
  research remains justified; continuing is not pre-authorized as a bounded successor by this closure.)
- Date: 2026-10-07
- Task: SEG-040-T001..T007 (one combined, mixed report-and-production delivery; T003 cancelled
  transitively by T002's STOP).
- Related, unchanged: ADR 0079 (M68K analysis instantiation, frames domain), ADR 0080 (hybrid
  admission; decision 11 unchanged, moot here), ADR 0083 (SEG-036, the current sole production
  strategy), ADR 0086 (SEG-038, STOP handler-effect path), ADR 0087 (SEG-039, STOP frame hypothesis
  path — the direct predecessor this ADR continues from for the M68K track).

## Question

ADR 0087 located the M68K interrupt-handler blocking mechanism precisely: a single, shared,
cross-candidate, partition-wide status fact (`clobbered[tag]`/`state.status`) that one problematic
handler's own defect permanently poisons, collaterally blocking an otherwise perfectly provable,
unrelated sibling handler in the same partition. Separately, the existing SEG-030-T010 Z80 write proof
returns a blanket `outcome:"all"` for every title, because the report driver has never supplied it a
real Z80 image. SEG-040 asked, as two **independent** tracks (a STOP in one must not cancel the
other): can M68K interrupt effects be analyzed as composable summaries (component-wise status split,
or a per-handler preemption graph with bounded SCC composition) rather than one shared partition-wide
poison source; and can the Z80 be analyzed as its own execution universe that exports only a proven,
bounded M68K-visible write-interference summary rather than invalidating all M68K work RAM?

## Outcome

### SEG-040-T001 (done): architecture census and external-analyzer design study — CONTINUE both tracks

Mapped current M68K frames/status fact granularity and the two separate, unconnected Z80 analysis
engines. Found the Z80 blanket result is **not** an analyzer-precision defect: the report driver
(`report.cpp`) always passed `z80_images = std::nullopt` (no production driver ever assigned it) and
`held_in_reset = false` unconditionally, independent of how precisely the existing, already-sound,
already-bounded Z80 write-proof engine (`z80_ram_write_proof.cpp`) could have bounded the real write
set given actual image bytes — confirming the required work is narrow, additive glue, not an analyzer
rewrite. For the M68K side, found that a bare split of the combined `(S,I)` status into independent
`FiniteValue` domains is **not** mechanically sound on its own, because both existing clobbering causes
correspond to a genuinely unproven arbitrary write to the *same* SR system byte that packs S and I
together; the only sound additive decomposition identified (a bounded, per-clobber-cause finite flag)
sits at the edge of becoming a general symbolic-SR/history domain. Studied Ghidra (per-function-local
SSA/Varnode locality with narrow summaries; bounded SCC-grouped recursive-function fixed points) and
Rizin/radare2 (ESIL/RzIL's separation of instruction semantics from analysis) narrowly for ideas
directly applicable to the two blockers; both patterns were found already structurally present in the
existing codebase (per-point finite-value fixed point; `libs/analysis/solver.hpp`'s generic `Adapter`
seam; `z80::effects.hpp`/`project_effect()`), with one genuine, scoped gap identified
(`conservative_stores()` partially duplicates `project_effect()`'s semantics coverage). No general
decompiler IR risk. Decision: **CONTINUE** to T002 (with an explicit caveat that a bare S/I split alone
would not work) and **CONTINUE** to T004.

### SEG-040-T002 (done): component-wise M68K status and handler-effect ceiling — STOP

Performed the required nine-class semantic audit (reset; MOVE to SR; ANDI/ORI/EORI to SR; STOP;
interrupt entry; synchronous exception entry; RTE; saved-SR modification; privilege exceptions),
confirming the critical saved-SR rule throughout. Traced (on paper, not shipped) a bounded
`clean_status` field design that would need to touch roughly 15 `status`-touching production sites.
Added a new test (`tests/analysis_m68k_frames_priority_order_test.cpp`) proving the shared
`clobbered[0]` collateral-poisoning mechanism is **priority-order-independent** — it reproduces
identically whether the self-nesting ("bad") vector's level is numerically above or below the
unrelated sibling's ("good") level, even when hardware priority rules mean the bad vector cannot
legally preempt the good handler's body at all. Via a temporary, reverted, non-shipped instrumentation
experiment, found a second, genuinely distinct, hardware-correct cause specific to the fixture's own
priority ordering: at bad-above-good, the good handler's own saved-SR frame is **also** legally
exposed to the bad handler's corruption via the existing (correct) frame-integrity mechanism — so no
entry-status fix alone can make that specific fixture's sibling fully provable without separately
weakening the unrelated frame-integrity check, which is forbidden. **Decision: STOP.** Neither
component-wise status splitting nor the per-child corruption-effect-summary alternative yields a
bounded, sound fix for the governing fixture. Zero production files changed; the entire diff is one
new test-only file plus its CMake registration.

### SEG-040-T003 (cancelled): activation condition unmet

Depended on T002 recording CONTINUE. T002 recorded STOP; T003 never ran — no preemption-graph/SCC work
was attempted, and no real-title cost was spent on the M68K track beyond T002's synthetic audit.

### SEG-040-T004 (done): Z80 to M68K interference summary — mechanism implemented, correct, real-title-inactive

Implemented a narrow, additive M68K-side producer (`platforms/genesis/analysis_report/src/
z80_boot_image.cpp` + a small pure `M68kFiniteAdapter::memory_write_values()` query factored out of
the existing write-value resolution switch, zero change to `effective_status()`/`clobbered`/
`apply_resumptions()`) that replays the completed M68K solution's edges over the main-flow partition,
tracks a bounded BUSREQ/`/RESET`/"never yet run" lattice per point (citing
`docs/architecture/genesis-z80-audio-contract.md` section 4 and cross-checked against two independent
public open-source Genesis emulators, diagnostic references only), and — only for boot-window byte
stores into the real Z80 RAM mirror — derives an exact byte image, failing closed (abandoning the
*whole* image, never a partial one) on any imprecise target/value or non-byte store. Wired into
`report.cpp`, replacing the previous hardcoded `std::nullopt`/`false` inputs. Five new synthetic tests
(`tests/analysis_genesis_z80_boot_image_test.cpp`) prove: a fully-unrolled byte-exact upload is
credited and the existing Z80-side proof correctly bounds the write set to `none`; one Unknown
boot-window byte fails closed to the full image; a fully M68K-exact image whose own Z80-side content is
itself unbounded still fails closed (`store_target_unknown`); a Z80-RAM read never promotes to a
write; a device-register write never corrupts the derived RAM image. Run against the authorized local
Sonic 1, Sonic 2, and Cool Spot images: all three completed analysis, and in all three the producer
correctly attempted derivation but honestly found the real boot/communication idiom (consistent with
an indexed/loop-driven upload and/or an ongoing register-indexed command interface) is not byte-exactly
derivable by this intentionally bounded, non-loop-aware mechanism — an explicitly anticipated "honest
`Unknown/all` result," not a failure, per this task's own acceptance criteria. The mechanism's own
correctness is independently proven by the five synthetic tests; it simply did not activate on the
three real titles measured.

### SEG-040-T005 (done): independent real-title evaluation of both tracks

Measured, by direct re-execution on the exact current head (not assumption), that the baseline,
M68K-interrupt-only, Z80-interference-only, and both-combined columns are **identical on every one of
the ten required metrics** for Sonic 1, Sonic 2, and Cool Spot. `D`, handler instances analysed (0 for
all three), proven resumptions (0), unproven resumptions, tracked/finite RAM cells (0), Z80 WRAM async
ranges, exact PC-index/address-indirect sites (0), and whole-image triggers are all byte-identical to
the SEG-038-T001/SEG-039 established baseline. The one new, non-reconstructable fact: the Z80 proof's
`reasons` field now additionally carries `m68k_store_into_z80_ram_unbounded` for all three titles,
confirming the corrected `held_in_reset` classification is genuinely active even though it did not
change the credited outcome class. OutRun/Streets of Rage/Golden Axe remain `broad_analysis_incomplete`
(unchanged). Neither track's causal questions showed a measurable effect on any of the three titles;
the combined-multiplier question is moot (nothing to multiply).

### SEG-040-T006 (done): integrated credited hybrid closure — classification C

A fresh, credited-only `--hybrid-plan` rerun (no oracle/ablation flags) on the exact current head
confirms `hybrid_total/U = 1.000000` for all three completing titles (fully broad). **Classification:
C — compositional approach insufficient for this delivery's measured scope**, with both tracks
evaluated independently (not merely because one failed): the M68K track hit a genuine, precisely-named
architectural limit (STOP, no mechanism implemented); the Z80 track produced a correct, tested, but
real-title-inactive mechanism. Neither materially reduced either precision wall in credited analysis on
any measured title, so B (both walls materially reduced) is not met either.

### SEG-040-T007 (this task): decision and independent adversarial gate

**Decision 1 — was shared interrupt-state over-poisoning reduced?** **NO.** T002/T003 implemented no
production mechanism; `clobbered[tag]`/`effective_status()` are byte-identical to pre-SEG-040 `main`.

**Decision 2 — did per-handler/preemption composition work?** **N/A.** T003 never ran (T002's
activation gate for it was not met).

**Decision 3 — could recursive interrupt SCCs be summarized safely?** **N/A**, same reason — no SCC
composition mechanism was attempted.

**Decision 4 — did Z80 analysis produce bounded M68K write sets?** **Partially: the mechanism can
(proven on 5/5 synthetic tests, including a positive case that bounds a write set to `none`), but it
did not on any of the three real titles measured** (Sonic 1/2/Cool Spot) — their boot upload idiom is
not byte-exactly derivable by this intentionally bounded, non-loop-aware producer.

**Decision 5 — did any title stop treating all work RAM as asynchronously writable?** **NO**, on the
titles measured. `z80_ram_write_proof.outcome` remains `all` for Sonic 1, Sonic 2, and Cool Spot.

**Decision 6 — did finite mutable M68K cells appear in credited analysis?** **NO.** `memory.max_cells`
remains 0 for all three titles — unchanged from the ADR 0086/0087 baseline.

**Decision 7 — did exact PC-index/address targets increase?** **NO.** `pc_index_recovery.resolved`
and `address_recovery.resolved` remain 0 for all three titles.

**Decision 8 — did hybrid become non-broad?** **NO.** `hybrid_total/U = 1.000000` for every title that
completes credited analysis, identical to the pre-SEG-040 baseline.

**Decision 9 — what blocker dominates next?** **Two independent, precisely-named blockers, unchanged
in kind from before this milestone but now more precisely understood.** M68K side: the frames domain's
*shared, cross-candidate* status fact (ADR 0087), now additionally confirmed to entangle with a second,
distinct, hardware-correct frame-integrity hazard at realistic (bad-preempts-good) priority orderings
— resolving it requires a non-monotone, cross-candidate-retractable fixed point or a foundational
status-representation split, both full-refinement-class architecture decisions. Z80 side: the specific,
narrower gap is that this milestone's boot-image producer is **not loop-aware** — real titles' Z80
driver uploads use indexed/DBRA-style copy loops (or an ongoing register-indexed command interface)
that this intentionally bounded, non-loop-aware mechanism cannot exactly derive. Unlike the M68K side,
this is explicitly **not** characterized as an unbounded or architecturally unresolvable gap: the
project already has existing bounded finite-loop-progress machinery (ADR 0016's bounded static
finite-loop progress proof; ADR 0035's per-invocation finite DBF loop completion) that a future bounded
task could adapt to recognize a finite, statically-bounded upload loop and derive its exact byte image,
without any new unbounded search or general symbolic execution.

**Decision 10 — is continuing selective-admission research still justified?** **YES.** SEG-038-T004's
finite mutable-state/exact-PC-index finding under the uncredited oracle combination remains real,
measured, and unweakened by this milestone. This milestone materially narrows what remains: the M68K
side needs a full-refinement-class architecture decision (unchanged conclusion from ADR 0087); the Z80
side needs one additional, precisely-bounded capability (loop-aware upload recognition) that reuses
existing bounded project infrastructure, not a new unbounded mechanism. The bar for `STOP
SELECTIVE-ADMISSION RESEARCH` (the remaining path being disproportionate or fundamentally unbounded) is
not met for the Z80 side; it may be closer to met for the M68K side specifically, but ADR 0087 already
declined to select that stronger classification for the same reasons, and this milestone does not add
evidence that changes that specific judgment.

**Final classification: `ADOPT Z80 INTERFERENCE REFINEMENT ONLY`.** The Z80-side production code
(`z80_boot_image.cpp`, the `memory_write_values()` query, and the corrected `report.cpp` wiring) is
adopted as the new production default: it is strictly more correct than the code it replaces (the
previous `held_in_reset = false` for every store was simply wrong, not merely imprecise), is fully
covered by new regression tests with zero observed regressions across the full existing suite (332/332
passing), and provides genuinely improved diagnostic precision (the new `m68k_store_into_z80_ram_
unbounded` reason, precise `unknown_target_stores` counts) even though it did not change any measured
title's credited admission outcome. The M68K-side refinement is not adopted because none was
implemented (T002/T003 STOPped before production code was written). `ADOPT BOTH COMPOSITIONAL
REFINEMENTS` is not selected because there is no M68K refinement to adopt. `STOP SELECTIVE-ADMISSION
RESEARCH` is explicitly not selected: per Decision 10, the Z80 side's remaining gap is a precisely
bounded, plausibly tractable capability, not an absence of information or an unbounded requirement, and
the M68K side's own full-refinement-class characterization is unchanged from ADR 0087, not worsened by
this milestone.

## Independent adversarial review

A fresh independent review was run over the full combined diff (`origin/main...task/seg-040-t001`,
product PR #79, head `785e18b` at review time). The reviewer was directed to attack, for the one
track that produced a production mechanism (Z80 interference, T004): bank value changes; bank value
Unknown; wrap/truncation; READ mistakenly counted as WRITE; device access mistaken for RAM; banked ROM
read mistaken for writable memory; M68K work-RAM write; multiple possible bank values; indirect Z80
address; unsupported Z80 instruction in address computation; call/return; loop; unreachable writer;
reachable unknown-target writer; Z80 self-modifying RAM if relevant; Z80 analysis bound exhaustion;
machine mapping alias; 68K-to-Z80 control handoff assumptions — and, for the M68K track's test-only
finding (T002), to independently re-derive the priority-order-independence and second-hazard claims
from unchanged production code rather than trusting this ADR's prose.

[Independent adversarial review results recorded below once the review completes.]

## Consequences

- Production diff of this entire combined delivery: a small, pure, non-mutating `M68kFiniteAdapter::
  memory_write_values()` query (behavior-preserving refactor of `transfer_memory`'s existing
  write-value resolution, zero change to interrupt/frames semantics); a new, Genesis-generic,
  bounded, fail-closed Z80 boot-image producer (`z80_boot_image.{hpp,cpp}`); and the corresponding
  `report.cpp` wiring replacing two previously-hardcoded inputs. Plus two new test-only files
  (`tests/analysis_m68k_frames_priority_order_test.cpp`, `tests/analysis_genesis_z80_boot_image_test.cpp`)
  and this ADR.
- Broad AOT (ADR 0083's compact direct-entry representation) remains the unconditional, sole
  correctness and production strategy for M68K/Genesis. SEG-031's hybrid containment/admission policy
  is unchanged; no title's admission outcome changed as a result of this milestone.
- The M68K interrupt-handler obstacle is unchanged in kind from ADR 0087 but is now additionally known
  to entangle with a second, distinct, hardware-correct frame-integrity hazard at realistic priority
  orderings — reinforcing, not weakening, ADR 0087's full-refinement-class characterization.
- The Z80 interference obstacle is now precisely re-characterized: not "the Z80 proof is imprecise" and
  not "no production glue exists" (both now fixed/adopted), but specifically "the M68K-side boot-image
  derivation is not loop-aware." A future bounded successor adapting the project's existing finite-loop-
  progress machinery (ADR 0016/0035) to recognize a statically-bounded upload loop is a plausible next
  step, not pre-authorized by this closure (per the continuation rules, not registered as a successor
  here without further evidence this specific task's scope could not also gather).
- The SEG-038-T004 fixed mutable-state finding remains durable, unchanged, reusable evidence for any
  future attempt on either track.
