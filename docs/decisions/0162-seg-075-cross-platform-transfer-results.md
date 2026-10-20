# ADR 0162: SEG-075-T004 cross-platform transfer results

- Status: Accepted. **Transfer classification: `EVIDENCE_INSUFFICIENT`** (pre-registered noise rule: tau = 0.103 > 0.05).
- Task: SEG-075-T004, combined SEG-075 product PR (checkpoint C4). The protocol is the frozen plan `fde026ee` (ADR 0161). The sanitized
  record is `tools/segarecomp_xplat_transfer.results.json` (aggregates only).
- Execution:
  - The pre-registered reproduction gate passed: SEG-072 fold D05 retrained bit for bit for M3-R, M4-Dense seed 1 and M1. So C0's
    Genesis fold scores are SEG-072's preserved scores, bound by digest.
  - The 118 remaining pre-registered fits all completed, with none failed or missing. The table is complete.
  - CPU backend, using the rebuilt research environment.

## 1. Noise floor (decisive)

The pre-registered seed replicate (M3-R, condition C0, seed 50 against seed 49) changes the per-lineage max ER by:

| D01 | D02 | D03 | D04 | D05 | D06 | D07 | D09 | D10 | D11 | D12 |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.481 | 0.001 | 0.019 | 0.000 | 0.002 | 0.006 | 0.000 | 0.042 | 0.003 | 0.011 | 0.002 |

It also changes D01's B50 misses from 0 to 15.

tau = max(0.01, 2 x mean) = **0.103**, which exceeds the pre-registered limit of 0.05, so every transfer decision is
`EVIDENCE_INSUFFICIENT`. The instability sits in D01, the Sonic-family lineage of 39 images. Its worst image is a small module whose ER
moves between 0.23 and 0.71 with the seed alone. The other ten lineages are stable to within 0.04.

## 2. Development results (descriptive; per lineage first)

Genesis held-out lineages (11), mean of per-lineage max ER (lower is better), worst-lineage ER, and summed B50 misses:

| configuration | C0 mean ER | C1 mean ER | C2 mean ER | C0 worst | C2 worst | B50 misses C0 / C2 |
|---|---|---|---|---|---|---|
| M3_FROZEN (reference) | 0.198 | - | - | 0.937 | - | 2 / - |
| M1_HGB | 0.177 | 0.155 | 0.149 | 0.768 | 0.485 | 1 / 1 |
| M3_R (seed 49) | 0.146 | 0.165 | 0.122 | 0.280 | 0.225 | 0 / 0 |
| M3_R seed 50 (C0 replicate) | 0.182 | - | - | 0.709 | - | 15 / - |
| M4_DENSE@128 | 0.144 | - | 0.140 | 0.568 | 0.446 | 2 / 0 |
| M4_DENSE@64 | 0.132 | - | 0.133 | 0.427 | 0.327 | 0 / 0 |

- **Every C2 change is smaller than the seed effect.** M3-R's C2 improvement of 0.023 in mean ER is smaller than the 0.036 difference
  between its two C0 seeds. It is concentrated in D01 and D03, the two lineages that move most under a seed change.
- **Exploratory, not a decision.** Excluding D01, tau would be about 0.017. The remaining C2 change for M3-R is then driven by D03 alone
  (0.280 to 0.116); the other nine lineages change by at most 0.01. This exclusion was not pre-registered and decides nothing.
- **Condition rankings are not monotone.** C1, adding Atari ST alone, is worse than C0 for M3-R (0.165 against 0.146) and better for M1.
- **False positives are essentially unchanged.** Negative cells admitted at B50 stay at 32.9k-33.7k for 128-byte configurations in every
  condition (66.7k-66.8k for M4@64, which counts 64-byte cells).
- **Fixed budgets reach every start.** The complete fixed budget is B50 for every configuration.

Cross-platform leave-one-lineage-out (`xplat_lolo`: C2-minus-x against C0-all; mean of per-lineage max ER over the four development
lineages):

| configuration | C0-all | C2-minus-x |
|---|---|---|
| M3_FROZEN (zero-shot) | 0.066 | - |
| M1_HGB | 0.084 | 0.095 |
| M3_R | 0.043 | 0.036 |
| M4_DENSE@128 | 0.036 | 0.032 |
| M4_DENSE@64 | 0.047 | 0.263 |

- **The cross-platform development lineages are mostly too easy to measure transfer.**
  - BattleIsleEditor and amiga-realtime3d have ER 0 for every model, including the zero-shot frozen production model.
  - Every lineage has a mean AUROC of 0.9996-1.0.
  - toslibc's representatives are small, so their ER sits at a cell-granularity floor (0.10-0.15). M4@64 gives 1.0 there because a single
    tiny image dominates.
  - Only EmuTOS has headroom. With C2, M3-R goes from 0.072 to 0.046 and M4@128 from 0.042 to 0.026, while M1 goes from 0.235 to 0.280.
- **Fixed-budget misses are high on cross-platform images.** On these code-dense images (about 80% positive cells), fixed-budget misses
  run to roughly 28k-30k for every model. As pre-registered, ER carries the comparison there.

## 3. Classification and consequences (pre-registered)

- **T004:** `EVIDENCE_INSUFFICIENT` overall (M3-R, C2). M1, M4@128, M4@64 and C1 are also `EVIDENCE_INSUFFICIENT`.
- **T005:** not admitted. The reviewed T005 runner (addendum `e1854647…`) refuses on this class. Model B's gate requires T004 not to be
  `EVIDENCE_INSUFFICIENT`, and T006 outcome rule 1 fixes the final class at `DATA_INSUFFICIENT` whatever model A would show.
  - The plan text calls model A "always built". That phrase sits beside a condition rule that has no C0/C2 choice for this class, and the
    bound runner it was reviewed with stops here. A was therefore not built: it could not change any pre-registered decision, and
    running it now would be post-hoc exploration.
  - A, B and C stay implemented and bound (ADR 0161 addendum), so a future, better-powered study can run them unchanged.
- **T006:** selection is empty, so there is no candidate to freeze. The one-shot evaluation is **not run**. The reserved lineages
  (neogeosdk, space-invaders) remain sealed and unexposed for a future study. The outcome class is `DATA_INSUFFICIENT` (rule 1).

## 4. Why: what the data says about the next attempt

1. **The noise floor is the binding constraint.** A single-seed M3-R on SEG-072's Genesis development set cannot resolve effects below
   about 0.1 ER, because of D01's small Sonic CD modules. Any future transfer test needs either a multi-seed protocol, as M4-Dense
   already uses (an ensemble of three seeds), or an ER aggregation that is robust to a single tiny image, pre-registered before results.
2. **The qualified cross-platform programs are not hard enough to differentiate models.** Assembler-written Amiga programs are recognized
   almost perfectly even zero-shot. Only compiler-generated firmware (EmuTOS) leaves room. Useful additional training data would be
   code that is mixed with data and resembles compiler output: C-compiled games, and programs with jump tables and inline data in code.
   This is the same "missing kinds of programs" list as ADR 0161, section 3.
3. **No consistent harm either.** In C2 no configuration added B50 misses; C1 added 2 for M3-R. C2's worst-lineage ER is lower than C0's
   for every model:

   | model | C0 worst-lineage ER | C2 worst-lineage ER |
   |---|---|---|
   | M1 | 0.768 | 0.485 |
   | M3-R | 0.280 | 0.225 |
   | M4@128 | 0.568 | 0.446 |
   | M4@64 | 0.427 | 0.327 |

   These are still within the seed effect, so the data supports neither adoption nor rejection.
