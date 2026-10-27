# ADR 0203: SEG-093 - commercial-era corpus ablation and M68K model generalization audit: `EVIDENCE_INSUFFICIENT`

- Status: Accepted (research, SEG-093 T001-T004: C1 `CORPUS_PROTOCOL_VALID`, C2 `A_MIXED_EFFECT`, C3 `MIXED_OR_SEED_DEPENDENT`, final `EVIDENCE_INSUFFICIENT`). Follows ADRs 0100, 0151-0155, 0162, 0165-0170, 0188-0189, 0202. No ADR is amended.
  **No product change, no model promotion, no default change.** M3+B35 (`--immutable-rom-aot-ml-gen2-admission`) is unchanged and remains the incumbent; BROAD remains the fail-closed fallback. B and C-v2 were not trained.
- Privacy: aggregate only (counts, rates, digests). ROMs, TAS streams, per-address truth, labels, model weights and per-cell scores stay in the private harness. Pre-registration, tooling and per-image results are harness-private.
- Scope: development data only (reused for evaluation); nothing here is independent evidence.

## Question
Did adding modern SGDK homebrew (and, separately, Sonic CD modules) to M68K code-recognition training degrade generalization or B35 ranking efficiency on commercial-era Genesis games, or did seed instability, exposure or incomparable evaluation explain the earlier M3 / M3-R difference?

## Method (frozen before any comparative fit)
Arms H0 = seven commercial games, H1 = +six TAS games (strong tier), H2 = +seven SGDK projects, H3 = +TAS+SGDK, H4 = H3 + 36 Sonic CD modules, R0 = commercial+SGDK+Sonic CD. Strict leave-one-lineage-out over four independent commercial lineages (seven images, full source-backed truth);
SEG-075/076 metric code unchanged (ER, FRF, verified starts missed at B35, B45/B50 diagnostics) plus the production prune/closure/validator on every B35 selection. M3: one matched-exposure wrapper (original architecture; 10,000 updates; all-unknown windows excluded; seeds 49-52), 168 unique fits.
Model A: `a_features` + `A_CONFIG`, deterministic, with subsample noise refits. Corpus audit: 56 images identity-verified, no cross-corpus containment above 0.17 (rule 0.30), historical M3-R, model-A and frozen-M3 results reproduced bitwise.

## Results
1. **M3 training-seed instability dominates.** The same recipe and data give held-out ER from 0.04 to 0.98 across seeds (e.g. commercial-only on the Shining Force lineage: 0.037 / 0.152 / 0.975 / 0.210; the worst run misses 182 verified starts at B35). Macro ER (mean +/- SD, 4 seeds): H0 0.134 +/- 0.097, H1 0.038 +/- 0.023, H2 0.229 +/- 0.197, H3 0.103 +/- 0.104, H4 0.046 +/- 0.018, R0 0.114 +/- 0.107.
2. **No M3 contrast is resolved** against seed noise: SGDK without TAS +0.096 [-0.129, +0.321]; SGDK with TAS +0.066 [-0.106, +0.238]; TAS without/with SGDK -0.096 / -0.126; Sonic CD -0.116 / -0.058 (all unresolved; materiality margin 0.02 ER). All SGDK point estimates (including the exposure-controlled and half-budget diagnostics) lie in the harmful direction; this is a hint, not a finding, and SGDK is not cleared.
3. **Model A**: B35 recall is saturated (0 of 310,567 verified starts missed in every arm; validator accepts everything). ER shows no SGDK harm without TAS (one lineage markedly better) and a mixed result with TAS (one lineage better, one worse): `A_MIXED_EFFECT`.
4. **Production path**: validator rejections or admitted sets missing verified starts occurred in 3, 0, 6, 3, 0, 4 of 28 M3 image-runs for H0, H1, H2, H3, H4, R0 (0 of 42 for model A). The two arms with no collapses or failures contain TAS (H1, H4); H3 (TAS plus SGDK) does not share that; unresolved. The admission stage was added after the freeze and is descriptive.
5. **Previous M3-R regression**: its Shining Force ER is not distinguishable from the matched R0 seed spread (its B35 miss count is higher than every matched R0 run), and the frozen-M3 comparison mixed in-sample (five games) with held-out evaluation. Seed instability and evaluation differences are therefore not separable from a real regression.

## Decision
Final class `EVIDENCE_INSUFFICIENT` (frozen rule: M3 contrasts unresolved against seed noise). Not supported: SGDK harm, SGDK benefit, or "no effect". Not established: commercial-only or commercial+TAS as a better foundation (no BENEFIT-classified candidate, so the economics step was not triggered).
M3+B35 is retained. No product task is proposed. A separately approved research follow-up is the justified next step: many more seeds (at least 16) or seed-robust training/selection for H0-H3 and a TAS-inclusive arm, before any targeted B/C-v2 comparison.

## Limits and non-claims
Four independent lineages (two single-image and near-saturated for A); intervals describe training-seed variation only; mean endpoints are outlier-sensitive and no robust endpoint was frozen; the wrapper (fixed update budget, labelled-cell loss normalisation) differs from the SEG-072 recipe so absolute values do not compare with archived M3-R;
exploratory counts (collapses, sign counts) are post hoc. Operator addendum: the agent's CPU ceiling was withdrawn; observed CPU 81.9 CPU-h (M3 farm), 3.6 (historical reproduction), 0.8 (model A). No completeness claim; development images only.
