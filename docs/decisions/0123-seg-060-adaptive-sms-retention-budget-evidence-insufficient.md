# ADR 0123: SEG-060 adaptive SMS retention budgets — density estimation fails the exploratory screen; score-based hypothesis untested; fixed 65% retained

- Status: Accepted. **Classification: `SMS_ADAPTIVE_EVIDENCE_INSUFFICIENT`.** The current evidence does not justify adopting adaptive budgets; it does **not** show that adaptive budgeting is impossible. What is established: density-based estimation (even with oracle, label-derived density) fails the exploratory leave-one-lineage-out screen. What is not established: whether CNN score-tail, score-gap, distribution-knee or ensemble-agreement features predict the required retention. That original score-based hypothesis is **untested**, because the out-of-fold logits needed to test it were unavailable. Decision: no adaptive policy; keep fixed 0.65. Research stopped at Checkpoint A; no estimator, corpus, native code or policy change was produced. The SEG-056 (ADR 0117) and SEG-057 (ADR 0118) classifications are preserved unchanged, as is the SEG-057 SMS Optimized mode. (An earlier revision of this ADR classified the outcome `SMS_ADAPTIVE_SIGNAL_ABSENT`; review found that overclaimed, and it was corrected without changing any result.)
- Related: ADR 0117 (D512HP ensemble), 0118 (selective admission, fixed 0.65), 0119 (native producer).

## Question

Can the frozen D512HP outputs predict a conservative upper bound on the retention needed for complete source-backed instruction-start recall (*worst-miss estimation*, not ranking and not density estimation) so that a per-ROM budget can replace the fixed 0.65?

## Method (Checkpoint A, cheapest falsification first)

1. **Inventory.** The raw out-of-fold byte logits, development images, truth and per-seed rankings of SEG-056 were ignored private research outputs and are **not preserved** on any current machine (no NumPy/PyTorch either). Only the committed per-lineage summaries in `tools/segarecomp_ml_region_z80_gen2.dev_results.json` exist. The in-sample weights were deliberately not used as a stand-in. Reconstruction would need about 24 D512HP fits plus rebuilding 8 lineages' source-backed truth (four from privately held cartridge images); its expected value was assessed below before any such effort.
2. **Oracle bound.** The committed folds contain the **label-derived** true code-cell fraction and the recall-versus-retention curve for the shipped candidate (`D512HP/cell32/top2`, out-of-fold). They are label-derived and unobservable at inference time. They bound what *density-type* and *recall-curve-type* estimators could achieve even if they were estimated perfectly; they do not cover other score statistics (tail shape, entropy, knee, seed agreement), which this screen never saw. The screen is exploratory (n=8, four hand-chosen rule forms) and tests whether such quantities pass a leave-one-lineage-out (LOLO) rule. `tools/segarecomp_sms_adaptive_budget_screen.py` (stdlib, deterministic, tested by `segarecomp_sms_adaptive_budget_screen_test`) computes the table, at most four fixed rules, and a scatter (`docs/testing/sms-adaptive-budget-oracle-scatter.svg`). Every rule's parameter is taken from the other seven lineages only; the cap is 0.65 (abstain = 0.65); no thresholds were tuned. **Scope of inference:** failure of an oracle-density rule shows only that density (alone, with max-ratio calibration) is insufficient. It does not mathematically rule out estimators built from other score-derived features: those are not a subset of, or bounded by, the density quantity, and nothing here evaluated them.

## Result

| lineage | cells | code fraction (oracle) | r90 | r99 | full recall | full/code | seed pstdev |
|---|---|---|---|---|---|---|---|
| columns | 4096 | 0.105 | 0.125 | 0.146 | 0.159 | 1.51 | 0.004 |
| duckslayer | 1024 | 0.139 | 0.284 | 0.305 | 0.315 | 2.27 | 0.037 |
| g2048 | 1024 | 0.086 | 0.078 | 0.110 | 0.110 | 1.29 | 0.044 |
| kots | 4096 | 0.181 | 0.177 | 0.196 | 0.202 | 1.11 | 0.015 |
| ps1 | 16384 | 0.059 | 0.096 | 0.126 | 0.153 | 2.60 | 0.063 |
| solomon | 4096 | 0.162 | 0.185 | 0.444 | 0.513 | 3.17 | 0.034 |
| stalactites | 1024 | 0.281 | 0.320 | 0.357 | 0.362 | 1.29 | 0.011 |
| supertennis | 1024 | 0.419 | 0.379 | 0.462 | 0.592 | 1.41 | 0.168 |

| LOLO rule (oracle input) | mean budget | gain vs 0.65 | smaller by >=10 pts | misses |
|---|---|---|---|---|
| density_x_max_ratio (positive_fraction) | 0.441 | +0.209 | 5 | solomon |
| density_x_max_ratio_x1.2 (positive_fraction) | 0.492 | +0.158 | 5 | solomon |
| r99_x_max_ratio_x1.1 (retained_for_recall_0.99) | 0.374 | +0.276 | 6 | none |
| constant_max_of_others (none) | 0.582 | +0.068 | 1 | supertennis |

(`r90`/`r99`: retention needed for 90%/99% instruction-start recall; `full`: retention for complete labelled recall including roots; all out-of-fold, label-derived.)

- **Density does not determine the requirement.** Required retention is 1.1–3.2 times the true code fraction. Solomon (3.17x) exceeds every other lineage (next 2.60x), so any density rule calibrated on the others underestimates it: with *perfect* density knowledge the LOLO rule misses Solomon at 1.0x (budget 0.421) and at 1.2x (0.505) against 0.513 needed. A margin large enough to cover it would abstain on most lineages.
- **The hard cases are not distinguishable by coarse measures.** Solomon and Knights of the Square have nearly the same code fraction (0.16 / 0.18) and the same r90 (0.185 / 0.177) yet need 0.51 and 0.20 for complete recall; the difference lives only in the last 1% of positives (r99 0.44 vs 0.20). That is exactly the worst-miss quantity, i.e. where the model is *wrong*.
- **The only rule without a miss used the label-derived r99** (which no inference-time feature supplies) and passed with narrow slack (e.g. Solomon 0.63 vs 0.513). Without any feature (constant = largest other lineage) Super Tennis is missed (gain 0.068 at that setting).
- **Seed disagreement** (per-seed worst-rank spread) flags Super Tennis (0.168) but not Solomon (0.034, lower than easy 2048-SMS 0.044 and Phantasy Star 0.063); it is also computed from truth, so it is not an inference feature either.
- **Fixed 0.65 headroom is itself thin:** Super Tennis needs 0.592 (6 points), and individual seeds of the two hardest lineages exceed 0.65 (Solomon 0.657, Super Tennis 0.899 and 0.654). Anything below it for a hard ROM would fail; an estimator can only save retention on easy ROMs, by recognizing them from scores alone.

Untested conjecture (reasoning, not a measurement, and not evidence of absence): worst-miss cells are model errors, i.e. positives the network ranks as non-code, which gives a reason for skepticism that score-distribution statistics locate the worst misranked positive. The opposite is equally unmeasured: tail shape, score gaps, knees or seed agreement could still correlate with it. Only a test on preserved out-of-fold logits can decide.

## Screen against the registered criteria

Against the screening criteria and the evidence actually examined: full recall on all eight under predicted budgets is not met by the density rules and is met only by the label-derived r99 rule; the mean gain, materially-smaller-budget and hard-lineage criteria are met only by label-derived inputs or fail on Solomon. No score-derived feature was evaluated against any criterion. Decision: do not adopt adaptive budgets on this evidence; stop the implementation path; no new games were acquired and no adaptive policy introduced.

## Limits of this conclusion

- This shows that density is insufficient and that the only passing screened quantity is a label-derived rank statistic. The Solomon failure rests on one outlier ratio (3.17x) in n=8, so it is exploratory evidence, not general falsification.
- **The primary proposed predictors (score tail, score gaps, distribution knee, ensemble agreement) were not tested**, because the raw out-of-fold byte logits are not preserved (established on this machine only). Their expected value is therefore unknown, not low. The conclusion is that the current evidence does not justify adaptive budgets, not that they are disproven.
- The benefit ceiling is bounded (SEG-057 already saves 29–37% of native build time at 0.65, ADR 0118/0119), and any positive would still need a frozen registration and at least three new independent lineages before adoption.
- Eight lineages (two sharing a compiler) are an exploratory sample; no statistical reliability is claimed in either direction.

## Answers

1. Unknown: not established from the available evidence (density fails the screen; score features untested). 2. None established; true density and coarse recall points carry partial information but fail LOLO; only a label-derived 99%-recall point succeeds. 3. Not established (Solomon and Knights are indistinguishable by density and r90; score features untested). 4. Nothing realizable (0 points; label-oracle-only: 16–28 points, but each such rule either misses Solomon or needs the label-derived r99). 5. No change: fixed 0.65 stays (SEG-057 measured 29–37% saving). 6. Not applicable (nothing deployed). 7. Not justified on current evidence. 8. Not adopted and not disproven; fixed 0.65 SMS Optimized, Compatibility default and the typed fallback contract are unchanged.

## Prerequisites and estimated cost of a future independent score-feature experiment

Not started and not authorized by this ADR; it would be a new protocol, registered before any score is examined.

1. **Out-of-fold logits (required).** Recover the original private `logit_D512HP_<held>_s<seed>.npy` arrays if they still exist elsewhere, or reconstruct them deterministically: `tools/segarecomp_ml_region_z80_gen2_train.py fit-fold --held <lineage> --models D512HP` for each of the eight development lineages (8 folds x seeds 1, 2, 3 = 24 fits; the other seven lineages train each fold; `STEPS = 3000`, `BATCH = 8`, CPU, deterministic algorithms). Environment: PyTorch 2.14 CPU and NumPy matching ADR 0117 (the weight export reproduced the original ensemble digest exactly with PyTorch 2.14.1+cpu and 8 threads, which is evidence, not a guarantee, that the fold fits are reproducible). Reproduction must be checked against the committed per-fold results in `dev_results.json` (residual full-recall retention per seed) before any feature is computed; a mismatch means the logits are not the original ones.
2. **Inputs for those fits.** The eight development reference images with source-backed truth: four are source-built or SDCC-rebuilt (`2048-SMS`, `Knights of the Square`, `Duckslayer`, `Stalactites`, `tools/segarecomp_ml_region_z80_sdcc_build.sh`, `..._sdcc_truth.py`), `ps1` and `solomon` are source-built from upstream disassemblies, and `columns` and `supertennis` require byte-identical privately held cartridge images. Every image must match the SHA-256 in the frozen plan. Images, truth and logits stay in ignored private storage, never in a commit.
3. **Estimated cost (rough, partly unmeasured).** Compute: ADR 0117 measured about 33 ms per 2-thread CPU training step for a network of this size class, i.e. very roughly 100 s per fit at 3000 steps and on the order of an hour of CPU time for 24 fits, plus loading; this per-fit figure was not measured for D512HP here and should be measured first. The dominant cost is operator effort: re-establishing the truth tooling and images (item 2) and re-verifying hashes, probably much larger than the compute.
4. **Then, in order (each a gate):** freeze the feature definitions and decision rule (at most two or three simple features, LOLO or nested validation, abstention to 0.65, plan/code digests); evaluate on the preserved logits, including the Solomon-versus-Knights and Super Tennis cases; only if that passes, register a new independent corpus of at least three new lineages (not revisions or relatives of the existing ones, and not SEG-056's spent sealed games), then economics and gameplay-coverage evidence, then conditional native integration. A future ranking experiment should persist its out-of-fold logits so this recovery is never needed again.
