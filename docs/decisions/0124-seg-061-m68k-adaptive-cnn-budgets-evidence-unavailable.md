# ADR 0124: SEG-061 adaptive M68K M3 retention budgets — authentic out-of-fold evidence unavailable; hypothesis untested; fixed B35 retained

- Status: Accepted. **Classification: `M68K_ADAPTIVE_EVIDENCE_UNAVAILABLE`.** The five frozen title-held-out M3 score arrays could not be recovered or verified against their frozen digests on this machine, so the score-based adaptive-budget hypothesis is **untested**. This is not a failed hypothesis and not evidence against adaptive budgets. Decision: no adaptive policy, no native change; keep fixed B35 (explicit experimental `gen2`). Compatibility stays default and Genesis Optimized stays v1. ADR 0100–0103, 0108/0109 are unchanged.
- Related: ADR 0099 (truth methodology), 0100 (Gen2 M3), 0101 (rank budgets), 0123 (lessons from the inconclusive SMS experiment).

## Question

Can observable, label-free properties of the frozen M3 outputs predict a conservative upper bound on the minimum retained fraction giving complete source-backed instruction-start recall (the last-positive-cell rank, "FRF"), so that a per-ROM budget could replace B35?

## Checkpoint A — recovery of out-of-fold scores

| input | status |
|---|---|
| `SEG049_ROOT/scores/M3.<title>.npy` (5 arrays) | absent; `.cache/seg049` did not exist |
| five pinned ROM images in `games/` | present |
| token tooling (`m68k-decode-token-report`), seed/universe CLI | present; release CLI rebuilt |
| research environment | rebuilt from the frozen plan (Python 3.13, numpy 2.5.3, scikit-learn 1.9.1, torch 2.14.1; the PyPI wheel is the CUDA build, run CPU-only) |
| source-backed truth, Flicky / Sonic 1 / Sonic & Knuckles | **rebuilt**: sources at the pinned revisions rebuild byte-identical ROMs (SHA-256 verified); two independent extractions each reproduce the frozen truth digests (`33916a8e…`, `fd21b96b…`, `76be1ccc…`; 5,707 / 24,180 / 119,319 starts). Sonic & Knuckles requires `--placeholder-fill`, `--max-entries` and producer id `as-listing-v1`. Seed/universe sizes match ADR 0099 (246,293 and 64,352 for Sonic 1 and Flicky). |
| source-backed truth, Shining Force / Phantasy Star II | **not recovered**: both builds need wine-hosted Windows tools (`ASM68K`, `asw`), whose execution was refused in this environment (code-from-external policy) and could not be performed manually by the operator |

The fold trainer (`loto --model M3 --held-out <title>`) loads all five titles' truth and trains each fold on four titles, so no frozen fold, including the one-fold pilot, can be run or digest-verified without Shining Force and Phantasy Star II. Distinguishing the causes: missing authorized ROMs: no; toolchain/dependencies: no (reproduced); nonreproducible results: **not observed** (the three recovered truths reproduce exactly); digest mismatch: **not applicable (no frozen fold could run)**; blocking cause: two truth builds not executable here. Cost note: a reduced fold fit takes 30–110 s on 16 CPU cores, so training cost is not the barrier; truth recovery is. Whether the fold fits would match the committed 9-decimal prediction digests across the torch build is unknown.

## Reduced-corpus exploratory run (not frozen evidence)

At operator direction the experiment continued on the three verified titles, with the frozen M3 recipe (seed 49) but a **reduced training set** (the other two available titles). These scores are NOT the frozen out-of-fold predictions, are not digest-verified and are never used as such. Held-out FRF of the reduced folds: Sonic 1 0.219, Flicky 0.195, Sonic & Knuckles 0.421 (frozen five-title M3: 0.211, 0.177, 0.269). Sonic & Knuckles degrades sharply because it was predicted by a model trained on two smaller titles.

Frozen label-free features (defined before truth was joined, `tools/segarecomp_m68k_adaptive_budget.py`): family A, logit at rank 10/20/30 % and their spread; family B, tie fraction at rank 25 % and largest logit gap between ranks 10–40 %; family C (decoder-token disagreement) not evaluated.

| held out | FRF | L10 | L30 | L10−L30 | tie25 | max gap 10–40 % |
|---|---|---|---|---|---|---|
| Flicky | 0.195 | 14.2 | −10.0 | 24.2 | 0.546 | 1.08 |
| Sonic 1 | 0.219 | 14.2 | −12.0 | 26.1 | 0.000 | 1.05 |
| Sonic & Knuckles | 0.421 | 9.1 | −4.1 | 13.3 | 0.000 | 0.13 |

Observations (n = 3, descriptive only): the smaller spread and gap for the hard title separate it, and Flicky shows a large equal-score group at the budget boundary (ties are ambiguity, not evidence of data). These do **not** support any inference about adaptive budgets: three title-level observations, scores from a degraded non-frozen model, and the "hard" title is hard largely because its reduced model is weak, so difficulty of the ROM and quality of the model are confounded. No estimator was fitted or evaluated (Checkpoint C not entered: the screening criteria could not be assessed, and the leave-one-title-out leakage limitation would apply to any fit).

## Decision and prerequisites for a valid experiment

Retain fixed B35. To test the hypothesis, supply Shining Force and Phantasy Star II truth (run the wine builds of ADR 0099 section 4/5 on a machine that permits it), then run the five frozen `loto --model M3` folds (about 30–110 s each; pilot first), verify each against `frozen_from.outer_score_digests` and FRF, and apply the registered Checkpoint B/C protocol with the leakage limitation stated. Independent titles (Checkpoint D) are not justified yet.

## Answers

1. Out-of-fold predictions recovered and verified: no. 2. Features examined: families A and B (on non-frozen reduced scores). 3. Easy vs hard ROMs: not established. 4. Estimator: none fitted. 5. Sonic & Knuckles: FRF 0.421 under reduced training (0.269 frozen); not a valid estimate. 6. Improvement over B35: none shown. 7. Generalization evidence: none. 8. Native integration: not justified. 9. Retain B35; the line remains open and untested.
