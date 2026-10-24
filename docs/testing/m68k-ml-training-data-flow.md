# M68K code-recognition models: what data each model trains on (SEG-084 audit)

Measured on 2026-10-24 from the live private workspace by `tools/segarecomp_seg084_audit.py inventory` (aggregate output committed as
`tools/segarecomp_seg084_audit.inventory.json`: opaque keys, digests, counts). Line numbers refer to this repository at the SEG-083 merge (`082ec56`).
Nothing in this audit opened a sealed or reserved path.

## Plain answer

- **Every model trained so far (SEG-072 M1-HGB, SEG-076 A / B / C / C-v2) trains on the SEG-072 Genesis development registry only**: 50 images of 11 qualified
  lineages (D01, D02, D03, D04, D05, D06, D07, D09, D10, D11, D12; D08 is unqualified), always leave-one-lineage-out (the held-out lineage's images are excluded).
- **No persisted or frozen model consumes SEG-080 / 081 / 083 labels.** They were used only by offline diagnostics: SEG-083's `segarecomp_genesis_tasbatch5_ml.py`
  (and SEG-081's) trains the SEG-072 **M1-HGB** (`m1_features` + `HGB_CONFIG`, an earlier, simpler model) per fold with the new labels added. The SEG-076 structural
  model A (`a_features` + `A_CONFIG`) and B / C / C-v2 have never been given a TAS label.
- **SEG-075 cross-platform images (36 Atari / Amiga, 5 lineages) are not training data for the SEG-076 pilots**: SEG-076 ran condition C0 (Genesis-only training;
  `segarecomp_xplat_explore.py:run_fit` raises on any `xp/` training image). They were used by SEG-075's transfer conditions (C2) only.
- **SEG-077's Chuck Rock samples are exploratory and unused** (STOP class; the static-first relabel is superseded by a measured 96 % wrong-IP defect).

## Corpora (verified present and digest-checked on this machine)

| Corpus | Source milestone | Images | Effective lineages | Cells (pos / neg / masked) | Role today | Persisted in Git? |
|---|---|---|---|---|---|---|
| SEG-072 Genesis development | SEG-072 (ADR 0151) | 50 (14 Genesis ROM images + 36 Sonic CD modules) | 11 qualified (D01 = Sonic family incl. the CD modules) | 15,024 / 83,204 / 3,243 | training (other 10 lineages) and development evaluation (held-out lineage) for M1, A, B, C, C-v2 | no: `.cache` + a local 308 MB handoff tar |
| SEG-075 cross-platform development | SEG-075 | 36 (8 representatives) | 5 (BattleIsleEditor, HelloAmi, amiga-realtime3d, emutos, toslibc) | 6,845 / 1,749 / 349 | SEG-075 transfer training (C2 only); diagnostic-only for A / B / C | no: `.cache` + local 3.6 GB handoff |
| SEG-080 Sonic 2 TAS | SEG-080 | 1 (same image as registry D01 `s2`) | 0 additional | in `seg081-v1` | diagnostic only; excluded from lineage counting | yes (`seg081-v1`) |
| SEG-081 TAS (Vectorman 2, Batman & Robin, Earthworm Jim 2) | SEG-081 | 3 | 3 candidate | TAS strong 3,516 / 13,440 / 48,580 | offline M1 diagnostic only | yes (`seg081-v1`) |
| SEG-083 TAS (Shadowrun, Doraemon, The Terminator qualified) | SEG-083 | 3 | 3 candidate | TAS strong 1,431 / 2,397 / 24,844 | offline M1 diagnostic only | yes (`seg083-v1`) |
| SEG-077 Chuck Rock | SEG-077 | 1 title, 3 sample variants | 0 (exploratory) | 277 / 483 / 7,432 (T001); T009 variants superseded | none | no |
| Sealed / reserved cohorts | SEG-072, SEG-075, SEG-077 | 7 + 18 opaque entries in public manifests; 8 reserved titles | n/a | not read | one-shot sealed evaluation only | custodian-controlled |

The SEG-072 registry is 14 Genesis ROM images (D01: Sonic 1, Sonic 2, Sonic & Knuckles built from disassembly projects; D02 Flicky; D03 Shining Force 1 and 2; D04 Phantasy
Star II; D05-D07, D09-D12 SGDK homebrew builds) plus 36 Sonic CD modules assigned to lineage D01 (`kind = scd_module`, ADR 0140 module reuse): **the Sonic CD modules are
not independent engines**; D01 counts once. Label origin of SEG-072 and SEG-075 is independent source-backed truth (assembler listings / reproducible build maps), not
observation. TAS labels are partially observed (E executed strong code, R static reach weak code, H header and M DMA-source strong data, W ordinary read weak data, U unknown)
with no source truth.

## Data flow per model

All models share the sample loader `segarecomp_ml_v3_train.py:Image` (49): bytes, CPU-owned decode tokens (`tokens.bin`, from `m68k-decode-token-report`), per-byte
classes `I/c/D/U`, cell labels (`segarecomp_ml_v3.py:cell_labels` 344: **positive if any `I`, negative if every byte `D`, otherwise masked -1**), word labels
(`word_labels` 358: positive if either byte is `I`/`c`, negative if both `D`, else masked) and seed cells (`seeds.json`). The registry (`registry` 86) is every
`meta.json` carrying an `audit` block; `qualified_lineages` (115) reads the SEG-072 corpus manifest, so a new lineage is invisible to every frozen loader until registered.

| Model | Source / filter | Input features | Target and granularity | Masked / balance / weights | Lineage / split | Frozen configuration | SEG-081/083 labels? |
|---|---|---|---|---|---|---|---|
| **SEG-072 M1-HGB** (`fit_m1` 199) | registry, lineages != held-out | `gen2_train.m1_features` (149): 64-byte block statistics at 128-B cell, 512-B mid and 1024-B context scales: byte histogram entropy, printable fraction, repetition, token histograms. **No seed, no relocation, no control-flow** | 128-byte cell, `labels >= 0` only, positive = any `I` | masked cells dropped; `class_weight="balanced"` over the pooled cells; no sample weights | leave-one-lineage-out (`lolo_folds`); all cells pooled, images and lineages weighted by cell count | `HGB_CONFIG` (gen2_train 44): 200 iterations, depth 4, lr 0.08, `random_state` 49 | **Only in SEG-081/083 offline diagnostics** (the fit is `HistGradientBoostingClassifier(**HGB_CONFIG)` with a `sample_weight` argument added by the diagnostic, not by the model code) |
| **SEG-076 structural model A** (`xplat_models.fit_a` 163) | `xplat_transfer.fold_sets` condition C0 (Genesis lineages != held-out); images loaded by `load` (87) and `xplat_t005.attach` (123) | `a_features` (104): token histograms per cell, ordinary-decode chain lengths, PC-relative direct-control targets (`structure` 45), byte statistics, **relocation count (`relocs.json`), seed flag (`seeds.json`), PROVEN reach from the independent seed addresses** (`d.txt`), plus +/-1 and +/-4 neighbour means; 210 features | 128-byte cell (positive/negative/masked as above) | masked dropped; `class_weight="balanced"`; **no sample-weight path** | strict leave-one-lineage-out; no lineage balancing (cells pooled) | `A_CONFIG` (32): 300 iterations, depth 6, lr 0.05, `random_state` 49; deterministic | **No.** Never trained on a TAS label |
| **SEG-076 B** (`fit_bc` 276, `graph=False`) | same C0 folds | per-word embeddings of bytes (hi/lo) and the 8 token fields, 64-wide encoder (2 layers, 4 heads, ff 128) over 256-word blocks with 64 words of context | per-word logit (word label), cell score = max over its 64 words | `mask = y >= 0`; `pos_weight` = mean over lineages of (negative words / positive words) (`m4_pos_weight` 268); a step whose windows are all masked is skipped | **lineage-uniform sampling**: each step draws a lineage uniformly, then an image by size, then a random window start; M4 recipe, 4000 steps, batch 8, AdamW, seeds 1-3 | `M4` (35) and `B_ARCH` (34) | **No** |
| **SEG-076 C** (`fit_bc` `graph=True`, `window_graph` 248) | same | B plus typed message passing over candidate edges (fall-through, PC-relative branch/call/DBcc, overlap) and a PROVEN flag from `im.seed_addresses` | same as B | same as B | same as B | `B_ARCH` + 2 rounds | **No** |
| **SEG-076 C-v2** (`models_v2.fit_cv2` 120) | same | C's function, deterministic vectorised message passing | same as B | same as B | same as B; ROCm, seeds 1-3 | M4 recipe + `build_cv2` (30) | **No** |
| **SEG-083 model-A diagnostic** (`tasbatch5_ml.cmd_diagnose` 69) | `tr.registry` of the 11 qualified lineages + restored TAS samples | **M1** features (`g2.m1_features`), not A | 128-byte cell | `sample_weight` 1.0 for registry, 1.0 / 0.25 (TW arm) for TAS | leave-one-lineage-out; 12-byte-shingle containment withholding | `g2.HGB_CONFIG` | Yes, offline only |

Seed and relocation dependencies: `seeds.json` (cell indices) and `d.txt` (addresses) of the 11 accepted Genesis ROM images are produced by the production CLI (`emit-general-startup-bridge-c
--reset-entry --direct-control-address-report`, `segarecomp_ml_v3_data.py:cmd_register`) and are label-free; 3 of the 14 Genesis images (D10, D11, D12) have no `d.txt` because the bridge rejected them (the Sonic CD modules take seeds from
`seed_starts.txt`). **The retained TAS samples carry only the reset vector as seed (`seeds_source`)**, so feeding them to A or C
unchanged would give those images a PROVEN-reach distribution unlike every registry image. SEG-084 regenerates the registry-style `d.txt` for each TAS ROM with the same pinned CLI.

Partially observed labels with the existing training functions: A and M1 accept them unchanged (masked cells drop out; a cell with any executed instruction is positive even if the
rest is unknown; a negative needs every byte `D`). B / C / C-v2 accept them unchanged too, but lineage-uniform sampling draws windows over the whole image, so a TAS ROM with
65-97 % masked words yields mostly all-masked windows (skipped) and is under-sampled relative to a fully labelled lineage. Neither `fit_a` nor `fit_bc` has a sample-weight path.

## What would have to change to train A / B / C on the expanded corpus

1. Register the restored TAS samples in a registry directory (an `audit` block in `meta.json`) and extend `qualified_lineages()` (currently the frozen SEG-072 manifest list).
   The frozen SEG-075/076 modules are digest-bound, so this must be a new wrapper, not an edit.
2. Regenerate registry-style seeds (`d.txt`) for each TAS ROM (done in SEG-084) so A's `reach` / seed features and C's PROVEN flag are distributed like the registry's.
3. For A: nothing else (strong-only). For weak labels: a versioned optional `sample_weight` path (SEG-084 adds `seg084.a_weighted.v1`, outside the frozen module).
4. For B / C / C-v2: decide how to sample mostly-masked images (the current lineage-uniform window draw under-samples them) and whether to weight weak words; neither exists today.
5. Persist the training inputs durably (SEG-084-T002): today the SEG-072/075 samples exist only under `.cache` and in local handoff tarballs.
