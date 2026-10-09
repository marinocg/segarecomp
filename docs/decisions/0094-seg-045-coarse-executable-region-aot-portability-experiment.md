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
- Code-seed policies P0/P1/P2 over the precise direct-control discovery (599 identities) at 2/4/8 KiB: P2 at 8 KiB reaches only 12.5%
  of ROM yet misses 11 of 15 `C` pages (jump/object tables are not seen by direct discovery). All fail; no manual page added.
- ROM-only flow-terminator density at 4 KiB: no threshold works (<= 0.007 gives R > 40%... at 0.006-0.007 R = 40.6-41.4%; >= 0.008
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
