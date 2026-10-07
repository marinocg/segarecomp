# ADR 0089: SEG-041 External Proof Backend and Hybrid Selective-AOT Recompilation Map

- Status: Accepted (decision: **ADOPT HYBRID RECOMP-MAP / ANGR EXPERIMENTAL**. angr, once actually
  obtainable, proved genuinely leverageable on BOTH tracks this milestone pursued. Z80 track (real
  title): a real Z80 boot image was recovered from Sonic 1 by concretely executing the guest's own
  data-dependent decompression routine, independently certified byte-exact against the project's pinned
  Musashi oracle, and fed through the existing production `z80_images` consumer with a measured effect.
  M68K track (synthetic only, after a real soundness defect was found by independent review and
  corrected): a new, structurally-re-verified, fail-closed `GenesisExternalM68kFacts` consumer was built
  into the existing SEG-031 planner (PASS-WITH-MINOR adversarial review) and a genuinely sound
  exact-target producer (`tools/segarecomp_angr_m68k_facts.py`, corrected after an operator-identified
  exhaustiveness defect, re-reviewed PASS) closed a project-authored synthetic from `broad_whole_image`
  (`H/U=1.000000`) to `hybrid` (`H/U=0.000366`) end to end. No available p-code variant is bit-exact
  MC68000 (segarecomp's own CPU-legality authority remains mandatory and was exercised, not assumed, on
  both tracks), and angr's RTE lift carries no usable exception-return semantics at all. **Real
  commercial-title `complete_hybrid` has not yet been demonstrated on the M68K track** (all 13 of Sonic
  1's real unresolved sites were inspected and found to be calling-convention/interrupt-frame-adjacent,
  not the shape this backend's demonstrated strength addresses); a new milestone, SEG-042, is registered
  (not implemented here) to pursue that question once this PR is merged. Status is EXPERIMENTAL, not
  full production adoption.)
- Date: 2026-10-07/08 (first pass, environment-unavailable REJECTED; same-day correction after the
  operator fixed the execution environment; T008 implemented, then corrected after an operator-
  identified soundness defect in the producer's exhaustiveness proof, then independently re-reviewed)
- Task: SEG-041-T001..T008 (one combined, mixed report-and-production delivery; T002's verdict was
  corrected in place after an environment fix, which reopened T003-T006 from their original transitive
  cancellation; T008, a continuation successor for the deferred M68K-track production integration, was
  implemented, found to have a real exhaustiveness-soundness defect by independent review, corrected,
  and re-reviewed PASS, all within this same task before completion).
- Related, unchanged: ADR 0080 (SEG-031 hybrid admission planner; adoption threshold not reached, not
  moved), ADR 0083 (SEG-036, the current sole production strategy for every title not touched by this
  milestone's narrow Z80-image consumer change), ADR 0088 (SEG-040, the direct predecessor whose Z80-
  track finding — a data-dependent decompression routine, not a statically-bounded copy loop — this
  milestone directly resolves via external concrete execution rather than hand-modeling).

## Question

ADR 0088 (SEG-040) located the Z80 side's remaining wall precisely as a data-dependent cartridge-
compression routine over immutable ROM data, and declined to hand-model it. SEG-041 asked: can an
optional external backend (beginning with angr) do what segarecomp's own bounded analyses could not,
producing facts the existing SEG-031 planner can consume, while global broad AOT remains the
unconditional, tooling-free correctness fallback?

## Outcome

### SEG-041-T001 (done): hybrid recomp-map architecture and trust contract — CONTINUE

Unchanged from the first pass: the proposed `segarecomp-recomp-map-v1` architecture (map schema,
completeness classes, island semantics, trust tiers, fallback table, cache identity, producer/consumer
contract) was designed as a conservative extension of the already-production `GenesisHybridAdmissionPlan`/
`GenesisHybridPlan` types, not a parallel system, and does not resemble a general-purpose decompiler IR.

### SEG-041-T002 (done): angr/p-code backend qualification — corrected to `PARTIAL`

The original pass recorded `REJECTED` on pure environment-availability grounds (angr unobtainable under
the session's original interpreter). The operator subsequently fixed the execution environment (a
different `asdf`-managed Python with angr pre-installed, and working package-mirror credentials); this
task was reopened and genuinely re-qualified rather than left on a stale environment finding, per the
operator's explicit instruction not to cancel work solely for a since-resolved environment gap.

Real findings, not hypothetical: angr's ordinary `archinfo` architecture path does not support M68K at
all; the working path is the p-code-native `ArchPcode` class. Exactly five 68000-family p-code
variants exist (`default`/labelled 68040, `MC68030`, `MC68020`, `Coldfire`, `CPU32`) and **none is
labelled or behaves as plain MC68000**. Decoding segarecomp's own pre-existing, already-validated
65,536-word MC68000 legality partition against all five variants found concrete, reproducible over-
acceptance: `default`/`MC68030`/`MC68020`/`Coldfire` all decode genuine 68010+/68020+-only forms
(`cmp2.l`, `cas.w`, `moves.l`, long-operand `chk.l`, `pack`, bitfield ops) as valid where real MC68000
silicon would take an illegal-instruction trap; `Coldfire` additionally repurposes the reserved Line-A
opcode space for real, different Coldfire-only instructions; `MC68020`/`MC68030` repurpose reserved
Line-F space for real MC68881/68882 FPU instructions. `CPU32` has the narrowest (not zero) false-
acceptance footprint across all three reserved classes sampled. **Decisive, specific finding**: `RTE`
lifts, under every available variant, to a bare "return to address 0" with no SR pop, no PC pop, no
frame-format read at all — not an imprecise exception-return model, an absent one; angr itself carries
no M68K-specific `SimProcedure` to patch this. Separately, full angr symbolic execution over M68K via
`ArchPcode` was confirmed genuinely working end-to-end (project construction, blank-state symbolic
bitvector creation, constraint solving, and one-instruction stepping all succeeded) despite this angr
release vendoring its own internal `claripy` reimplementation rather than shipping it as a separate
top-level package, and despite `unicorn` concrete-execution acceleration being unavailable (an optional
speed feature only). **Verdict: `PARTIAL`** — qualified for ordinary (non-RTE) M68K control-flow and
data semantics; explicitly not usable for RTE-centric/exception-frame semantic-completeness facts. This
revised verdict meets SEG-041-T001's pre-registered gate for T003/T004 (QUALIFIED or PARTIAL for a
backend able to concretely execute real guest M68K code), reopening both from their original transitive
cancellation.

### SEG-041-T003 (done): Z80 boot-image reconstruction — primary success condition MET

Reopened and genuinely executed. Using segarecomp's own existing production report tool to re-confirm
the unchanged ADR 0088 baseline first, a short structural scan located the one M68K subroutine Sonic 1
calls, while the Z80 is held in `BUSREQ`+`RESET`, that reads from immutable ROM data and writes to the
Z80 RAM base — exactly the data-dependent decompressor ADR 0088 named without an address. Concretely
executing that routine with `angr`'s qualified p-code engine (bounded, 60,417 steps) produced a
complete, unique 7,110-byte decompressed image whose leading bytes are recognizably real, well-formed
Z80 startup code (interrupt-disable, stack/index-register initialization, bank-register programming).

**The decisive verification step**: re-executing the identical routine and inputs through the project's
own pinned Musashi oracle (genuine `MC68000` CPU type, via a new, small, non-production blob-memory
harness reusing the vendored Musashi sources unmodified) reached the identical return point and produced
a **byte-identical (SHA-256-equal) 7,110-byte image** — independent cross-validation against an
authority genuinely outside the angr/pyvex/p-code stack, not a self-consistency check. Separately,
every opcode word angr actually executed (65 distinct addresses) was independently re-verified against
segarecomp's own MC68000 legality partition: **100% legal**, confirming this specific routine does not
exercise any of T002's found over-acceptance classes. Feeding the recovered, certified image into the
existing (unmodified) `z80_images` production input resolved `image_set_unknown` and let the Z80-side
analysis reach 104 real instructions; the overall `z80_ram_write_proof` bound remains `all` because a
second, independent, already-present class of uncertainty (386 M68K-side unknown-target stores into the
Z80 bus area, numerically unchanged before/after, most plausibly ordinary runtime M68K-to-Z80
communication) is untouched by this task — an honest, measured, partial effect, not a forced "none."

### SEG-041-T004 (done): M68K exact-target and containment proof experiment — PASS

Reopened, scoped to the backend's qualified (non-RTE) subset per T002. Built the mandatory containment
synthetic exactly as specified (region A -> dynamic `jmp (An)` site -> contained island I with two
entries -> declared exits -> region B), using project-authored, self-disassembly-verified MC68000 bytes.
**Exact-target proof**: symbolic exploration from a fully-symbolic decision cell correctly enumerated
the computed jump's feasible target set as precisely the two island-entry addresses — no spurious or
missing target. **Containment proof**: exploring from each island entry found zero violations against
the declared domain. **Mutation**: corrupting one island exit to escape the declared domain was
correctly detected by the identical, unmodified containment-check procedure as a violation — the
required widen-or-incomplete behavior, driven by real discovered evidence. A general, reusable
island-widening *loop* (as opposed to the detection mechanism demonstrated here) was designed but not
implemented as production code in this task.

### SEG-041-T005 (done): end-to-end producer + consumer — Z80 track delivered, M68K track deferred

Reopened once the gate (T002 PARTIAL + T003/T004 leverage) was met. **Delivered**: the Z80 track,
completely, as a real producer (T003's certified concrete-execution mechanism) feeding a real, minimal,
reviewed consumer change — a new report-only `--z80-image` flag on `segarecomp-genesis-analysis-report`
(never linked into `segarecomp`/the compiler/runtime) that feeds the existing, unmodified
`GenesisAnalysisReportConfig::z80_images` input the production driver already consumes. **Not
delivered, by deliberate scope decision rather than any environment limitation**: the M68K track's
production integration (the full `segarecomp-recomp-map-v1` schema, its validator, and extending
`GenesisHybridSite`/`island_entries` to consume T004's validated mechanism) — correctly judged too
large and too risky (a correctness-critical planner component) to rush within this task's remaining
budget without adequate independent review. Deferred to the newly-created SEG-041-T008, not abandoned.

### SEG-041-T006 (done): real-title measurement — one complete row, two honest gaps

Measured the delivered Z80-track pipeline on Sonic 1 in full (see T003's numbers). Attempted the same
structural-scan technique on Sonic 2 (found an analogous boot-time call site, but it writes a short
fixed literal sequence, not a decompression call — a different, already-exactly-solvable case, not
evidence against the approach) and Cool Spot (no analogous call site located, consistent with ADR
0088's own prior note that Cool Spot's Z80 surface "was not fully hand-traced given the scale"). Neither
gap was forced to a false positive. The mandatory fallback matrix reduces, for the one real change this
milestone shipped, to the already-true, now-reconfirmed fact that `broad`/the unmodified default
production path requires zero external tooling and is unaffected by this change's presence or absence.

### SEG-041-T007 (this task): decision and independent adversarial/fallback gate

**The sixteen required questions (final pass, after SEG-041-T008's implementation and correction):**

1. **Recomp-map abstraction sound/useful?** Sound, and now partially implemented: `GenesisExternalM68kFacts`
   is a real, PASS-WITH-MINOR-reviewed, production-integrated input to the planner (T008), not merely a
   paper design (T001). The full multi-site, multi-track `segarecomp-recomp-map-v1` schema remains
   unimplemented.
2. **Island abstraction sound/useful?** Already production-sound (pre-existing `GenesisHybridSite`); the
   externally-proven container class (`exact`, and `points_to_region` for "contained" facts) is now
   wired in and reviewed, demonstrated end to end on a synthetic (T008). A general, reusable
   island-*widening* loop (as opposed to the detection T004 demonstrated) remains unimplemented.
3. **Every difficult site classified exact/island/unresolved?** Schema enforces no silent third case by
   design (T001); the real consumer now populates `exact`/`points_to_region`/`whole_image` for a single
   externally-supplied fact per site (T008); a real multi-site, multi-title producer is SEG-042's job.
4. **Did angr qualify for the required MC68000 subset?** Partially: yes for ordinary control-flow/data
   forms (demonstrated twice — Z80 real-title concrete execution, M68K synthetic exact-target proof,
   both independently re-verified); no for RTE (demonstrated absent, not merely imprecise).
5. **Did angr recover any real Z80 boot image?** **YES** — Sonic 1, byte-exact-certified.
6. **Did Z80 interference improve?** Partially: `image_set_unknown` resolved; overall bound unchanged
   (`all`) due to an independent, unrelated remaining source of uncertainty.
7. **Did angr resolve exact dynamic target sets?** **YES, soundly, on the mandatory synthetic** (after a
   real exhaustiveness defect was found and corrected — the original algorithm could have under-approved
   a target set; the corrected one is independently re-reviewed PASS). **Not on any real title's
   computed sites**: attempted on Sonic 1's 11 non-RTE unresolved sites; none resolved (calling-
   convention/interrupt-frame-adjacent, not a "needs concrete data" gap). SEG-042 carries this forward.
8. **Did angr prove useful containment where exact proof failed?** **YES**, demonstrated on the
   synthetic (T004's mutation-detection experiment); not yet applied to a real title or wired as a
   general widening loop.
9. **Did any real title produce `complete_hybrid`?** **NO** — the Z80-track real-title result used a
   single-fact input, not a map/classification; the M68K-track `complete_hybrid` result is synthetic
   only.
10. **Did any produce `complete_selective`?** **NO**, same reason.
11. **Selective count, island count, `H`, `U`, `H/U`?** Measured on the project-authored synthetic only:
    `U=8191`, `H=3`, `H/U=0.000366`, 1 island (`exact`). **Not measured on any real title** — the
    credited Sonic 1 hybrid-plan baseline is confirmed unchanged (`U=246293`, `D=1276`,
    `whole_image_fallback_count=13`, `external_facts_applied=0` with no facts supplied).
12. **Generated-source/compile reduction?** **NONE on any real title** — zero emitter/runtime/generated-
    code bytes changed anywhere in this milestone, on any title.
13. **Did behavior match broad AOT?** Trivially yes — nothing in the generated-code path changed for any
    title; the synthetic's `hybrid` plan is a planner-only artifact, never emitted/compiled/run.
14. **Did fallback work with zero external tooling?** **YES** — both new consumer inputs (`--z80-image`,
    `--external-m68k-facts`) are optional and additive; the unmodified default path is completely
    unaffected by either, independently re-verified for the M68K track's own producer-side fallback
    matrix (resource exhaustion, errored/unconstrained paths, entry-bound violations, unasserted
    premises) by both adversarial reviews.
15. **Should `auto` remain opt-in or become default?** Moot — no `auto`/strategy-selection code exists yet
    (only two optional, non-default, single-purpose consumer inputs).
16. **Should angr remain the producer, be experimental, or be rejected?** **Experimental, with real
    demonstrated value on both tracks, and a real soundness defect found and corrected on one of them.**
    Not rejected (it worked on a real title and, after correction, soundly on a synthetic); not full
    production adoption (no map format, no multi-site real-title producer, no real-title `complete_hybrid`
    yet — SEG-042's job).

**Distinguishing "angr failed" from "the hybrid recomp-map architecture failed":** neither failed.
angr demonstrably succeeded on both its real-title and synthetic tests, within a clearly-bounded
subset (not RTE). The recomp-map/island architecture was validated on paper and partially exercised
(via direct consumption of an existing input, bypassing the full map format) rather than fully built.

**Final classification: `ADOPT HYBRID RECOMP-MAP / ANGR EXPERIMENTAL`.** Not `ADOPT ... + ANGR BACKEND`
(full production integration — a multi-site, multi-title real producer and the full map format — does
not yet exist; real-title `complete_hybrid` has not been demonstrated); not `ADOPT RECOMP-MAP + ISLAND
CONTRACT ONLY` (angr itself delivered real, specific, credited value on both tracks, not merely
informing an architecture); not `REJECT ANGR` (it worked, on a real title and, after a real defect was
found and corrected, soundly on a synthetic); not `REJECT EXTERNAL HYBRID ANALYSIS` (the opposite of
what was measured). SEG-042 (a new, separate milestone, not a continuation successor under SEG-041) is
registered — but not implemented on this PR — to carry the real-title M68K measurement question forward
once this PR is confirmed merged.

### SEG-041-T008 (done): M68K-track external-facts producer and SEG-031 island consumer — implemented,
corrected, independently re-reviewed PASS

Delivered the M68K-track half T005 deferred: `GenesisExternalM68kFacts`, an optional, ROM-bound,
fail-closed input to the **unchanged** `genesis_hybrid_container()`/`validate_genesis_hybrid_round()`
pure functions (`platforms/genesis/analysis_report/{include,src}/.../hybrid_plan.{hpp,cpp}`). External
facts are consulted only at the points the existing function would otherwise return `whole_image`,
never overriding an already-sound internal classification; every cited entry is independently
re-verified against segarecomp's own `image.mapped()`/`image.decode()` authority before being trusted;
one unverifiable entry discards the whole fact, never partial trust; the validator is given the
identical facts the planner used, preserving the existing "freshly recompute and compare" soundness
property. A new `segarecomp.m68k_external_facts.v1` parser mirrors `GenesisHybridAdmissionPlan`'s
existing conventions; a new report-only `--external-m68k-facts` CLI flag (requires `--hybrid-plan`;
never linked into `segarecomp`/the compiler/runtime) wires it in. **A first independent adversarial
review of this diff (correctness-critical SEG-031 planner extension) returned `PASS-WITH-MINOR`**: zero
soundness defects in the consumer/validator/parser; three non-blocking documentation/test-coverage
refinements, all addressed in the same task.

**A second, independent (human) review of the accompanying Python producer,
`tools/segarecomp_angr_m68k_facts.py`, found a real soundness defect before this task could be
considered complete**: the original exhaustiveness algorithm used an "idle step" heuristic as its
completeness signal, which could under-approximate a computed-jump target set (a short feasible path
found quickly, a longer feasible path to the identical site found later, discarded by the idle timer)
— an unsound `exact` claim exactly in the one place this two-tier trust model cannot catch it after
structural re-verification passes. **Corrected**: exploration now runs every feasible path to genuine
closure (no active state remains, no errored/unconstrained path was ever seen, the step bound was never
exhausted with unresolved paths) before declaring anything `exact`; a query-complete state is removed
from stepping via `simgr.move` (a direct `simgr.active` reassignment was found, by hand-debugging, to
silently break angr's own internal stash bookkeeping); every opcode word visited is checked against
segarecomp's own `m68k-legal-forms.json` word-class partition (a proof-path screen, not full structural
requalification, which remains a named gate for the SEG-042 successor); `--ram-premise` now requires an
explicit `--caller-asserts-premise-completeness` assertion, since a caller-supplied starting-scope's
completeness is not something this tool or segarecomp can verify. **An independent re-review of the
correction returned `PASS`** (not `PASS-WITH-MINOR`; per the explicit bar, no mutation was found that
causes an omitted feasible target while the tool still emits `exact`) — including a reviewer-constructed
second poisoning mechanism (an illegal/reserved opcode fetched mid-path) tried across 160 sampled cases
with zero false `exact` credits.

**A third, independent (human) review identified one more bounded correctness issue before this task
could be considered complete**: the corrected (exhaustive) producer could still traverse an `RTE`
(0x4E73) instruction on its way to a queried site and emit a credited `exact` fact, even though
SEG-041-T002 already established `RTE` has no usable MC68000 exception-return semantics in the
available p-code backend — a violation of the producer's own qualified subset (ordinary, non-RTE
control/data semantics only), since `RTE` is itself a legal base-MC68000 opcode and so was never caught
by any prior check. **Corrected**: every opcode word any explored state fetches is now checked for this
single, exact value; encountering it anywhere poisons the whole proof immediately
(`unsupported_proof_path_rte`) — a one-line qualification-boundary check, not RTE emulation, p-code
patching, exception-frame reconstruction, or a general instruction-blacklist framework. (A prior,
incorrect attempt at a *general* proof-path legality screen, reading the project's independent
legal-base-MC68000-form test-side dataset, was removed in the same correction: that dataset is
deliberately decoupled from every consumer except two specifically whitelisted ones, enforced by its
own dedicated independence test in both directions, which the full gate correctly caught as a
violation.) A second, unrelated full-gate regression from an earlier commit in this same task — three
`return site;` lines inside `genesis_hybrid_container()` had been rewritten, breaking two of the
project's own exact-string-match mutation-testing operators — was fixed in the same correction by
restructuring into an internal helper byte-for-byte identical to the pre-T008 function body, with a
single, centralized external-fact consultation added only where that unmodified internal ladder
returns `whole_image`. **A third independent re-review returned `PASS-WITH-MINOR`**: the RTE exclusion
was confirmed sound (fires pre-execution, confirmed load-bearing via a disabled-check negative control
that reproduced the original unsound `exact` claim on the identical fixture) and narrowly scoped (a
single exact-value check, not a broader framework); the restructuring was confirmed line-for-line
identical to the pre-T008 baseline and behavior-preserving, with one real-but-currently-unreachable gap
identified (the centralized wrapper had stopped distinguishing an undecodable-site early exit from the
other three `whole_image` exits) and fixed in the same task (the wrapper now re-checks `image.decode(pc)`
explicitly alongside the `whole_image` condition).

**End-to-end result (project-authored synthetic, no commercial input, now backed by a genuinely sound
exhaustiveness proof, with RTE explicitly excluded, rather than either earlier unsound/under-scoped
version): `broad_whole_image` (`H/U = 1.000000`) -> `hybrid` (`H/U = 0.000366`)** via the real pipeline:
the corrected angr producer -> a written
`segarecomp.m68k_external_facts.v1` file -> the unmodified structural re-verification gate -> the
unmodified SEG-031 planner -> the CLI's `--hybrid-plan` artifact. **This result was not re-derived from
a real title in this task**: a real Sonic 1 measurement was attempted (all 13 of Sonic 1's currently-
unresolved dynamic sites were inspected structurally); none was resolved — the RTE sites (2) are
out of scope per T002; the remaining sites (4 `pc_index_explicit`, 7 `rts_computed`) are
calling-convention/interrupt-frame-adjacent ambiguities (shared-epilogue stack-history dependence, one
instance of a deliberate extra stack adjustment before `RTS`), the same architectural class ADR 0087/
0088 already named, not a "needs concrete data" gap this backend's demonstrated strength addresses.
The credited Sonic 1 hybrid-plan baseline is confirmed byte-for-byte unchanged with this task's change
present but no external facts supplied (`U=246293`, `D=1276`, `whole_image_fallback_count=13`,
`external_facts_applied=0`).

**A newly-created, separate milestone, SEG-042 ("Real-title external-fact harvest and hybrid-AOT
evaluation"), was created in the harness backlog (not implemented on this PR) to carry the real-title
measurement question forward**: SEG-042-T001..T007, all `draft`, gated on this milestone's product PR
(`#80`) being confirmed **merged** (not merely marked `done` in the backlog) before SEG-042-T001 is
promoted to `ready`.

## Mandatory fallback test matrix

For the Z80-track `--z80-image` flag: unchanged from the first pass above (optional, `nullopt`-default
byte-for-byte unaffected). For the M68K-track `--external-m68k-facts` flag, verified directly against
the built CLI (not merely designed): a wrong-ROM-hash fact file is rejected outright (exit 2, no plan
written); a missing file is rejected (exit 2); a tampered fact citing one unverifiable (odd) target
among its entries is discarded in its entirety, falling back to `broad_whole_image` exactly as if no
fact had been supplied (`external_facts_applied: 0`); using the flag without `--hybrid-plan` is a clear
usage error. The producer's own fallback matrix (resource exhaustion, an errored/unconstrained path, an
entry-count-bound violation, an unasserted premise) all correctly yield "nothing written" (exit 1 or 2),
independently verified by both adversarial reviews. The broader strategy-selection fallback matrix
(`auto` degrading to broad, explicit `hybrid` failing closed at the CLI-strategy level rather than the
map-fact level) remains unimplemented — there is still no `broad`/`hybrid`/`auto` strategy-selection
layer, only the two additive, optional, non-default consumer inputs (`--z80-image`,
`--external-m68k-facts`) described above.

## Independent adversarial/completion gate

Two dedicated adversarial-validator subagent reviews ran against this milestone's correctness-critical
SEG-031 planner extension and its accompanying producer (not a self-conducted review, given the change
touches a correctness-critical component): the first (C++ consumer/validator/parser) returned
`PASS-WITH-MINOR` (three non-blocking refinements, all addressed); the second (after the operator's
correction request, focused on the Python producer's exhaustiveness algorithm specifically) returned
`PASS` with no soundness defect found, including reviewer-constructed mutations beyond what this task's
own tests covered. Checked explicitly across both reviews and this reconciliation: no claim in this ADR
overstates what was actually measured; "angr failed" is never conflated with "the map architecture
failed"; the Z80-track real-title success, the M68K-track synthetic-only success, and the real-title
M68K negative result are all stated without minimizing any of them.

A fresh full gate was run against the exact final head (product branch `task/seg-041-t001`, commit
`c4a097e`): see this ADR's companion harness record (SEG-041-T007/T008) for the exact recorded pass
count. Includes the new `segarecomp_angr_m68k_facts_test` (gracefully SKIPPED under whatever Python
interpreter CMake resolves in an environment lacking working angr M68K p-code support, matching this
suite's existing optional-dependency convention — independently confirmed PASSING for real, including
the new RTE-exclusion regression, under the interpreter that does have it) and
`analysis_hybrid_mutation_test`/`m68k_legal_forms_test` (both previously broken by an intermediate
commit in this same task, both confirmed passing again at this final head).

## Consequences

- Product diff across the entire combined delivery: `docs/decisions/0089-...md` (this ADR); one small,
  reviewed, additive Z80-track consumer flag (`--z80-image`); and one reviewed, corrected, re-reviewed
  M68K-track consumer-plus-producer pair (`GenesisExternalM68kFacts`/`--external-m68k-facts`/
  `tools/segarecomp_angr_m68k_facts.py` and its own regression tests). Zero change to the emitter,
  runtime, or any generated program; zero change to SEG-031's admission *policy* or ADR 0080's adoption
  threshold (two new, optional, non-default inputs to the existing policy's existing mechanism, not a
  policy change).
- Broad AOT (ADR 0083) remains the unconditional, sole correctness and default production strategy for
  every title, real or synthetic. Both new consumer inputs require an operator-supplied artifact; neither
  is invoked automatically and neither has any effect unless explicitly used.
- angr, once actually available, is a real, demonstrated, bounded-but-genuine proof producer for Z80
  boot-image recovery via concrete execution (real title) and for exact-target/containment proof over
  ordinary (non-RTE) M68K control flow (synthetic only, now genuinely soundly, after a real defect was
  found and corrected). It remains concretely unusable for RTE/exception-frame semantics on any
  currently-available p-code variant. segarecomp's own CPU-legality authority remains mandatory and was
  shown necessary, not merely precautionary, by concrete counter-examples on both tracks.
- **Real commercial-title `complete_hybrid` has not yet been demonstrated; real-title generated-C/
  compile/runtime economics have not yet been measured; the production default remains unchanged broad
  AOT.** The synthetic `H/U = 0.000366` result demonstrates the mechanism is sound and effective when it
  has a fact to consume — it is not evidence of real-title benefit by itself.
- SEG-042 (a new milestone, not a continuation successor under SEG-041) is registered to carry the
  real-title measurement question forward once this milestone's product PR is confirmed merged; it must
  not begin on this PR.
- The SEG-038/040 finite-mutable-state and data-dependent-Z80-decompression findings remain durable;
  this milestone is the first to demonstrate a concrete, independently-certified resolution mechanism
  for the latter, via an external backend rather than an in-house hand-modeled recognizer, and the first
  to demonstrate (on a synthetic, soundly) that the SEG-031 planner can consume such a fact end to end.
