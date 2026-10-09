# ADR 0094: SEG-045 coarse executable-region AOT portability experiment

- Status: In progress (section 1 frozen by T001; later sections appended by T003..T006).
- Predecessor: ADR 0093 (SEG-044), ADR 0080 (hybrid admission seam).

## 1. Contract (frozen before any region measurement)

Question: can a conservative, approximately detected executable ROM region `R` be broad-decoded locally, structurally pruned,
and compiled by the unchanged AOT machinery so real titles build and survive their existing execution oracles - with no exact
instruction-start completeness and no SEG-030 whole-program analysis?

Definitions. `U` = the broad immutable-ROM AOT identity set of the emission (`FrontendAnalysis::immutable_rom_aot_entries`).
`K0 = { u in U | address(u) in R }`. `K` = the greatest subset of `K0` closed under the production obligations of
`apply_genesis_hybrid_admission()`: for every `x` in `K`, every fixed control successor and call continuation of `x`
(`m68k_control_successors`) that is itself in `U` is in `K`. Pruning removes `x` while an obligation leaves `K`; it only
removes (`K ⊆ K0 ⊆ U`), never adds. The kernel is C++ and lives in the hybrid-admission owner; the result is an ordinary
`segarecomp.m68k_hybrid_admission_plan.v1` accepted by the **unchanged** validator.

Fail-closed rules. After convergence: a machine root in `U` absent from `K`, a materialized/alias identity in `U` absent from
`K`, an empty `K`, or an exhausted resource cap => proposal REJECTED (never broadened; broad AOT remains the fallback). `R` is
an *assumption* about where executable code lives; the kernel does not prove it. Escapes are caught only by the existing
fail-closed dispatcher and the complete execution-PC oracle (falsifier, never an input to `R` or `K`).

Anti-overfit. Exactly one generic region policy is frozen on Sonic 1 (the only calibration title) before Sonic 2, Cool Spot or
Streets of Rage are processed: no title names, labels, hand-entered regions, per-title thresholds, or escape-driven page additions.

Pre-registered cross-title gate (production successor justified only if all hold): >= 2 of 3 titles pass completely; the passing
set includes Cool Spot or Streets of Rage; each passing title has `K/U <= 0.50`, 0 execution-PC escapes on the long oracle,
broad/selective guest-visible state evidence equal, unchanged validator acceptance, and no title-specific policy. Otherwise no
implementation successor; the failure is classified (region recall / pruning collapse / validator incompatibility / runtime
escape / executable-image behavior / economics).

Hierarchy under test: exact source map > region + structural prune > broad AOT (unconditional fallback).

Baselines. Product main 8abcd947552907d0bc903164caccbf0552521667; harness main 2f12800c1867631834a7f9ddcf62070507f9ec4e.
Current-main `U` (aligned starts / accepted identities): Sonic 1 262,144 / 246,293; Sonic 2 524,288 / 496,387;
Cool Spot 524,288 / 498,276; Streets of Rage 262,144 / 247,761 (broad enumeration 4.0 / 9.3 / 7.7 / 4.4 s, peak RSS
500 / 949 / 917 / 485 MiB; Release, 4 vCPU). These equal the historical figures.

## 2. Structural-pruning kernel (T002, report-only)

`prune_genesis_region_admission` / `plan_genesis_region_admission` (C++, `hybrid_admission`) compute the greatest fixed point by a
reverse-edge worklist in removal waves (`rounds`), reusing `m68k_control_successors`, `genesis_reachability_roots` and the
hybrid range builder; a Python re-implementation of M68K classification does not exist. Region proposals travel as
`segarecomp.m68k_executable_regions.v1`; the output is an ordinary `segarecomp.m68k_hybrid_admission_plan.v1`. CLI: `segarecomp
emit-general-startup-bridge-c --immutable-rom-aot --immutable-aot-region-proposal <regions> --region-admission-plan-output <plan>`
(report-only: no C is generated). Note: sequential fall-through is itself an obligation, so any run of code-looking data whose
fall-through chain reaches the region boundary is pruned; a true region must therefore be terminated by an unconditional transfer.
Tests: `genesis_region_prune_test` (explicit scenarios, 24 random images against a naive reference fixed point, order independence,
caps) and `segarecomp_region_proposal_test`.

## 3. Frozen region policy (T003; frozen on Sonic 1 ONLY, before Sonic 2 / Cool Spot / Streets of Rage were processed)

Calibration truth: the SEG-044 exact source universe `C` (24,180 identities, 32 pages of 4 KiB; 25.0% of the 512 KiB ROM).

Rejected on Sonic 1 (criterion: 100% page coverage of `C` and R <= 40% of ROM):
- Code-seed policies P0/P1/P2 over the precise direct-control discovery (599 identities) at 2/4/8 KiB: P2 at 8 KiB selects only 12.5%
  of ROM and misses 11 `C` pages (jump/object tables are not seen by direct discovery). All fail; no manual page added.
- ROM-only flow-terminator density at 4 KiB: no threshold works (thresholds <= 0.007 give R = 40.6-41.4% or more, > 40%; >= 0.008
  misses the sparse C page 29 holding 93 `C` identities).

Frozen policy `FLOW8-D` (`tools/segarecomp_region_proposal.py`; constants are code-frozen and unit-tested):
- page size 8 KiB (an allowed sensitivity size; the primary 4 KiB failed as above);
- select a page when `flow_terminators / (page_bytes/2) >= 0.010` (flow terminator = BRA / direct JMP / RTS / RTE / RTR / JMP ea as
  classified by the C++ control-successor owner; 0.010 is twice the random-data baseline of ~0.005 and the middle of the passing plateau
  0.008-0.020 on Sonic 1) OR the page contains a precise direct-control-discovery identity (machine-root reachable); merge adjacent pages;
- no halo, bridging, per-title exclusion or addition. Anything not stated here is not part of the policy.

Sonic 1 calibration result (current main, Release): ROM 524,288 B; R 180,224 B = 34.38%; U 246,293; |K0| 87,422; |K| 87,372
(50 pruned, 8 rounds); K/U 0.3547; |C| 24,180, C/K 0.2767; C ⊆ R yes; C ⊆ K yes; unchanged production validator accepted;
analysis+prune CLI wall 0.6 s, peak RSS 306 MiB (report-only run; no code generation). Sonic execution oracle is a sanity check only
(T005).

## 4. Blind cross-title run (T004; policy of section 3 applied unchanged, no per-title input)

Release CLI, 4 vCPU. `R` = selected 8 KiB pages; `K` = pruned set; "plan" = unchanged production validator verdict (run inside the CLI on a
scratch copy and again by the emitter in the build).

| title | ROM | R | U | K0 | K | K/U | pruned (rounds) | roots / materialized | validator | analysis+prune wall / RSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Sonic 1 (calibration) | 512 KiB | 34.38% | 246,293 | 87,422 | 87,372 | 0.3547 | 50 (8) | retained | accepted | 0.6 s / 306 MiB |
| Sonic 2 | 1 MiB | 32.81% | 496,387 | 167,871 | 164,089 | 0.3306 | 3,782 (135) | retained | accepted | 1.6 s / 594 MiB |
| Cool Spot | 1 MiB | 37.50% | 498,276 | 187,822 | - | - | rounds 206 | machine root pruned | REJECTED (`machine_root_not_admitted`, class `pruned`) | 0.5 s / 432 MiB |
| Streets of Rage | 512 KiB | 23.44% | 247,761 | 59,971 | 59,753 | 0.2412 | 218 (57) | retained | accepted | 0.4 s / 307 MiB |

Pruning rounds never widened anything (`K ⊆ K0`, unit-tested). No result depended on SEG-030/031 analysis.
Sonic 2 source-truth oracle: NOT AVAILABLE. A public disassembly project exists, but building it requires executing downloaded
third-party code (a Lua build script and prebuilt assembler binaries) which the execution environment did not authorize; no ROM was
downloaded and no `C2` was derived. The oracle was optional and did not block the experiment.

## 5. Build, runtime, oracle and economics (T005)

Workload: the existing complete execution-PC oracle, 23,200 no-render no-input frames, dispatch cap 1.5e9, ordinary production emitter and
runtime, no runtime change. Broad and selective runs on identical input. (`gcc` 14 rejects an unreachable decoded `divu #0` in the
broad Streets of Rage / Cool Spot programs under `-Werror=div-by-zero`; those two broad oracle builds used `clang`. Digests are
compiler independent. Economics below use `clang` for every build.)

| title | outcome (broad / selective) | distinct PCs | escapes outside K | coverage / final-state digest | verdict |
| --- | --- | --- | --- | --- | --- |
| Sonic 1 (sanity only) | frames_reached x2 | 10,512 | 0 | identical | pass |
| Sonic 2 | frames_reached x2 | 2,478 | 0 | identical (also dispatches, retirements, frame-stream digest) | PASS |
| Cool Spot | plan rejected before build | broad run: 4,928 | n/a | n/a | FAIL (region recall) |
| Streets of Rage | broad frames_reached; selective `guest_stop` at frame 283, typed `unresolved_indirect_target` / `reached_unresolved_direct_edge` | broad 8,758 | selective stopped fail-closed (no wrong state) | n/a | FAIL (runtime escape) |

Failure attribution (runtime coverage used strictly as a falsifier AFTER the fact; nothing was fed back): the broad run executes
13 PCs (Streets of Rage) and 176 PCs (Cool Spot) outside `R`, each inside a single 8 KiB page whose flow-terminator density was 0.0034
and 0.0020 - below the random-data baseline of ~0.005, so no density threshold near the frozen 0.010 could select it; a code island
sits inside an otherwise data page. In Streets of Rage the missing 13 PCs additionally prune 88 executed in-region PCs through their fixed
edges (101 observed PCs outside `K`): pruning amplifies a recall miss into callers but fails closed, it never admits anything. In Cool Spot
the same cascade reaches a machine root, so the proposal is rejected up front (the best possible outcome: a build-time rejection).
Sonic 1 and Sonic 2: 0 observed PCs outside `R`.

Economics of the two passing configurations (`segarecomp build`, clang, -O2 default, 4 vCPU, broad -> selective):

| title | generated C | compile CPU | build wall | peak RSS | executable | 23,200-frame run |
| --- | --- | --- | --- | --- | --- | --- |
| Sonic 2 | 314.4 -> 123.9 MB (-60.6%) | 404.8 -> 154.9 s (-61.7%) | 123.0 -> 54.0 s (-56.1%) | 991 -> 439 MiB (-55.7%) | 48.2 -> 18.9 MB (-60.8%) | 22.8 -> 22.2 s (noise) |
| Sonic 1 | 189.4 -> 78.4 MB (-58.6%) | 249.9 -> 106.0 s (-57.6%) | 78.9 -> 35.3 s (-55.2%) | 530 -> 237 MiB (-55.3%) | 29.6 -> 12.6 MB (-57.4%) | 21.0 -> 22.0 s (noise) |

## 6. Gate decision

Pre-registered gate (section 1): >= 2 of 3 titles pass, including Cool Spot or Streets of Rage. Result: 1 of 3 (Sonic 2 only);
neither Cool Spot nor Streets of Rage passes. **GATE: FAIL. No implementation successor is registered.** Failure class: **region
detector recall** (executed code islands in data-like pages). Structural pruning did not collapse on a closed region (Sonic 1, Sonic 2,
Streets of Rage plans are accepted by the unchanged validator; the 24 random-image property tests equal the naive greatest fixed
point), the production validator was compatible, there were no title-specific executable-image findings, and economics (K/U 0.24-0.35,
about -60% generated C / compile CPU / RSS / executable) would have been worthwhile. The experiment's tested weaker contract
("conservatively bound where code lives with a ROM-statistics + direct-discovery proposal") is rejected as insufficiently
recall-safe; "pruning plus fail-closed dispatch" is sound but cannot repair missing regions.
No page, threshold or policy was changed after observing any title.

## 7. Answers to the final questions

1. Frozen on Sonic 1: `FLOW8-D` (8 KiB pages; flow-terminator density >= 0.010 OR a page containing a precise direct-control-discovery
   identity; adjacent pages merged). 4 KiB (primary) failed the calibration criterion; 8 KiB was an allowed sensitivity size.
2. R covered 34.38% of the Sonic 1 ROM (180,224 of 524,288 bytes).
3. Yes: C (24,180) ⊆ R and C ⊆ K (50 identities pruned, none in C).
4. Sonic 1: U 246,293; K0 87,422; K 87,372.
5. Sonic 2: PASS. U 496,387; R 32.81%; K 164,089; K/U 0.3306; 0 escapes; identical digests; generated C -60.6%, compile CPU -61.7%,
   RSS -55.7%, executable -60.8%, runtime unchanged.
6. Cool Spot: FAIL. U 498,276; R 37.50%; K0 187,822; K none (machine root pruned, proposal rejected); 176 executed PCs outside R.
7. Streets of Rage: FAIL. U 247,761; R 23.44%; K 59,753; K/U 0.2412; validator accepted; selective stopped fail-closed at frame 283
   (typed unresolved direct edge); 13 executed PCs outside R.
8. Not available (source oracle skipped; see section 4).
9. No per-title tuning was done or needed to run the policy; none was applied to repair a failure.
10. No result depended on SEG-030/031 convergence.
11. No. False-positive decoded data never widened anything (`K ⊆ K0 ⊆ U`); pruning only removed identities.
12. No. The runtime was unchanged; a missing entry stops with the existing typed fail-closed stop.
13. No. Productionizing executable-region admission with this region producer is not justified. A different region producer would have to
    capture small code islands in data pages (for example a high-recall code-entry inventory) before the pruning + fail-closed machinery
    is worth productionizing; that is a new experiment, not a successor of this one.
14. Yes. Broad AOT remains the unconditional fallback and default; exact source maps remain the best case.
