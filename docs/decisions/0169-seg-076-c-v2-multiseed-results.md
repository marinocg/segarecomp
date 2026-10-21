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

- **Every C-v2 seed, and the 3-seed ensemble (+0.022), has a higher Genesis mean ER than A.**
  - A is better on 9 of the 11 lineages. The exact sign-flip p over the seed-averaged per-lineage differences is 0.006.
  - The largest gaps are D03 (+0.36), D12 (+0.16) and D01 (+0.12).
  - So the direction is consistent. The pre-registered seed-level interval is what falls short with 3 seeds.
- **The T001 amendment's C result was a favourable draw.**
  - C-v2 seed 1 computes the same function from the same seed and initial weights. It differs only in floating-point summation order
    (equivalent within 7e-7 per step).
  - It reaches 0.150, against the original's 0.116. Its D03 is 0.479 against 0.086.
  - The original C's non-determinism plus chaotic training dynamics therefore spans at least 0.034 in Genesis mean ER.
  - The 0.116 that made C look second-best in ADR 0165 should not be read as C's typical level.
- **C shares M3-R's tail instability (ADR 0167).** D03 and D01 dominate C-v2's between-seed variation. Seed 2 has 65 B50 misses:
  64 in D12 and 1 in D01.
- **Cost.** One C-v2 fit costs about 1.6 CPU-hours even with GPU training. An A fit costs about 29 to 40 CPU-seconds. Inference differs by
  about 2x per image (0.48 s against about 0.2 s).

## Decision

- SEG-076-T005 records `EVIDENCE_INSUFFICIENT` for a C advantage, following the pre-registered mapping (ADR 0168). This is the synthesis
  class for C.
- The descriptive evidence points consistently towards the lightweight structural model A on these development folds:
  - every seed and the ensemble are worse than A;
  - A is better on 9 of 11 lineages;
  - A costs about 1/150 of a C-v2 fit.
- The pre-registered statistic cannot classify this as `A_ADVANTAGE_OVER_C` with 3 seeds.
- C-v2's deterministic, about 4x faster implementation is preserved as a research artefact. It is not a production candidate.
- Nothing here reverses SEG-076-T001's pre-registered `NEGATIVE_STOP` or SEG-076-T002's cancellation.
