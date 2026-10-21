# ADR 0169: SEG-076 deterministic C-v2 and matched multi-seed C vs A: results

- Status: accepted (exploratory, operator-directed, development-only)
- Task: SEG-076-T005, checkpoint C5a results
- Plan: ADR 0168, `tools/segarecomp_xplat_cv2.plan.json` (digest `ffe0bdce...`; frozen and approved before any fit)
- Results: `tools/segarecomp_xplat_cv2.results.json` (aggregates only)

## Execution

- All 35 planned jobs completed `ok` on the first attempt, on ROCm (RX 9070 XT): 33 fold fits, the seed-1 `all` fit and the determinism
  refit.
- Each fit took about 25 minutes, with 3 in parallel; 55.3 CPU-hours in total for 34 fits.
- Inference took 0.48 s per image, against about 7.2 s for the frozen C.
- **The full-length determinism refit (D05, seed 1) is bitwise identical.** C-v2 is run-to-run deterministic on ROCm at production length,
  including under 3-way concurrency. This is the empirical claim ADR 0168 rested on.

## Results (C0; Genesis mean ER = mean over the 11 lineages of the lineage max ER)

| configuration | Genesis mean ER | worst lineage ER | B50 misses | boundary missed @B50 |
|---|---|---|---|---|
| A (T001, deterministic) | 0.105 | 0.211 | 0 | 0 |
| C-v2 seed 1 | 0.150 | 0.479 | 1 | 1 |
| C-v2 seed 2 | 0.218 | 0.519 | 65 | 1 |
| C-v2 seed 3 | 0.154 | 0.398 | 0 | 0 |
| C-v2 3-seed logit ensemble (descriptive) | 0.127 | 0.253 | 0 | 0 |
| original C seed 1 (T001 ROCm amendment, non-deterministic) | 0.116 | 0.211 | 0 | 0 |

### Pre-registered class: `EVIDENCE_INSUFFICIENT`

- d_s = C-v2 minus A is +0.045, +0.113 and +0.048, with mean +0.069.
- The 95% t interval (df = 2) is [-0.026, +0.163]. It crosses 0, so neither advantage class applies, and it is far wider than the
  +/-0.02 equivalence margin.
- The power statement in ADR 0168 anticipated this outcome.

### Descriptive (not the rule)

- **Every C-v2 seed, and the 3-seed ensemble (+0.022), has a higher Genesis mean ER than A. The gap is concentrated in D03.**
  - Averaged over the three seeds, A is better on 9 of the 11 lineages. The exact two-sided sign-flip p, with lineages as units and
    compared against a single deterministic A fit, is 0.006.
  - Per seed, A is better on 7, 9 and 10 lineages. Against the ensemble it is better on only 5, worse on 5, with 1 tie.
  - D03 is systematically worse for C-v2 in every seed (0.29 to 0.48, against A's 0.029):
    - the ensemble's +0.022 gap is almost entirely D03, and the other 10 lineages average +0.001;
    - seed 1's +0.045 is likewise mostly D03 (+0.004 without it).
  - The fair descriptive reading:
    - the C-v2 ensemble matches A except on D03, where it is systematically worse;
    - single C-v2 seeds add large, seed-dependent tail failures, mainly in D12 and D01.
- **The T001 amendment's C result was likely a favourable draw.**
  - C-v2 seed 1 starts from the same seed and initial weights as the original C seed 1, and computes the same function.
  - The two runs are not otherwise identical:
    - C-v2's equivalence (within 7e-7) was measured on CPU torch 2.14;
    - on ROCm the two use different kernels (a gather with a masked sum, against the original's index-add path);
    - the original also ran non-deterministically in a 4-way farm.
  - So 0.116 against 0.150 is one pair of runs from two implementations, not a measured bound on the original's own spread.
  - Descriptively, a sub-1e-6 implementation perturbation at a fixed seed moved Genesis mean ER by 0.034, and D03 from 0.086 to 0.479.
    Since 0.116 is below all three C-v2 seeds (0.150 to 0.218), it should not be read as C's typical level.
- **C shares the M3-R tail instability (ADR 0167).**
  - C-v2's between-seed variation is dominated by D12 (range 0.49) and D01 (range 0.38). D03 is consistently high rather than variable
    (range 0.19).
  - Seed 2 has 65 B50 misses: 64 in D12 and 1 in D01.
- **Cost.**
  - One C-v2 fit costs about 1.6 CPU-hours even with GPU training, plus GPU time. An A fit costs about 29 to 40 CPU-seconds, about 1/150
    to 1/200 of that.
  - Inference is cross-device and not strictly comparable: about 0.48 s per image on the GPU for C-v2, against about 0.2 s on the CPU for
    A.

## Decision

- SEG-076-T005 records `EVIDENCE_INSUFFICIENT` for a C advantage, following the pre-registered mapping (ADR 0168). This is the synthesis
  class for C.
- The descriptive evidence favours the lightweight structural model A on these development folds.
  - Every C-v2 seed and the ensemble have a higher Genesis mean ER than A.
  - That ensemble gap is concentrated in one lineage (D03). Elsewhere the C-v2 ensemble roughly matches A.
  - A has no seed noise and costs about 1/150 to 1/200 of a C-v2 fit.
- The pre-registered statistic cannot classify this as `A_ADVANTAGE_OVER_C` with 3 seeds.
- C-v2's deterministic, about 4x faster implementation is preserved as a research artefact. It is not a production candidate.
- Nothing here reverses SEG-076-T001's pre-registered `NEGATIVE_STOP` or SEG-076-T002's cancellation.
