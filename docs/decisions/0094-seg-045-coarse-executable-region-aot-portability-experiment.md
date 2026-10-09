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
