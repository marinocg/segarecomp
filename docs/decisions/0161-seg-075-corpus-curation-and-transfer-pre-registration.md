# ADR 0161: SEG-075-T003 cross-platform corpus curation, evaluation barrier and pre-registration

- Status: Accepted. **Feasibility gate: T004 admitted. The T006 one-shot evaluation is admitted with credibility LIMITED.**
- Task: SEG-075-T003, combined SEG-075 product PR (checkpoint C3). This pre-registration is committed and pushed, and approved by
  independent adversarial review, before any model of the cross-platform experiments is fitted.
- Frozen record: `tools/segarecomp_xplat_design.plan.json`, digest `tools/segarecomp_xplat_design.plan.digest`
  (`f551fea791c307e15f5203ed30102d1b80db6a583128ae04a82df2565a053ad5`). The tools refuse to run when the plan differs from its digest.
- Tooling:
  - `tools/segarecomp_xplat_design.py` (lineages, freeze, check, and the development API with its barrier);
  - `tools/segarecomp_xplat_transfer.py` (the T004 runner, committed with the freeze and executed only after review);
  - tests in `tests/segarecomp_xplat_design_test.py`.

## 1. Frozen corpus

- Corpus: T002 manifest v2 `d2956dbf3eb11e000bc8fb4fb83b98d548488469b6b0adb912363691c00accb5` (ADR 0160).
- Frozen private snapshot: `<HARNESS_ROOT>/.cache/seg075/t003/snapshot` (`SNAPSHOT.json` `9c074704903ebf77acb32e6585bf529a096de4301f963cb011e0e6f22a43473d`).
  - 36 development samples sit under `dev/`.
  - The 18 samples of the two reserved lineages are copied only into `sealed/`. They are bound by opaque digests in the snapshot
    manifest, and no label statistic beyond the T002 coverage aggregates is published.
  - `check` re-verifies every development file and every sealed binding.
- Genesis: the SEG-072 qualified development registry, 50 images in 11 lineages. Every image is physically available and
  identity-checked, including the 36 Sonic CD modules of D01 restored from the operator's disc (ADR 0160 section 10). Any image unavailable
  at freeze would have been excluded from every condition.

## 2. Lineages, independence and deduplication

Lineage identity is author x toolchain x shared-code family.

Independence was measured between every pair of cross-platform programs, and between each of them and all Genesis development lineages,
with two measures:

- 12-byte shingles of labelled code (SEG-072's measure);
- 8-instruction sequences of (opcode word, length), which are insensitive to relocated addresses.

The maximum pairwise containment is 0.0009, against SEG-072's 0.30 merge threshold. Every program is its own lineage, and none overlaps
Genesis code.

Within a lineage, near-duplicates are reduced deterministically: largest first, an image whose instruction sequences are at least 50%
contained in the kept images is dropped. Only representatives train; near-duplicates are diagnostic only. Raw instruction-start counts
are therefore not presented as diversity.

| lineage | platform | toolchain family | samples | labelled starts | representatives (starts) | size class | role |
|---|---|---|---|---|---|---|---|
| BattleIsleEditor | Amiga | A68K | 12 | 28,521 | 2 (4,860) | medium | development |
| amiga-realtime3d | Amiga | vasm | 9 | 13,126 | 1 (2,349) | medium | development |
| toslibc | Atari ST | GCC 13 | 7 | 14,086 | 2 (4,783) | medium | development |
| EmuTOS | Atari ST (+Amiga config) | GCC 4.6 + gas | 4 | 208,244 | 1 (66,110) | large | development |
| neogeosdk | Neo Geo | GCC 13 | 1 | 93,317 | 1 (93,317) | large | **reserved evaluation** (platform held out) |
| space-invaders | Atari ST | vbcc | 17 | 3,240 | 14 (3,232) | medium | **reserved evaluation** (compiler held out) |
| HelloAmi | Amiga | vasm | 4 | 82 | 2 (82) | small | diagnostics only |

Other exclusions:

- From T001, not used anywhere: six candidates pending licensing review, five commercial-derived ports, three reconstructions, two
  blocked builds, and the toolchain-infrastructure demos.
- The SEG-072 sealed cohort is exposed and is never fresh evidence.
- EmuTOS is one firmware lineage. It is counted as Atari ST in every condition, and its four configurations reduce to one representative.

## 3. Feasibility gate (computed from aggregates at freeze; all checks pass)

| check | result |
|---|---|
| independent lineages (no pair >= 0.30) | yes (max 0.0009) |
| Atari ST training lineages >= 2 | yes (toslibc, EmuTOS) |
| Amiga training lineages >= 2 | yes (BattleIsleEditor, amiga-realtime3d) |
| training toolchain families >= 3 | yes (A68K, vasm, GCC 13, GCC 4.6) |
| unknown fraction <= 0.10 in every used lineage | yes (max 5.1%) |
| mixed code/data present | yes (A68K/vasm programs: data 41-57% inside code hunks) |
| reserved lineages >= 2 | yes |
| reserved set holds a platform held-out and a compiler held-out | yes (Neo Geo; vbcc) |
| Genesis folds intact | yes (50/50 images) |

Evaluation credibility is **LIMITED**. There are only two reserved lineages; one is small and object-only (3,232 starts), and both are
non-Genesis. T006 results are descriptive cross-platform evidence only. There is no Genesis generalization claim: no fresh Genesis
held-out exists, and the SEG-072 sealed cohort is exposed.

What a stronger evaluation would need (not requested; recorded for a future acquisition):

- at least two further independently authored, reproducibly built base-68000 programs per platform, with listings or byte-identical
  source rebuilds;
- in particular an assembler-written Atari ST game, a C-compiled Amiga game (SAS/C or vbcc), and a 68000 program from another compiler
  family (e.g. Lattice/Pure C);
- ideally new open-source Genesis homebrew with source-backed truth, never used by SEG-072.

## 4. Experimental design (consumed by T004-T006)

- **Conditions.** C0 Genesis-only, C1 Genesis + Atari ST (toslibc, EmuTOS), C2 Genesis + Atari ST + Amiga (+ BattleIsleEditor,
  amiga-realtime3d). The conditions are nested. No reserved lineage appears in any condition. Training effort, preprocessing and
  recipes are identical across conditions: the unchanged SEG-072 recipes; only the training image list differs.
- **Weighting.** M3-R samples 32-cell segments uniformly over all images (unchanged recipe), so images under 4 KB add no M3-R segments and
  EmuTOS's 256 KB representative dominates the cross-platform additions. M4-Dense samples lineages uniformly. Every metric is aggregated
  per lineage first, so EmuTOS counts once.
- **Models.**
  - M3_FROZEN: zero-shot reference.
  - M1_HGB: non-neural control, all conditions.
  - M3_R: all conditions, plus a seed-50 replicate of C0 as the noise floor.
  - M4_DENSE: seeds 1-3, cells 128 and 64, conditions C0 and C2.
  - C0 reuse: C0's Genesis fold scores are SEG-072's preserved fold scores, which use the same images, recipe and environment. This is
    admitted only if one M3_R fold and one M4_DENSE seed-1 fold reproduce bit for bit in the rebuilt environment; otherwise every C0 fold
    is retrained.
- **Folds.**
  - `genesis_lolo`: leave one Genesis lineage out, with the condition's cross-platform representatives added to every training set.
  - `xplat_lolo`: for each cross-platform development lineage x, C2-minus-x against C0-all.
- **Metrics.**
  - Primary: the mean over Genesis lineages of the per-lineage max FRF.
  - Co-primary: worst-lineage FRF.
  - Secondary: misses and negative-cell admissions at B35/B50/B60; residual FRF; AUROC; the short/mixed stratum (positive cells with at
    most 8 starts or any data byte); `xplat_lolo` results; and per toolchain family and platform. False positives and false negatives are
    reported separately.
- **Noise and decision.** tau = max(0.01, 2 x the mean |FRF(seed 49) - FRF(seed 50)| over lineages) in C0. If tau > 0.05, the result is
  `EVIDENCE_INSUFFICIENT`. Otherwise, per model:
  - `TRANSFER_HELPS`: d_mean <= -tau, no lineage worse by more than tau, and no extra B50 misses.
  - `TRANSFER_HARMS`: d_mean >= +tau, or any lineage worse by more than 2 tau, or any extra B50 miss.
  - `TRANSFER_NEUTRAL`: otherwise.

  The overall class is that of M3_R in C2; M4_DENSE and C1 corroborate.
- **T005 gating.**
  - Model A (lightweight structural gradient boosting over CPU-owned decode features) is always built on the best transfer condition
    (C2 unless the result is HARMS).
  - Model B (a bounded local instruction-aware Transformer) is built only if the best model's mean FRF exceeds the mean positive-cell
    density by at least 0.05 and T004 is not `EVIDENCE_INSUFFICIENT`.
  - Model C (graph-guided) is built only if B beats A by at least tau without a lineage regressing by more than tau.
  - When a simpler model is within tau and has no extra B50 misses, the simpler model is selected.
- **T006.**
  - One frozen candidate and its comparators (M3_FROZEN, C0-all M3_R) are committed to `segarecomp_xplat_design.frozen.json` before the
    sealed labels are opened.
  - The sealed evaluation runs once on the reserved lineages, with a one-shot marker; a second run is refused.
  - Economics: SEG-074's exact-code benchmark applies to Genesis development images only (development evidence); it is never run on the
    reserved cross-platform lineages.
  - Outcome classes: `CORPUS_IMPROVES_EXISTING`, `LIGHTWEIGHT_STRUCTURAL_JUSTIFIED`, `LOCAL_TRANSFORMER_JUSTIFIED`,
    `GRAPH_TRANSFORMER_JUSTIFIED`, `CORPUS_EXPANSION_ONLY`, `DATA_INSUFFICIENT`, `NO_ECONOMIC_IMPROVEMENT`. The simplest class supported
    by development evidence and not contradicted by the one-shot result is chosen. No production model is promoted.
- **Backend.** CPU, per SEG-072's benchmark: ROCm was 8.9% faster, below the 25% rule, so it is not re-benchmarked. The environment is
  the rebuilt research venv. Parallelism: M3_R 4x4 threads, M4_DENSE 8x2 threads.

## 5. Barrier

`segarecomp_xplat_design.dev_sample_dirs` and `refuse_sealed` stop development, tuning and selection code from opening:

- the `sealed/` directory;
- any reserved lineage;
- any sealed sample id.

The T004 runner loads cross-platform data only through this API. The T002 corpus directory still holds the unsealed copies (it is the
extraction output). Development code never reads it; it reads the snapshot. The sealed labels are opened once, by T006, after the frozen
candidate is committed.
