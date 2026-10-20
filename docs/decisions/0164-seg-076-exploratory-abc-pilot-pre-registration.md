# ADR 0164: SEG-076 exploratory A/B/C pilot pre-registration

- Status: accepted (pre-registration; frozen before the first A/B/C fit)
- Task: SEG-076-T001, checkpoint C1 of the combined SEG-076 PR
- Plan: `tools/segarecomp_xplat_explore.t001.plan.json`, digest `tools/segarecomp_xplat_explore.t001.plan.digest` (re-frozen after the C1 review, still before any fit)

## Context

SEG-075 ended `DATA_INSUFFICIENT` (ADR 0163). Its models A, B and C were implemented and bound, but its pre-registered T005 gate refused
to train them. On 2026-10-20 the operator authorized a new **exploratory, development-only** experiment that fits all three. SEG-076
does not reverse or reinterpret SEG-075. SEG-075's plan, results, thresholds, decision and sealed cohort are unchanged.

Before any planning, the SEG-075 handoff (`c7-final-handoff`, manifest sha256 `1e8ba1e6...`) was verified with
`segarecomp_xplat_final.py verify`. It returned `ARTIFACT_HANDOFF_PASS` (32154 files, 0 physical problems, diagnostic bitwise identical).
No preserved model was rebuilt or retrained.

## Decision

The frozen plan binds the following by digest. `segarecomp_xplat_explore.py check` refuses any drift, and every fit record carries the
plan digest.

- **Code.** The new runner and its pure evaluation module, plus the whole local import closure. That closure is the unchanged SEG-075
  model / transfer / design / decision code and T005 `attach` (which supplies seed roots and relocations), and the SEG-072 recipes and
  their region-model modules.
- **SEG-075 identities.** Its plan, its snapshot, and its handoff manifest and receipt.
- **Comparator scores.** Every comparator score file, 72 in total.
- **Corpus contents.** The sha256 of every file in every image directory T001 reads: the Genesis registry directories of the 11 lineages
  and the 4 cross-platform representatives. These files are labels, tokens, seeds, relocations and `meta.json`, which carries the ROM
  digest that `Image` verifies. The directory set itself is also bound, so registry drift is refused. Each fold's scored image set must
  equal the bound corpus for every configuration.

The experiment design:

- **Condition and folds.** C0 (Genesis-only training) on the 11 Genesis leave-one-lineage-out folds, plus one `all` fit per model.
  - C0 matches the bit-reproduced SEG-072 C0 fold scores of M3-R, M4-Dense and M1-HGB.
  - The `all` fit preserves the weights and scores the four cross-platform development representatives, as evaluation only. Under C0,
    cross-platform leave-one-lineage-out fits would equal the `all` fit.
- **Models.** The SEG-075 definitions, unchanged:
  - A: fixed HGB, 210 features, `random_state` 49.
  - B: the bounded local Transformer with M4-Dense's exact recipe.
  - C: B plus typed candidate-edge message passing and the PROVEN flag.

  B and C use seed 1 (matched to M4-Dense seed 1). Order: A, then B and C. No result of A cancels B or C.
- **Comparators.** These are existing predictions; nothing is refitted.
  - Single-seed: M3-R seed 49, the M3-R seed-50 replicate, M4-Dense seed 1 at 128 bytes, and M1-HGB.
  - Ensemble (reported separately): the M4-Dense 3-seed ensemble. It is never used to judge B or C.
  - Descriptive: production M3 (M3_FROZEN).
- **Metrics.** All computed per lineage first. The SEG-075 metric code is reused unchanged, and the following are added:
  - SEG-075 metrics: max ER per lineage and Genesis mean ER (the mean of lineage max ER), worst-lineage ER, FRF, B35/B50/B60 missed
    starts and cells, negative cells admitted at B50, and AUROC.
  - Mean image ER.
  - Boundary strata: positive cells holding a D byte, positive cells adjacent to a D-only cell, and positive cells with 8 or fewer starts.
    For each stratum, missed cells at B35/B50 and normalized rank quantiles.
  - Cost: wall and CPU time per fit, inference time per image, and peak RSS.
  - Determinism: A, B and C are refitted once on fold D05 and the scores are compared bitwise.
- **Expansion rule.** Applied once and never tuned. The best one-seed comparator is the lowest Genesis mean ER among M4-Dense seed 1,
  M3-R seed 49 and A. B or C qualifies only if all of the following hold against that comparator:
  - its Genesis mean ER is lower by at least 0.02;
  - no lineage is worse by more than 0.05;
  - it has no more boundary-stratum missed cells at B50;
  - it has no more total B50 misses.

  The qualifying set gives the class: none gives `NEGATIVE_STOP`, B gives `EXPAND_B`, C gives `EXPAND_C`, and both give
  `EXPAND_B_AND_C`. A model with any failed planned Genesis fit does not qualify. The class only decides whether SEG-076-T002's
  multi-seed compute is justified. One seed cannot establish architectural superiority or inferiority.
- **Resources.**
  - CPU backend. 6 parallel fits at 2 threads each.
  - Wall-clock caps: A 30 min, B 90 min, C 160 min. These are about 3x the pre-freeze probe estimates of 26 min for B and 54 min for C.
  - RSS watchdog at 4 GB. A fit starts only when at least 3 GB of memory is available.
  - One attempt per fit. Fits are write-once. One identical, recorded re-run is allowed only after an external signal kill; never after
    a cap, an RSS-limit kill or a model error.
  - A failure is recorded and never retuned.
  - Candidate scores count only through the farm's recorded `ok` statuses. A planned job missing from the farm state counts as failed.
    Each record must match its file name (model, fold, seed, condition) and carry the plan digest and product commit.
- **ROCm.** ROCm was measured but not selected. Under the SEG-072 rule (ADR 0153), every candidate is on the same backend and the gain
  must be at least 25% end to end. Measured: torch 2.6.0+rocm6.4.2 on an RX 9070 XT (gfx1201, WSL2).
  - B: 0.038 s/step against 0.394 s/step on CPU. Reproducible.
  - C: 0.54 s/step against 0.81 s/step on CPU. Reproducible. C stays CPU-bound because its edge indexing is done in Python.

  The deciding reason is the same-backend clause. Every comparator the rule uses (M4-Dense seed 1, M3-R, A) is a CPU result, and CPU and
  ROCm outputs are not bit-identical (ADR 0117). A ROCm-trained B or C would therefore be an unmatched comparison. For throughput
  context: one ROCm process for all B and C fits needs about 0.55 h plus 7.2-7.8 h, against about 2.7-2.9 h on the CPU farm. Several
  concurrent ROCm processes were not measured.
- **Barrier.** The SEG-075 reserved cohort stays sealed. New entrypoints add a fail-closed sealed-subtree resolver that:
  - checks lexical components under both slash conventions, keeping drive, UNC host/share and `\\?\` prefixes, and applying `..`;
  - also checks the non-strictly resolved path, so a nonexistent child under an existing symlink into `sealed/` is refused;
  - case-folds names, strips Windows trailing dots and spaces and `:stream` suffixes, and matches `#`/`@` sample suffixes and 8.3
    short names;
  - refuses nonexistent paths too.

  SEG-075's frozen guard, which recognizes POSIX separators only for nonexistent paths, is not modified. Tests cover existing and
  nonexistent paths.

## Known comparator values at freeze

These values are disclosed so that a `NEGATIVE_STOP` is not over-read. They were reproduced exactly by the new metric path from the
bound comparator scores during the C1 review, and they match SEG-075's T004 records.

| C0 comparator | Genesis mean ER | worst lineage ER | B50 misses |
|---|---|---|---|
| M3-R seed 49 | 0.145608 | 0.280197 | 0 |
| M3-R seed 50 | 0.182093 | 0.708861 | 15 |
| M4-Dense seed 1 @128 | 0.211924 | - | 4 |
| M4-Dense 3-seed ensemble @128 (not used by the rule) | 0.144078 | 0.567738 | 2 |
| M1-HGB | 0.176716 | 0.767830 | 1 |
| M3_FROZEN (descriptive) | 0.198314 | 0.936709 | 2 |

Unless A turns out better, the best one-seed comparator is M3-R seed 49. B or C then needs:

- Genesis mean ER of 0.125608 or less;
- zero B50 misses;
- no more boundary-stratum missed cells at B50 than M3-R seed 49 has (0).

M3-R's own seed-50 replicate fails this bar. Its seed spread of 0.036 is larger than the 0.02 margin. The bar is strict by design,
following the operator's rule. A `NEGATIVE_STOP` therefore means only that one seed did not clear a bar set by the strongest single-seed
model, not that B or C is inherently worse.

## Consequences

- C2 runs the farm only after this ADR and the plan are committed, pushed and independently reviewed.
- Results go into a separate aggregate-only results document and a results ADR. Private scores, weights and logs stay under
  `<HARNESS_ROOT>/.cache/seg076` and are later added to the extended handoff (SEG-076-T004).
- No production model, admission budget, default, validator or runtime change.
