# ADR 0193: SEG-085-T003 - DYN_SCORE, unchanged ADAPT on structural model A, leakage-safe calibration and protocol freeze (checkpoint C3)

- Status: Accepted (SEG-085-T003, checkpoint C3). Follows ADRs 0190-0192. Frozen plan `tools/segarecomp_seg085_dyn.plan.json`, digest `59813af76417d99e76be302e37059f013860b1d160d09678722a10575a683d8f`, committed before any cohort image was scored. Model A, SEG-072 ADAPT, the admission validator, closure and production defaults are unchanged.
- Results (counts and ratios only): `tools/segarecomp_seg085_dyn.results.json`. Reproduce: `segarecomp_seg085_dyn.py inner | nested | freeze`.

## Frozen definitions

- **DYN_SCORE**: unchanged model-A logits on 128-byte cells; select every cell with score >= T (ties inclusive) and union the independently established seed cells; pass the proposal to the existing prune/closure/validator; abstain to broad AOT when the retained fraction exceeds 0.60 (the SEG-072 `MAX_RETENTION`) or when inputs violate the assumptions. Non-finite or non-numeric scores are rejected, an image without a seed universe is unsupported, abstention (cap) is a fourth, separately counted status; none of them truncates a selection.
- **Calibration** (one statistic, no search, no per-game or per-era input): `m_L` = lowest non-seed positive-cell score of lineage L; `T0 = min_L m_L`; `shortfall = max_L (min_{L' != L} m_L' - m_L)` (the miss of the plain minimum rule when each lineage is treated as unseen); `T = T0 - shortfall`.
- **Isolation**: for a held-out lineage H the calibration record of lineage C comes from a model fitted on the nine lineages that are neither H nor C (110 bounded inner refits, one per ordered pair, 6-way parallel); the non-isolated variant (C scored by its own leave-one-lineage-out model, which trained on H; the disclosed SEG-072 ADAPT limitation) is reported beside it and not used for the decision. The deployment calibration uses the eleven leave-one-lineage-out records: `T = -13.655` (`T0 = -10.339`, shortfall `3.316`).
- **ADAPT on model A**: the unchanged `fit_adaptive_margin` / `adaptive_budget` with model A's logit as the rank key and `sigmoid(logit)` as the probability; no other adapter. Deployment margin (eleven records) `0.174`. An isolation-corrected margin (same rule, isolated records) is reported as a sensitivity.
- The final model-A fit used for the cohort reproduces the stored SEG-076 all-Genesis cross-platform scores bitwise (gate recorded); no hyperparameter was touched.

## Development results (nested leave-one-lineage-out, lineage-separated; 50 images; not independent evaluation)

| policy | commercial-era (4 lineages, 43 images) | SGDK (7 lineages, 7 images) |
|---|---|---|
| B35 | retained 0.368, **223 missed starts** (3 Sonic CD modules) | 0.350, 7,837 missed (D07) |
| B50 | 0.513, 0 | 0.500, 0 |
| ADAPT, unchanged protocol (non-isolated margin) | **0.315**, **1 missed start** (s2; 116 starts outside K after the production prune), 4 abstentions | 0.489, 0, 1 abstention |
| ADAPT, isolation-corrected margin | 0.529, 0, 9 abstentions | 0.516, 0, 1 abstention |
| DYN_SCORE (isolated, frozen) | **abstains on all 43** (pre-cap selected fraction 0.94-1.00) | **abstains on all 7** (1.00) |
| DYN_SCORE (non-isolated variant) | 0.834, 18 missed starts in 4 images (s1, s2, sk, one module), 9 abstentions | 1.0, 0 |

## Findings

1. **DYN_SCORE does not produce a dynamic budget.** The frozen threshold sits 13-15 logits below zero because the per-lineage minima of the lowest non-seed positive score are not comparable: D01 -10.3, D12 -7.0, D03 -6.4 against +0.25 to +10.4 elsewhere (ADR 0191). A threshold that holds the weakest positive of any lineage lies among the negatives of the others, so every image retains more than the cap and abstains. The unmasked, truth-informed per-image headroom is large (oracle full-recall fraction: commercial-era mean 0.167, SGDK 0.241, against B50 0.5), so the loss is calibration, not the ranking.
2. The non-isolated variant misses (18 starts in 4 images) even though its threshold is higher, which shows why isolation matters: an optimistic threshold fitted on models that saw the target lineage is not safe.
3. **Why ADAPT overretained historically and what changes on model A.** The margin is the worst fitting image's residual-minus-estimate plus 0.05, a single constant. With model A the density estimate tracks labelled density on commercial-era images (ADR 0191) and the deployment margin is only 0.174 (M3-R 0.301, frozen M3 0.826), so the non-isolated fit retains 0.315 (lineage mean; image mean 0.348, cell-weighted 0.278) on commercial-era images. **That is the optimistic, non-isolated figure: with the isolation-corrected margin the lineage mean is 0.529 with 9 abstentions, no better than B50 (0.513), i.e. no saving.** All retained figures in this record are lineage means unless stated. The failure mode is unchanged: the lineage that sets the margin is unprotected when it is held out (D01's margin is 0.087 when D01 is the target; s2 then misses), exactly as D03 did in SEG-072 (32 misses).
4. Fixed budgets: B35 is infeasible on dense images (ADR 0191); B50 had zero misses everywhere in development.

## Frozen decision criteria (unchanged from ADR 0190)

Cohort scoring and economics may not change these definitions, the cap, T or the margins; the tooling refuses (`verify_plan`) to evaluate if policy or calibration code differs from the digests above.
