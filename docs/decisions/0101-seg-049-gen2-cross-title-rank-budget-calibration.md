# ADR 0101: SEG-049 gen2 cross-title rank-budget calibration

- Status: experiment complete; **classification `GEN2_CALIBRATION_READY_FOR_NATIVE_PORT`** (see section 12 for the review result and the limits of this claim).
- Predecessors: ADR 0100 (SEG-049-T001: M3 selected, `GEN2_REPRESENTATION_VALID_CALIBRATION_UNSOLVED`), ADR 0096 (SEG-047 native v1), ADR 0099 (SEG-048).
- Scope statement: **SEG-049-T002 DOES NOT CHANGE PRODUCTION OPTIMIZED AOT.** Compatibility stays broad AOT; Optimized stays the SEG-047 native v1 model; the v1
  artifacts are byte-identical (enforced by test, line-ending independent). Everything here is offline research tooling plus frozen *research* artifacts.
- Evidence statement: **the five titles are not blind** (T001 exposed their FRF). This establishes a frozen calibration candidate on a known source-backed
  corpus, **not** fresh blind generalization proof. `Optimized` must not default to this candidate on this evidence alone; SEG-049's later broader-evidence task must
  test the frozen candidate on additional authorized titles.

## 1. Post-#91 baseline

| repository | branch | SHA at synchronization |
| --- | --- | --- |
| segarecomp (product) | main | `ad61ec58865ef8ec15fc93cdb36df169d5c307e8` (PR #91 squash merge, all CI shards green) |
| segarecomp-harness | main | `2a3f9538ef8721440ae7de1a0986cac5593c4a30` |

T002 is product branch `task/seg-049-t002` (PR #93), cut from post-#91 `main`. The T002 plan was committed in `fb3e829` *before any budget was evaluated against labels*.

## 2. Why probability calibration was abandoned

T001 evaluated absolute probability thresholds Q0-Q3 derived from the minimum positive scores of inner three-title models. Absolute score scales do not transfer
as the training corpus changes (inner three-title positive minima 1e-6..1e-8, outer four-title minima 1e-3..1e-2): the inherited thresholds were far too
permissive on Flicky and S&K (R/ROM 0.77-0.94) and a single Shining Force positive fell below Q0-Q2. The ranking, in contrast, transferred: AUROC >= 0.9998 on every
title, top 30 % of cells C outside = 0, FRF 0.112-0.269. M3 is therefore used as a **ranking model**; sigmoid outputs are not required to be calibrated
probabilities. No temperature/Platt/isotonic family was tried (that was only permitted if every rank policy failed).

## 3. Why rank calibration is justified

The selected region only has to contain every executable cell and stay small. A fixed fraction of the highest-ranked cells is invariant under any monotone
rescaling of the score (tested), needs no per-ROM tuning, depends on nothing but `(scores, number of cells, fixed budget)`, and degrades gracefully: the
margin to the weakest positive is directly the *rank headroom* below. The structural prune and the unchanged validator remain the correctness authority.

## 4. Frozen inputs (nothing was changed)

M3 exactly as in T001 (`tools/segarecomp_ml_region_gen2.plan.json`, plan digest `84ab1134...af9`): 128-byte cells, 1024-byte context, raw bytes + CPU-owned
MC68000 tokens, 77,441 parameters, AdamW, 30 epochs, seed 49, BCE with positive weighting, no hard-positive mining, representation SHA unchanged. The five-title corpus,
pinned ROM/source/truth identities and the five title-held-out folds are those of ADR 0100. The T001 outer score arrays were reused only after every prediction
digest matched the digest frozen in the T002 plan (all five matched). No new truth was generated; no address, per-cell label, score array or ROM byte is committed.

## 5. Pre-registered policy (`tools/segarecomp_ml_region_gen2_rank.plan.json`)

Exactly five budgets: **B30, B35, B40, B45, B50** (percent of the ROM's 128-byte cells). `N = ceil(cells * pct / 100)` in integer arithmetic (floating point would
turn `100 * 0.30` into 31). Ranking: score descending, then cell ordinal ascending - the ordinal is a deterministic tie-break only, never a model input (the
model input-channel guard rejects `ordinal`, `cell_id`, `address`, `title`, `truth`, `runtime_pc`, ... and is covered by tests). `R` = rank-selected cells UNION
certain-code seed cells. Gates, on all five outer title-held-out folds:

- `C outside R = 0`, `R/ROM <= 0.60` (preferred 0.50), `rank_headroom = (pct/100) / FRF >= 1.25`;
- then the unchanged structural prune + validator: validator accepted, `C outside K = 0`; `C outside R` and post-ML pruning losses attributed separately
  (`POST_ML_PRUNING_BLOCKER` would never be masked by retraining);
- choice: the **smallest** budget passing every gate; none -> `GEN2_CALIBRATION_UNSOLVED` and stop.

The selection function takes only `(scores, percent)`; labels, truth, title, ROM digest and runtime coverage have no entry point (signature and source checks in tests).

## 6. R-stage results (five outer folds)

FRF per title (outer held-out M3): Sonic 1 0.2109, Flicky 0.1768, Shining Force 0.2079, Phantasy Star II 0.1125, Sonic & Knuckles 0.2688 (worst 0.2688, mean 0.1954).

Rank headroom `(pct/100)/FRF`:

| title | B30 | B35 | B40 | B45 | B50 |
| --- | --- | --- | --- | --- | --- |
| Sonic 1 | 1.42 | 1.66 | 1.90 | 2.13 | 2.37 |
| Flicky | 1.70 | 1.98 | 2.26 | 2.55 | 2.83 |
| Shining Force | 1.44 | 1.68 | 1.92 | 2.16 | 2.40 |
| Phantasy Star II | 2.67 | 3.11 | 3.56 | 4.00 | 4.45 |
| Sonic & Knuckles | **1.12 (fails >= 1.25)** | 1.30 | 1.49 | 1.67 | 1.86 |

`C outside R = 0` and `C outside ML-only = 0` for every budget on every title. `R/ROM` = 0.300-0.301 (B30), 0.350-0.351 (B35), 0.400 (B40), 0.450 (B45),
0.500 (B50) - the certain-code seed cells were already inside the top-ranked cells on every title, so they added nothing. **B30 touches the weakest positive
(it also has C outside R = 0) but leaves only 12 % headroom on S&K, so it is rejected by the pre-registered rule; B35 is the smallest budget passing all R gates.**

## 7. K-stage results (unchanged prune + validator, R-passing budgets only)

B35, per outer title:

| title | cells / selected | R/ROM | validator | K / U | C outside R | C outside K | post-ML losses |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Sonic 1 | 4,096 / 1,434 | 0.350 | accepted | 87,478 / 246,293 = 0.355 | 0 | 0 | 0 |
| Flicky | 1,024 / 359 | 0.351 | accepted | 22,566 / 64,352 = 0.351 | 0 | 0 | 0 |
| Shining Force | 12,288 / 4,301 | 0.350 | accepted | 256,303 / 735,560 = 0.348 | 0 | 0 | 0 |
| Phantasy Star II | 6,144 / 2,151 | 0.350 | accepted | 122,923 / 367,930 = 0.334 | 0 | 0 | 0 |
| Sonic & Knuckles | 16,384 / 5,735 | 0.350 | accepted | 350,886 / 989,195 = 0.355 | 0 | 0 | 0 |

B40/B45/B50 also pass every gate on every title (K/U equals about the budget). **Selected policy: `B35`.** (Contrast with production v1 on S&K, ADR 0099: 237 truth
starts outside R, 6,383 outside K.)

## 8. Final full-corpus research freeze

One M3 fit on all five titles with the exact T001 recipe, run twice in independent processes: weights and in-sample predictions bitwise equal (prediction digest
`a25c8a37...8273`). Artifacts (new gen2 research names; production v1 untouched):

- `tools/segarecomp_ml_region_gen2.model.json` - weights, SHA-256 `11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1`;
- `tools/segarecomp_ml_region_gen2.frozen.json` - representation / token-schema identities, recipe, seed, corpus, model + rank-plan digests, policy `B35`;
- `tools/segarecomp_ml_region_gen2.parity.json` - stdlib reference evaluator vs PyTorch on three deterministic synthetic contexts (max |diff| 1.3e-6), reference-logit
  digest, in-sample aggregate digests. **This is a research candidate, not the production Optimized model.** No native inference exists yet.

## 9. Runtime falsifier (broad vs production v1 vs gen2 explicit plan; 600 no-render frames, explicit instruction budget, clang)

Source-backed titles use the **outer held-out** model + B35 plan; the four SEG-046 historical titles (no new truth: runtime-only evidence) use the final all-five model,
none of them being a training title. The plan is the validator-accepted prune of `R` and is passed via `--admission-plan`.

| title | broad terminal | gen2 == broad (outcome, frames, final state, frame stream, coverage) | broad-executed ROM PCs outside K | v1 == broad |
| --- | --- | --- | --- | --- |
| Flicky | guest_stop @26 | yes | 0 | yes |
| Sonic 1 | 600 frames | yes | 0 | yes |
| Phantasy Star II | 600 frames | yes | 0 | yes |
| Shining Force | 600 frames | yes | 0 | yes (v1 prune rejected -> identical broad program) |
| Sonic & Knuckles | guest_stop @9 (pre-existing device frontier) | **yes** | 0 | **no** (v1 fails closed earlier, ADR 0099) |
| Sonic 2 | 600 frames | yes | 0 | yes |
| Cool Spot | 600 frames | yes | 0 | yes |
| Streets of Rage | 600 frames | yes | 0 | yes |
| OutRun | 600 frames | yes | 0 | yes |

Executed-PC sets of gen2 and broad are identical on all nine titles. The S&K divergence of production v1 does not occur with gen2. A pre-existing hardware frontier
(Flicky, S&K) is reached equivalently by both.

## 10. Economics (single samples, one 16-core host; the bridge = generate + clang compile + 600-frame bounded run; CPU = user+sys of the process tree)

| title | generated C broad -> gen2 | bridge CPU reduction | bridge wall reduction | peak RSS reduction | v1 generated C reduction (for reference) |
| --- | --- | --- | --- | --- | --- |
| Flicky | -51.0 % | -48.7 % | -47.0 % | -20.8 % | -49.4 % |
| Sonic 1 | -60.7 % | -62.3 % | -61.5 % | -53.3 % | -73.9 % |
| Phantasy Star II | -54.8 % | -57.7 % | -57.2 % | -51.2 % | -59.5 % |
| Shining Force | -57.2 % | -48.8 % | -48.6 % | -53.4 % | 0.0 % (fallback) |
| Sonic & Knuckles | -58.9 % | -60.4 % | -59.3 % | -51.9 % | -63.0 % |
| Sonic 2 | -60.1 % | -62.7 % | -62.4 % | -52.8 % | -71.4 % |
| Cool Spot | -51.7 % | -51.5 % | -51.9 % | -49.9 % | -81.1 % |
| Streets of Rage | -56.7 % | -57.6 % | -56.9 % | -53.2 % | -70.4 % |
| OutRun | -53.6 % | -56.2 % | -56.4 % | -48.6 % | -73.8 % |

The gate (generated C >= -30 %, compile CPU >= -20 %, no material wall regression) holds on all nine titles. **Honest caveat:** gen2 B35 keeps more code than v1 on seven of nine titles - its generated C is 1.1x-2.6x that of v1 (Flicky 0.97x, Shining Force 0.43x because v1 falls back to broad); it retains 39-49 % of broad against v1's 19-51 % (100 % on the Shining Force fallback). v1 is smaller where it is accepted but misses source code on S&K (ADR 0099) and falls back to broad on Shining Force; B35 buys correct coverage on all five source-backed titles at a measurably larger (still roughly half-of-broad) footprint. Research inference (PyTorch CPU, whole ROM): 0.09-1.43 s at one thread (0.06-0.93 s at four) for the five corpus
ROMs, 0.27-0.54 s for the historical ROMs - far below the compile savings, so `RESEARCH_INFERENCE_COST_NOT_REPRESENTATIVE` does not apply; the T001 native estimate
(77,441 parameters, ~308 KB folded float32) stands for the port.

## 11. Limits

- Known, previously exposed corpus: five titles, one engine family overlap (Sonic 1 / S&K), `B35` chosen from a 5-point grid on these titles.
- Headroom 1.30 on S&K is the smallest margin; a differently built ROM could be worse. The validator and broad fallback remain the safety net.
- `FRF` ties are counted pessimistically; cells are a 128-byte decision unit - sub-cell structure is the structural prune's job.
- Runtime evidence is a bounded 600-frame window per title (two titles stop at a hardware frontier), not full-game equivalence.
- Single timing samples; no repeated-run variance study.

## 12. Independent review

A fresh adversarial reviewer session (head `e4533bc`) attacked held-out leakage, post-hoc budget search, rank order/tie determinism, FRF / headroom / R/ROM arithmetic,
truth or runtime leakage into selection, v1 drift, M3/recipe/corpus drift, K/R conflation, private-data leakage and model nondeterminism. **No blocker or major finding; verdict
ACCEPT.** Confirmed independently (numpy + truth lists, not the repository code): plan commit `fb3e829` precedes the first evaluation output and is the only commit of the plan;
outer score digests match the plan; FRF and headroom match to 4 decimals (S&K B30 headroom 1.116, B35 1.302); ML-only misses are 0 at B30/B35/B50; the six v1 artifact digests are
unchanged; the two final fits are byte-identical; gen2 == broad on all nine titles from the raw results. Minor findings, all accepted as stated limits:

- **Tie groups are low-score, not saturated.** The plan text says saturated ties sit at the top; in fact the large tie groups are low-score groups of identical-context cells
  (Flicky: a 559-cell group straddling every budget boundary; S&K: a 24-cell tie at the B35 boundary). The ordinal tie-break is deterministic and cost no truth cell (the worst
  positive is far above), but it spends some budget on arbitrary cells. The frozen plan wording is left unchanged (it is part of the pre-registration); this ADR is the correction.
- **`frame_stream` equality is vacuous** (the digest of an empty stream for every run, `--coverage-no-render`); the runtime claim rests on outcome, frames, final state and coverage
  digests plus the executed-PC set comparison. Flicky and S&K stop at a pre-existing frontier after 26 and 9 frames, so their equality is weak, bounded-window evidence.
- **v1 comparison:** v1 is smaller / cheaper to compile than gen2 on every title where it is accepted (compile CPU gap largest on OutRun and Cool Spot); gen2's advantage is correctness
  (S&K) and acceptance (Shining Force). The economics gate is against broad, not against v1 (section 10).
- The Sonic 2 falsifier is engine-correlated with Sonic 1 / S&K; the four historical titles are runtime-only evidence.

## 13. Next SEG-049 action

SEG-049-T003: implement deterministic **native** build-time inference for frozen M3 + frozen B35 (CPU-owned gen2 representation/token generation, frozen CNN
inference, frozen rank policy) and prove exact selected-cell/region parity with the research reference. It does not include launcher/default-policy work.
