# ADR 0173: SEG-077 A/B/C-v2 retraining, development and blind evaluation: pre-registration

- Status: accepted (pre-registration; frozen before the first SEG-077 fit and before any blind exposure)
- Task: SEG-077-T005, checkpoint C5 of the combined SEG-077 PR
- Plan: `tools/segarecomp_seg077.plan.json` + `.digest` (binds the runner, the frozen SEG-075/076 model, evaluation and stability code,
  the evidence labeller and its plan digest, the SEG-076 comparator arrays and weights, the qualified corpus identities, the blind cohort
  identities, the resource policy, the decision rules and the 113-job matrix)
- Runner `tools/segarecomp_seg077.py`; tests `tests/segarecomp_seg077_test.py`

## Context

SEG-077-T002 (ADR 0172) bound the expansion gate `STOP`. Exactly one new commercial development lineage qualified: G077-01 (Chuck Rock).
It has sound but partial evidence-tier labels: 255 positive, 276 negative and 7,661 masked cells. The milestone must still retrain A, B
and C, compare them, measure economics where justified, and record an independently validated decision. It does so on the corpus that
actually qualified.

## Decision

### Corpus, conditions and folds

- **K0** is the SEG-076 C0 training set: the 11 SEG-072 Genesis development lineages, 50 images, complete source-backed truth.
- **K1** is K0 plus G077-01.
- **Folds.** Development folds are SEG-075's unchanged leave-one-lineage-out folds over the 11 Genesis lineages. They are scored on
  complete truth, directly comparable with SEG-076. A new lineage is always training data, never a Genesis fold.
- **`all` fits** train on every condition image. They are the models the blind cohort sees. They also score G077-01, out-of-sample for
  K0 and in-sample (flagged) for K1.
- **Learning curve.** The 4/8/12/16-game learning curve is infeasible and is recorded as such.

### Labels, seeds and graph integrity (leakage)

- **Masked cells never train.** Masked cells (U) never enter any loss: A uses `labels >= 0`, and B and C-v2 use word labels `>= 0`.
- **Sampling caveat.** The frozen M4 lineage-uniform sampler gives the mostly masked G077-01 1/12 of the windows. This is disclosed and
  not changed.
- **Seeds.** Commercial images get their seeds from the cartridge header only (the reset-vector cell), never from labels. The same rule
  applies to the blind cohort.
- **Graph integrity.** C-v2's candidate edges and PROVEN flag come only from CPU-owned decode tokens, instruction words and these
  label-free roots (`segarecomp_xplat_models.window_graph`, unchanged).
- **Leakage proof.** `segarecomp_seg077_test.py` replaces `classes.bin` and checks that A's features, the seed set and C's candidate
  graph stay bit-identical.

### Models, seeds, reuse and resources

- **Models.**
  - A: `fit_a`/`score_a`, CPU venv identical to SEG-076.
  - B: `fit_bc_device`, ROCm.
  - C-v2: `fit_cv2`, ROCm, bitwise deterministic in SEG-076-T005.
  - Recipes are unchanged. B and C-v2 use matched seeds 1-3.
- **K0 reuse.** SEG-076's K0 arrays (A; B seed 1 from the ROCm amendment; C-v2 seeds 1-3; their `all` weights) are admitted only if
  `reuse` refits A.K0.D05, B.K0.D05.s1 and CV2.K0.D05.s1 under this plan and all three are bitwise identical. Otherwise the K0 refits
  require a recorded amendment.
- **Job matrix (113 jobs).**

  | Group | Fits |
  |---|---|
  | Reuse checks | 3 |
  | A K1 | 12 |
  | B and C-v2 K1 | 72 |
  | B K0 seeds 2-3 | 24 |
  | C-v2 K0 `all` seeds 2-3 | 2 |

- **Runtime/memory preflight** (K1 D05 training set, 150-step probes):
  - B: 0.049 s/step, about 3.3 min per fit.
  - C-v2: 0.342 s/step, about 23 min per fit.
  - About 0.9 GB GPU and 3.6 GB host RSS per process.
- **Resources.** The resource policy (3 parallel GPU fits, caps of 60/150 min, a 4 GB RSS limit, a host-memory guard, a single attempt,
  and failures recorded rather than retuned) follows SEG-076's measured practice.

### Development rules (T006)

- **ER definition.** Genesis ER is the SEG-075 definition: the mean over lineages of the lineage maximum image ER. B and C-v2 are scored
  on the 3-seed word-logit-mean ensemble, cell max at 128 bytes, and per seed for variance.
- **Per model.** Compare K1 with K0 by the paired lineage differences. `IMPROVED` means mean d <= -0.005 with sign-flip p <= 0.05;
  `WORSENED` is symmetric; anything else is `NO_CHANGE`.

### Blind protocol and rules (T007)

- **Cohort.** The reserved 8 are pinned by size and SHA-256 (`blind_cohort` in the plan; Puyo Puyo (Japan) and Zombies Ate My Neighbors
  (USA) were provided by the operator).
- **Custodian/evaluator boundary.** A custodian session runs the frozen evidence protocol (plan `03ebc8f8...`) into a sealed private
  directory. The evaluator scores with the frozen models only after `tools/segarecomp_seg077.models.json` (the model digests) is
  committed. Labels are read only by `blind-table`. Nothing changes after exposure.
- **Metrics.** They use labelled cells only: labelled ER (masked cells are excluded), plus B35/B50/B60 misses of labelled starts under
  SEG-072's unchanged budget regions. A title is informative when it has at least 50 labelled positive and 50 labelled negative cells.
  Incomplete truth is reported as incomplete.
- **Neural advantage** of X (B or C-v2, K1 ensemble) over A_K1 requires all of:
  - blind mean labelled ER at least 0.01 lower;
  - lower on at least 75% of the informative titles;
  - no more blind B50 misses;
  - development mean ER at least 0.005 lower, with sign-flip p <= 0.05;
  - every seed's development mean ER below A's.
- **Class precedence:**
  1. `LABEL_TRUTH_INSUFFICIENT`: fewer than 4 informative blind titles.
  2. `DATA_INSUFFICIENT`: fewer than 4 qualified new development lineages. The model comparison is still run and reported.
  3. `ECONOMICS_NOT_JUSTIFIED`: a neural advantage without at least 10% measured savings.
  4. `C_ADVANTAGE` / `B_ADVANTAGE`.
  5. `EQUIVALENT_WITH_COST_DIFFERENCE`.
  6. `A_PREFERRED`.
- **Economics.** They are measured with SEG-076's unchanged C5 bench only for a K1 model whose development mean ER beats SEG-076's A by
  at least 0.01. Otherwise they are reported as not measured, with the reason.
- **Adaptive budget.** There is no separately validated recall-safe adaptive policy for these models, so only fixed budgets are used.

## Consequences

- **The class is constrained before exposure.** With one qualified new lineage the precedence yields `DATA_INSUFFICIENT` unless the blind
  truth is itself insufficient. This follows from facts fixed before any fit (ADR 0172), and the full A/B/C comparison is still run and
  reported descriptively.
- **Cost.** The study costs about 7 hours of GPU wall time (3 parallel fits) plus the custodian's roughly 9 CPU-hours per blind title.
