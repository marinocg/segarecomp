# ADR 0195: SEG-086 - dynamic score calibration and retention feasibility: scope, data protection and score-distribution diagnosis (checkpoints C0-C1)

- Status: Accepted (SEG-086, checkpoints C0/C1). Follows ADRs 0190-0194; none is amended. SEG-085's decision (`EVIDENCE_INSUFFICIENT`) stands.
- Aggregates: `tools/segarecomp_seg086_diag.results.json` (aggregate only); code `tools/segarecomp_seg086_diag.py`, `tools/segarecomp_sample_roles.py`.

## Question and status of the evidence

Can a simple, label-free, per-image robust normalisation of the **unchanged** structural model A scores make one threshold transfer across development lineages, giving genuinely variable per-image budgets with no missed verified start and a
measured saving over A+B50? This is an offline **calibration-feasibility assessment on development data**; it is not independent qualification and authorises nothing.

- The SEG-085 independent image (Phantasy Star IV, H01) was scored and its outcomes examined in SEG-085. It is a **previously exposed diagnostic case**: its labels, outcomes and economics are not used to choose the formula or any parameter, it may be reported after the
  protocol is frozen as `REUSED_HISTORICAL_DIAGNOSTIC - NOT INDEPENDENT CONFIRMATION`, and it never counts toward a fresh evaluation-lineage minimum. ToeJam & Earl remains recorded as exposed (ADR 0192) and is not eligible as new untouched material. SEG-072 sealed, SEG-075 reserved and
  SEG-077 reserved material is not opened.
- Independent confirmation needs genuinely new, untouched historical programs.

## C0: evaluation-only role protection (audit and safeguards)

Audit result: the private snapshot `seg085-cohort-v1` declares H01 `evaluation-only`, but `restore-sb` did **not** copy the role into the restored sample's `meta.json`; a restored H01 sample was indistinguishable from a legacy development sample, and the training registry
(`segarecomp_ml_v3_train.registry`/`Image`) enumerates every `meta.json` with an `audit` field. Only the lineage allow-list (`H01` is not among the eleven fitting lineages) kept it out of the SEG-085 fits.

Safeguards (the frozen SEG-072/076/084/085 tools bind `segarecomp_ml_v3_train.py` by digest and were therefore **not** edited; the wrappers below are the intended route for new code (see the correction above)):

1. Harness PR (marinocg/segarecomp-harness #104): `restore-sb` writes `role`, `role_class`, `evaluation_only` and the snapshot tree digest into `meta.json` (after `restore_meta`, so it cannot be overridden) and refuses to restore an evaluation-only
   image without `--allow-evaluation-only`. Existing immutable snapshots are untouched. The local H01 cache was patched to carry the same fields from the private manifest.
2. `segarecomp_sample_roles.require_trainable(meta)`: the guard is a library for training, fitting and calibration entry points. **Correction after review:** the SEG-086 tools themselves (`load_records`, the diagnostic `_load`) call the unchanged registry loader and apply only the lineage-name guard (`require_trainable_lineages`) plus SEG-085's reserved-identity guard; the per-sample role check is exercised by tests and by `EvaluationContext` (used for H01), not on the development loading path. Those tools are digest-bound by the frozen plan and were not changed. This is safe today (H01 is not among the eleven fitting lineages and its restored metadata carries the role) but is a real gap for any future caller. Original wording follows. It rejects evaluation-only, exploratory diagnostic-only, contradictory
   (`role`/`role_class`/`evaluation_only`), unknown roles, and any sample whose image digest or lineage appears in a frozen evaluation manifest (so a *legacy* sample that is really H01 is still refused). Legacy samples with no role fields remain usable (backward compatibility).
3. `EvaluationContext`: the only reader of evaluation-only samples; built from a frozen manifest (optionally digest-pinned) with a stated purpose; `admit` refuses reserved identities **first**, then requires exact (image digest, lineage) membership, then a consistent role.
   `discover` rejects stray non-member samples, duplicates and missing members instead of trusting directory contents.
4. `load_trainable_images`/`load_evaluation_images` wrap the unchanged loaders so that no `Image` is constructed before the check passes.

Tests: `tests/segarecomp_seg086_roles_test.py` (synthetic datasets only: contradictory roles, legacy compatibility, evaluation-only calibration, frozen-identity denial, reserved-first ordering, manifest mismatch, stray/duplicate/missing members, no `Image` before admission) and the harness
`tests/corpus_snapshot_test.py` (role preservation through restore, explicit-flag refusal).

Residual limit: the guards are metadata- and identity-based. A frozen tool that loads a directory through the old loader directly is not protected; the SEG-086 evaluation uses only the wrappers, and the frozen results are not altered.

## C1: why raw model-A logits do not transfer (development data, out-of-fold and stored inner-refit scores)

All numbers are from stored SEG-076/084/085 scores (no refit except one final-model fit, 60 CPU-s) and are aggregates; per-cell arrays stay in the ignored cache.

1. **There is no extreme negative tail.** Model-A logits are confined to a narrow band (global minimum over the 50 images -13.1, maximum about +12.5). Obvious data forms a plateau at about -11.4 to -12.4 (per-lineage negative-score median). The frozen SEG-085 `DYN_SCORE` threshold (-13.655) is
   **below the minimum score of every one of the 50 development images**, so it selects every cell and abstains everywhere *by construction*. The cause is the rule's doubly conservative construction (the lowest verified-positive score of the lowest lineage, minus the full leave-one-lineage-out
   shortfall), not the model's ranking. The per-held-lineage isolated thresholds (-12.1 to -14.8) are also at or below the plateau.
2. **Weakest-positive instability is real but localised.** Per-lineage weakest non-seed positive scores span 20.7 logits (-10.34 to +10.40, median +1.58). Only **205 of 14,186 non-seed positive cells score below 0** (-5: 52; -9: 3), and they are confined to three lineages (D01 132 in 14 of its 39
   images, D12 47 in one, D03 26 in two); seven lineages have none. Across fit subsets the weakest-positive score of one lineage moves by up to several logits (D01 -5.2 to +3.6; D03 -6.6 to +0.3; D07 -5.7 to +0.3; the other lineages within about 2.7), i.e. an extreme order statistic of a handful of cells.
3. **Magnitude changes while ranking is stable.** Median Spearman agreement between the out-of-fold (10-lineage) scores and the inner-refit (9-lineage) scores of the same image is at least 0.87 for every lineage (0.93-0.99 for ten of them); the median/10th-percentile location shifts between fits are at most 1.2 logits for ten lineages (at most 0.6 for nine). The exception is D01,
   whose dense Sonic CD modules sit near the bimodal gap: median shift up to 5.9 and 10th-percentile shift up to 8.9. A **median is unstable for a dense image** (it falls in the gap between plateau and code mode); a low-quantile location is not.
4. **Final versus leave-one-lineage-out scale.** The final all-development model's negative plateau (median / 10th percentile) differs from the out-of-fold plateau by at most 0.25 logits for ten lineages (D01 up to 1.8). The scale of the final model is therefore the same as that of the calibration models; positive-side differences of the final model are in-sample and not evidence.
5. **Sparse commercial ROMs versus dense modules; masked regions.** The 39 Sonic CD modules (lineage D01) have mean positive density 0.27 and four of them are 100 % code (class `BUDGET_INFEASIBLE`: the density exceeds the 0.60 cap, so abstention is correct there and costs nothing). SGDK images carry 17-54 % masked
   (unknown / SDK) cells, 13-76 % of which score above 0; these raise the upper quartile and make the IQR of four SGDK images 17-22 logits while a plateau-dominated image has an IQR of 1.2-1.8 (within-lineage IQR range 0.4-22.5). **IQR therefore measures how much unlabeled code-like mass an image
   has, not how noisy its plateau is**, and a lower-half spread is 0.0 for plateau-dominated images (degenerate).
6. **The label-free score distribution carries image-level code fraction.** The share of cells with logit above 0 correlates 0.978 with verified code density (0.975 with density plus masked share) over the 50 images. A genuinely dynamic budget is therefore plausible from scores alone; the open question is only
   whether a *threshold* transfers.
7. **Error classes (50 images, SEG-085 isolated thresholds).** `RANKING_ERROR` 0 (oracle full-recall fraction at most 0.46 on every image where it is defined, against the 0.60 cap; at most 0.33 on every commercial ROM and SGDK image except d07); `BUDGET_INFEASIBLE` 4 (all-code modules); `CALIBRATION_ERROR` 46; `OK` 0. Only one verified positive cell in the corpus
   scores below the 90th percentile of its own image's negatives: inside an image the ordering is adequate. **The measured defect is calibration (absolute threshold), not ranking.** Evaluation uncertainty (not a class): masked share up to 54 %; H01 label audit 4/27,916 direct-target contradictions;
   development audit 13/102,021 (ADR 0191). These bound label artefacts but do not remove the possibility that a few low-tail positives are boundary errors; they are **not** removed or trimmed.

## Predeclared candidates and formula-selection rule (frozen here, before T002)

Candidates were chosen after the C1 diagnostics above, on development data only; no H01 label, outcome or economic figure was consulted.

- **N1 (requested starting candidate):** `z = (s - median(S)) / IQR(S)`.
- **N2 (the single additional candidate; concrete reason = findings 3 and 5: the median is unstable for dense images and IQR measures unlabeled code-like mass; the logit scale itself is shared, finding 4):** plateau-anchored location shift with unit scale, `z = s - Q10(S)`.
- Both are computed from one image's scores only, are affine-covariant by construction for N1 (N2 is shift-covariant and scale-sensitive, which is intended: the scale is shared across images because it is one model), preserve within-image ranking, are deterministic, and **fail closed**: a non-finite score, fewer than 64 cells, or
  (N1) `IQR < 0.5` logits is a normalisation failure that abstains to broad AOT and is counted separately from cap abstention and validator rejection. Normalised scores are never interpreted as probabilities.
- **Selection rule** (computed on the out-of-fold development scores before any nested threshold was fitted; the command ran more than once, see the amendments below and ADR 0196). *Amended before any selection computation existed:* the first draft compared the across-lineage **range** of the normalised weakest-positive score, but ranges in N1 units
  (IQRs) and N2 units (logits) are not comparable. The rule is now unit-free: for each candidate (and for the raw scores as a control) take `tau* = min over the eleven lineages of the lineage-minimum weakest-positive normalised score` (the in-sample full-recall threshold, zero misses by construction), apply the
  selector to every development image with the SEG-072 retention cap, and compute the **lineage-balanced mean retained fraction** (an image that abstains to the cap, or whose normalisation fails, counts as retained 1.0; the four all-code modules are excluded from the mean for every candidate). The candidate with the smaller mean wins;
  a difference below 0.01 -> N2 (simpler, no failure mode). A candidate with a normalisation failure on any non-all-code development image is disqualified. This is in-sample and used for **formula selection only**; the honest estimate is the nested evaluation (T003/T004).
  *Second amendment (recorded after the first run of the selection command showed that the rule as written disqualified both candidates on a technicality; the first-run numbers had been seen):* (a) an image without an independently established seed universe (three SGDK images, no broad-AOT seed
  report) is `unsupported` by the SEG-085 contract and is excluded from the mean, not counted as retained 1.0; (b) an image with fewer than 64 cells (8 KiB; seven development images, four of them all-code) is outside the domain of any distribution statistic: it fails closed to broad AOT (cost-trivial), is counted as
  `below_minimum_cells`, and does not disqualify a candidate; a `scale_below_floor` failure on an image of at least 64 cells does disqualify (finding 5 predicted exactly this for N1). The cell-weighted retained fraction is reported next to the lineage-balanced mean. The first-run numbers
  (raw 0.567, N1 0.555, N2 0.581 with those images mis-counted) are recorded in the SEG-086 evidence, not used.
- A purely rank-based cutoff is B35/B50 under another name and is not a candidate; T002 additionally demonstrates that the normalised selection responds to image distributions (retained fraction varies with the image's code fraction beyond what a fixed quantile would).
- Not done: mixture models, classifiers, learned budget estimators, hyperparameter search, a third candidate.

## Consequences

SEG-086 proceeds to T002 (selector and tests). Success is not presumed: if normalisation does not shrink the cross-lineage spread, the milestone ends `CALIBRATION_INEFFECTIVE`; if a specific limiting error class remains, `RANKING_LIMITATION`.
