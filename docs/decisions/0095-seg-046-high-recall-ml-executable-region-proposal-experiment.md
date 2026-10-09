# ADR 0095: SEG-046 high-recall ML executable-region proposal experiment

- Status: In progress (section 1 frozen by T001; later sections appended by T002..T006).
- Predecessor: ADR 0094 (SEG-045, gate FAIL: region-detector recall), ADR 0093 (SEG-044 exact source universe).

## 1. Contract (frozen before any model was trained)

Question: can a small, deterministic ML classifier trained ONLY from Sonic 1 exact source truth propose executable ROM regions `R` with
enough recall that the unchanged SEG-045 machinery (`K0 = U ∩ R`, structural pruning to `K`, unchanged admission validator, existing AOT
emitter/runtime) works on unrelated blind Genesis titles - in particular the two sparse code islands inside data-like pages that falsified
FLOW8-D (Cool Spot, Streets of Rage)?

This is a bounded ML PRODUCER experiment, not a production milestone. ML is never a correctness oracle and never runs in generated programs.
Correctness stays with the existing broad decoder, structural pruning, admission validator, generated-native dispatcher and fail-closed
missing-target stop. SEG-030 analysis, pruning redesign, Ghidra and runtime ML/decode/JIT/fallback are out of scope.

Corpus. Training/calibration: Sonic 1 REV00 only (ROM SHA-256 `46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6`; SEG-044
exact source universe `C`, 24,180 instruction starts). Blind falsification: Sonic 2, Cool Spot, Streets of Rage, OutRun (existing complete
23,200-frame no-input M68K execution oracles; runtime coverage is a FALSIFIER only, read after the freeze). Golden Axe: exploratory only.
Only operator-authorized local images are used; none is fetched.

Unit and labels. Primary unit: 256-byte ROM window; one bounded sensitivity comparison may use 512 bytes; no other size. Label: positive iff
the window contains >= 1 exact source instruction start of `C`; else negative. Labels exist for Sonic 1 only and are bound to its ROM hash
(wrong hash fails closed). Execution coverage never creates labels; unknown windows of any other title are never labelled.

Features (generic; frozen schema, version `seg046-features-v1`). A feature is a pure function of ROM bytes and the report-only C++
window-feature export (`segarecomp ... --window-feature-report`, owner `hybrid_admission`, built on `m68k_control_successors` / the decoder /
the IR; Python duplicates no M68K classification). Families: byte statistics (entropy, zero / 0xFF / printable fractions, unique-byte
fraction, longest run, word-repeat rate, zlib ratio, 8-bin value histogram); decoder/control (identity density, length histogram, IR-family
histogram, conditional/unconditional/call/return/indirect/terminator/exception densities, fixed-edge landing same-window / adjacent / far /
dangling, sequential fall-off, incoming local/external fixed edges and ratios); neighbour context = previous and next window's base vector.
FORBIDDEN as a feature: title identity, absolute address, normalized offset, window/page ordinal, source symbols/names/labels/filenames,
runtime counts or PC coverage, human annotation. `assert_feature_schema` rejects any feature whose name carries such a fragment; the window
start is only a row key and is dropped before modelling.

Models. At most two families: (1) logistic regression (standardized, balanced class weights); (2) one shallow histogram gradient boosting
model (depth 3, 60 iterations). No neural network, no raw-byte embedding, no hyperparameter search. Fixed seed 46. Bounded size, CPU only.

Validation. BLOCKED cross-validation, never random windows: 16 contiguous groups, training excludes the held-out group plus 2 purge windows on
each side (neighbour features overlap). Objective: false negatives are far costlier than false positives. Selection order: (1) highest
out-of-fold positive recall (the threshold is lowered to the lowest positive out-of-fold score, i.e. 100% observed recall); (2) among those,
lower selected-ROM fraction (R includes the certain-code union); (3) among materially equal choices (within 0.01 of the minimum fraction),
the simpler model (logistic regression; 256 before 512). The frozen threshold is that lowest positive out-of-fold score. If exact 100% recall
requires essentially the whole ROM the experiment fails calibration.

Certain-code union (frozen): `R = ML-selected windows ∪ windows containing a precise direct-control-discovery identity (includes the
machine roots)`. No title-specific seeds.

Sonic 1 calibration gate (all required, else STOP SEG-046 before any blind title): `C ⊆ R`; `C ⊆ K`; `K/U <= 0.50`; the unchanged production
admission validator accepts `K`; zero observed Sonic 1 oracle escape (sanity only).

Freeze. Before any blind title is processed the definition (window size, schema hash, model family, hyperparameters, seed, threshold, union
rule, pruning behaviour, model artifact SHA-256) is committed. After it: no retraining, feature, threshold, window-size, per-title model,
page/window addition or escape-driven correction. The trained artifact stays private (ignored `.cache`); only its digest and the
deterministic recipe are recorded. No ROM-derived table, listing, address or byte is committed.

Pre-registered primary gate (a production/expanded-training successor is justified only if ALL hold): >= 3 of 4 blind titles PASS; AND Cool
Spot PASS; AND Streets of Rage PASS; every passing title has 0 runtime escapes / missing-admission stops, broad and selective state evidence
equal, the unchanged validator accepting `K`, no title-specific feature/threshold/model, and no blind title influencing training or model
selection. A title may pass behaviourally up to `K/U <= 0.60` (preferred `<= 0.50`).

Economics gate (>= 3 passing blind titles): generated C <= broad -30%, compile CPU <= broad -25%, runtime regression <= +15%. Thresholds are
never tuned after a blind result. A failing title is not repaired; post-hoc analysis is uncredited diagnostic only.

Classifications: ML REGION PRODUCER PORTABLE / ML IMPROVES RECALL BUT NOT ENOUGH / ML OVERSELECTS / ML DOES NOT GENERALIZE / INSUFFICIENT
TRAINING MATERIAL / DEPENDENCY OR TOOLING STOP. On a full pass exactly one successor SEG-047 is registered; otherwise none.

Baselines. Product main `a4664ed4abbb357f04976a2a8525ba5988214880` (PR #84 merged 2026-10-09); harness main
`06e625a89293ef1e5f1553f3db3915f67ef3c5da`.

Isolated experiment environment (no repository dependency added; ignored `<HARNESS_ROOT>/.cache/seg046-ml-venv`, created with
`python3 -m venv` + `pip install scikit-learn==1.9.1`): Python 3.13.5, scikit-learn 1.9.1, numpy 2.5.3, scipy 1.18.1 (linux aarch64 wheels,
dev container). The public product build, CI and tests do not import any ML package (the unit tests cover only the pure-Python parts).

## 2. Window-feature export and tool (T002, report-only)

`genesis_window_feature_report` (C++, `hybrid_admission`, reusing `m68k_control_successors` / decoder / IR) writes 27 generic per-window
counts of the broad identities (identity count, instruction-span histogram, IR family histogram, conditional/unconditional/call/return/
indirect/terminator/exception counts, fixed-edge landing same/adjacent/far/dangling, sequential fall-off, incoming local/external fixed
edges); CLI `--window-feature-report <path> --window-feature-bytes <256|512>` (report-only; broad emission unchanged). Python
(`tools/segarecomp_ml_region.py`) adds 16 byte statistics and the previous/next window context: 3 x 47 = 141 features. Window start is a row
key only. Tests: `genesis_window_features_test` (C++), `segarecomp_ml_region_test` (forbidden-feature rejection, extraction determinism and
order independence, title/path independence, wrong-ROM-hash and malformed-universe fail-closed, dropped-source-positive containment failure,
oracle-input refusal, artifact digest mismatch, blocked-fold purge).

## 3. Sonic 1 blocked CV, model shootout, freeze (T003; Sonic 1 is the only title that influenced anything)

Sonic 1: 512 KiB; 2048 windows of 256 B (434 positive, 1614 negative); 1024 windows of 512 B (230 positive). Blocked CV: 16 contiguous groups,
2-window purge, fixed seed 46. Threshold = lowest positive out-of-fold score (100% observed OOF recall). R includes the 25-34 certain-code
windows. Out-of-fold results (the CV was run once; the selection rule above was fixed in the tool before it was run):

| window | model | OOF AUC | threshold | selected ROM fraction at 100% OOF recall |
| --- | --- | --- | --- | --- |
| 256 | logistic regression | 0.9991 | 0.00136 | 0.3179 |
| 256 | hist. gradient boosting | 0.9994 | 0.01378 | 0.2764 |
| 512 | logistic regression | 0.9985 | 0.00835 | 0.2588 |
| 512 | hist. gradient boosting | 0.9995 | 0.04535 | 0.2500 |

Rule outcome: the minimum is 512/HGB (0.2500); 512/logistic regression is within 0.01 and simpler, so it is chosen. (The 256 B primary size was
not retained because the frozen selection order puts selected-ROM fraction ahead of the window-size preference. This is recorded honestly: the
choice was made on Sonic 1 numbers alone, before any blind title was processed.)

Frozen definition (`tools/segarecomp_ml_region.frozen.json`): window 512 B; feature version `seg046-features-v1`, 141 features, schema hash
`d2e7c82913139c29450511326d6de76e91579ec654edde81afae16b13cd1f570`; logistic regression (standardize, C=1, lbfgs, balanced classes, 2000
iterations); seed 46; threshold 0.00835406801187952; certain-code union = windows containing a precise direct-control-discovery identity
(includes the machine roots); final model trained on all 1024 Sonic 1 windows; two independent fits produce the identical artifact;
model artifact SHA-256 `b5ddae5aa0fe6571455803c244d2a6be4a2c3349e3436c8780bfc3d8aa8fa62b` (private, ignored; not committed).

Sonic 1 calibration gate (Release CLI, 4 vCPU): R = 257 windows = 131,584 B = 25.1% of the ROM (all selected by the model; the 25 certain-code
windows are a subset); U 246,293; K0 64,510; K 64,233 (277 pruned, 42 rounds); K/U 0.2608; C 24,180 ⊆ R yes; C ⊆ K yes (C ∩ U = C); C/K 0.376;
unchanged production validator accepted; Sonic 1 oracle sanity (23,200 frames, clang): broad and selective `frames_reached`, 10,512 distinct
PCs, 0 escapes outside K, coverage/final-state/frame-stream digests identical. GATE: PASS. (C ⊆ R is in-sample for the final model; the
meaningful recall evidence is the blocked OOF recall.) Freeze committed before any blind title was processed.

## 4. Blind region + prune (T004; frozen definition of section 3 applied unchanged, no per-title input; BEFORE any blind runtime result was read)

Release CLI, 4 vCPU, same unchanged production validator (run inside the CLI on a scratch copy; again by the emitter in the build).
"extract" = broad analysis + window export (wall / RSS); "infer" = Python byte features + model (wall / RSS, includes interpreter and sklearn import).

| title | ROM | R | R/ROM | U | K0 | K | K/U | pruned (rounds) | validator | extract | infer |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Sonic 2 | 1 MiB | 286,720 B (560 windows) | 27.34% | 496,387 | 140,439 | 137,230 | 0.2765 | 3,209 (80) | accepted | 31.6 s / 950 MiB | 16.7 s / 132 MiB |
| Cool Spot | 1 MiB | 124,928 B (244) | 11.91% | 498,276 | 60,459 | 56,980 | 0.1144 | 3,479 (77) | accepted | 20.7 s / 917 MiB | 7.7 s / 132 MiB |
| Streets of Rage | 512 KiB | 126,464 B (247) | 24.12% | 247,761 | 61,771 | 61,374 | 0.2477 | 397 (70) | accepted | 12.9 s / 484 MiB | 9.4 s / 125 MiB |
| OutRun | 1 MiB | 174,592 B (341) | 16.65% | 496,950 | 84,289 | 78,283 | 0.1575 | 6,006 (427) | accepted | 17.0 s / 858 MiB | 4.3 s / 132 MiB |
| Golden Axe (exploratory) | 512 KiB | 129,024 B (252) | 24.61% | 249,843 | 63,077 | 61,939 | 0.2479 | 1,138 (49) | accepted | 9.7 s / 478 MiB | 3.1 s / 125 MiB |

Prune CLI wall 0.6-3.8 s, peak RSS 306-594 MiB. No machine root or materialized identity was pruned, no proposal was rejected. Every ML-selected
region is a subset of the certain-code union's complement plus the union (the certain-code windows were all already selected by the model).
