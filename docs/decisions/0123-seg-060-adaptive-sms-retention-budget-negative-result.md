# ADR 0123: SEG-060 adaptive SMS retention budgets — premise not supported; fixed 65% retained

- Status: Accepted. **Classification: `SMS_ADAPTIVE_SIGNAL_ABSENT`** (scoped, see "Limits of this conclusion"). Research stopped at Checkpoint A; no estimator, corpus, native code or policy change was produced. The SEG-056 (ADR 0117) and SEG-057 (ADR 0118) classifications are preserved unchanged, as is the SEG-057 SMS Optimized mode.
- Related: ADR 0117 (D512HP ensemble), 0118 (selective admission, fixed 0.65), 0119 (native producer).

## Question

Can the frozen D512HP outputs predict a conservative upper bound on the retention needed for complete source-backed instruction-start recall (*worst-miss estimation*, not ranking and not density estimation) so that a per-ROM budget can replace the fixed 0.65?

## Method (Checkpoint A, cheapest falsification first)

1. **Inventory.** The raw out-of-fold byte logits, development images, truth and per-seed rankings of SEG-056 were ignored private research outputs and are **not preserved** on any current machine (no NumPy/PyTorch either). Only the committed per-lineage summaries in `tools/segarecomp_ml_region_z80_gen2.dev_results.json` exist. The in-sample weights were deliberately not used as a stand-in. Reconstruction would need about 24 D512HP fits plus rebuilding 8 lineages' source-backed truth (four from privately held cartridge images); its expected value was assessed below before any such effort.
2. **Oracle bound.** The committed folds contain the **label-derived** true code-cell fraction and the recall-versus-retention curve for the shipped candidate (`D512HP/cell32/top2`, out-of-fold). These are strictly *more* informative than any score statistic an inference-time estimator could compute. If even these fail a leave-one-lineage-out (LOLO) rule, a score-derived rule cannot pass. `tools/segarecomp_sms_adaptive_budget_screen.py` (stdlib, deterministic, tested by `segarecomp_sms_adaptive_budget_screen_test`) computes the table, at most four fixed rules, and a scatter (`docs/testing/sms-adaptive-budget-oracle-scatter.svg`). Every rule's parameter is taken from the other seven lineages only; the cap is 0.65 (abstain = 0.65); no thresholds were tuned.

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

- **Density does not determine the requirement.** Required retention is 1.1–3.2 times the true code fraction. Solomon (3.17x) exceeds every other lineage (next 2.60x), so any density rule calibrated on the others underestimates it: with *perfect* density knowledge the LOLO rule misses Solomon at 1.0x and 1.2x margins (0.51 vs 0.513 needed). A margin large enough to cover it would abstain on most lineages.
- **The hard cases are not distinguishable by coarse measures.** Solomon and Knights of the Square have nearly the same code fraction (0.16 / 0.18) and the same r90 (0.185 / 0.177) yet need 0.51 and 0.20 for complete recall; the difference lives only in the last 1% of positives (r99 0.44 vs 0.20). That is exactly the worst-miss quantity, i.e. where the model is *wrong*.
- **The only rule without a miss used the label-derived r99** (which no inference-time feature supplies) and passed with narrow slack (e.g. Solomon 0.63 vs 0.513). Without any feature (constant = largest other lineage) Super Tennis is missed, and with a 5-point margin the gain over 0.65 is under 2 points.
- **Seed disagreement** (per-seed worst-rank spread) flags Super Tennis (0.168) but not Solomon (0.034, lower than easy 2048-SMS 0.044 and Phantasy Star 0.063); it is also computed from truth, so it is not an inference feature either.
- **Fixed 0.65 headroom is itself thin:** Super Tennis needs 0.592 (6 points), and individual seeds of the two hardest lineages exceed 0.65 (Solomon 0.657, Super Tennis 0.899 and 0.654). Anything below it for a hard ROM would fail; an estimator can only save retention on easy ROMs, by recognizing them from scores alone.

Mechanistic argument (reasoning, not a measurement): worst-miss cells are model errors, i.e. positives the network ranks as non-code; scores that are confidently low in those cells contain no information that those cells are code, so a statistic of the score distribution has no clear route to the position of the worst misranked positive.

## Screen against the registered criteria

Full recall on all eight under predicted budgets: **not met** by any density rule; met only by a label-derived oracle. Mean gain >= 10 points: only under label-derived inputs. Three materially smaller budgets: met only by those. Hard lineages not systematically underestimated: **not met** (Solomon). No leakage / not a flexible fit: the passing configuration is leakage by construction. Decision: **stop the implementation path**; no new games were acquired and no adaptive policy introduced.

## Limits of this conclusion

- This falsifies the *density* route and shows that the only workable LOLO quantity is label-derived. It does **not** test score-tail/knee/seed-agreement features on raw out-of-fold logits, because those artifacts no longer exist; their expected value is judged low (previous bullet), the benefit ceiling is bounded (SEG-057 already saves 29–37% at 0.65), and any positive would still require frozen registration and at least three new independent lineages. The closed classification is therefore `SMS_ADAPTIVE_SIGNAL_ABSENT` for the evidence available, not a proof that no such estimator can exist.
- Eight lineages (two sharing a compiler) are an exploratory sample; no statistical reliability is claimed in either direction.
- Reopening requires a new protocol with preserved out-of-fold logits (a future ranking experiment should persist them) and new independent evidence.

## Answers

1. Not predictable from the available evidence. 2. None established; true density and coarse recall points carry partial information but fail LOLO; only a label-derived 99%-recall point succeeds. 3. No (Solomon vs Knights). 4. Nothing realizable (0 points; oracle-only: 16–28 points, but each such rule either misses Solomon or needs the label-derived r99). 5. No change: fixed 0.65 stays (SEG-057 measured 29–37% saving). 6. n/a (nothing deployed). 7. Not justified: a heuristic whose predictive reliability cannot be established must not be maintained. 8. Adaptive selection **rejected**; fixed 0.65 SMS Optimized, Compatibility default and the typed fallback contract are unchanged.
