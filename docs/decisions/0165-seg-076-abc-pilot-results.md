# ADR 0165: SEG-076 exploratory A/B/C pilot results

- Status: accepted (exploratory, development-only)
- Task: SEG-076-T001, checkpoint C2 of the combined SEG-076 PR
- Plan: ADR 0164, `tools/segarecomp_xplat_explore.t001.plan.json` (digest `04eb81e8...`, frozen and reviewed before the first fit)
- Results: `tools/segarecomp_xplat_explore.t001.results.json` (aggregates only)

## Pre-registered result: `NEGATIVE_STOP`

All results use the C0 condition (Genesis-only training) on the 11 Genesis leave-one-lineage-out folds, at 128-byte cells. B and C are
single-seed pilots (seed 1). Mean ER is the mean over the 11 lineages of each lineage's max ER, the SEG-075 definition.

| configuration | Genesis mean ER | worst lineage ER | B50 misses | boundary missed @B50 / @B35 | negative cells @B50 | AUROC | xplat mean ER |
|---|---|---|---|---|---|---|---|
| **A** structural HGB | **0.105** | **0.211** | **0** | 0 / 5 | 32929 | 0.9992 | 0.044 |
| B local Transformer (s1) | 0.155 | 0.485 | 117 | 1 / 12 | 32864 | 0.9987 | 0.140 |
| C graph-guided Transformer | failed: every planned fit exceeded the frozen CPU cap (see below) | | | | | | |
| M3-R seed 49 | 0.146 | 0.280 | 0 | 0 / 6 | 33514 | 0.9988 | 0.043 |
| M3-R seed 50 | 0.182 | 0.709 | 15 | 3 / - | 33659 | 0.9989 | - |
| M4-Dense seed 1 @128 | 0.212 | 0.690 | 4 | 3 / 17 | 32896 | 0.9985 | 0.146 |
| M4-Dense 3-seed ensemble @128 (not used by the rule) | 0.144 | 0.568 | 2 | 2 / - | 32874 | 0.9990 | 0.036 |
| M1-HGB | 0.177 | 0.768 | 1 | 1 / 9 | 32943 | 0.9992 | 0.084 |
| M3_FROZEN (descriptive) | 0.198 | 0.937 | 2 | 1 / - | 33071 | 0.9988 | 0.066 |

How the rule applied:

- The best one-seed comparator is **A**. B fails every criterion against it:
  - its mean ER is 0.050 *worse*;
  - D01 and D03 regress by more than 0.05;
  - it has 1 boundary-stratum miss at B50, against 0 for A;
  - it has 117 B50 misses, against 0 for A.
- C has no completed fit, so it does not qualify.
- The pre-registered class is `NEGATIVE_STOP`. SEG-076-T002 (the multi-seed expansion) is therefore **not admitted**.

## Findings (exploratory, single seed for B)

- **The lightweight structural model A is the strongest development-fold model measured so far.** Against M3-R seed 49 it lowers
  Genesis mean ER by 0.040:
  - It is better on 9 of 11 lineages, and worse on D07 (+0.001) and D12 (+0.004).
  - The largest gains are D03 (-0.251), D09 (-0.094) and D01 (-0.075).
  - Its worst lineage is 0.211 against 0.280 for M3-R. It has no B50 or B60 misses, and its cross-platform representatives are at
    M3-R's level.
  - A fit takes about 40 s on the CPU (0.21 CPU-hours for all 12 fits), against about 22 min for M3-R and about 15 min for M4-Dense.
  - A is deterministic (`random_state` 49, no subsampling), so the training-seed noise that dominated SEG-075 does not apply to it.
  - Caveat: this is still reuse of exposed development folds and is not confirmatory. A should get a confirmatory study (SEG-076-T004
    states the decision).
- **B (local Transformer).** B is a different architecture from M4-Dense and does not use M4's region features. Against its matched
  single-seed comparator, M4-Dense seed 1 (same recipe), B's mean ER is lower by 0.057, mostly D03 (-0.484) and D01 (-0.166). But B
  misses many starts at the fixed budgets: 117 at B50, concentrated in a few lineages. Neither the expansion rule nor the economics
  favour it.
- **Boundary strata.** Every model misses many boundary-stratum cells at B35 (5 to 17) but almost none at B50. The rank quantiles of
  boundary cells are similar across models (D01 median about 0.12 to 0.15 of the ranking). The differences between models are in the
  tail (the q100 rank), and that tail is what drives the lineage max ER.
- **Determinism.** The A and B refits on fold D05 are bitwise identical to the original fits.

## C under the frozen resources

- The frozen farm ran 6 CPU fits x 2 threads on an 8-core / 16-thread host. A CPU-bound IDE process was also running.
- Under that load C's per-edge Python message passing ran at about 5.5 s/step, against 0.8 s/step in the less-contended pre-freeze probe.
  This was a planning error in the caps.
- C.D01 to C.D06 hit the 160-minute cap at about 1500 of 4000 steps.
- A host crash then ended the farm while C.D07, C.D09 to C.D12 and C.all were running. They are absent from the farm state and count as
  failed. They were not re-run on the CPU.
- Under the frozen plan, C is therefore "failed", which counts as no advantage.
- The operator asked for C to be run on the GPU. A separately frozen and reviewed **resource-only amendment** refits B and C on ROCm
  with identical code, steps, seed and folds (`segarecomp_xplat_explore_rocm.py`, amendment `bb5c35e5...`). It is reported below as an
  amendment and never replaces this class.

## Amendment result (ROCm)

Pending: the farm is running. This section is completed at the end of C2.

## Review notes carried forward (non-blocking)

These come from the C1 re-review and the amendment re-reviews.

- M3_FROZEN's frozen weights and the SEG-049 historical scores are descriptive and unbound.
- Drive-relative `C:sealed` is caught only by path resolution, not by the lexical layer.
- Fit write-once is enforced only when a fit starts; an orphaned fit after a farm crash could race.
- A failed `.all` fit also disqualifies a model, which is stricter than the rule text.
- Cost aggregation is not status-gated.
- A kernel OOM kill is recorded as `killed_signal9`.
- The amendment farm accepts a duplicated `--infra-rerun` argument (operator misuse; not used).
- The plan prose says one attempt, while the code allows one infra re-run.
