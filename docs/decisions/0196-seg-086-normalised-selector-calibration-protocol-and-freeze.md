# ADR 0196: SEG-086 - normalised dynamic-score selector, leakage-safe calibration and frozen evaluation protocol (checkpoints C2-C3)

- Status: Accepted (SEG-086, checkpoints C2/C3). Follows ADR 0195; amends nothing earlier. Frozen by plan `tools/segarecomp_seg086_cal.plan.json` (digest in `.plan.digest`) **before** the final nested evaluation and before any new-policy H01 result.

## C2: the selector (`tools/segarecomp_seg086_norm.py`)

- N1 `z = (s - median) / IQR` and N2 `z = s - Q10` (predeclared in ADR 0195), linear-interpolated quantiles, one image's scores only, monotone (within-image ranking preserved), deterministic, no labels / ROM identity / era / game parameter, never probabilities.
- Fail-closed normalisation: non-finite or non-numeric score or an empty image -> `rejected`; no seed universe -> `unsupported`; fewer than 64 cells, N1 scale below 0.5 logits, or a non-finite normalised score -> `normalisation_failed` (broad AOT, counted separately); selected fraction (cells with `z >= tau`, ties inclusive, united with the independently established seed cells) above the 0.60 cap -> `abstain_cap`
  (broad AOT, **never a truncated region**). Validator rejection is a downstream event and is accounted separately (`policy_cost`: a rejected plan followed by the broad fallback is charged in full, normalisation/inference overhead is charged to every policy including on fallback).
- Synthetic tests (`tests/segarecomp_seg086_norm_test.py`): affine score changes, near-constant and flat distributions, extreme positive/negative outliers, non-finite scores, seed-only regions, cap abstention, dense and sparse-island images, threshold ties and ordering, and that the retained fraction follows the image's code fraction at one threshold (not a disguised fixed budget).
- Formula choice (ADR 0195 rule, with its two recorded amendments), in-sample on out-of-fold development scores, `tools/segarecomp_seg086_cal.selection.json`: lineage-balanced mean retained fraction at the in-sample full-recall threshold: RAW 0.405, N1 0.388 (**disqualified**: two `scale_below_floor` failures on images of at least 64 cells), N2 0.423 with no failure: **N2 is chosen**. In-sample the normalisations are not clearly better than the raw control (cell-weighted RAW 0.329, N1 0.388, N2 0.342; lineage-balanced RAW 0.405, N1 0.388, N2 0.423); that is expected (the same labels fit and judge) and the nested evaluation is the honest test. N1 is reported as a secondary diagnostic only and cannot be promoted.

## C3: calibration, leakage safety and the frozen protocol

- **Calibration rule** (unchanged SEG-085 `DYN_SCORE` rule applied in the normalised space; `calibrate_tau`): `tau = T0 - shortfall`; `T0` = the lowest lineage-minimum of the weakest verified non-seed positive; `shortfall` = the largest leave-one-lineage-out miss of the plain minimum rule among the calibration lineages. **Low tail:** nothing is trimmed - every verified positive of every calibration image enters the minimum; an image whose normalisation fails is skipped (it falls back to broad AOT and needs no threshold).
  The plain minimum rule without the shortfall is computed and reported as a diagnostic, never as the policy.
- **Nested isolation** (`calibrate_fold`, `LeakageError`): for held-out lineage H, the calibration records of every lineage C != H come from the stored SEG-085 inner refit `H.C`, a model A trained on neither H nor C. H is scored by its own leave-one-lineage-out model and judged at `tau_H`. A record from a model that trained on H or on its own lineage, a record of H itself, or a missing isolation record raises. The inner refits are reused (unchanged configuration): two spot re-fits (held D09 / calibration D05 and held D12 / calibration D01)
  reproduce the stored scores **bitwise** (85 CPU-s); the RAW control reproduces the SEG-085 isolated `DYN_SCORE` threshold of all eleven folds (gate inside `run_nested`); the stored out-of-fold scores are the SEG-076/084 model A (bitwise-gated in SEG-085).
- **Score compatibility check** (reported, not assumed): the weakest-positive score of the same image under the 9-lineage inner model and the 10-lineage out-of-fold model differs by a median 0.2-0.9 logits and up to 5-7 logits for D01/D03/D07 in raw space; N2 (a pure location shift) does not reduce these differences; so the check can show the normalisation corrects image-level plateau location but not model-to-model changes of the extreme order statistic.
- **Compute:** one final-model refit (C1, 60 CPU-s), two spot refits (85 CPU-s), everything else reuses stored scores: about 3 CPU-minutes against an 8 CPU-hour ceiling. Economics ceiling 12 CPU-hours (unused unless the gate passes).
- **Populations:** primary = historical commercial-era Genesis (D01-D04); robustness = modern SGDK (D05-D12); both reported separately; every known miss on any image is disclosed regardless of population. All-code images (density above the cap), images below 64 cells and images without a seed universe are counted but are not in the gate domain.

### Success and failure definitions (frozen constants: `GATES` in the plan)

Evaluated on the nested development rows of the chosen normaliser over the domain above:

- **R (recall):** zero missed verified instruction starts on every non-fallback image. Zero-miss means zero missed *verified* starts on the evaluated data, not a completeness guarantee.
- **A (abstention):** fallback (cap abstention or normalisation failure) on at most 25 % of domain images and at most 25 % of domain cells.
- **S (selectivity):** lineage-balanced mean retained fraction and cell-weighted retained fraction both at most 0.40 (at least 0.10 below B50's fixed 0.50; fallbacks count as 1.0).
- **D (dynamics, not a disguised fixed budget):** at least 5 selected images; max-min of retained fractions at least 0.15; standard deviation at least 0.05; at least half of the selected images retain a fraction at least 0.05 away from both 0.35 (B35) and 0.50 (B50).
- **E (economics, only if R, A, S, D hold):** measured total build CPU including inference and normalisation overhead at least 10 % below A+B50 and at most 5 % above the deployed incremental policy, with equal runtime digests; the SEG-074/076 exact-code benchmark with unchanged settings, bounded to 12 CPU-hours, stopping early on any recall failure.

Decision ladder (first match): `EVIDENCE_INSUFFICIENT` (isolation or determinism not established, or fewer than 8 evaluable lineages) -> `RANKING_LIMITATION` (even the per-image oracle normalised threshold exceeds the cap on more than 20 % of domain images) -> `RECALL_FAILED` (any missed verified start on a non-fallback image) ->
`CALIBRATION_INEFFECTIVE` (A, S or D fails; universal abstention is **not** a recall pass) -> `SCORE_CALIBRATION_FEASIBLE` only after E also holds. A development-only `SCORE_CALIBRATION_FEASIBLE` is never independent generalisation and authorises nothing; it would recommend a separate confirmation milestone on genuinely new historical programs.

### Pre-freeze disclosure

An exploratory nested run of exactly this code on the development lineages preceded the freeze (it was used to debug the pipeline and the reproduction gate). It showed that the shortfall margin drives the chosen normaliser to universal cap abstention, and that the plain minimum rule has no missed start. The margin rule is the SEG-085 precedent that the brief says to reuse and was not changed
afterwards; the plain minimum rule is kept as a clearly labelled diagnostic and cannot determine the decision. No H01 label, score or economic figure was consulted.

### H01

After the plan and the development results are committed, the previously exposed Phantasy Star IV image (H01) may be scored under the frozen policy through `segarecomp_sample_roles.EvaluationContext` (frozen-manifest membership, reserved identities first) and reported as `REUSED_HISTORICAL_DIAGNOSTIC - NOT INDEPENDENT CONFIRMATION`. It does not count toward any minimum and cannot change the decision.
