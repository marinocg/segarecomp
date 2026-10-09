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
