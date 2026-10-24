# ADR 0191: SEG-085-T001 - error taxonomy by historically relevant software category and SEG-072 ADAPT reproduction (checkpoint C1)

- Status: Accepted (SEG-085-T001, checkpoint C1). Follows ADR 0190. No model was fitted, tuned or retrained: every score is a stored SEG-084 `A0` leave-one-lineage-out score (bitwise the SEG-076 structural model A) or the stored SEG-076 all-Genesis transfer score; the ADAPT
  reproduction re-evaluates the stored SEG-072 scores with the unchanged `segarecomp_ml_v3` policy code.
- Machine-readable aggregates (counts and ratios only; no addresses, bytes or label maps): `tools/segarecomp_seg085_diag.taxonomy.json`, `tools/segarecomp_seg085_diag.adapt.json`. Reproduce: `tools/segarecomp_seg085_diag.py taxonomy|adapt-repro` (private cache inputs).

## 1. How representative the development corpus actually was

Origin and toolchain were taken from the qualification records (ADR 0152, 0159, 0161), not from titles:

| category | lineages | images | share of positive cells | lineage mean of max ER |
|---|---|---|---|---|
| commercial-era Genesis (community reconstruction, build-identical) | 4 (D01 Sonic engine family, D02 Flicky, D03 Shining Force I/II, D04 Phantasy Star II) | 43 (3 full D01 games + 36 Sonic CD modules + 4 full games) | 86.5 % | **0.046** |
| modern SGDK/GCC homebrew | 7 (D05, D06, D07, D09, D10, D11, D12) | 7 | 11.5 % | **0.139** |
| cross-platform, period tooling (BattleIsleEditor: author's 1991 source, A68K 2.71) | 1 | 2 | 0.7 % | 0.0 (all-positive images) |
| cross-platform, modern authoring or toolchain (EmuTOS GCC 4.6, amiga-realtime3d vasm, toslibc GCC 13) | 3 (HelloAmi, a diagnostic, excluded) | 3 | 1.2 % | 0.05 |

SEG-084's headline mean ER 0.1052 is the mean of those eleven per-lineage maxima: four commercial-era lineages averaging 0.046 and seven SGDK lineages averaging 0.139. By lineage count the corpus is 64 % SGDK; by positive cells it is 87 % commercial-era;
and by independent *full commercial ROMs* it is thin: seven (s1, s2, sk, Flicky, Shining Force I, II, Phantasy Star II), two of which (s1/s2/sk) are one engine family and two (sf1/sf2) another. The 36 Sonic CD modules are one lineage, not 36 samples.

## 2. Why SGDK lineages dominate the failure statistics

- **D07 (240p Test Suite) owns 97.2 % of the Genesis B35 missed starts** (7,837 of 8,060) and every other SGDK lineage and every full commercial ROM has zero B35 and zero B50 misses. The remaining 223 B35 misses are three small Sonic CD *modules* (57-218 cells, 33-44 % code).
- The cause is **density, not ranking**. D07's labelled code density is 0.41 (oracle fraction 0.336); the next densest SGDK image is 0.18. Counting its 407 masked (SDK/runtime) cells as code, the upper bound is 0.524. B35 retains 0.3506 and **admits 239 of D07's 407 masked cells**: the SDK code the labels
  deliberately leave unknown consumes the budget that the labelled application code needs. At B50 D07 has zero misses. A fixed 35 % budget cannot hold a 0.34-0.52 code image for any ranker; this is budget infeasibility, an evaluation artefact of fixed percentages on dense images.
- Mechanism of D07's 199 missed cells: 198 lie within two cells of a dynamic jump/call (compiler-generated dispatch), 187 are unreached by direct flow from the seeds, all are adjacent to retained code. These are real structural phenomena, but the dominant factor is the dense, SDK-heavy image.
- Relevance to historical commercial Genesis: **low for the D07 count itself, moderate for the lesson.** Full commercial ROMs have density 0.09-0.25 and are ranked almost perfectly (lineage max ER 0.0002-0.153; D01's 0.153 is s2). Dense images do exist in the period set (the three Sonic CD modules), and the cross-platform images are denser
  still, so a fixed percentage is the wrong abstraction at both ends: wasteful for sparse games, infeasible for dense modules. That motivates a density-aware policy without making D07 the objective.

## 3. Error taxonomy (aggregate)

- **Mixed code/data cells.** Commercial-era positives that mix labelled data: low (a few cells); the budget-independent tail (positive cells ranked beyond the first quarter of the image) is **empty for every commercial-era image except `sk`** (17 cells, all mixed with data, 14 short runs, 16 boundary-adjacent, all unreached
  by direct flow: small code islands inside data), and D07 (346 cells, mostly dispatch-adjacent). Short isolated code islands inside data are therefore the period-relevant residual failure family.
- **Seed and structural reach.** Direct-flow reach from the production seeds covers only part of the labelled starts: unreached share 0.70-0.93 for the Shining Force / Phantasy Star ROMs, 0.45-0.60 for the Sonic CD modules, 0.90 for D07. Reach does not separate D07 from commercial games; model A does not rely on it alone.
- **Jump tables and indirect branches.** Dynamic jump/call proximity is common in positives of every category (`positive_cells_near_dynamic_control`: sf1 432 of 1,310 cells, D07 594 of 667), so near-dynamic-control is a weak discriminator by itself; it is reported per image.
- **Label reliability (no model involved).** Of 102,021 direct control-flow targets of labelled commercial-era starts, 102,008 land on a labelled start; 12 land on labelled data and 1 inside a labelled instruction (contradiction rate 1.3e-4: sf1 1, sf2 3, six Sonic CD modules 9). SGDK 9,269 targets: 0 contradictions, 1,500 on unknown (SDK) bytes.
  The commercial-era labels are very consistent; the 13 contradicted targets are candidate label defects and are listed per image for the T002 corroboration design, not repaired here.
- **Score calibration across styles.** Raw model-A logits are not comparable in the tail: the lowest non-seed positive score is -10.3 (D01), -7.0 (D12), -6.4 (D03) against +4.5 to +10.4 for D05, D06, D09, D10, D11. A single score threshold that holds every positive of every lineage must sit near the worst lineage's minimum, which sits among the negatives of the others.
- **Cross-platform transfer (all-Genesis model A, not out-of-fold).** B35 and B50 both miss on tiny, code-dense images (BattleIsleEditor's main executable density 0.62, toslibc libraries 0.83-0.96): oracle fractions exceed 0.5, so B50 is infeasible by construction there (937 and 2,055 missed starts). These are not instances of the commercial-Genesis problem but are reported.
- **B35 versus B50.** Across all 55 images B50 has zero misses wherever it is oracle-feasible and misses exactly on the oracle-infeasible cross-platform images; B35 misses only on dense images (D07, three Sonic CD modules, cross-platform).

## 4. Historical ADAPT reproduction

The SEG-072 development results reproduce exactly from the stored scores: M3-R + ADAPT mean retained 0.5847, 32 missed starts (all D03); frozen M3 + ADAPT 0.9467, 2 missed; M1-HGB + ADAPT 0.9485, 1 missed; B50 0 / 2 / 1 misses at 0.5048 retained. Fitted margins: M3-R 0.301 (0.244 when D03 is held out), frozen M3 0.826, M1 0.665.

Mechanism of the historical behaviour: the margin is `max over fitting images (residual_frf - density estimate) + 0.05`, a single additive constant set by the one worst fitting image. (i) With a worst image the margin is large, so ADAPT retains `estimate + margin` everywhere (M3-R) or abstains to 100 % (frozen M3, M1: 15-16 of 50 images abstain, whole
lineages retained at 1.0). (ii) The estimate came from different, balanced-class-weight classifiers. (iii) The nested fit used out-of-fold records produced by models that had trained on the held-out lineage (the disclosed SEG-072 limitation), and D03's 32 misses occur precisely when D03 was the lineage that set the margin.
The same `adapt_density_estimate` computed from model A's balanced logits tracks labelled density closely on commercial-era images (s1 0.2002 vs 0.2002, Phantasy Star II 0.0923 vs 0.0920) but over-estimates on SGDK images that contain unlabelled SDK code (MegaPong 0.267 vs 0.051).
Whether the unchanged ADAPT rule suits model A is T003's question.

## 5. Consequences for the milestone

D07's miss count is a density/SDK-masking artefact of fixed percentage budgets, not an acquisition priority. The period-relevant failure families to watch in T004 are dense small modules, short code islands in data, and the score-tail non-comparability between lineages. No model was changed.
