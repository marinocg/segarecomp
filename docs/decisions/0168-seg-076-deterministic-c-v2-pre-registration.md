# ADR 0168: SEG-076 deterministic C-v2 and matched multi-seed C vs A: pre-registration

- Status: accepted (pre-registration; frozen before the first C-v2 fit)
- Task: SEG-076-T005, checkpoint C5a of the combined SEG-076 PR (operator-directed)
- Plan: `tools/segarecomp_xplat_cv2.plan.json` + `.digest`. Equivalence evidence: `tools/segarecomp_xplat_cv2.equivalence.json`

## Context

In T001's ROCm amendment (ADR 0165), the graph-guided Transformer C was the second-best model: Genesis mean ER 0.116 against A's 0.105,
with 0 B50 misses. A post-hoc refit showed that the frozen C is **not run-to-run deterministic on ROCm**: D05 ER was 0.2053 in one run and 0.2086 in
another. The root cause is **not established**. The installed torch documents `index_add` as deterministic under
`use_deterministic_algorithms` on CUDA, and C-v2 shares C's encoder layers and the backward pass of its gather. So the C result was one non-reproducible single-seed draw.

The operator authorized a follow-up, delivered as a separate task. It does not reverse T001's pre-registered `NEGATIVE_STOP` or the
cancellation of SEG-076-T002.

## Decision

### C-v2 is an implementation revision only

`tools/segarecomp_xplat_models_v2.py` keeps the following unchanged: the network function, its parameters (a frozen C `state_dict` loads
directly), the M4 training recipe, the candidate edges and the PROVEN flag. The message passing is re-implemented:

- the edge index is built once per forward pass;
- **one** padded in-edge table covers all three edge types, with sources in a stable sorted order;
- aggregation is a gather plus a masked sum over a fixed axis, written with a unique-index copy.

The frozen `segarecomp_xplat_models.py` is untouched. C-v2's run-to-run determinism is an **empirical** claim. It rests on the
150-step probe and on the pre-registered full-length D05 refit. If that refit is missing or not bitwise identical, the result is
`EVIDENCE_INSUFFICIENT`. The refit runs inside the 3-way parallel farm, so it also tests determinism under concurrency.

### Evidence before the freeze

- **CPU equivalence** (4 real training batches, identical weights): logits agree within 4.8e-7 and every parameter gradient within
  6.7e-7 relative (tolerances 1e-5 and 1e-4). The function is the same; only the float summation order differs. This check ran on the
  research venv's torch 2.14 on CPU, while the fits run on ROCm torch 2.6.
  - The independent C5a review reproduced it with adversarial edges: cross-block edges, duplicate (src, dst) pairs, and empty edge sets
    (logits within 7.2e-7).
- **ROCm determinism probe** (150 steps, seed 1, twice): parameters and inference are bitwise identical, at 0.33 to 0.39 s per step with
  the CPU shared. The frozen C runs at 1.6 s per step in the 4-way farm. Its logs show divergence at the first logged step (500); that
  is a log-granularity bound, so the short probe is suggestive and the full-length refit is the safeguard.

### Design

- C-v2 under C0, seeds 1-3, on the 11 Genesis leave-one-lineage-out folds, plus the seed-1 `all` fit (weights and cross-platform
  scores).
- A full-length determinism refit: D05, seed 1.
- All C-v2 fits run on ROCm (one backend).
- Comparator: A's deterministic CPU fits from T001.
- Resources: 3 parallel fits, 30 s staggered starts, a 120-minute cap, the host-RAM guard, crash-safe single attempts.

### Rule

The statistic is d_s = Genesis mean ER (C-v2, seed s) - Genesis mean ER (A), for s = 1..3, with a two-sided 95% t interval (df = 2,
t = 4.303).

| class | condition |
|---|---|
| `C_ADVANTAGE_OVER_A` | the interval is entirely below 0, \|mean\| >= 0.01, and no seed has more B50 misses than A (0) |
| `A_ADVANTAGE_OVER_C` | the interval is entirely above 0, and \|mean\| >= 0.01 |
| `NO_MEANINGFUL_DIFFERENCE_BOUNDED` | otherwise, when the whole interval lies within +/-0.02 |
| `EVIDENCE_INSUFFICIENT` | otherwise, or when any planned fit fails, or when the determinism refit is not bitwise identical |

A seed-level advantage (interval entirely below 0, |mean| >= 0.01) in which some seed has more B50 misses than A is not
`C_ADVANTAGE_OVER_A`. It falls through: to `NO_MEANINGFUL_DIFFERENCE_BOUNDED` if the interval lies within +/-0.02, otherwise to
`EVIDENCE_INSUFFICIENT`.

Descriptive outputs:

- per-lineage differences and the exact sign-flip p;
- the 3-seed C-v2 logit ensemble against A;
- C-v2 seed 1 against the original C seed 1;
- costs.

Mapping to the synthesis:

- `C_ADVANTAGE_OVER_A` maps to `MULTISEED_ADVANTAGE_SUPPORTED_ON_DEVELOPMENT`.
- `A_ADVANTAGE_OVER_C` and `NO_MEANINGFUL_DIFFERENCE_BOUNDED` map to `LIGHTWEIGHT_MODEL_PREFERRED`, because A is far cheaper.

Power, stated in advance: with 3 seeds, a between-seed SD of 0.01 gives an interval half-width of about 0.025. `EVIDENCE_INSUFFICIENT` is
therefore plausible.

### Disclosures

- The frozen C, A, B and M3-R results were all visible when this plan was written. C-v2 changes no hyperparameter, threshold or input.
- The A comparator is deterministic. C-v2 is evaluated over three seeds, so the comparison is between an ensemble of draws and one fixed
  model; the rule tests seed-level differences only.
- Development folds only. No evidence on unseen lineages.
