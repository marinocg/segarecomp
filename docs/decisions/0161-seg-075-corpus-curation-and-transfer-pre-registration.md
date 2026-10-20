# ADR 0161: SEG-075-T003 cross-platform corpus curation, evaluation barrier and pre-registration

- Status: Accepted. **Feasibility gate: T004 admitted. The T006 one-shot evaluation is admitted with credibility LIMITED.**
- Task: SEG-075-T003, combined SEG-075 product PR (checkpoint C3). This pre-registration is committed and pushed, and approved by
  independent adversarial review, before any model of the cross-platform experiments is fitted.
- Frozen record: `tools/segarecomp_xplat_design.plan.json`, digest `tools/segarecomp_xplat_design.plan.digest`
  (`fde026ee1075d0c2a2f71679a4a40bdf9cffe7376baabf7b7982685d2915dd92`).
  - It supersedes two unused earlier freezes, `f551fea7…` and `9c4b166d…` (section 6). No model was fitted under either.
  - The plan binds by digest:
    - the T004 runner, the design module and the decision module;
    - the SEG-072 code they call: `segarecomp_ml_v3{,_train,_eval}.py` and `segarecomp_ml_region_gen2{,_train}.py`;
    - SEG-074's economics benchmark `segarecomp_oracle_bench.py`;
    - the 61 preserved SEG-072 / SEG-049 score files that may be reused: C0 folds and M3_FROZEN.

    Every tool refuses to run when any of them, or the plan, drifts.
- Tooling:
  - `tools/segarecomp_xplat_design.py` (lineages, freeze, check, and the development API with its barrier);
  - `tools/segarecomp_xplat_transfer.py` (the T004 runner: reproduce, fit, jobs, table);
  - `tools/segarecomp_xplat_decide.py` (pure, pre-registered decisions: selection, the strict better-than relation, budget rank,
    contradiction, economics gate, outcome classes);
  - tests in `tests/segarecomp_xplat_design_test.py`.

## 1. Frozen corpus

- Corpus: T002 manifest v2 `d2956dbf3eb11e000bc8fb4fb83b98d548488469b6b0adb912363691c00accb5` (ADR 0160).
- Frozen private snapshot: `<HARNESS_ROOT>/.cache/seg075/t003/snapshot` (`SNAPSHOT.json`
  `74799cca5f87f263f7ea3550c385cc050cc57941e33fceb8a46399a136080086`).
  - 36 development samples sit under `dev/`, with every file digest-bound.
  - The 18 samples of the two reserved lineages exist only in `sealed/`, bound by opaque digests. No label statistic beyond the T002
    coverage aggregates is published.
  - The unsealed copies the T002 extraction wrote were moved out of the corpus directory into a mode-000 directory. The handoff store
    also holds copies, as preservation, never development input.
- Genesis: the SEG-072 qualified development registry, 50 images in 11 lineages, all physically present and identity-checked. This
  includes the 36 Sonic CD modules of D01, restored from the operator's disc (ADR 0160 section 10).

## 2. Lineages, independence and deduplication

Lineage identity is author x toolchain x shared-code family.

Independence was measured between every pair of cross-platform programs, and between each of them and all Genesis development lineages,
with two measures:

- 12-byte shingles of labelled code (SEG-072's measure);
- 8-instruction sequences of (opcode word, length), which are insensitive to relocated addresses.

The maximum pairwise containment is 0.0009, against SEG-072's 0.30 merge threshold. The review independently recomputed this: reserved
images contain at most 10 development code shingles, toslibc and neogeosdk share no meaningful runtime code, and Genesis versus
neogeosdk is 7e-5.

Within a lineage, near-duplicates are reduced deterministically: largest first, ties broken by id. An image whose instruction sequences
are at least 50% contained in the kept images is dropped; the result is stable for thresholds 0.4-0.68. An image with labelled code
but no 8-instruction run cannot be shown to be a duplicate and is kept. Only representatives train.

| lineage | platform | toolchain family | samples | labelled starts | representatives (starts) | role |
|---|---|---|---|---|---|---|
| BattleIsleEditor | Amiga | A68K | 12 | 28,521 | 2 (4,860) | development |
| amiga-realtime3d | Amiga | vasm | 9 | 13,126 | 1 (2,349) | development |
| toslibc | Atari ST | GCC 13 | 7 | 14,086 | 2 (4,783) | development |
| EmuTOS | Atari ST (+Amiga config) | GCC 4.6 + gas | 4 | 208,244 | 1 (66,110) | development |
| neogeosdk | Neo Geo | GCC 13 | 1 | 93,317 | 1 | **reserved evaluation** (platform held out) |
| space-invaders | Atari ST | vbcc | 17 | 3,240 | 16 | **reserved evaluation** (compiler held out) |
| HelloAmi | Amiga | vasm | 4 | 82 | 2 | diagnostics only |

T001 exclusions (licensing review, commercial-derived, reconstructions, blocked, infrastructure) and the exposed SEG-072 sealed cohort
are never used. EmuTOS is one firmware lineage. It counts as Atari ST in every condition, and its four configurations reduce to one
representative.

## 3. Feasibility gate (aggregates at freeze; all checks pass)

| check | result |
|---|---|
| independent lineages (no pair >= 0.30) | yes (max 0.0009) |
| Atari ST / Amiga training lineages >= 2 each | yes / yes |
| training toolchain families >= 3 | yes (A68K, vasm, GCC 13, GCC 4.6) |
| unknown fraction <= 0.10 in every used lineage | yes (max 5.1%) |
| mixed code/data present | yes |
| reserved lineages >= 2, with a platform held-out and a compiler held-out | yes (Neo Geo; vbcc) |
| Genesis folds intact | yes (50/50 images) |

Evaluation credibility is **LIMITED**:

- neogeosdk holds out the platform only; its compiler family (GCC 13) is in training via toslibc.
- space-invaders holds out the vbcc compiler, but shares the vasm assembler and the Atari ST platform with training.
- Both reserved lineages are non-Genesis, and one is small and object-only.
- T006 results are therefore descriptive cross-platform evidence. There is no Genesis generalization claim.

A stronger evaluation would need:

- at least two further independently authored, reproducibly built base-68000 programs per platform, with listings or byte-identical
  rebuilds;
- in particular an assembler-written Atari ST game, a C-compiled Amiga game (SAS/C or vbcc), and a program from another compiler family
  (Lattice/Pure C);
- new open-source Genesis homebrew with source-backed truth.

## 4. Experimental design (consumed by T004-T006)

- **Conditions.** C0 Genesis-only, C1 Genesis + Atari ST (toslibc, EmuTOS), C2 Genesis + Atari ST + Amiga (+ BattleIsleEditor,
  amiga-realtime3d). The conditions are nested, and no reserved lineage appears in any of them. Recipes, effort and preprocessing are
  SEG-072's, unchanged; only the training list differs.
- **Models.**
  - M3_FROZEN: zero-shot reference, tabulated.
  - M1_HGB: all conditions.
  - M3_R: all conditions, plus a C0 seed-50 replicate.
  - M4_DENSE: seeds 1-3 at cells 128 and 64, conditions C0 and C2.
  - C0 reuse: C0 Genesis fold scores are SEG-072's preserved scores only if `reproduce` retrains one fold each of M3_R, M4_DENSE seed 1
    and M1_HGB under this plan digest and all three match bit for bit (`reproduce.json`). Otherwise all C0 folds are fitted.
- **Folds.**
  - `genesis_lolo`: leave one Genesis lineage out (11 lineages), with the cross-platform representatives added to training.
  - `xplat_lolo`: C2-minus-x versus C0-all, scored on x's representatives.
- **Decision metric: excess retention ER.** ER = (cells needed for full recall - positive cells) / (cells - positive cells), defined on
  informative images (at least one positive and one non-positive cell).
  - It replaces FRF, which is 1.0 by construction on all-positive images (for example D01's small modules, toslibc's WINDOW.PRG and
    BattleIsleEditor's 2-cell hunk), so FRF would make the worst-lineage metric constant.
  - Primary: the mean over the 11 Genesis lineages of the per-lineage max ER.
  - Co-primary: the worst-lineage ER.
  - Secondary, reported with false negatives and false positives separately:
    - FRF;
    - per-lineage misses and negative cells admitted at B35/B50/B60;
    - residual ER;
    - AUROC;
    - the short/mixed stratum;
    - `xplat_lolo` ER and misses;
    - trivial-image counts;
    - results per toolchain family and per platform.
  - Seed cells come from seed addresses at each cell size (SEG-072's convention).
  - Aggregation is per lineage first.
  - On cross-platform code-dense images, fixed budgets necessarily miss starts (most cells are positive). There, fixed-budget misses are
    comparative only, and ER carries the comparison.
- **Noise.** tau = max(0.01, 2 x the mean |ER_l(seed 49) - ER_l(seed 50)|) for M3_R C0. The per-lineage B50 miss allowance is
  a_l = |misses_l(seed 49) - misses_l(seed 50)|, applied to every model. The outcome is `EVIDENCE_INSUFFICIENT` if tau > 0.05 or any
  planned fit is missing or failed; the table checks completeness over all 11 lineages and all seeds.
- **Transfer rule (per model).** With d_l = ER difference and e_l = max(0, extra B50 misses - a_l):
  - `HELPS`: d_mean <= -tau, every d_l <= tau, and every e_l = 0.
  - `HARMS`: d_mean >= tau, or any d_l > 2tau, or any e_l > 0.
  - `NEUTRAL`: otherwise.
  - The overall class is that of M3_R in C2. M4_DENSE, M1_HGB and C1 corroborate. Amiga attribution relies on M4_DENSE (section 5).
- **Selection** (`decide.select`, used for T005 gating and for selecting the T006 candidate) uses nested pools, which makes it
  transitive:
  1. keep the configurations whose Genesis B50 misses are within the allowance sum of the minimum;
  2. of those, keep the ones whose Genesis mean ER is within tau (<=) of the minimum;
  3. of those, keep the ones whose `xplat_lolo` mean ER is within tau (<=) of the minimum;
  4. take the simplest: M1_HGB < A < M3_R < M4@128 < M4@64 < B < C.

  Rules for values and conditions:
  - A None value ranks after every value on its key.
  - Each configuration enters in its own best transfer condition: C2 if its class is HELPS or NEUTRAL, else C0.
  - M3_FROZEN is a reference and comparator only. It cannot be refit and is lineage-correlated on D01/D03 (ADR 0153).

  Improvement over the comparator uses the strict `decide.better_than`: fewer misses by more than the allowance sum; or not more by more
  than it and ER lower by more than tau; or ER within tau and `xplat_lolo` ER lower by more than tau. The complete-budget rank is
  B35 < B50 < B60 < none.
- **T005.** Models A/B/C train on C2 unless T004 is HARMS (then C0), on the same folds. They use fixed configurations with no
  hyper-parameter search, so nothing is tuned on the folds being compared.
  - **A:** a gradient-boosted model over per-cell aggregates of the CPU-owned decode tokens, ordinary-decode chain statistics, computed
    direct-control targets, byte context, relocation-field counts, the seed flag, and the same aggregates over neighbouring cells. Always
    built.
  - **B:** a bounded local instruction-aware Transformer (64-wide, 2 layers, 4 heads, 256-word windows plus 64 words of context), trained
    on M4's exact budget and recipe. Built only if `decide.select` over {M1, A, M3_R, M4@128, M4@64} returns Genesis mean ER >= 0.10.
  - **C:** B plus typed sparse message passing over candidate fall-through, direct-target and overlap edges, and a PROVEN
    seed-reachability flag. Hypothetical edges never mark a position reachable. Built only if B beats A by tau.
  - The A/B/C code is bound by digest in a reviewed T005 pre-fit addendum before any fit.
  - The T005 runner must refuse an A/B/C fit without that addendum digest and stamp the digest into every fit record. Implementation
    choices not fixed here, such as neighbour aggregates as means, are closed by binding the code, not by trial fits.
- **T006.**
  - The ordering's best configuration at B50, with comparators M3_FROZEN and C0-all M3_R+B50, is committed before the seal is opened.
  - It is refit on all development data and evaluated once on every sealed sample that has labelled starts, with a one-shot marker.
  - Confident starts are I-labelled starts.
  - Economic measure: the complete budget, i.e. the smallest of B35/B50/B60 whose Genesis misses fall within the summed allowance. A
    smaller complete budget than C0 M3_R is confirmed only if SEG-074's unchanged exact-code benchmark shows at least 10% less generated C
    and at least 10% less compile CPU on the Genesis development images it supports.
  - Outcome predicates are applied in order (`decide.outcome`, plan `decisions.t006.outcome_rule`):
    1. `DATA_INSUFFICIENT`.
    2. Contradiction by the one-shot evaluation: more reserved B50 misses on any reserved lineage than the comparator, or reserved ER
       (per-lineage max, then the mean over the two lineages) worse by more than tau.
    3. `CORPUS_IMPROVES_EXISTING`, `LIGHTWEIGHT_STRUCTURAL_JUSTIFIED`, `LOCAL_TRANSFORMER_JUSTIFIED` or `GRAPH_TRANSFORMER_JUSTIFIED`, by
       candidate type: better than the comparator, economically confirmed and not contradicted. An existing architecture trained on
       Genesis only (C0) that satisfies this is a re-selection outside SEG-075's question; it is classified `NO_ECONOMIC_IMPROVEMENT`
       with a note.
    4. `NO_ECONOMIC_IMPROVEMENT`.
    5. `CORPUS_EXPANSION_ONLY`: no Genesis gain, but `xplat_lolo` ER improves by tau.
    6. Otherwise `NO_ECONOMIC_IMPROVEMENT`.

    No production model is promoted.
- **Backend.** CPU, per SEG-072's benchmark (ROCm +8.9%, below its 25% rule). The rebuilt research venv uses the recorded versions.

## 5. Disclosures

- **M3_R is mostly an EmuTOS test.** M3_R samples 32-cell segments uniformly over images. The cross-platform representatives contribute
  about 64 EmuTOS segments, about 9 Amiga and about 3 toslibc. M3_R's C2 class is therefore essentially an EmuTOS test, and C1-versus-C2
  (Amiga) attribution can only come from M4_DENSE, which samples lineages uniformly. "EmuTOS counts once" holds for metrics, not for M3_R
  training.
- **The noise rule is conservative.** It rests on one seed pair with a 0.01 floor, so it is biased toward NEUTRAL/HARMS. Fits are
  deterministic and C0 seed 49 is fixed, so tau cannot be adjusted.
- **Commensurability and provenance.**
  - M4_DENSE@64's ER and complete budget are measured in 64-byte cells, finer than the others, yet ranked on the same keys as
    pre-registered.
  - `seed_addresses` were added to the corpus metadata after T002's manifest. They are not part of the manifest digest, but the
    snapshot binds every file.
- **M3_FROZEN sanity check.** The runner's metric code reproduces SEG-072's evaluator: M3_FROZEN has 2 Genesis B50 misses, as SEG-072
  recorded. Four D01 modules are trivial (all positive) and are excluded from ER.

## 6. Barrier, and the superseded freeze

The barrier is **code plus procedure, not filesystem secrecy**. The tools run as root, so file modes protect nothing. The reserved
programs' public sources and T001 build trees exist locally (`seg075/q/{src,build}`), and the handoff store holds copies.

What is enforced:
- Every unsealed reserved copy produced by SEG-075 is moved into one quarantine directory: the T002 extraction, the truth listings, and
  the scratch re-extractions of development and review.
- Development code reads cross-platform data only through the snapshot API.

`dev_sample_dirs` / `refuse_sealed` resolve paths, including symlinks, and refuse:

- any path component equal to `sealed`;
- any reserved lineage name;
- any sealed sample id (also with the `#`/`@` section suffix).

The T004 runner loads cross-platform data only through this API. The sealed labels are opened once, by T006, after the frozen candidate
is committed.

Two earlier freezes were reviewed APPROVE_WITH_CONDITIONS before any fit, so no model has been trained under either.

The first, `f551fea7…` (e805b8e), had these defects:

- degenerate FRF;
- no miss allowance;
- selection, economics and outcome rules left open;
- the runner was not bound to the plan, and C0 reuse was not gated;
- representatives omitted images without an 8-instruction run;
- string-matching gaps in the barrier.

The second, `9c4b166d…` (fe68dd3), had these defects:

- the selection rule was not transitive and was not implemented in bound code;
- M3_FROZEN was in the candidate pool;
- reserved material was readable outside the quarantine;
- the SEG-072 score files and the benchmark were not bound.

All of these are corrected here, and the plan records both superseded digests.
