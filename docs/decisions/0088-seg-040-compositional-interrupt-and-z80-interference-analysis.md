# ADR 0088: SEG-040 Compositional Interrupt and Z80 Interference Analysis for Selective M68K AOT

- Status: Accepted (decision: **ADOPT Z80 INTERFERENCE REFINEMENT ONLY**. Shared interrupt-state
  over-poisoning was NOT reduced. The M68K track was attempted twice under two materially distinct
  mechanisms — component-wise status splitting (T002) and a vector-level-gated preemption graph (T003,
  reopened by continuation review) — and both independently confirmed the same architectural
  conclusion: the hazard is a fact about a clobbering child and its own parent partition, not a
  pairwise sibling-preemption relation, so no bounded per-candidate/per-child/per-sibling-pair
  mechanism can resolve it without weakening `apply_resumptions()`. Z80 analysis DID produce a sound,
  bounded, independently-tested M68K write-set derivation mechanism, which is adopted as the new
  production default because it strictly improves correctness/precision over the previous hardcoded
  inputs with zero regressions, and was hardened once more by an independent adversarial fix — but
  neither this mechanism nor an investigated, bounded loop-aware extension of it bound any real
  title's write set measured in this milestone, so hybrid remains broad everywhere. Selective-
  admission research remains justified; continuing is not pre-authorized as a bounded successor by
  this closure.)
- Date: 2026-10-07 (first pass); 2026-10-07 (continuation review, same day)
- Task: SEG-040-T001..T007 (one combined, mixed report-and-production delivery). T003 was initially
  cancelled transitively by T002's STOP, then **reopened by continuation review** (see "Continuation
  review" below) after evidence showed that cancellation was too broad; T003 then independently
  reconfirmed STOP under its own, materially distinct mechanism. T004 was extended once with a
  bounded loop-aware upload-reconstruction investigation, which was honestly declined after actual
  investigation found it would not activate on any measured title.
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

## Outcome — first pass

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
experiment, hypothesized a second, genuinely distinct, hardware-correct cause specific to the
fixture's own priority ordering (at bad-above-good, the good handler's own saved-SR frame also
legally exposed to the bad handler's corruption). **Decision: STOP** on component-wise status
splitting and the per-child corruption-effect-summary alternative specifically. Zero production files
changed; the entire diff is one new test-only file plus its CMake registration.

### SEG-040-T003 (first pass — cancelled): activation condition unmet

Depended on T002 recording CONTINUE. T002 recorded STOP; T003 never ran in the first pass — no
preemption-graph/SCC work was attempted, and no real-title cost was spent on the M68K track beyond
T002's synthetic audit. **This cancellation was reconsidered and reversed by continuation review —
see below.**

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
correctly attempted derivation but honestly found the real boot/communication idiom is not
byte-exactly derivable by this intentionally bounded, non-loop-aware mechanism — an explicitly
anticipated "honest `Unknown/all` result," not a failure, per this task's own acceptance criteria.

### SEG-040-T005/T006 (first pass — done): measurement and closure, classification C

All ten/eleven required metrics measured identical across all four columns on Sonic 1/2/Cool Spot
(zero production change on the M68K side; the Z80-side mechanism never activated on these titles).
Classification **C — compositional approach insufficient** for the measured scope, with both tracks
evaluated independently.

### SEG-040-T007 (first pass): ADR drafted, one adversarial defect found and fixed

A first-pass ADR and independent adversarial review were run against the T004 mechanism (head
`3009742`). **Verdict: PASS, with one genuine soundness defect found and fixed**: the reviewer found
that `derive_genesis_z80_boot_image()`'s worklist only replayed the main-flow (tag 0) partition, but
`report.cpp` unconditionally *replaced* (rather than unioned) the pre-existing, all-partition
`observed_store_ranges` aggregate with this producer's own `area_stores` whenever the latter was
non-empty — silently dropping any reachable interrupt-handler-partition store into the Z80 bus area.
Fixed by folding `tag != 0` Z80-area writes into the "running" group after the main-flow worklist
converges (a handler instance is still never credited into the boot window itself). New regression
test added and passing; full gate reconfirmed (332/332). The reviewer also attempted to independently
reproduce T002's "second hazard" hypothesis from unchanged production code and **could not**: running
the fixture with `FRAMES_DEBUG=1` showed `instances=0`, `frame_integrity_failures=0` for the
bad-above-good ordering — the frame-integrity check T002's Finding 2 cited never actually executes for
that fixture, because an unresolved self-nesting instance is excluded from contributing to *any*
parent's frame-integrity composition, not merely its own. **This finding, together with re-examining
T002's own `priority_order_bad_below_good` fixture, is what triggered the continuation review below**:
T002's STOP was for component-wise status splitting specifically; whether a genuinely different
mechanism (a preemption graph, T003's original, never-attempted scope) could still work had not
actually been tested, and the cancellation of T003 was too broad.

## Continuation review — T003 reopened, T004 extended, T005/T006/T007 rerun

**Correction recorded (same day, same task record lineage, not a new milestone)**: T002's own
`priority_order_bad_below_good` fixture (bad=2, good=4 — no legal preemption edge under MC68000
interrupt-mask priority rules, since 2 is not > 4) independently proves the collateral block in that
case is purely an artifact of the shared `clobbered[parent]`/`state.status` representation, not a
hardware-required conservativism — exactly the condition T003's original, never-attempted
preemption-graph scope was designed to exploit. T002's own STOP correctly rejected component-wise
status splitting and the per-child corruption-effect summary as *insufficient mechanisms*; it did not
test, and could not rule out, whether an explicit preemption-graph mechanism could soundly distinguish
"no real preemption edge" from "a real preemption edge with an unproven effect." **T003 was reopened**
(state corrected from `cancelled` to `ready` directly in the backlog record, documented as an explicit
correction, not a normal lifecycle advance) and actually run.

### SEG-040-T003 (reopened, done): priority-aware interrupt-preemption mechanism — STOP, confirmed with new evidence

Traced, via temporary reverted instrumentation (`git diff -- libs/` empty throughout), exactly why
both priority orderings collapse identically: the self-nesting `bad` handler's own `MOVE SR,#$2000`
unconditionally re-opens mask 0 *regardless of bad's nominal vector level*, creating a self-nested-
`bad`-inside-`bad` contribution that poisons `bad`'s own partition and, because an unanalysed child's
write footprint is conservatively `async_all`, also fails `bad`'s own frame-integrity check against
`bad`'s **own parent** (main flow) — independently of `good`'s level entirely. `clobbered` never
acquires an entry keyed by `good`'s own partition tag in either ordering; only `clobbered[0]` (main
flow) and `clobbered[bad_tag]` are ever populated. This directly explains, and supersedes, T002's
Finding 2 hypothesis the first-pass adversarial reviewer could not reproduce: there is no second,
independent frame-integrity hazard specific to the bad-above-good ordering — both orderings share
the *identical* mechanism, confirmed by tracing, not merely by the earlier test's outcome.

Built a second, deconfounded fixture
(`tests/analysis_m68k_frames_preemption_scope_test.cpp`) specifically to rule out the original
fixture's self-nesting confound (the original `bad` always re-opens every level via its own SR write,
so even the "cannot preempt" ordering has *some* real edge, just not the nominal one): `bad` here
(level 2, strictly below `good`'s level 6) never touches SR at all — no self-nesting possible — and
its only defect is one ordinary, unrelated, undescribed-target memory write having nothing to do with
interrupts. Under plain MC68000 priority, `bad` cannot preempt `good`'s body by any definition. Result:
**`good` is still collaterally blocked**, identically to the self-nesting case — proving definitively
that the shared-partition collapse is not a pairwise `bad`-preempts-`good` eligibility fact. A
companion control (same fixture, `bad`'s write target made precise) confirms both handlers become
fully analysed once the isolated defect is removed, isolating the defect to exactly the
unresolved-write-target frame-integrity path.

**Why the prescribed vector-level-gated preemption graph is the wrong mechanism, not merely
unimplemented**: gating `bad`'s contribution to `clobbered[0]` (main flow) by a comparison against
`good`'s mask has no architectural justification and would be **unsound** — main flow's own SR really
may be corrupted by `bad`'s unanalysed write/RTE regardless of `good`'s level, and suppressing that
fact using an unrelated sibling's priority would let `good`'s resumption be falsely "proven" in a case
where the hardware genuinely could have left main flow's own processor status unprovable before
`good` was ever taken. A sound fix would require decomposing `clobbered[tag]` by originating cause and
re-deriving, per consuming point, whether a specific cause's hazard actually reaches that point via
genuine path/history sensitivity relative to the shared parent's own propagated status — exactly the
unrestricted path-sensitive/history-domain territory this task's own pre-registered stop rule forbids,
and exactly the non-monotone, cross-candidate-retractable fixed point (or foundational
status-representation split) ADR 0087 already scoped as a full-refinement-class architecture change.

**Decision: STOP, reconfirmed under a materially different mechanism with stronger evidence.** No
SCC/cyclic composition was attempted (the acyclic case itself already fails for architectural, not
merely implementation, reasons). No production code changed; the entire diff is one new test-only
file. Full gate at this exact head: 100% passed, 0 failed out of 333.

### SEG-040-T004 extension (done): bounded loop-aware boot-image reconstruction — investigated and declined

Investigated, using only segarecomp's own M68K decoder/analysis tooling (Ghidra was unavailable in
this environment — no running docker-compose stack — and was not required), whether a bounded,
statically-fixed-trip-count copy-loop recognizer could materialize an exact boot image for Sonic 1,
Sonic 2, and Cool Spot. Finding, reduced to non-reconstructable classifications: the real Z80-area-
touching M68K code reachable in Sonic 1 and Sonic 2's main flow is **not** a statically-bounded
fixed-trip-count copy loop; it is a general-purpose, data-dependent bitstream (LZ/run-length-style)
decompression routine whose per-iteration trip counts are themselves decoded at runtime from
compressed immutable cartridge bytes, and which performs back-reference reads from its own
already-written destination bytes. Cool Spot's surface is consistent with the same picture. Soundly
crediting the decompression shape would require encoding the cartridge's compression-format semantics
as a bounded interpreter inside this producer — precisely the "new CPU-adjacent semantics"/"general
loop-recognition framework" explicitly forbidden by this extension's own scope. **Decision: do not
implement a bounded copy-loop recognizer** — it would be dead code on every real title measured, and
shipping an unexercised mechanism risks silently papering over the real (compression) frontier instead
of reporting it honestly. Documented as a production, comment-only addition to `z80_boot_image.hpp`.
No new test added (no new behavior exists to test); existing suite re-run and passed unchanged.

Separately, the first-pass adversarial fix (folding handler-partition Z80-area stores into the
classification, rather than silently dropping them) is adopted as part of T004's own mechanism.

### SEG-040-T005/T006 (reopened, done): rerun from the exact post-continuation head

Rerun (head `2d91ef7`) with Release rebuilt at the current head. All eleven/ten required metrics
reconfirmed **byte-identical** to the first pass for every title (`U`/`D`/trigger breakdown/solver
completion all unchanged) — expected, since neither T003's reconfirmed STOP nor T004's declined
extension touched the M68K solve path that produces `D`/`U`. The one count that changed is the Z80-
area store *classification* precision (via the adversarial fix): Sonic 1's `known_ranges` moved from 0
to 33 and `unknown_target_stores` from 24 to 386 (Sonic 2: 1→1 unchanged range count, 34→73 unknown
stores; Cool Spot: 0→0 unchanged range count, 2,577→2,582 unknown stores) — a diagnostic precision
improvement, not an outcome-class change (`z80_ram_write_proof.outcome` remains `all` for all three).
**Classification: C, reconfirmed**, now resting on stronger, independently re-examined evidence for
both tracks rather than a merely-cancelled prerequisite.

## Outcome — final decision (SEG-040-T007, this task)

**Decision 1 — was shared interrupt-state over-poisoning reduced?** **NO.** Neither T002's
component-wise status splitting nor T003's reopened vector-level-gated preemption-graph mechanism
implemented any production change; `clobbered[tag]`/`effective_status()` are byte-identical to
pre-SEG-040 `main`. Both attempts independently confirm the same architectural root cause.

**Decision 2 — did per-handler/preemption composition work?** **NO**, and not merely "not attempted"
this time: T003 was reopened specifically to test this, traced the exact mechanism with temporary
instrumentation, and found that the prescribed vector-level-gated edge construction would be
*unsound*, not merely unimplemented — the hazard it would need to gate is a fact about a clobbering
child and its own parent partition, independent of any specific sibling's priority.

**Decision 3 — could recursive interrupt SCCs be summarized safely?** **N/A.** The acyclic two-handler
case itself already fails for the architectural reason in Decision 2; no SCC/cyclic composition was
attempted, consistent with the task's own instruction to stop at the acyclic case if it fails rather
than escalate scope.

**Decision 4 — did Z80 analysis produce bounded M68K write sets?** **Partially, and now additionally
confirmed via a declined-but-investigated loop-aware extension: the mechanism can** (proven on 5/5
synthetic tests, including a positive case that bounds a write set to `none`), **but it did not on any
of the three real titles measured**, and a deliberate, genuine attempt to extend it with bounded
loop-awareness was investigated and honestly declined because the real titles' upload idiom (a
data-dependent decompression routine) falls structurally outside any bounded copy-loop recognizer's
scope — not merely outside this specific implementation's reach.

**Decision 5 — did any title stop treating all work RAM as asynchronously writable?** **NO**, on the
titles measured, in both the first pass and the rerun. `z80_ram_write_proof.outcome` remains `all` for
Sonic 1, Sonic 2, and Cool Spot.

**Decision 6 — did finite mutable M68K cells appear in credited analysis?** **NO.** `memory.max_cells`
remains 0 for all three titles in both passes — unchanged from the ADR 0086/0087 baseline.

**Decision 7 — did exact PC-index/address targets increase?** **NO.** `pc_index_recovery.resolved`
and `address_recovery.resolved` remain 0 for all three titles in both passes.

**Decision 8 — did hybrid become non-broad?** **NO.** `hybrid_total/U = 1.000000` for every title that
completes credited analysis, identical to the pre-SEG-040 baseline, reconfirmed on rerun.

**Decision 9 — what blocker dominates next?** **Two independent, precisely-named blockers, now
confirmed under two independent mechanism attempts each rather than one.** M68K side: the frames
domain's *shared, cross-candidate* status fact (ADR 0087), now additionally confirmed — via a
materially different, specifically-designed-to-test-this mechanism (T003's preemption graph), not
merely via T002's narrower component-split finding — to be a fact about a clobbering child and its own
parent partition, independent of any sibling's priority; resolving it requires a non-monotone,
cross-candidate-retractable fixed point or a foundational status-representation split, both
full-refinement-class architecture decisions. Z80 side: the specific, narrower gap is that the real
titles' upload idiom is a data-dependent bitstream decompression routine, not a statically-bounded
copy loop — confirmed by actual investigation (not assumption) using segarecomp's own tooling. Unlike
the M68K side, this is **not** characterized as an unbounded or architecturally unresolvable gap in
general — it is simply outside what *any* bounded, Genesis-generic copy-loop recognizer (loop-aware or
not) could soundly reach for these specific titles, since the real obstacle is compression-format
semantics, not loop structure.

**Decision 10 — is continuing selective-admission research still justified?** **YES.** SEG-038-T004's
finite mutable-state/exact-PC-index finding under the uncredited oracle combination remains real,
measured, and unweakened by this milestone, including its continuation. This milestone (both passes
combined) materially narrows what remains, now with each track tested under two independent mechanism
attempts: the M68K side needs a full-refinement-class architecture decision (unchanged conclusion from
ADR 0087, now reconfirmed rather than merely untested against a second mechanism); the Z80 side's
remaining gap is specifically a cartridge compression-format semantic, not a loop-recognition
capability — a materially different (and likely larger-scope) problem than "add loop-awareness," which
this milestone correctly avoided mischaracterizing as such. The bar for `STOP SELECTIVE-ADMISSION
RESEARCH` (the remaining path being disproportionate or fundamentally unbounded) is not met by this
milestone's evidence for either track: the M68K side's full-refinement-class characterization is
unchanged from ADR 0087 (not worsened), and the Z80 side's newly-precise characterization (compression
semantics, not loop structure) is a different, not necessarily harder, problem that this milestone
simply did not attempt to solve (it was explicitly out of scope for a bounded extension).

**Final classification: `ADOPT Z80 INTERFERENCE REFINEMENT ONLY`.** The Z80-side production code
(`z80_boot_image.cpp`, the `memory_write_values()` query, the corrected `report.cpp` wiring, and the
adversarial fix folding handler-partition stores into the classification) is adopted as the new
production default: it is strictly more correct than the code it replaces, is fully covered by
regression tests with zero observed regressions across the full existing suite (333/333 passing at
the final head), and provides genuinely improved diagnostic precision even though it did not change
any measured title's credited admission outcome. The M68K-side refinement is not adopted because none
was implemented under either of the two independently-attempted mechanisms (T002, T003). `ADOPT BOTH
COMPOSITIONAL REFINEMENTS` is not selected because there is no M68K refinement to adopt. `STOP
SELECTIVE-ADMISSION RESEARCH` is explicitly not selected: per Decision 10, neither track's remaining
gap is an absence of information or a disproof of the underlying finite-state hypothesis — both are
precisely-located, evidence-backed obstacles, one full-refinement-class, one a different (compression-
semantic) problem than this milestone attempted.

## Independent adversarial review (final pass, exact head)

A fresh independent review was run over the full combined diff (`origin/main...task/seg-040-t001`,
product PR #79, final head). The reviewer was directed to attack, for every track that produced a
production mechanism (Z80 interference, T004, including the adversarial-fix commit): bank value
changes; bank value Unknown; wrap/truncation; READ mistakenly counted as WRITE; device access mistaken
for RAM; banked ROM read mistaken for writable memory; M68K work-RAM write; multiple possible bank
values; indirect Z80 address; unsupported Z80 instruction in address computation; call/return; loop;
unreachable writer; reachable unknown-target writer; Z80 self-modifying RAM if relevant; Z80 analysis
bound exhaustion; machine mapping alias; 68K-to-Z80 control handoff assumptions — and, for the M68K
track's two independent test-only findings (T002, T003), to independently re-derive the
priority-order-independence claim, the deconfounded preemption-scope claim, and the architectural
"gating would be unsound" argument from unchanged production code rather than trusting this ADR's
prose.

[Independent adversarial review results recorded below once the review completes.]

## Consequences

- Production diff of this entire combined delivery (both passes): a small, pure, non-mutating
  `M68kFiniteAdapter::memory_write_values()` query (behavior-preserving refactor of
  `transfer_memory`'s existing write-value resolution, zero change to interrupt/frames semantics); a
  new, Genesis-generic, bounded, fail-closed Z80 boot-image producer (`z80_boot_image.{hpp,cpp}`),
  including the adversarial-fix commit folding handler-partition stores into its classification; and
  the corresponding `report.cpp` wiring replacing two previously-hardcoded inputs. Plus four new
  test-only files (`tests/analysis_m68k_frames_priority_order_test.cpp`,
  `tests/analysis_m68k_frames_preemption_scope_test.cpp`,
  `tests/analysis_genesis_z80_boot_image_test.cpp`, and its adversarial-fix regression addition) and
  this ADR. Zero change anywhere to `effective_status()`, `clobbered` population, or
  `apply_resumptions()`.
- Broad AOT (ADR 0083's compact direct-entry representation) remains the unconditional, sole
  correctness and production strategy for M68K/Genesis. SEG-031's hybrid containment/admission policy
  is unchanged; no title's admission outcome changed as a result of this milestone.
- The M68K interrupt-handler obstacle is unchanged in kind from ADR 0087 but is now confirmed under
  two independent, materially different mechanism attempts (component-wise status splitting;
  vector-level-gated preemption-graph composition), both reaching the same architectural conclusion
  for precisely-traced reasons — reinforcing, not merely repeating, ADR 0087's full-refinement-class
  characterization. A future attempt should not re-attempt either of these two mechanism classes; both
  are now closed with evidence, not merely untested.
- The Z80 interference obstacle is now precisely re-characterized a second time: not "the Z80 proof is
  imprecise," not "no production glue exists," and not "the derivation is not loop-aware" (all three
  now investigated and resolved/fixed/declined-with-evidence), but specifically "the real titles' Z80
  upload idiom is a data-dependent cartridge compression format, not a loop-recognition gap." A future
  successor targeting this would need to reconstruct or interpret the cartridge's own compression
  semantics — a materially different, likely larger-scope undertaking than this milestone's bounded
  extension — and is not pre-authorized by this closure.
- The SEG-038-T004 fixed mutable-state finding remains durable, unchanged, reusable evidence for any
  future attempt on either track.
