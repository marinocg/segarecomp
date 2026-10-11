# ADR 0100: SEG-049 second-generation cross-title ML executable-region experiment

- Status: experiment complete; **classification `GEN2_REPRESENTATION_VALID_CALIBRATION_UNSOLVED`**. Sections 1-9 were fixed by the committed plan
  (`tools/segarecomp_ml_region_gen2.plan.json`, commit C1) before any M1-M4 title-held-out result existed; later sections only append results.
- Predecessors: ADR 0093 (exact source universe), ADR 0095 (SEG-046), ADR 0096 (SEG-047 native v1), ADR 0099 (SEG-048 multi-title hardening).
- Scope statement: **SEG-049-T001 DOES NOT CHANGE PRODUCTION OPTIMIZED AOT.** Compatibility stays broad AOT; Optimized stays the SEG-047 native v1
  model; the v1 artifacts are byte-identical (enforced by test). Everything here is offline research tooling plus one report-only C++ export seam.
- Evidence statement: the five titles were already used by SEG-048 and are **not** a sealed blind cohort. The result is *five-title source-backed
  title-held-out generalization evidence*. A later production/default decision should gain fresh source-backed evidence if another suitable title
  becomes available.

## 1. Why ML remains the selected strategy (and what is not reopened)

ML is the first approach in this project that produced large measured executable-set reductions across real titles while keeping a simple
production architecture. Earlier deterministic discovery, CFG-expansion, frontier, prefix, loop/progress and indirect-target machinery consumed
substantial effort, did not give a general scalable solution and was largely removed or superseded. **This experiment does not compare against,
revive or evaluate that machinery.** Deterministic code remains only where it already is: structural consistency/pruning of the ML proposal and the
admission validator (the correctness authority). SEG-048 showed that the *v1 representation/model* is under-powered across titles; it did not
show that ML should be replaced.

Architecture (unchanged): immutable ROM -> ML executable-region proposal `R` -> existing structural processing -> `K` -> existing validator ->
generated-native AOT. No interpreter, JIT, runtime guest-opcode decoding, runtime ML, semantic fallback, title-specific model/targets; broad AOT is
the unconditional compatibility fallback; exact source/recomp maps outrank ML; fail closed; CPU semantics/features are CPU-owned; training is
offline and the release never needs Python ML packages.

## 2. Synchronized baselines

| repository | branch | SHA at synchronization |
| --- | --- | --- |
| segarecomp (product) | main | `a7d0d59a49636661b399583c3b005c6d5ceddc39` (PR #90 / SEG-048 squash merge) |
| segarecomp-harness | main | `e428e8898e5744b016b6a34bb3a1ca0b04822b29` |

Product branch `research/seg-049-gen2-ml-region-model`, one product PR. Checkpoint commits: C1 representation + token seam + frozen plan, C2 v1
characterization + five-way LOTO table, C3 nested calibration + R/K stages, C4 ADR/review.

## 3. Corpus (five SEG-048 exact source-backed titles; truth stays private)

Source-backed truth `C` = MC68000 instruction starts from assembler listings of exact byte-identical rebuilds (ADR 0099 section 5). The private
truth artifacts of SEG-048 were reused; their ROM and truth digests were re-verified on load (a mismatch aborts).

| id | title | ROM SHA-256 | source @ pinned revision | starts `C` | truth artifact SHA-256 |
| --- | --- | --- | --- | --- | --- |
| s1 | Sonic the Hedgehog (REV00) | `46160baa...f2a6` | sonicretro/s1disasm @ `7ebe4b3d...558da` | 24,180 | `fd21b96b...fc13` |
| flicky | Flicky | `4df1a91e...8c11` | oranguthang/flicky_src @ `d9a0d479...e9a89` | 5,707 | `33916a8e...d6e0` |
| sf1 | Shining Force | `6c43b41b...522d` | ShiningForceCentral/SF1DISASM @ `1f466bf0...6c6f7` | 44,740 | `7f5289dc...7059` |
| ps2 | Phantasy Star II (REV02) | `a0bd97f5...696b` | wyatt8740/ps2disasm @ `c23877b8...89167` | 19,261 | `87174c56...ea96` |
| sk | Sonic & Knuckles | `6e12e6b3...fec1` | sonicretro/skdisasm @ `044fa467...dc024` | 119,319 | `76be1ccc...7b72` |

(Full 64/40-hex identities are in the plan file `corpus` block and are asserted by test.) Positive 128-byte cells: s1 820/4096, flicky 180/1024,
sf1 1310/12288, ps2 565/6144, sk 4073/16384.

## 4. Generation-2 representation (frozen)

- **Decision cell 128 bytes (64 words); context 1024 bytes (512 words)**, the cell sitting in the middle (448 bytes on each side). A cell is
  positive iff it contains at least one source-backed instruction start (a label, not an input).
- **ROM edges**: symmetric *word-block reflection* with period 2n (bytes inside a word keep their order, so word alignment is preserved). Padding
  is derived from ROM content, never a sentinel/zero marker, so there is no first/last-cell signal. No absolute or relative address, cell ordinal,
  first/last marker or ROM size is an input; the leakage guard `assert_input_channels` rejects any such channel and a test mutates each.
- **Channels**: raw bytes (hi/lo byte of each word position, learned 8-dim embedding each) and the **CPU-owned decode tokens**.
- **Token export seam** (C++, `genesis_decode_token_report`, CLI `m68k-decode-token-report`): for every even ROM position the existing MC68000
  decoder + IR lift + control-successor projection yields 8 bounded generic fields: `status` (rejected / architectural exception form /
  ordinary), `length` (words, 5 = 5+), `family` (move / arithmetic / logic-bit / shift / control / exception-raising / other), `control`
  (conditional / direct jump / direct call / dynamic jump / dynamic call / return / always-exception), `ea_src`, `ea_dst` (EA-mode ordinals),
  `width`, `flags` (privileged, PC-relative displacement form). Python implements no decoder. No decoded target address, ROM position, ordinal,
  symbol or title is emitted (C++ test: a branch's displacement does not change its token; a word's token is independent of its position).
  Token schema SHA-256 and representation SHA-256 are in the plan.
- Cross-checks: the padded model input arrays and the M1 aggregates are tested against independent reference implementations of the same
  definitions (cell/context/edge rules); the C++ token export has its own unit test.

## 5. The v1 false-negative characterization (before any new training)

Zero-shot frozen v1 (512-byte windows, 141 features, threshold `0.00835406801187952`) is reproduced exactly at cell granularity: C outside R =
0 (s1, training title), 34 (flicky), 11 (sf1), 0 (ps2), 237 (sk), matching ADR 0099. Cell view: 13 source-positive cells are missed by v1 in the four
zero-shot titles (flicky 2, sf1 1, sk 10) versus 6,110 selected by score. Threshold-independent ranking of v1 (M0 FRF, below) is weak: 0.316 (flicky),
0.985 (sf1), 0.985 (sk).

Aggregate comparison, pooled over the four zero-shot titles (diagnosis only; no rule was derived from it):

| property (mean) | positive cells v1 selected (6,110) | positive cells v1 missed (13) |
| --- | --- | --- |
| instruction starts per word position | 0.482 | 0.339 |
| fraction of word positions with an ordinary decode | 0.834 | 0.781 |
| exception-form decode fraction | 0.150 | 0.207 |
| control-transfer fraction of instructions | 0.328 | 0.229 |
| zero-byte / 0xFF-byte density | 0.197 / 0.016 | 0.206 / 0.022 |
| normalized byte entropy | 0.613 | 0.603 |
| bytes covered by source instructions (code coverage) | 0.914 | 0.581 |
| embedded non-instruction bytes | 0.086 | 0.419 |
| mixed code+data cell (25-75 % coverage) | 0.104 | 0.462 |
| distance to nearest non-code cell (cells, mean / median) | 15.4 / 10 | 8.2 / 1 |
| executable density of the 8 neighbouring cells | 0.936 | 0.490 |
| positive cells among the 4 cells of the 512-byte window | 3.86 | 2.08 |
| position of the cell inside its 512-byte window (quarters 0..3) | 0.25 / 0.25 / 0.25 / 0.25 | 0.15 / 0.15 / 0.23 / 0.46 |

Interpretation (small missed sample, 13 cells): what v1's representation does not expose is **local structure at sub-window scale**. The missed
code sits in mixed code+data cells at code/data boundaries, in windows where only one or two of the four cells are code, and the window
aggregate (a 512-byte mean over everything including jump tables/graphics and padding) is dominated by the non-code part; instruction family,
length and EA mix, entropy and zero/FF density do not distinguish the groups. The missed cells also lean toward the last quarter of the
window, i.e. an arbitrary 512-byte boundary cuts through a code run. This motivates a finer decision cell and a larger sequence context, not a
handcrafted rule.

## 6. Pre-registered candidate set (frozen in C1; nothing outside it was trained)

Common: seed 49, CPU only, deterministic kernels, probability = sigmoid(logit), `FRF` / recall as the criteria; precision, accuracy and AUROC are
secondary diagnostics only.

- **M0** frozen production v1, no retraining (each 128-byte cell inherits its 512-byte window probability). Comparator only.
- **M1** `HistGradientBoostingClassifier(max_iter=200, max_depth=4, learning_rate=0.08, min_samples_leaf=40, l2_regularization=1.0,
  max_bins=255, early_stopping=False, class_weight=balanced, random_state=49)` over **219** multi-scale aggregates (cell 128 B, mid 512 B, context
  1024 B) of the same representation: 6 byte statistics (entropy, zero, 0xFF, printable, unique, word-repeat), 8 byte-bucket fractions and 57
  token-class fractions per scale. No sequence learning.
- **M2** raw-byte valid-convolution CNN (input channels 16; **72,481** trainable parameters).
- **M3** raw-byte + token CNN, same architecture (input channels 36; **77,441** parameters) - the primary candidate.
- **M4** M3 + hard-positive emphasis: identical to M3 for epochs 1-10; at the end of epochs 10 and 20 the TRAINING titles are scored (eval mode)
  and training positive cells below the 10th percentile of the pooled training-positive scores get loss multiplier 4.0 (reset and re-mined at each
  mining epoch). No held-out-title mining; the cell identities are runtime-only and never persisted.

CNN architecture (all Conv1d are *valid* so a cell's logit depends on exactly its 512-word context; evaluating a whole ROM/segment at once is
therefore identical to per-cell evaluation, which a test checks to float32 precision): word-resolution sequence of 512 positions, input =
concat(embed(hi byte, 8), embed(lo byte, 8)[, token embeddings: status 2, length 2, family 3, control 3, ea_src 3, ea_dst 3, width 2, flags 2]);
stem Conv1d(k=5,d=1,48)+BN+ReLU -> Conv1d(k=5,d=2,48)+BN+ReLU -> MaxPool1d(2) -> Conv1d(k=5,d=2,64)+BN+ReLU -> Conv1d(k=5,d=4,64)+BN+ReLU
(512 -> 508 -> 500 -> 250 -> 242 -> 226 positions; the cell is final positions [97,129)); head = concat(mean, max over the 32 cell positions;
mean, max over all 226 positions) = 256 -> Linear(256,64) -> ReLU -> Linear(64,1). Bound <= 500,000 parameters: 72,481 / 77,441.

Training recipe (identical for every fold; no per-title tuning): AdamW, lr 0.002 with per-step cosine decay to 0, weight decay 1e-4, 30 epochs
(fixed; the held-out title cannot influence duration), an "epoch" = every training title randomly offset (seeded) and cut into 32-cell segments, all
shuffled, batches of 8 segments (256 cells); loss BCE-with-logits with `pos_weight = #negative / #positive` cells of the TRAINING titles; gradient
clip 5.0; torch threads 4; Python 3.13.14, numpy 2.5.3, scikit-learn 1.9.1, torch 2.14.1 (CPU). Training environment is private
(`<harness>/.cache/seg049/venv`); none of it is a product dependency.

Selection rule (frozen): viable iff FRF <= 0.60 on all five held-out titles; then lowest worst-title FRF; candidates within 0.02 of the best
worst-title FRF form a tie set in which a more complex model (order M1 < M2 < M3 < M4) replaces a simpler one only if its mean or worst FRF is
lower by >= 0.02; otherwise the simpler model wins. **Calibration is evaluated only after the representation/model is selected.**

## 7. Five-way title-held-out results (source recall and FRF)

Folds: train {B,C,D,E} -> test A, and so on (the held-out title is excluded from fitting, class weights, hard-positive mining and calibration;
`assert_split` and per-command checks enforce it). **FRF** = smallest fraction of cells (descending score; ties with the lowest positive all
counted) that contains every source-backed instruction start; it is threshold-independent.

| model | s1 | flicky | sf1 | ps2 | sk | worst | mean | viable (<=0.60) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| M0 frozen v1 (s1 is its training title) | 0.225 | 0.316 | 0.985 | 0.206 | 0.985 | 0.985 | 0.543 | no |
| M1 HGB aggregates | 0.245 | 0.179 | 0.132 | 0.097 | 0.388 | 0.388 | 0.208 | yes |
| M2 raw-byte CNN | 0.216 | 0.200 | 0.960 | 0.186 | 0.388 | 0.960 | 0.390 | no |
| **M3 byte+token CNN** | 0.211 | 0.177 | 0.208 | 0.112 | 0.269 | **0.269** | **0.195** | **yes** |
| M4 M3 + hard positives | 0.211 | 0.185 | 0.291 | 0.114 | 0.268 | 0.291 | 0.214 | yes |

Source recall at fixed cell budgets (instruction starts outside the top-f cells; threshold-independent): top 30 % - M3 and M4 0 outside in all five
titles, M1 2 (sk), M2 15 (sf1) + 24 (sk), M0 21/101/271 (flicky/sf1/sk); top 45 % and 60 % - M1, M3 and M4 0 in all five titles, M2 5 (sf1), M0
101 (sf1) + 160 (sk). AUROC (secondary): M3 >= 0.9998 on every title. Preferred (<= 0.45 every title) and strong-mean (<= 0.35) targets are met by M1,
M3 and M4.

**Selected: M3** by the frozen rule (the only candidate within 0.02 of the best worst-title FRF; M4's sf1 FRF is 0.291 and M1's sk FRF is 0.388).
Observations: the token channels are what makes the CNN generalize (M2 vs M3 on sf1: 0.960 vs 0.208; a single raw-byte-only positive cell scores
7.6e-11); a richer-feature boosted tree (M1) already moves FRF from 0.985 to <= 0.388, i.e. most of the gain over v1 comes from the finer cell and
multi-scale representation; hard-positive emphasis (M4) does not help (sf1 gets worse). Because FRF is set by the single lowest-scoring positive, it
is a strict and outlier-sensitive measure.

## 8. Nested title-level calibration, R stage and K stage (selected M3)

Calibration (pre-registered, after selection): for each outer held-out title H, each of the four training titles is scored by a model trained on
the *other three* training titles (20 inner fits); `m_H` = minimum positive-cell probability over those four inner predictions; policies
`Q0..Q3` = 1, 0.5, 0.25, 0.1 x `m_H`; the highest factor passing all five outer folds is chosen. The outer model is trained on the four training
titles and the frozen threshold is applied to H. `R` = ML-selected cells union the certain-code seed cells (cells holding a precise
direct-control-discovery identity - the existing production seeding). Gate: `C outside R = 0` and `R/ROM <= 0.60` on every outer title.

`C outside R` is evaluated first and is the only ML-miss count; `K` comes from the unchanged structural prune and validator afterwards.

| outer | inner `m_H` | policy | C outside R | R/ROM | outer-min-positive / threshold | validator | K/U | C outside K | post-ML losses |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s1 | 1.73e-6 | Q0 / Q1 / Q2 / Q3 | 0 / 0 / 0 / 0 | **0.518** / 0.605 / 0.692 / 0.783 | 3.5e3 .. 3.5e4 | accepted x4 | 0.518 / 0.603 / 0.689 / 0.779 | 0 | 0 |
| flicky | 1.28e-6 | Q0 / Q1 / Q2 / Q3 | 0 / 0 / 0 / 0 | 0.832 / 0.865 / 0.909 / 0.941 | 3.9e4 .. 3.9e5 | accepted x4 | 0.838 .. 0.943 | 0 | 0 |
| sf1 | 4.30e-5 | Q0 / Q1 / Q2 / Q3 | **1 / 1 / 1 / 0** | 0.126 / 0.163 / 0.175 / 0.210 | 0.10 / 0.21 / 0.42 / 1.04 | rejected x3, accepted (Q3) | - x3, 0.213 | -, 0 (Q3) | -, 0 |
| ps2 | 5.08e-6 | Q0 / Q1 / Q2 / Q3 | 0 / 0 / 0 / 0 | 0.355 / 0.442 / 0.531 / 0.632 | 9.6e3 .. 9.6e4 | accepted x4 | 0.339 / 0.428 / 0.521 / 0.625 | 0 | 0 |
| sk | 6.87e-8 | Q0 / Q1 / Q2 / Q3 | 0 / 0 / 0 / 0 | 0.771 / 0.820 / 0.862 / 0.906 | 2.3e4 .. 2.3e5 | accepted x4 | 0.775 .. 0.907 | 0 | 0 |

**No pre-registered policy passes all five folds**: Q0, Q1, Q2 fail on sf1 (one missed instruction start, which makes the existing structural
closure prune a machine root - `machine_root_not_admitted` after 136 rounds - and the validator rejects the proposal), and every policy fails
the selectivity half on flicky (R/ROM 0.83-0.94) and sk (0.77-0.91); Q1-Q3 also fail on s1/ps2 at the lower factors. No threshold was searched
after seeing these numbers.

Why (diagnostics, not selections):
- The inner models are 3-title models and are much weaker than the 4-title outer model in the same fold (inner FRF up to 0.94 on flicky/sf1 for
  the fold holding out sk, versus outer FRF 0.269); their minimum positive scores (1e-6..1e-8) are therefore ~3-4 orders of magnitude below the
  outer model's minimum positive scores (0.0016..0.0495), which makes the inherited thresholds far too permissive (large R). The statistic
  "minimum positive score" is also single-cell sensitive.
- The opposite failure is sf1: its outer model has exactly one positive cell (a lone instruction start) scoring 4.5e-6, 10x below the inner
  threshold, which is enough to lose recall.
- With an ideal (oracle, held-out-informed) threshold - an exact full-recall threshold per fold, **not a policy and never selected from** - the
  structural stage accepts all five titles with `C outside K = 0`, R/ROM = 0.211 / 0.177 / 0.208 / 0.112 / 0.269 and K/U = 0.219 / 0.176 / 0.212 /
  0.114 / 0.278. Positives scoring below 1e-3 in the outer models: 0 in s1, flicky, ps2, sk and 1 cell in sf1.

So the ranking/representation is good; the unsolved piece is converting scores into a transferable complete-recall threshold.

## 9. R/K separation and the post-ML questions

- **ML false negatives** (C outside R, with seeds): all four policies 0 on s1, flicky, ps2, sk; sf1 1 start under Q0-Q2, 0 under Q3. The oracle
  diagnostic is 0 everywhere. (Without seeds the numbers are identical: `c_outside_ml_only`.)
- **Post-ML losses** (C in R but outside K): **0 in every evaluated fold/policy** where the validator accepted. No `POST_ML_PRUNING_BLOCKER`
  was observed for the gen2 proposals.
- **Shining Force**: the earlier "proposal rejected by structural processing" is, at least for this model, a consequence of a single missed
  start (the validator rejects under Q0-Q2 with C outside R = 1) and disappears when the proposal is complete (Q3 and the oracle: accepted,
  K/U 0.21). It is therefore an ML-stage miss that the fail-closed structural stage amplifies; the model is not blamed for the validator.
- **Sonic & Knuckles "3 executed PCs in R but absent from K"** (SEG-048, v1: 237 starts outside R, 6,383 outside K, i.e. ~6,146 starts lost *after*
  the proposal): with gen2 every source-backed instruction start of S&K, which includes any executed source-confirmed PC, is inside both R and K
  under all four policies and the oracle (C outside R = C outside K = 0, post-ML losses 0). The pruning behaviour that dropped them under the v1
  region is not reproduced; this experiment cannot attribute it to a specific prune defect (the loss is region-shape dependent), and at the
  tightest region shape tried (oracle, R/ROM 0.269) nothing is lost. A focused structural task remains optional, not demanded by this evidence.

## 10. Native implementability (selected M3; nothing productionized)

- Weights: 77,441 parameters = 309,764 bytes float32 (307,972 after folding eval-mode BatchNorm into the convolutions). Peak activation memory for a
  64-cell chunk: ~1.7 MB. Reference PyTorch CPU (1 thread, 2 MiB ROM): 1.4 s.
- Multiply-adds: 2.45 M per 128-byte cell with the shared fully-convolutional trunk (19.5 M if every cell recomputes its own context). Whole
  ROM, shared trunk: 1 MiB 20 G, 2 MiB 40 G, 4 MiB 80 G MACs - 20/40/80 s at 1 GMAC/s scalar, 5/10/20 s at 4 GMAC/s (SIMD-friendly loops).
  The cost is dominated by convolution weights/activations and is bounded and deterministic; whether it pays for itself is an end-to-end build
  economics question for the follow-up task.
- A dependency-free reference evaluator (`tools/segarecomp_ml_region_gen2_reference.py`, standard library only) reproduces the torch logit
  to < 1e-9 on a random-weight network with non-trivial BatchNorm statistics (test), as an executable specification for a later native port.
  No ONNX/TensorFlow/PyTorch/Python runtime enters the product.

## 11. Determinism and cost

Independent refit of M3/flicky in a separate process under different machine load: score vector bitwise identical (digest equal). 40 fits in
total (5 M1 + 15 CNN outer + 20 M3 inner): ~3.1 h of summed fit wall time at 4 parallel fits x 4 torch threads (<= ~12.5 core-hours upper bound),
CPU only, no GPU.

## 12. Not executed (and why)

- **Runtime differential, economics, model freeze:** the pre-registered R-stage gate was not met by any deployable policy, so by the task rules the
  runtime falsifier and economics were not run, and no research model artifact was frozen (the freeze rule requires at least
  `GEN2_ML_VALID_POSTPROCESS_BLOCKED`). The identity needed to continue is the committed plan (representation, architecture, recipe, seed, corpus
  digests); the five outer scores are reproducible from it deterministically. K/U figures above are structural, from the unchanged production
  prune; generated-C/compile/RSS numbers were not measured.

## 13. Final classification and next SEG-049 action

**`GEN2_REPRESENTATION_VALID_CALIBRATION_UNSOLVED`**: FRF <= 0.60 (indeed <= 0.27) on every held-out title for the selected M3, but no
pre-registered Q0-Q3 training-title-only threshold policy achieves complete outer recall at <= 60 % selection. ML is not discarded; the heuristic
discovery architecture is not reopened.

Next SEG-049 task (T002): *solve cross-title score calibration for the M3 representation/model with a bounded, pre-registered calibration
experiment* - e.g. rank/budget-based and quantile-based policies derived from inner-title FRF-like statistics instead of single-cell minima,
possibly combined with an inner-model strength correction; its gate is the same R-stage gate (all five outer titles `C outside R = 0`,
`R/ROM <= 0.60`) followed by the existing K/validator stage and, only then, the runtime/economics falsifier and a research freeze. Native
port, UX and default-policy tasks (T003-T009) stay draft.

## 14. Independent review

(Appended after the fresh-session review; see below.)
