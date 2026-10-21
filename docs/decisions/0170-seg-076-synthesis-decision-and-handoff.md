# ADR 0170: SEG-076 synthesis: exploratory M68K code-recognition architectures, M3-R stability and compilation economics

- Status: accepted (exploratory, development-only research decision; no production change)
- Task: SEG-076-T004, checkpoints C5 (economics), C6 (synthesis) and C7 (verified handoff) of the combined SEG-076 PR
- Inputs:
  - ADR 0164/0165: T001, the A/B/C pilot and its ROCm amendment
  - ADR 0166/0167: T003, M3-R stability and transfer
  - ADR 0168/0169: T005, deterministic C-v2 multi-seed
  - `tools/segarecomp_xplat_economics.{plan,results}.json`: C5
- These are SEG-076 classifications. They do not replace SEG-075's `DATA_INSUFFICIENT` (ADR 0163).
- The SEG-075 reserved cohort (neogeosdk, space-invaders) is still sealed and unopened. Every result here comes from development folds
  (lineage-held-out). No evidence comes from unseen lineages (tier 3).

## The question

Does structural ML (A), a local Transformer (B) or graph-guided recognition (C) rank difficult Genesis executable fragments well enough
to reduce retained regions and actual compilation cost, without losing verified instruction starts? And was training-seed instability
hiding real progress?

## Separate classifications

| subject | SEG-076 class | basis (tier) |
|---|---|---|
| 1. Structural baseline A | `LIGHTWEIGHT_MODEL_PREFERRED` | T001, T003 and T005 (tier 2, development folds) |
| 2. Local Transformer B | `NO_MEANINGFUL_ADVANTAGE` | T001 pre-registered `NEGATIVE_STOP`; T002 not admitted (tier 2, single seed) |
| 3. Graph-guided Transformer C | `EVIDENCE_INSUFFICIENT` | T005 pre-registered class (tier 2, 3 seeds); descriptively not better than A |
| 4. M3-R seed stability | `NO_MEANINGFUL_ADVANTAGE` | T003 frozen rule; 4-seed ensembles help descriptively (tier 2) |
| 5. Cross-platform transfer | `EVIDENCE_INSUFFICIENT` | M3-R paired 4-seed `EVIDENCE_INSUFFICIENT`; A `NO_MEANINGFUL_ADVANTAGE` (tier 2) |
| 6. M1-HGB descriptive transfer | `NO_MEANINGFUL_ADVANTAGE` | T003 `DESCRIPTIVE_EFFECT_NOT_ROBUST` (tier 1 reanalysis) |

### 1. A, the structural gradient boosting model: `LIGHTWEIGHT_MODEL_PREFERRED`

- A is the best development-fold model measured in SEG-075 or SEG-076:
  - Genesis mean ER 0.105, with 0 B50 misses, 0 boundary-stratum misses and worst lineage 0.211;
  - it beats M3-R seed 49 (0.146) on 9 of 11 lineages;
  - it beats the M3-R 4-seed ensemble (0.117) and every C-v2 seed (0.150 to 0.218).
- It is deterministic, so it has no seed noise. It costs about 1 CPU-minute per fit, and inference takes about 0.2 s per image.
- Compared with C, the gap to the C-v2 3-seed ensemble (0.127) is concentrated in one lineage (D03). Elsewhere the ensemble roughly matches
  A. A's lead over C is therefore mostly cost and robustness, not a large accuracy margin.
- **Recommended for a confirmatory study** (the only SEG-076 candidate recommended).

### 2. B, the local instruction-aware Transformer: `NO_MEANINGFUL_ADVANTAGE`

- Under the pre-registered pilot rule, B fails every criterion against A: mean ER 0.155 and 117 B50 misses on CPU.
- B is unstable across devices: the same seed scores 0.199 with 1 miss on ROCm.
- SEG-076-T002 (B/C multi-seed) was not admitted and is cancelled. One seed cannot show that B is inherently worse.

### 3. C, the graph-guided Transformer: `EVIDENCE_INSUFFICIENT`

- The frozen C could not complete under the pilot's CPU caps. In the ROCm amendment it scored 0.116, but it was not run-to-run
  deterministic.
- The deterministic C-v2 is equivalent to C within 7e-7 and about 4x faster.
  - Seeds 1-3 score 0.150, 0.218 and 0.154. The 95% interval of C-v2 minus A is [-0.026, +0.163], so the pre-registered class is
    `EVIDENCE_INSUFFICIENT`.
  - Descriptively, every seed and the ensemble are worse than A, mostly on D03.
- The original 0.116 was likely a favourable draw.
- Graph guidance helps the Transformer relative to B, but it does not beat the lightweight model on these folds. It costs about 1.6
  CPU-hours per fit even with GPU training.

### 4. M3-R seed stability: `NO_MEANINGFUL_ADVANTAGE`

- Seed noise is much larger than SEG-075's two-seed estimate:
  - Genesis mean ER spans 0.146 to 0.221 (C0) and 0.122 to 0.232 (C2);
  - B50 misses range from 0 to 39;
  - tau4 is 0.15 (C0) and 0.16 (C2).
- Seed 49, SEG-072's recipe seed, is the best of the four draws in both conditions.
- The instability is ranking-tail variance on held-out D01 and D03. It is not optimization failure.
- 4-seed logit ensembles beat every single seed (0.117 / 0.107, with 0 misses). But the frozen pair-level rule failed in C2 by one pair,
  so the class stays `NO_MEANINGFUL_ADVANTAGE`.
- Seed instability was obscuring comparisons. Single-seed M3-R numbers, including SEG-072's, are not a reliable baseline.

### 5. Cross-platform transfer: `EVIDENCE_INSUFFICIENT`

- M3-R C2 minus C0, paired over 4 seeds: +0.005 [-0.050, +0.059].
- A, deterministic: C2 minus C0 is -0.003 (p = 0.89).
- No measurable transfer effect at this corpus size. SEG-075's seed-49 benefit does not persist.

### 6. M1-HGB's descriptive transfer: `NO_MEANINGFUL_ADVANTAGE`

- The -0.028 comes about 93% from D01. Of the other lineages, 3 are better, 5 are worse and 2 are unchanged.
- Bootstrap CI [-0.080, +0.002], p = 0.46.
- This is a tier 1 reanalysis, not confirmation.

## Compilation economics (C5, through SEG-074's unchanged benchmark)

Setup:

- 11 gate images, cold cache, clang 18.1.3 `-O2`.
- Every admission at B50 was accepted, and every accepted build was runtime-equal to BROAD.
- Lineage-mean total-build-CPU ratio to the same-session BROAD; 11-image totals in parentheses:

| strategy | lineage-mean ratio | 11-image total (CPU-s) |
|---|---|---|
| M3-R seed 49 B50 (saved) | 0.626 | 2304 |
| A B50 | 0.733 | 2227 |
| C-v2 ensemble B50 | 0.745 | 2297 |
| M3-R 4-seed ensemble B50 | 0.602 | 2301 |

Findings:

- **Better ranking does not reduce compilation cost at a fixed budget.**
  - Every model admits the same 35,840 cells at B50.
  - A's better-ranked region contains more real code, so it compiles at least as much. Its lineage-mean ratio is worse, and its total is
    only 3% lower.
  - Lower ER at full recall pays off only if the budget shrinks.
- **Truth-informed full-recall regions show the potential, but these are diagnostics.**
  - A's total is 17% below M3-R's (1136 against 1375 CPU-s), mostly on D03.
  - The unchanged admission validator rejected 3 of A's 11 full-recall regions and 2 of M3-R's.
  - The lineage means (0.580 against 0.523) are dominated by those broad fallbacks.
  - A smaller, recall-safe budget would need a validated adaptive admission policy. That is not available here.
- **Analysis and inference CPU are negligible next to build CPU** for A: about 1 CPU-minute per fit, plus about 0.2 s per image. They are
  not negligible for C-v2: about 1.6 CPU-hours per fit.
- Context: SEG-074's frozen production INC (M3 + B35) remains the cheapest deployed route in its own session (2063 CPU-s over 11
  images).

Answer to the question: **no candidate reduces actual compilation cost at the existing budget.** A ranks difficult fragments better
(lower ER, no misses, deterministic, cheap). Turning that into compile savings needs a budget policy, not a bigger model.

## Recommendation (no automatic promotion)

- **One confirmatory study is justified: model A**, with:
  - a pre-registered, recall-safe adaptive budget (calibrated retention below B50);
  - SEG-074's economics, measured on that budget;
  - evaluation on genuinely unseen lineages. The SEG-075 reserved cohort is still sealed and could serve as a one-shot under a new
    pre-registration, alongside new source-backed programs.
- Not recommended for more compute on the current development folds: B, C / C-v2 multi-seed, M3-R C2 transfer, or M1 transfer.
- Any future M3-R comparison must use multiple seeds or an ensemble. Single-seed M3-R baselines, including the production-era seed 49,
  are not reliable.
- No production model, retention budget, default, validator, launcher or runtime change follows from SEG-076.

## Handoff (C7)

- The private store `<HARNESS_ROOT>/.cache/seg075-handoff` gets a new manifest label chained on SEG-075's `c7-final-handoff`. It covers
  every SEG-076 private tree:
  - plans' private outputs, scores, every fitted weight file, fit records and CPU timing, farm states, logs;
  - the C5 economics records and the environment snapshot.
- `tools/segarecomp_xplat_seg076_final.py verify` checks it in a fresh process with the task worktree unavailable:
  - physical rehash;
  - bitwise regeneration of an A prediction (CPU) and a C-v2 prediction (ROCm) from restored weights;
  - re-derivation of all 8 committed SEG-076 classes;
  - the SEG-075 sealed cohort confirmed still unopened.
- The committed receipt carries aggregates and digests only.
