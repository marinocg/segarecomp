# ADR 0091: SEG-042 Real-Title External-Fact Harvest and Hybrid-AOT Evaluation

- Status: Accepted (decision: **REAL-TITLE CLOSURE STILL BROAD** -- real, structural, zero-angr
  `rts_computed` containment facts were produced automatically on every one of the three real titles
  tested (6/11 Sonic 1, 1/4 Sonic 2, 10/14 Cool Spot), but no title reaches `complete_hybrid`: on every
  title, at least one reachable dynamic-control site still has no *proven* container narrower than the
  whole executable universe `U`. Under the current static-AOT architecture, a `whole_image` container is
  not a conservative label that could simply be loosened -- it means no sound sub-`U` target/container
  bound has been established for that site, so admitting anything less than `U` for it would be unsound.
  The unmodified SEG-031 planner's `whole_image -> broad` behavior is therefore the *correct*, sound
  consequence of the currently-available proof, not a separately relaxable policy; closing the remaining
  gap requires a new sound sub-`U` proof (or a reachability-unreachable proof) for each such site, not a
  change to that rule. angr's own exact-target mechanism (this milestone's central new experiment)
  produced **zero** real-title facts across all three titles and every eligible family attempted -- a
  second, narrower, independently informative negative result, not the same finding as the containment
  result above.)
- Date: 2026-10-08 (single combined session, after SEG-041/PR #80's merge was confirmed).
- Task: SEG-042-T001..T007 (one combined, measurement-only delivery; T004's one allowed blocker-directed
  refinement ran against all three titles, not just Sonic 1).
- Related, unchanged: ADR 0089 (SEG-041, `ADOPT HYBRID RECOMP-MAP / ANGR EXPERIMENTAL`; the
  `GenesisExternalM68kFacts` consumer, `genesis_hybrid_container()`/`validate_genesis_hybrid_round()`,
  and the original `tools/segarecomp_angr_m68k_facts.py` exact-target producer are all reused unmodified);
  ADR 0090 (SEG-042-T001, the real-title proof-scope/qualification contract this milestone's harvester
  implements); ADR 0080 (SEG-031 planner; adoption thresholds unmoved and untested by this milestone,
  since no title reached `complete_hybrid`).

## Question

SEG-041 demonstrated the external-fact mechanism works *in principle* (a project-authored synthetic
closed from `H/U=1.000000` to `0.000366`) but had never been run automatically against a real title; its
own narrow attempt against Sonic 1's 11 non-RTE sites resolved none. SEG-042's question:

> Can external exact facts and/or bounded containment facts make a real title's closed hybrid admission
> set `H` materially smaller than broad universe `U`?

## Outcome

### SEG-042-T001 (done): real-title proof-scope and qualification contract -- CONTINUE

ADR 0090 defined the generic (title-independent) contract the automated harvester must honor: which
`GenesisAnalysisFamily` sites are eligible (`rte` structurally excluded before any attempt, never merely
poisoned after the fact); a deterministic, existing-facts-only starting-scope rule for exact-target proof
(a backward walk through the program's own discovered-instruction contiguity to the nearest proven
entry, validated by a no-external-entry check, never a caller-asserted premise); and a zero-angr
structural containment mechanism for `rts_computed` sites reusing the unmodified SEG-031
`points_to_region` container tier. Two small, additive report fields (`call_target_continuations`,
`static_successors`, plus a `discovered_lengths` projection) made the contract checkable from data the
analysis already computes, with no new analysis; a tiny, non-production native CLI
(`segarecomp-m68k-primary-word-classify`) exposed the existing base-MC68000 legality authority so the
Python harvester never has to duplicate decoder logic for its own general proof-path legality screen.

### SEG-042-T002 (done): automatic real-title external-fact harvester -- built and tested

`tools/segarecomp_recomp_map_harvest.py` discovers its own site list from segarecomp's existing report
output (zero hand-entered target list for any title) and, for every eligible site, attempts exact-target
proof then containment then honest `unresolved`, exactly as specified. A new, more general angr
producer function, `explore_exact_target_pc` (added alongside the existing, untouched
`explore_exact_target`), reads the resulting program counter one step after the dynamic-control
instruction itself rather than a caller-named register beforehand -- correct and equivalent to the old
mechanism on the simple register-indirect case, and additionally the *first* mechanism in this project
able to describe `(d8,An,Xn)`/`(d8,PC,Xn)` index forms, which a single named register cannot express at
all. Verified end to end (real angr, real native classifier binary, synthetic fixtures) before any
real-title run.

### SEG-042-T003 (done): Sonic 1 real-title measurement -- partial closure, `complete_hybrid` not reached

Baseline confirmed matching ADR 0089 exactly: `U=246293`, `D=1276`, 13 unresolved sites (2 `rte`, 4
`pc_index_explicit`, 7 `rts_computed`). The harvester resolved **6 of 11** eligible non-RTE sites --
**the first real-title external-fact success this architecture has ever produced** -- all six via the
zero-angr structural containment mechanism, zero via angr exact-target proof. Feeding the result through
the unchanged SEG-031 planner: `external_facts_applied=6`, `whole_image_fallback_count` drops `13 -> 7`,
but the overall outcome **remains `broad_whole_image`, `H/U=1.000000` unchanged**. This is the sound
consequence, not a policy artifact: the 7 still-unresolved sites each remain `whole_image` because no
sub-`U` container has been proven for them, and the unmodified SEG-031 planner correctly admits `U` for
any site it cannot soundly bound narrower -- real, majority partial closure (6 of 11 sites) buys *zero*
measured reduction in `H` because the admission is only as narrow as its *least*-proven remaining site,
not an average or a majority.

### SEG-042-T004 (done): one blocker-directed refinement -- diagnosed, did not close

Identified the single largest generic blocker across every title measured so far: the starting-scope
rule only recognized call-target entries and program roots as valid anchors, so a function/region
entered only by an ordinary branch had no candidate at all -- even though the point where the walk's own
contiguity runs out is, by construction of how discovery works, necessarily reached via some proven edge.
One bounded fix (crediting that gap-termination point as a final-resort candidate, confirmed against a
proven edge, still subject to the identical external-entry check) was implemented and measured against
all three titles. Net effect: zero new facts anywhere (every title's fact file is byte-identical
before/after); one site per Sonic 1/Sonic 2 reclassified from the generic `no_sound_starting_scope` to
the more specific, honest `no_known_callers`. Per this task's own non-goal, no second refinement was
attempted; Sonic 1's remaining blockers (2 `rte`, 1 `rts_computed` with no caller set, 2 `pc_index_explicit`
genuinely-unconstrained paths, 1 `pc_index_explicit` resource-exhausted, 1 same-function-branch false
rejection) are recorded honestly rather than bypassed.

### SEG-042-T005 (done): Sonic 2 and Cool Spot -- identical mechanism, same qualitative shape

Zero title-specific code. Sonic 2: 1/4 sites contained; the same sound consequence applies (one
remaining unproven site is enough to keep `H=U`). Cool Spot (a structurally different title: real
`jsr_an`/`jmp_an` sites, zero `pc_index_explicit`): **10/14** `rts_computed` sites contained -- the
largest single real-title containment result this milestone measured -- with the remaining 7
`jsr_an`/`jmp_an` sites all independently already classified `interrupt_resumption_unproven` by
segarecomp's own pre-existing sound analysis, consistent with being interrupt-handler-adjacent code no
mechanism in this milestone (or SEG-034..041) could bound. Every title tested shows the identical shape:
real containment succeeds broadly, angr exact-target proof succeeds nowhere, and at least one reachable
site per title still lacks any proven sub-`U` container, so the sound planner result is `H=U` on all
three regardless of how much partial progress was made elsewhere.

### SEG-042-T006 (done): economics -- not measured, by this task's own pre-registered non-goal

No title reached `complete_hybrid`, so no generated-C/compile/runtime/differential measurement was
performed (this task's explicit non-goal: "No economics measurement for a title that did not reach
`complete_hybrid`"). ADR 0080's material-benefit thresholds (`-30%` generated-C, `-25%` compile CPU,
`+15%` runtime ceiling, two complete-oracle titles) remain unmoved and untested by this milestone.

### SEG-042-T007 (this task): independent adversarial completion gate and decision

**The four required classification questions:**

1. **Does external semantic analysis produce useful real-title facts?** Partially, and the real answer
   is more specific than the question assumes: the *structural, zero-angr call-graph containment*
   mechanism this milestone built (reusing the existing `GenesisExternalM68kFacts` consumer as its
   delivery conduit) produced real, sound, useful facts on every title (6/11, 1/4, 10/14) -- but this
   mechanism needs **no external tool at all**; it is pure, already-available call-graph reasoning over
   segarecomp's own existing report data. The *external* backend this milestone centrally investigated
   (angr's exact-target proof, generalized via the new `explore_exact_target_pc`) produced **zero** real-
   title facts across all three titles and every eligible family attempted, despite finding a sound
   starting scope in most of the cases it reached and running to genuine completion (not merely timing
   out) in several of them.
2. **Does bounded containment stop local uncertainty from widening to U?** Yes, *locally and genuinely*:
   every credited containment fact is a small, bounded, sound island (2-10 entries, never the whole
   program), independently re-verified by the unmodified SEG-031 consumer. But *globally*, no -- not
   because local containment is somehow undone, but because the sites it has not yet reached remain
   genuinely unproven: the planner's measured `H` stays at `U` the instant even one site has no proven
   sub-`U` container, which is the sound admission for an unproven site, so bounded local containment on
   *some* sites does not, by itself, establish a narrower sound bound for the *sites it was never applied
   to*, on any title tested.
3. **Does hybrid admission materially reduce H?** No. `H/U=1.000000`, unchanged from broad, on every one
   of the three titles measured.
4. **Is the economic benefit enough to change production strategy?** Not evaluated (SEG-042-T006): no
   title reached the state (`complete_hybrid`) this question presupposes.

**Angr specifically, examined per the milestone's own interpretation rules:** the negative exact-target
result is mainly evidence against the *current hybrid use of angr specifically* -- not against the
recomp-map/containment architecture, and not a blanket indictment of angr -- because the pattern matches
the rule's own stated test: ordinary, qualified, non-RTE sites, with a sound starting scope established
independently of angr, under a reasonable (unchanged, un-raised) resource bound, still repeatedly
produced neither an exact fact nor a useful containment fact *from angr itself*, and the identical
pattern repeated across three structurally different real titles (two genuinely-unconstrained paths on
Sonic 1, one resource-exhaustion on Sonic 2, two genuinely-unconstrained paths on Cool Spot -- never a
tooling crash, never an environment gap). This does **not** erase angr's already-established real,
specific value (SEG-041-T003's Sonic 1 Z80 boot-image recovery, independently Musashi-confirmed
byte-exact): angr remains a real, demonstrated, narrowly-scoped generation-time proof/evaluation tool.
It is specifically the *M68K exact-target reachability* use case that this milestone's real-title
evidence now weighs against, not external analysis in general. Characterized precisely: angr as a
**targeted, bounded guest evaluator/proof helper** (concrete execution of a specific, already-isolated
routine, as in the Z80 decompressor) remains demonstrated and useful; angr as the **primary M68K
dynamic-control exact-target reachability producer** has a poor real-title yield so far (zero credited
facts across three titles). This milestone does not claim angr is useless -- it claims this one specific
use case underperformed, repeatedly, under fair conditions.

**Distinguishing "the architecture failed" from "angr failed" from "a residual site remains
unproven":** none of the three is the same claim, and this ADR states all three separately rather than
conflating them. The recomp-map/island architecture did not fail: it correctly, soundly consumed every
fact handed to it, on every title, exactly as designed, and its own closure/validation round-trip is the
reason a weak or wrong external guess could never have produced an unsound result even if one had been
supplied. Angr specifically produced zero real-title exact-target value in this milestone, a real,
narrow, now twice-independently-replicated (Sonic 1 and Sonic 2's resource-exhaustion; Sonic 1 and Cool
Spot's genuinely-unconstrained paths) negative finding. The reason no title closes is an entirely
separate, third fact, and it is not a policy this milestone chose not to change: **on every title, at
least one reachable dynamic-control site still has no sound container narrower than `U`**, and the
planner's unmodified `whole_image -> broad` behavior is the architecturally-required, correct response to
that fact under the current static-AOT contract (reachable site with no proven sub-`U` bound -> the only
sound target approximation is `U` -> static AOT must admit `U`). Closing that gap needs a new sound
proof per remaining site (or a proof the site is unreachable), never a loosening of the admission rule
itself -- loosening it without such a proof would be unsound, not merely conservative.

**Final classification: `REAL-TITLE CLOSURE STILL BROAD`** (one of this task's four pre-registered
labels) -- equivalently, outcome class **B, "ANALYSIS USEFUL, CLOSURE STILL BLOCKED"** in the milestone's
own broader framing: real, sound, automatically-produced facts exist on every title tested, but at least
one reachable site per title still lacks a proven sub-`U` container -- correctly forcing `H=U` under the
planner's existing, unmodified, sound rule, compounded by angr's own zero real-title exact-target yield
toward closing those specific remaining sites. Not `ADOPT HYBRID AOT AS PRODUCTION CANDIDATE` (nothing
closed); not `RETAIN HYBRID AS OPT-IN / BROAD DEFAULT` alone (too weak a label for what was actually
found -- real automated facts, not merely an inert opt-in mechanism); not `EXTERNAL FACTS USEFUL BUT
ECONOMICS INSUFFICIENT` (economics was never reached, not merely insufficient).

## Independent adversarial/completion gate

Two independent adversarial-validator subagent review sessions ran against this milestone's full diff
(`origin/main...HEAD` from SEG-041's merge commit `0c6130a`), covering every production-code change
(`report.hpp`/`.cpp`'s three new additive fields, the new native CLI, the new angr producer function
`explore_exact_target_pc` alongside the untouched original, and the new harvester tool and its tests),
independently re-derived the Sonic 1 real-title numbers from the authorized local ROM rather than merely
trusting this ADR's own claims, and ran the project's full gate.

**First session**: found two full-gate failures at commit `274ee7a`. (1) A **real regression**:
`analysis_core_boundary_test` -- `platforms/genesis/analysis_report/CMakeLists.txt` is reserved for
exactly the analysis-report driver's own three targets; adding the new
`segarecomp-m68k-primary-word-classify` CLI there broke that strict allowlist. (2) An apparent,
reproduced-twice failure in `sonic_startup_inventory_adapter_failure_test` (`probe_timeout`), in a file
and code path this milestone's diff never touches, under extreme host contention from this session's own
many long-running real-ROM analysis processes.

**Fix** (commit `326c0f7`): relocated the classifier CLI to a new standalone `apps/m68k-primary-word-
classify/` directory (mirroring the existing `apps/segarecomp` precedent; only links
`segarecomp::cpu_m68k`, no analysis-core dependency), updating the harvester's default path and ADR 0090
accordingly.

**Second session (focused follow-up)**: independently confirmed the fix -- `analysis_core_boundary_test`
passes at the new head; a whole-repository grep for the old path/filename found zero stale references;
the new location introduces no new boundary violation. Independently re-examined the flakiness claim
(read `tools/sonic_startup_inventory.py`'s real 1-second wall-clock probe bounds directly, confirmed via
`git diff --stat` that this milestone's diff never touches that file or anything in its dependency
chain, reran the test in isolation twice, both passed) and found **no residual doubt**. Ran the full gate
directly at the fixed head: **325/326 tests passed**; the sole failure,
`analysis_mutation_test` (a transient build-race in its own temp-directory rebuild step, in
`libs/cpu/z80/analysis/src/adapter.cpp`, untouched by this diff), was independently reconfirmed passing
on an isolated rerun. Combined with the isolated confirmations above, the effective result across the
two sessions is a clean gate with zero reproducible failure attributable to this milestone's diff.

**Final verdict: PASS-WITH-MINOR.** Both minor findings (the CMake boundary violation; the uncommitted
milestone-closing ADR you are now reading, flagged by the second session before this commit) are
addressed in the commit that follows this one. The reviewer additionally flagged, as an explicitly
non-blocking structural/process observation out of this task's scope, that the combined-PR delivery
pattern (one product PR covering SEG-042-T001..T007, each child recording its own `branch` label while
sharing one real PR) is a systemic divergence from the literal text of
`docs/development/pull-requests.md`'s per-task-PR wording, repeated across roughly nine prior milestones
(SEG-034..041) under the same documented precedent; this ADR does not change that policy, consistent with
every prior milestone that used it.

Exact reviewed head (before this commit): `326c0f7`. Full gate log:
`.cache/agent-runs/20261008T154321+0200-seg042-full-gate-final-notask.log` (harness-local, not
committed).

## Consequences

- Product diff across the entire combined delivery: three small, additive `GenesisAnalysisReport`
  fields and their private-JSON exposure; one new, tiny, non-production native CLI; one new angr producer
  function alongside the untouched SEG-041-T008 original; one new automated harvester tool and its tests.
  Zero change to the emitter, runtime, or any generated program; zero change to SEG-031's admission
  *policy*, ADR 0080's adoption threshold, or the existing `GenesisExternalM68kFacts`
  consumer/validator/parser (all reused byte-for-byte unchanged).
- Broad AOT (ADR 0083) remains the unconditional, sole correctness and default production strategy for
  every title, real or synthetic, unchanged by this milestone.
- The automated real-title harvester is a real, demonstrated, deterministic measurement instrument, not
  a production default: it costs nothing when unused and never claims a fact it cannot independently
  re-verify as sound-within-its-own-declared-scope.
- **Real commercial-title `complete_hybrid` has not been demonstrated on any of the three titles tested.**
  Real, structural containment has been demonstrated, repeatedly, automatically, with zero title-specific
  code -- the central positive, reusable finding of this milestone. Angr's own exact-target reachability
  mechanism has been shown, on real-title evidence across three titles, not to add value for the dynamic-
  control-site closure question this milestone centrally investigated; its narrower, already-established
  value (generation-time concrete-execution evaluation, as in the Z80 boot-image case) is unaffected and
  undiminished by this finding.
- A future milestone revisiting this question should **not** re-attempt angr exact-target proof on these
  same sites and call a repeated failure new evidence, and should **not** target the admission rule
  itself: `whole_image -> broad` is the correct, sound consequence of an unproven site under the current
  static-AOT contract, not a relaxable policy -- loosening it without a new proof would trade soundness
  for a smaller `H`, which this project does not do. The measured, title-independent blocker is instead
  that at least one reachable dynamic-control site per title still lacks *any* sound sub-`U`
  target/container proof. SEG-043 ("Bound residual whole-image M68K control sites") is registered on
  harness `main` (not started on this PR) to pursue exactly that: whether the remaining real-title
  `whole_image` sites can be converted, generically, into sound sub-`U` containers, via (in priority
  order) a CFG-shaped proof-scope test that distinguishes a same-function backedge from a genuine
  external entry (the exact gap SEG-042-T004 found and left unresolved), a generalized branch-entered/
  shared-epilogue `rts_computed` containment region reusing existing SEG-031/report/return-slot facts
  (not a second whole-program stack analyzer), bounded selector-domain recovery for `pc_index_explicit`
  sites from existing finite-value evidence before resorting to fully-symbolic angr execution, and only
  then -- if RTE/interrupt-frame semantics remain the dominant blocker once every other site is sound --
  a narrow, segarecomp-owned RTE semantic summary (never a second emulator). Economics (SEG-042-T006's
  question) is deferred again until a title actually reaches `H < U` under the unchanged SEG-031 planner.
