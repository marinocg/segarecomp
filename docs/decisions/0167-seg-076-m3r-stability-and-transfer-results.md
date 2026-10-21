# ADR 0167: SEG-076 M3-R seed stability, paired transfer, A and M1 re-comparison: results

- Status: accepted (exploratory, development-only)
- Task: SEG-076-T003, checkpoint C4 results
- Plan: ADR 0166, `tools/segarecomp_xplat_stability.plan.json` (digest `62f257ee...`, frozen and independently approved before any fit)
- Results: `tools/segarecomp_xplat_stability.results.json` (aggregates only)

## Execution

- All 68 planned jobs completed `ok` on the first attempt:
  - 55 new M3-R fits (CPU, 4 threads, about 14 min each, 50 CPU-hours in total);
  - 11 A fits under C2 (about 29 s each);
  - 2 determinism refits.
- **Both determinism refits are bitwise identical** to the reused SEG-075 files: C0 / D01 / seed 50, and C2 / D01 / seed 49 (this one
  also covers SEG-076's loader path). The reuse of the SEG-072 and SEG-075 seeds is therefore valid, and the pre-registered override
  does not apply.

## Results (Genesis mean ER = mean over the 11 lineages of the lineage max ER; 128-byte cells)

| M3-R | seed 49 | seed 50 | seed 51 | seed 52 | 4-seed logit ensemble |
|---|---|---|---|---|---|
| C0 mean ER (B50 misses) | 0.146 (0) | 0.182 (15) | 0.221 (39) | 0.170 (2) | 0.117 (0) |
| C2 mean ER (B50 misses) | 0.122 (0) | 0.232 (28) | 0.200 (11) | 0.182 (2) | 0.107 (0) |
| C0 D01 max ER | 0.228 | 0.709 | 0.477 | 0.759 | 0.228 |
| C2 D01 max ER | 0.152 | 0.680 | 0.567 | 0.886 | 0.076 |

Reference points:

- A (deterministic): C0 0.105, C2 0.102, both with 0 B50 misses.
- M1-HGB: C0 0.177, C2 0.149.

### Evidence tier 2 (new training-seed experiment on the existing development folds)

- **M3-R training noise is large, and seed 49 was the best draw.**
  - Over four seeds, the SD of the Genesis mean ER is 0.027 in C0 and 0.040 in C2.
  - B50 misses range from 0 to 39.
  - tau4 is 0.150 in C0 and 0.164 in C2, against SEG-075's two-seed tau of 0.103. That tau is unchanged as SEG-075's decision.
  - Seed 49, the SEG-072 recipe seed behind every earlier M3-R comparison, is the best of the four in both conditions. SEG-072's
    published M3-R numbers therefore come from a favourable draw.
- **M3-R cross-platform transfer: `EVIDENCE_INSUFFICIENT`.**
  - The paired C2 - C0 difference is +0.005, with a 95% t interval of [-0.050, +0.059] and an 11-lineage sign-flip p of 0.34.
  - The per-seed differences are -0.023, +0.050, -0.021 and +0.012.
  - SEG-075's descriptive seed-49 transfer benefit does not persist across seeds.
  - The secondary statistic (mean image ER) agrees: +0.007, [-0.042, +0.056].
  - Descriptive only: the 4-seed ensembles differ by -0.010, carried by D01 (-0.152), while D09 shows +0.031.
  - The expected half-width (about 0.057) was stated before any fit.
- **Seed stability: `NO_MEANINGFUL_ADVANTAGE` under the pre-registered rule**, although ensembling clearly helps on average.
  - C0 meets every criterion:
    - 5 of 6 pair ensembles have a negative D01 g_score;
    - the mean pair g_score is -0.034;
    - the 4-seed g_score is -0.047.
  - C2 misses by one pair: only 4 of 6 pairs have a negative D01 g_score (one +0.139, one 0.000). Its mean pair g_score is -0.028 and its
    4-seed g_score is -0.058.
  - Descriptive:
    - In both conditions, the 4-seed logit ensemble is better than *every* single seed.
    - It is better by about 0.03 to 0.05 beyond pure averaging of outcomes (g_score), and the Jensen term is only -0.015 to -0.019.
    - It has no B50 misses.
    - Some pair ensembles are still poor. The two-seed ensemble of seeds 50 and 51 has D01 0.61 and 12 misses in C0.
  - The C0 49+50 pair's gains (-0.24 on D01) had been seen in the dry run before the freeze (ADR 0166). They are not decisive: C2 fails
    on its own pairs.
- **A cross-platform transfer: `NO_MEANINGFUL_ADVANTAGE`.**
  - C2 - C0 is -0.003 with p = 0.89. The only sizeable change is D01 (-0.048). A is deterministic, so no seed noise is involved.
  - A single A fit (about 29 s) is better than the 4-seed M3-R ensemble in C0 (0.105 against 0.117). In C2 the two are comparable (0.102
    against 0.107). The ensemble costs about 4 x 14 CPU-minutes x 4 threads per fold.

### Evidence tier 1 (reanalysis of existing development results)

- **M1-HGB's descriptive transfer: `DESCRIPTIVE_EFFECT_NOT_ROBUST`.**
  - The mean difference is -0.028, but all of it comes from D01 (-0.282).
  - Of the other lineages, 4 are better, 5 are worse and 1 is unchanged.
  - The lineage-bootstrap CI is [-0.080, +0.002] and the sign-flip p is 0.46.
- **The Step 1 diagnosis (ADR 0166) stands.** The D01 instability is variance in the ranking tail of held-out images. The four seeds
  confirm it: D01's max ER ranges from 0.15 to 0.89 across seeds and conditions, while most other lineages move by less than 0.05.

### Evidence tier 3

None. No unseen-lineage evaluation was made.

## Decision

- SEG-076 records, without replacing SEG-075's decision:
  - M3-R seed stability `NO_MEANINGFUL_ADVANTAGE` under the frozen rule (descriptively, 4-seed ensembles help);
  - cross-platform transfer `EVIDENCE_INSUFFICIENT` for M3-R and `NO_MEANINGFUL_ADVANTAGE` for A;
  - M1's descriptive effect `DESCRIPTIVE_EFFECT_NOT_ROBUST`.
- M3-R single-seed results are not a reliable basis for comparing models or training conditions. Any future M3-R comparison needs
  multiple seeds or an ensemble.
- The deterministic structural model A has no seed noise and needs about 1/30 of an M3-R fit's CPU. On these development folds it
  matches or beats M3-R's 4-seed ensemble. This feeds the SEG-076 synthesis (SEG-076-T004). It is not a promotion: these are reused
  development folds.
