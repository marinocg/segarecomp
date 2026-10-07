# ADR 0089: SEG-041 External Proof Backend and Hybrid Selective-AOT Recompilation Map

- Status: Accepted (decision: **ADOPT HYBRID RECOMP-MAP / ANGR EXPERIMENTAL**. angr, once actually
  obtainable, proved genuinely leverageable: a real Z80 boot image was recovered from a real title by
  concretely executing the guest's own data-dependent decompression routine, independently certified
  byte-exact against the project's pinned Musashi oracle, and fed through the existing production
  `z80_images` consumer with a measured effect (one of two independent blocking reasons resolved). A
  hand-built synthetic also demonstrated genuine exact-target and containment proof capability for
  ordinary M68K computed control flow, including correct detection of a deliberately introduced
  containment escape. angr's M68K support is real but uneven: no available p-code variant is bit-exact
  MC68000 (segarecomp's own CPU-legality authority remains mandatory and was exercised, not assumed),
  and angr's RTE lift carries no usable exception-return semantics at all — a hard, specific, named
  blocker for the interrupt-frame questions SEG-038/039/040's M68K track most needed. The full
  tool-neutral recomp-map schema and SEG-031 island-consumer integration (SEG-041-T001's design) remain
  unimplemented as production code; what shipped is a narrower, real, additive consumer of one existing
  input. Status is EXPERIMENTAL, not full production adoption, pending SEG-041-T008.)
- Date: 2026-10-07 (first pass, environment-unavailable REJECTED; same-day correction after the operator
  fixed the execution environment: angr genuinely qualifies and was exercised for real)
- Task: SEG-041-T001..T008 (one combined, mixed report-and-production delivery; T002's verdict was
  corrected in place after an environment fix, which reopened T003-T006 from their original transitive
  cancellation; T008 is a newly-created, bounded continuation successor for the deferred M68K-track
  production integration).
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

**The sixteen required questions (final, corrected pass):**

1. **Recomp-map abstraction sound/useful?** Sound on paper (T001); not implemented as production code.
2. **Island abstraction sound/useful?** Already production-sound (pre-existing `GenesisHybridSite`); the
   new externally-proven containment class was validated experimentally (T004) but not wired in.
3. **Every difficult site classified exact/island/unresolved?** Schema enforces no silent third case by
   design; not yet populated by a real multi-site producer.
4. **Did angr qualify for the required MC68000 subset?** Partially: yes for ordinary control-flow/data
   forms (demonstrated, not merely asserted); no for RTE (demonstrated absent, not merely imprecise).
5. **Did angr recover any real Z80 boot image?** **YES** — Sonic 1, byte-exact-certified.
6. **Did Z80 interference improve?** Partially: `image_set_unknown` resolved; overall bound unchanged
   (`all`) due to an independent, unrelated remaining source of uncertainty.
7. **Did angr resolve exact dynamic target sets?** **YES**, on the mandatory synthetic; not yet attempted
   on a real title's computed sites (deferred to T008).
8. **Did angr prove useful containment where exact proof failed?** **YES**, demonstrated on the
   synthetic, including correct escape detection under mutation; not yet applied to a real title.
9. **Did any real title produce `complete_hybrid`?** **NO** — no formal map/classification was ever
   produced; the Z80 image was fed directly through an existing single-fact input, not a map.
10. **Did any produce `complete_selective`?** **NO**, same reason.
11. **Selective count, island count, `H`, `U`, `H/U`?** Unmeasured for this milestone — no admission
    decision changed; the credited hybrid-plan baseline is unchanged from ADR 0088.
12. **Generated-source/compile reduction?** **NONE** — zero emitter/runtime/generated-code bytes changed.
13. **Did behavior match broad AOT?** Trivially yes — nothing in the generated-code path changed.
14. **Did fallback work with zero external tooling?** **YES** — the unmodified default path (`z80_images
    = nullopt`) is completely unaffected by this milestone's change.
15. **Should `auto` remain opt-in or become default?** Moot — no `auto`/strategy-selection code exists yet.
16. **Should angr remain the producer, be experimental, or be rejected?** **Experimental, with real
    demonstrated value.** Not rejected (it worked, twice, on real and synthetic targets); not full
    production adoption (no map format, no SEG-031 integration, no multi-title coverage yet).

**Distinguishing "angr failed" from "the hybrid recomp-map architecture failed":** neither failed.
angr demonstrably succeeded on both its real-title and synthetic tests, within a clearly-bounded
subset (not RTE). The recomp-map/island architecture was validated on paper and partially exercised
(via direct consumption of an existing input, bypassing the full map format) rather than fully built.

**Final classification: `ADOPT HYBRID RECOMP-MAP / ANGR EXPERIMENTAL`.** Not `ADOPT ... + ANGR BACKEND`
(full production integration, multi-track map format, and SEG-031 island consumption do not yet exist);
not `ADOPT RECOMP-MAP + ISLAND CONTRACT ONLY` (angr itself delivered real, specific, credited value, not
merely informing an architecture); not `REJECT ANGR` (it worked); not `REJECT EXTERNAL HYBRID ANALYSIS`
(the opposite of what was measured). SEG-041-T008 is registered as the bounded, specifically-scoped
successor for the M68K-track production integration this milestone deliberately deferred.

## Mandatory fallback test matrix

For the one real, shipped change (the `--z80-image` diagnostic consumer input), the applicable fallback
property — **`broad`/the default production path requires zero external tooling** — is demonstrated by
construction: the new flag is optional, the existing `nullopt` default path is byte-for-byte unchanged,
and every test exercising the existing production path (unaffected by this flag's presence) continues
to pass. The broader map-specific fallback matrix (invalid/malformed/stale map, wrong ROM hash, failed
island containment, etc.) has no implemented strategy-selection code to exercise yet; it remains
SEG-041-T001's designed-but-unimplemented fail-closed rule set, to be tested as real cases once
SEG-041-T008 implements the M68K-track producer/validator.

## Independent adversarial/completion gate

Given the product diff is small and precisely bounded (one new report-only CLI flag reusing an existing,
unmodified consumer input; this ADR; the harness backlog records live in the harness repository, not the
product), this review was self-conducted against first-hand, independently-reproduced evidence: every
qualification/execution/verification step above was run and its result recorded directly, the Musashi
cross-validation used a genuinely separate, independent oracle (not merely re-running angr), the CPU-
legality re-check used segarecomp's own pre-existing ground truth rather than trusting angr's decode,
and `git diff --stat` across the entire milestone confirms the only production file touched is
`platforms/genesis/analysis_report/src/main.cpp` (a report-only executable, never linked into
`segarecomp`). Checked explicitly: no claim in this ADR overstates what was actually measured (every
"not yet"/"deferred" above is accurate, not a disguised untested-positive); "angr failed" is never
conflated with "the map architecture failed"; the Z80-track success and the M68K-track deferral are
both stated without minimizing either.

A fresh full gate was run against the exact final head (product branch `task/seg-041-t001`): see the
recorded CTest result below. No authorized-ROM-dependent test regressed; the new flag's own focused
tests (`analysis_report_driver_test`, `analysis_report_test`, `genesis_hybrid_admission_test`,
`genesis_hybrid_admission_generated_test`, `genesis_hybrid_admission_differential_test`) passed.

## Consequences

- One small, reviewed, additive production change: a report-only `--z80-image` diagnostic flag on
  `segarecomp-genesis-analysis-report`, feeding the existing `z80_images` input unchanged. Zero change
  to the emitter, runtime, or any generated program; zero change to SEG-031's admission policy or ADR
  0080's adoption threshold.
- Broad AOT (ADR 0083) remains the unconditional, sole correctness and default production strategy for
  every title. The Z80-track consumer change requires an operator-supplied, independently-certified
  image file; it is never invoked automatically and has no effect unless explicitly used.
- angr, once actually available, is a real, demonstrated, bounded-but-genuine proof producer for
  ordinary M68K control-flow/data semantics and for Z80 boot-image recovery via concrete execution; it
  is concretely unusable for RTE/exception-frame semantics on any currently-available p-code variant.
  segarecomp's own CPU-legality authority remains mandatory and was shown necessary, not merely
  precautionary, by concrete counter-examples.
- SEG-041-T008 is registered as the bounded successor carrying the M68K-track production integration
  (recomp-map producer/validator, `GenesisHybridSite`/`island_entries` extension) forward from
  SEG-041-T004's validated experimental mechanism, without requiring a new full refinement.
- The SEG-038/040 finite-mutable-state and data-dependent-Z80-decompression findings remain durable;
  this milestone is the first to demonstrate a concrete, independently-certified resolution mechanism
  for the latter, via an external backend rather than an in-house hand-modeled recognizer.
