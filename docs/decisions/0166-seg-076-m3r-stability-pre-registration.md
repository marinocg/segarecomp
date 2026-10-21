# ADR 0166: SEG-076 M3-R seed stability and transfer study: diagnosis and pre-registration

- Status: accepted (diagnosis and pre-registration; frozen before the first T003 fit)
- Task: SEG-076-T003, checkpoint C4 of the combined SEG-076 PR
- Plan: `tools/segarecomp_xplat_stability.plan.json` + `.digest`. Diagnosis: `tools/segarecomp_xplat_stability.diagnosis.json`

## Step 1 diagnosis (evidence tier 1: reanalysis of the preserved seed-49 and seed-50 M3-R C0 fits; no new fit)

The seed instability that blocked SEG-075 (tau 0.103) is, in substance, the D01 lineage.

- **Other lineages are stable.** The 10 other Genesis lineages move by at most 0.042 in lineage max ER between seeds 49 and 50; nine of
  them by at most 0.019.
- **D01 moves a lot:**

  | D01 statistic | seed 49 | seed 50 |
  |---|---|---|
  | lineage max ER | 0.228 | 0.709 |
  | mean image ER over its 35 informative images | 0.029 | 0.072 |
  | B50 misses | 0 | 15 |

- **The D01 jump is not driven by one small module, but it is concentrated in a few images.**
  - Six of D01's 35 informative images worsen by more than 0.05. One improves.
  - The lineage max ER is set by the same 98-cell module under both seeds (0.23 to 0.71). With 79 non-positive cells, a one-cell rank
    swing of its weakest positive moves its ER by about 0.013.
  - Two of the largest D01 images also worsen strongly:
    - the 8192-cell image goes from 0.21 to 0.68, with 12 B50 misses;
    - the 16384-cell image goes from 0.07 to 0.40.
  - D01 is therefore genuinely unstable, not just a small-module max-ER artifact.
- **The metric dimension matters.**
  - Genesis mean ER, which averages the lineage max ER, moves from 0.146 to 0.182.
  - The Genesis mean of per-lineage mean image ER barely moves: 0.1157 against 0.1139.
  - The instability lives in the tail of a few images' rankings, and the max-ER aggregate amplifies it.
- **Mechanism: tail ranking variance on held-out code, not optimization failure.**
  - The weakest positive cell (the one that sets FRF and ER) of every unstable image lies in the boundary strata: D bytes, adjacency to
    D-only cells, and short runs.
  - Between-seed cell-rank Spearman correlation in D01 has a median of 0.71.
  - The loss curves of both seeds converge to the same range on every fold: final loss 4e-4 to 1.5e-3, with similar counts of epochs
    that rise by more than 20%.
  - The training class balance is identical across seeds, because it depends only on the fold (D01-held-out pos_weight 7.79 against
    about 5.5 elsewhere).
  - The segment-sampling *exposure* is identical across seeds (same training images and segment counts). Only the offsets and the order
    differ.

## Decision (pre-registered before any T003 fit)

### M3-R paired multi-seed (tier 2)

- **Design.**
  - Seeds 49 to 52 under C0 and under C2, on the 11 Genesis leave-one-lineage-out folds.
  - Valid preserved fits are reused: C0 seed 49 (SEG-072), C0 seed 50 and C2 seed 49 (SEG-075). That leaves 55 new CPU fits, using the
    unchanged recipe with only `RECIPE.seed` changed, which is the mechanism SEG-075 used for seed 50.
  - One determinism refit (C0, D01, seed 50) is compared bitwise with SEG-075's file.
  - Four seeds was a budget choice (about 7 to 8 hours of CPU). The rule's EVIDENCE_INSUFFICIENT branch covers an under-powered result.
- **Noise.** tau4 = 2 x the mean over lineages of the mean absolute pairwise difference in lineage max ER.
- **Transfer.** The statistic is d_s = mean ER (C2, s) - mean ER (C0, s), paired by seed.
  - `TRANSFER_EFFECT_MEASURABLE` requires all three: the 95% t interval excludes 0, |mean d| >= 0.01, and the exact 11-lineage sign-flip
    p <= 0.05.
  - `EVIDENCE_INSUFFICIENT` applies when the interval half-width exceeds 0.05.
  - `NO_MEANINGFUL_ADVANTAGE` applies otherwise.
  - A secondary statistic (mean image ER) and a 4-seed-ensemble contrast are reported descriptively.
- **Seed stability.** The spreads compared are the SD over the 4 single seeds and the SD over the 6 two-seed score ensembles.
  - `SEED_STABILITY_IMPROVED` requires all of the following:
    - in both conditions, the ensemble spread is at most 0.6 x the single-seed spread, for the Genesis mean ER *and* for D01 max ER;
    - the 4-seed ensemble is not worse than the mean of the single seeds.
  - `NO_MEANINGFUL_ADVANTAGE` applies otherwise.

### A under C2 (tier 2, deterministic)

- A is refitted under C2 on the 11 Genesis folds; its C0 fits are T001's.
- `TRANSFER_EFFECT_MEASURABLE` requires |d| >= 0.01 and sign-flip p <= 0.05.

### M1-HGB (tier 1 reanalysis)

- M1 is deterministic: its `random_state` has no effect below sklearn's 200k-sample binning threshold.
- The analysis uses the SEG-075 C0 and C2 fold scores: per-lineage paired differences, a lineage-bootstrap 95% CI (10000 resamples,
  `default_rng(0)`) and the exact sign-flip p.
- `DESCRIPTIVE_EFFECT_ROBUST` requires both: the CI excludes 0 and p <= 0.05. Otherwise the class is `DESCRIPTIVE_EFFECT_NOT_ROBUST`.
- This is never a confirmatory claim.

### Common rules

- D01 is never excluded.
- Every finding carries its evidence tier. No tier-3 evidence (unseen lineages) exists here.
- SEG-075's tau and decision are unchanged.

### Resources

- CPU only, because every reused M3-R fit is a CPU result.
- 3 parallel fits: M3-R at 4 threads, A at 2.
- Caps: M3-R 90 min, A 20 min.
- RSS limit 4 GB. A fit starts only with at least 4000 MB available, and the newest fit is killed below 2000 MB.
- Crash-safe single attempts: a persisted `started` status becomes `interrupted`. One identical infra re-run is allowed only after
  `interrupted` or an external signal kill.

## Consequences

C4 results follow in an ADR section or a separate ADR after an independent review of this freeze. Compile-cost measurement, if any, is
SEG-076-T004's.
