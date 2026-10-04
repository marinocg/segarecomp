# ADR 0080: Hybrid Selective Admission and the Per-CPU Production Strategy

- Status: Accepted (SEG-031-T001; the production strategy, decision 12, is recorded by SEG-031-T007).
- Date: 2026-10-04
- Task: SEG-031-T001..T007 (T001 accepts the contract; later children append their records below).
- Related: ADR 0079 (M68K analysis instantiation; decision 1 report-only placement, decision 8 closure premise), ADR 0078 (generic core,
  no production linkage), ADR 0077 (executable images), ADR 0076 (Gen-3 activation and per-CPU strategy), ADR 0051 (the SEG-024
  operand-width region rule and its snowball), ADR 0049 (immutable-copy aliases), ADR 0052 / ADR 0071 (broad-AOT build economics),
  ADR 0053 (execution-PC coverage).

## Context

SEG-030 delivered a sound, report-only M68K instantiation (`precise target set OR Unknown`). Its soundness infrastructure is proven, but
sound precision on Sonic 1 is poor: `U` 246,293 broad identities, `D` 1,276 (D/U 0.52%), observed recall 9.73%, zero escapes. The
earlier ~43% recall relied on an unsound interrupt-register assumption and is historical.

Low recall alone does not decide production value. The production question is:

> Given sound precise discovery `D`, how much of broad AOT `U` must be retained as conservative fallback to produce a complete static
> generated-native program?

## Decision

1. **Production metric.** `hybrid_admission = D ∪ fallback islands ∪ transitive static closure`; the primary metric is
   `hybrid_ratio = |hybrid_admission ∩ U| / |U|`, followed by measured generated-C, build, binary and runtime effects. Observed recall is
   a precision diagnostic and falsifier only.
2. **Safety invariant.** For every uncovered dynamic control site of the hybrid set (an unresolved computed site, or a return the
   SEG-030 continuation model does not cover): `PossibleTargets(site) ⊆ island entries(site) ⊆ H`. The exact target need not be known;
   the container of the uncertainty must be proven.
3. **Authorities and the widening ladder.** Uncertainty only enlarges the admitted set:
   1. exact SEG-030 target sets (followed by the analysis; no island);
   2. a `JMP/JSR (An)` or `d16(An)` site whose address register holds a proven SEG-030 points-to set (joined over every context and
      partition of the site, not width-derived): the island entries are exactly the set's members plus the displacement (strided
      members enumerated) that are mapped even PCs of the executable-image view (`points_to_region`, `executable_image` when the set
      strides over a whole image, `materialized_image` when it names `static_proof` alias PCs);
   3. anything else has no bound narrower than the whole program: the result is whole broad AOT (`whole_image`).

   The executable-image view is the SEG-028 artifact (ADR 0077) through ADR 0079 decision 3. **Forbidden** as authorities: byte
   appearance, Ghidra or any external disassembler, runtime coverage (it can neither create nor shrink an island), operand width (the
   SEG-024 rule, ADR 0051), nearest functions, and title identity.
4. **Real fixed point.** Island members enter the same SEG-030 instantiation as computed edges of their site that carry the site's own
   state, exactly as a resolved site's targets (a call enters its callees in the call-site context and they become callees of its
   continuation). Island code is therefore analysed (its returns, stores, further transfers and interrupt boundaries), and the ADR 0079
   closure premise ("every fact is relative to the discovered set") holds over the hybrid set `H` instead of over `D`. Each round's
   uncovered sites widen the islands (a union per site); the planner stops at the first round whose islands do not grow. The CPU-owned
   adapter change is opt-in (`M68kAnalysisConfig::island_entries` / `opaque_entries`, empty for every SEG-030 report, whose output is
   byte-identical).
5. **Fail-closed outcomes.** Each of these yields whole broad AOT, never a partial hybrid program, and a resource ceiling is never
   evidence that omitted code is unreachable: an image set that does not validate; a broad analysis that rejects the program; an
   incomplete solve; a frames round that does not validate (`historical_assumption` is never credited); an unbounded container; more
   than 65,536 island entries; no fixed point within 8 rounds; a failed independent validation.
6. **Independent validator.** At the fixed point the planner re-checks, outside the solver's bookkeeping: every machine root, every
   fixed successor and stacked call continuation (`m68k_control_successors`) of every admitted instruction, every resolved target and
   every island entry that decodes is admitted; every uncovered site is configured with its whole recomputed container; no reached
   PC that the analysis cannot decode is a broad identity (otherwise the hybrid could stop where broad runs).
7. **Machine roots.** The roots are the machine-delivered vector roots (`genesis_reachability_roots`, shared with SEG-030), always
   analysed. Every `static_proof` (ADR 0049) executable image is admitted whole as a mandatory root set, entered as opaque all-Unknown
   entries from the startup entry (no proven source transfer gives their entry state). A delivered handler that SEG-030 cannot
   analyse precisely is covered conservatively (entered with an Unknown state), never removed. Installed interrupt vectors the
   generated runtime does not deliver (the Genesis level-2/level-4 autovectors and the spurious vector under the ADR 0079 machine
   premise) are analysed only as writer-only instances and are not admitted: see decision 8.
8. **Inherited premises.** The hybrid inherits exactly two SEG-030 premises and adds none of its own: (a) the named return-slot
   integrity premise for an ordinary RTS (ADR 0079 decision 8; the planner reports the premise sites); (b) the machine-delivery premise
   (ADR 0079 decision 7, `genesis_reachability_roots`): only the vectors the generated runtime can raise (IRQ6 and the synchronous
   vectors) are roots, so the code of installed but undelivered interrupt vectors is not admitted even though broad `U` contains it.
   (b) is exact for the current runtime; if interrupt delivery is ever extended (for example H-interrupts), those vectors become
   delivered roots of the same owner and every plan must be recomputed (the universe fingerprint of decision 10 does not detect a
   runtime change, so such a change must invalidate existing plans explicitly).
9. **Placement.** The planner consumes the analysis, so it lives in the report-only `platforms/genesis/analysis_report` target (ADR 0079
   decision 1); `segarecomp-genesis-analysis-report --hybrid-plan <path>` writes the plan. Production never links the analysis
   (`analysis_build_graph_test` unchanged). The M68K interpretation of SEG-030 facts is M68K/Genesis-owned; no generic CPU-neutral
   abstraction is added because no second CPU exercises it.
10. **Production seam (explicit candidate).** The plan is a strict, bounded text artifact (`segarecomp.m68k_hybrid_admission_plan.v1`:
    ROM digest, broad-universe fingerprint, alias set, strategy, admitted half-open ranges over the broad identities). The production emitter consumes it only
    through `emit-general-startup-bridge-c --immutable-rom-aot-admission <plan>` (and `segarecomp build --admission-plan <plan>`). The
    machine owner (`platforms/genesis/machine` `hybrid_admission.hpp`) validates it fail-closed (digest, universe fingerprint, alias set, well-formed ranges,
    non-empty, structural closure over the broad identities: machine roots, `static_proof` identities, fixed successors and stacked call
    continuations) and only then filters `FrontendAnalysis::immutable_rom_aot_entries`. Dynamic containment is the planner's proof; the
    production owner cannot re-derive it without the analysis. The filtered program is an ordinary generated-native program: no
    identity knows whether it came from `D`, an island or broad admission, and the runtime is unchanged. Without the option the emission
    is byte-identical to pre-SEG-031 `main`.
11. **Production-default rule (pre-registered before any candidate measurement; milestone record).** Mandatory per title: zero observed
    escapes on its complete oracle, identical guest behaviour to broad on the oracle workload, deterministic output, no title-specific
    logic. Material benefit per title: generated C bytes -30% or better **and** generated-code compile CPU -25% or better. No runtime
    regression beyond +15% (the documented host noise band). A class switches its default only with at least two complete-oracle titles
    meeting the benefit and no title failing the mandatory checks.
12. **Production strategy (SEG-031-T007, from the measurements recorded below, under decision 11 without moved thresholds).**

    | CPU / image class | production | why |
    | --- | --- | --- |
    | M68K immutable cartridge (Genesis) | **broad AOT** (unchanged default) | the hybrid plan degenerates to broad on every authorized title (ratio 1.000000): generated C, executables and behaviour identical, so decision 11's benefit test fails on every title (0% C, 0% compile CPU beyond noise) |
    | M68K `static_proof` RAM images (ADR 0049 aliases) | **broad AOT** | admitted whole as mandatory roots by the hybrid anyway; no title produces a narrower result |
    | Master System Z80 | **broad AOT** | no Z80 analysis or admission claim; the new observer confirms zero escapes and a deterministic footprint on the authorized titles |
    | Genesis Z80 materialized images | **broad AOT** | same; the observer runs inside the build-time materialization pass |

    The hybrid machinery stays as an explicit, tested measurement candidate (report-only planner, opt-in plan seam), never a default:
    it is the instrument that re-measures the question once precision improves, and it costs nothing when unused (broad emission is
    byte-identical to pre-SEG-031 `main`). It is not retired because removing it would discard the containment proof machinery the
    candidates below need to be evaluated.

    **Generic precision blockers (later candidates; not implemented by SEG-031).** Every title degenerates in round 1 because at least
    one uncovered site has no bound narrower than the whole program, and one such site costs 100% of `U`; the trigger counts are
    small (5-22 sites per title) but each is unbounded. In order of measured frequency:
    1. **interrupt-resumption register effects** (`interrupt_resumption_unproven` on PC-indexed and `JSR/JMP (An)` sites, RTE
       `interrupt_resumption`): no handler instance is analysed (`entry_unknown`), so every resumption makes D0-D7/A0-A6 Unknown;
       per-handler effect precision would bound these;
    2. **unbalanced and computed RTS** (`stack_unbalanced`, `return_slot_rewritten`): symbolic relative-A7 save/restore;
    3. **pointer provenance** (`base_unknown`, `context_bound`): mutable-object/field-sensitive pointers and context depth;
    4. **resource completion**: SEG-030 `all` does not complete on three authorized titles (`iteration_bound`), so no hybrid can be
       credited there.

    Every complete title has triggers in at least two of classes 1-3 (Sonic 1 and Cool Spot: 1, 2 and 3; Sonic 2: 1 and 2) and the
    three other titles are blocked by class 4, so a candidate that fixes one class alone cannot change any measured title's admission.
    Removing a title's current triggers only reaches the next closure round: island code may expose further unbounded sites, which
    the planner measures before any production claim.

## Consequences

- SEG-031 can answer the milestone question per title with a sound plan and sanitized attribution, without a second decoder, discovery
  or recompiler: islands select from the existing broad identities.
- If every title degenerates to broad, broad AOT stays production and the attribution names the generic precision blockers.
- The plan artifact holds exact addresses: for a commercial input it is private and ignored, like the SEG-030 private outputs.

## Records

### SEG-031-T001 (contract and report-only feasibility)

Report-only planner on the authorized Genesis corpus (sanitized aggregates; Release driver):

| title | `U` | sound `D` | outcome | uncovered sites with no bound (round 1) | planner cost |
| --- | --- | --- | --- | --- | --- |
| Sonic 1 | 246,293 | 1,276 | `broad_whole_image` | 14 | 146-150 s / 274-287 MiB |
| Sonic 2 | 496,387 | 335 | `broad_whole_image` | 5 | 110 s / 530-567 MiB |
| Cool Spot | 498,276 | 6,907 | `broad_whole_image` | 22 | 23 s / 568 MiB |
| OutRun | 496,950 | - | `broad_analysis_incomplete` | - | 400 s |
| Streets of Rage | 247,761 | - | `broad_analysis_incomplete` | - | 6 s |
| Golden Axe | 249,843 | - | `broad_analysis_incomplete` | - | 26 s |

Hybrid total equals `U` on every title (ratio 1.000000, reduction 0). Whole-image causes by family / reason / CPU sub-reason: Sonic 1:
computed RTS 8 (`stack_unbalanced` 5, `base_unknown` 2, `return_slot_rewritten` 1), PC-indexed 4 (`interrupt_resumption_unproven`),
RTE 2 (`interrupt_resumption`); 0 exactly resolved computed sites; 41 ordinary RTS under the inherited return-slot premise. Sonic 2:
PC-indexed 2 (`interrupt_resumption_unproven`), RTE 1, computed RTS 2 (`stack_unbalanced`). Cool Spot: `JSR/JMP (An)` 7
(`interrupt_resumption_unproven`), RTE 1, computed RTS 14 (`stack_unbalanced` 10, `context_bound` 3, `base_unknown` 1). OutRun,
Streets of Rage and Golden Axe: the current-main SEG-030 `--domains all` solve itself is incomplete (`iteration_bound`, reproduced
with the plain SEG-030 driver), so nothing is credited.

### SEG-031-T002 (closure and validator)

Implementation correction: entering island members with the opaque continuation state was needlessly conservative (an island entered
by a call returned through an RTS with an Unknown stack delta and widened at once). A member of a proven finite set is a target of its
site exactly as a resolved target is, so it carries the site's state (`island_entries`); only the source-less materialized roots are
opaque (`opaque_entries`). Synthetic shapes (`analysis_hybrid_plan_test`) cover every SEG-031 adversarial case: exact target, bounded
region, called island, two images, cross-island direct edge, second island, cyclic growth, overlap canonicalization, RAM mirror,
materialized image, Unknown provenance, operand-width index (PC-indexed and pointer-table forms), vector root, round and island
bounds, unverified provenance, incomplete solve, and validator rejection of an omitted member or an unconfigured site.

### SEG-031-T003 (explicit candidate emission)

A synthetic hybrid admits 262 of 8,187 broad identities and emits 337,496 instead of 2,091,073 bytes of C, with an identical result
and final runtime state under an explicit instruction budget (`genesis_hybrid_admission_generated_test`). A broad plan is
byte-identical to no plan; tampered plans are rejected, and a plan omitting a dynamic island member (invisible to the structural
check) stops fail-closed at that member.

### SEG-031-T004 (adversarial gate)

`analysis_hybrid_mutation_test`: 17 mutants of the planner, island edges and production seam, 17 killed, 0 equivalent.
`analysis_hybrid_differential_test` (SEG-030-T008 generator and interpreter): 304 images, 22 hybrid, 546 concrete steps inside `H`,
0 escapes. `genesis_hybrid_admission_differential_test`: 10 seeded random island images, generated hybrid against generated broad,
482 islands, identical results and final states.

### SEG-031-T005 (multi-title broad versus hybrid)

`tools/genesis_hybrid_admission_compare.py` per complete-oracle title (Release CLI and driver; `segarecomp build` default -O2; the
reference CLI is pre-SEG-031 `main` `ec8b57c`; runtime is the headless execution-coverage route, 3,000 frames, no input; oracles are
the SEG-030 complete 23,200-frame execution-PC bitmaps, private):

| title | `U` | sound `D` | plan | hybrid total | generated C (planned vs broad) | build wall / CPU, broad -> planned | executable | 3,000-frame run | escapes / observed PCs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Sonic 1 | 246,293 | 1,276 | broad_whole_image | 246,293 | 220.2 MB, identical (0.0%) | 40.2 / 193.3 s -> 34.8 / 180.8 s | 35.0 MiB, identical | 43.1 -> 42.4 s, identical state | 0 / 10,512 |
| Sonic 2 | 496,387 | 335 | broad_whole_image | 496,387 | 393.0 MB, identical | 57.1 / 281.4 s -> 56.7 / 287.0 s | 60.9 MiB, identical | 70.7 -> 71.6 s, identical | 0 / 2,478 |
| Cool Spot | 498,276 | 6,907 | broad_whole_image | 498,276 | 361.9 MB, identical | 49.1 / 252.5 s -> 50.0 / 255.1 s | 54.7 MiB, identical | 64.4 -> 62.2 s, identical | 0 / 4,928 |
| OutRun | 496,950 | - | broad_analysis_incomplete | 496,950 | 312.2 MB, identical | 43.5 / 212.3 s -> 43.2 / 217.0 s | 48.2 MiB, identical | 57.2 -> 58.4 s, identical | 0 / 5,286 |
| Streets of Rage | 247,761 | - | broad_analysis_incomplete | 247,761 | 206.4 MB, identical | 36.8 / 175.8 s -> 34.7 / 166.9 s | 32.9 MiB, identical | 43.3 -> 41.6 s, identical | 0 / 8,758 |

On every title the broad tree generated by the branch CLI is byte-identical to the pre-SEG-031 reference, and the planned tree and
executable are byte-identical to broad; the build and runtime differences are host noise (within the documented ±15%, identical
binaries). Every observed PC is a broad identity, and none lies outside the admission.

**Sonic 1 answer.** Broad `U` 246,293; sound `D` 1,276; fallback admitted 245,017; hybrid total 246,293; hybrid/broad 1.000000;
generated-C reduction 0%; compile-time change none beyond noise (identical units); binary-size change 0 (identical executable);
runtime change none (identical program and state); zero escapes on the complete oracle. Decision 11's benefit test fails.

### SEG-031-T006 (Z80 execution-PC observer and broad validation)

`tools/z80_execution_coverage.py` over the authorized Z80 corpus (measurement builds with `SEGARECOMP_Z80_EXECUTION_COVERAGE`; SMS:
3,600 frames, two observed runs and one unobserved run; Genesis: the materialization pass's own 600-frame window, two runs):

| title | code images observed | distinct PCs | retirements | outcome | deterministic | zero semantic effect | escapes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Alex Kidd in Miracle World (SMS) | 4 | 3,897 | 24,198,648 | frame target | yes | yes | 0 |
| Sonic The Hedgehog (SMS) | 5 | 5,344 | 17,517,510 | frame target | yes | yes | 0 |
| Sonic The Hedgehog 2 (SMS) | 8 | 6,354 | 21,865,631 | frame target | yes | yes | 0 |
| Sonic 1 (Genesis Z80 materialized) | 2 | 111 | 4,135,317 | window complete | yes | - | 0 |
| Cool Spot (Genesis Z80 materialized) | 2 | 48 | 8,148 | window complete | yes | - | 0 |

No unknown identity and no Z80 error occurred: every observed (code-image identity, PC) executed through an exact broad entry. Build
times of the measurement builds (8-56 s) stay within the existing Z80 economics (SEG-033, ADR 0071), which give no reason to leave broad
AOT; no Z80 analysis or selective admission was attempted.

### SEG-031-T007 (independent completion gate)

Independent adversarial gate at product `dd5c701`: PASS-with-minor. Full tier 318/319; the only failure, `restricted_files_test`,
is environmental (Git refuses `check-ignore` beyond the linked worktree's `games` symlink) and passes against the product root.
Adversarial fixtures (pin-and-restart around island-fed targets; `JMP d16(An)` with negative displacement, upper register byte and
32-bit wrap) produced no hybrid plan whose execution leaves `H`. Minor findings, corrected in the same PR: the second inherited premise
(decision 8 b) is now named; Sonic 1's blocker classes are corrected; the plan now carries a broad-universe fingerprint checked by the
production owner (`universe_mismatch`); the validator rejects a reached dynamic site that is neither uncovered nor resolved
(`unclassified_dynamic_site`) instead of passing it. Two mutants cover the new checks (19 hybrid mutants in total).
