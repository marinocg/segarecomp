# ADR 0187: SEG-083 - five-game TAS corpus expansion and neural-retraining readiness: result (checkpoint C6)

- Status: Accepted (SEG-083-T001). Follows ADR 0186 (frozen plan `228647dd...`, unchanged since the freeze; no amendment). ADRs 0181-0185 are preserved.
- Privacy: aggregate only. Derived labels live in the private harness (`private-corpus/seg083-v1`, harness PR #101); nothing label-, ROM- or trace-derived is in this repository.

## Decisions

| Quantity | Result |
|---|---|
| Acquisition class | **`EXPANSION_MIXED`** (2 of 5 games `QUALIFIED_USEFUL`, 3 qualified, no unresolved inconsistency) |
| Model A (continuity verdict) | `ML_INCONCLUSIVE` (pooled AP moved by at most 2e-4: saturated baseline). Not failed acquisition |
| Readiness | **`MORE_DIVERSITY_NEEDED`** (unmet: new useful independent lineages 2 < 3; new strong hard-negative cells 20 < 300) |

## Per game (primary control = all-controller, seed 1; TAS = the published movie, full length, two matching traced replays)

| Game | Class | Extra strong code cells vs control (TAS / control) | Structural signatures new vs registry and control | CPU h |
|---|---|---|---|---|
| Shadowrun | `QUALIFIED_USEFUL` | 326 (772 / 475; +69 %) | 106 | 1.15 |
| NHL '94 | `TRACING_UNAVAILABLE` | 17 (496 / 573) | 8 | 0.27 |
| Shove It! | `REPLAY_INVALID` | 5 (107 / 122) | 3 | 0.85 |
| Doraemon | `QUALIFIED_USEFUL` | 130 (465 / 342; +38 %) | 15 | 0.67 |
| The Terminator | `QUALIFIED_LOW_YIELD` | 32 (194 / 185; +17 %) | 7 | 0.26 |

- **NHL '94** failed the frozen fidelity gate: accepted-E fraction 0.9807 < 0.99 (204 executed starts the independent disassembler could not decode). Replay was valid and deterministic. Even without the gate it would be low-yield.
- **Shove It!** failed the frozen progression gate: the replay is deterministic and perturbation-free with six distinct states, but new code appears in only 1 of the 5 movie-time fifths (a 128 KB game executes almost all of its code early); the class name `REPLAY_INVALID` is the plan's name for any replay-gate failure.
- Equal-duration controls were NOT shallow in every game: control/TAS executed-PC ratios are 0.56 (Shadowrun), 0.72 (Doraemon), 0.85 (Terminator), 1.29 (NHL '94: the control executes more code than a 267 s hockey TAS), 1.06 (Shove It!). The corrected control narrows the SEG-081 gap (26-36 %), which is why yields are lower than SEG-081's.
- **Player-1-only versus all-controller control:** identical in executed-PC set and strong cells for every game (ratio 0.5550 vs 0.5550 for Shadowrun, and so on); only NHL '94 differs by 22 PCs. Player 2 inputs never mattered, so the SEG-081 player-1-only convention did not bias those yields. Classification is the same against either control.
- Evidence consistency: 0 length mismatches, 0 non-ordinary decodes, 0 odd starts, E-code inside DMA sources 0 bytes in all five. E bytes also read as data are disclosed (Shadowrun 2, NHL '94 19,686, Shove It! 10,294, Doraemon 45,508, Terminator 28): the blanket CDL data flag, never gating.
- Weak supervision was not lost to RAM execution in any of the five (no RAM PCs). Strong labels are very sparse in negatives: strong hard-negative cells in the TAS labels are 4 (Shadowrun), 16 (Doraemon), 2,117 (Terminator); the strong+weak variant recovers 3,452 / 1,389 / 3,950 (the weak read tier is where the useful negatives are), and the strong variant leaves 69-97 % of bytes masked.
- Cost per extra strong cell: 2.9 (Shadowrun), 4.3 (Doraemon), 6.9 (Terminator) CPU-seconds, against 0.4-2.7 for SEG-081. Total CPU for the five games: 3.2 h of 40 (a voided first attempt, below, added under 2 h).

## Eight-candidate comparison (titles versus independent engines)

Titles: 8 candidates (SEG-081's Vectorman 2, Batman & Robin, Earthworm Jim 2 plus these five). Qualified: 6 (SEG-081: 3 useful; SEG-083: 2 useful, 1 low yield). Max 12-byte code-shingle
containment (either direction) against any SEG-072 development lineage is <= 0.023 for every qualified game and every pair of games is <= 0.01, so by the frozen rule all six qualified titles are independent lineages
(**effective independent lineages: 5 useful + 1 low yield**). This is shingle containment, not engine ancestry: SEG-081's sequels remain engine-adjacent to SEG-077 candidates, and publisher/developer overlap is not measured.
Of the five usable (useful) lineages, only the two new ones count towards the new-batch readiness criteria.

## Model-A diagnostic (11 qualified SEG-072 lineages leave-one-out; frozen HGB; arms add the new labels to training)

Primary quantities are the SEG-075 excess-retention metrics (lineage-max ER, mean over folds), not AP. Noise floor (B0 on a random 90 % of base cells) = 0.0146 ER.

| Arm | Genesis mean ER | worst-fold ER | missed starts B35 / B50 | pooled AP |
|---|---|---|---|---|
| B0 baseline | 0.177 | 0.768 | 7,929 / 1 | 0.99786 |
| B0_sub (noise null) | 0.163 | 0.686 | 7,677 / 1 | 0.99784 |
| C (new-5 all-controller control) | 0.138 | 0.365 | 8,017 / 0 | 0.99767 |
| T (new-5 TAS strong) | 0.157 | 0.631 | 7,790 / 1 | 0.99778 |
| TW (new-5 TAS strong+weak) | 0.151 | 0.476 | 8,122 / 1 | 0.99774 |
| T_matched (control-sized) | 0.174 | 0.717 | 7,988 / 2 | 0.99782 |
| C81 / T81 (SEG-081 three) | 0.140 / 0.126 | 0.448 / 0.255 | 8,103 / 1; 7,819 / 0 | 0.99778 / 0.99781 |
| C_all / T_all (eight) | 0.149 / 0.137 | 0.517 / 0.369 | 8,027 / 1; 7,550 / 0 | 0.99782 / 0.99783 |

- Adding new-game labels lowers mean ER by 0.02-0.05 (beyond the noise floor: T vs B0 -0.020, C vs B0 -0.039, T_all vs B0 -0.040), but the effect is carried by 2-3 folds (D01 0.77 -> 0.37 with T_all/C; D03 0.124 -> 0.041) and is **not
  a TAS-specific gain**: the control labels (C) lower ER at least as much as TAS (T - C = +0.019, T worse on 1 fold, better on 2), and the volume-matched T_matched is no better than B0. D07 and the other hard folds (D05, D06, D09, D10, D11) do not move
  (the six "residual" folds). Missed instruction starts at B35 fall slightly with T (-13 per fold on average) and T_all (-34); B50 is already ~0. Boundary-stratum misses: 9 (B0) -> 5 (T_all) over 11 folds. Hard-negative false-positive rate is unchanged (0.0014).
- Pooled AP/AUC move by at most 2e-4 (0.9979 baseline): the saturated-baseline problem of ADR 0185 persists, so AP cannot decide anything. Lineage-max ER is outlier-driven (single images), so the ER deltas are indicative, not confirmatory; no significance claim is made.
- Isolation: no new game was withheld from any fold (containment <= 0.023). The evaluation targets are source-backed SEG-072 lineages; the new games have no truth and enter training only. No sealed cohort was read.

## Readiness decision: `MORE_DIVERSITY_NEEDED`

New batch: 2 independent useful lineages (need 3 for a pilot), 322 new structural signatures (need 300: met), **20 strong hard-negative cells (need 300)**, 6 residual folds (need 2: met, with the disclosed weakness that the criterion is easy to satisfy),
eight-candidate total of 5 useful independent lineages (need 5: met), provenance complete (met). The floor for `NEURAL_RETRAINING_NOT_JUSTIFIED` was cleared.

What is missing, concretely:
1. **More useful independent engines, not more volume.** The two new useful lineages came from the two games with long, multi-stage, scrolling gameplay (Shadowrun, Doraemon). The failures are structural: short or static material (a 128 KB puzzle game, a single hockey match) cannot yield 100 more cells than a random controller.
2. **Strong hard negatives.** Strong data labels need hardware-header or DMA-source evidence; games that move data by CPU reads (Shadowrun, Doraemon) produce almost none (4 and 16 cells). The weak+strong variant recovers ~4,800 hard-negative cells from these two games, which the frozen rule did not count. A pilot should be trained with the strong+weak policy (weights 1.0 / 0.25), or the rule should be re-registered to count weak hard negatives, as a new pre-registration (not applied retroactively).

## Recommendation

- **Stop** acquiring from short/static, one-screen or one-match titles; do not use more control-based volume for model A.
- **Acquire four to eight more games** selected for long multi-stage gameplay, DMA-fed graphics or sound data (to obtain strong M-tier negatives) and different engines; in practice: platformer/action and RPG titles with published full-game TAS movies (at least 60k frames), and a mix that includes
  DMA-heavy engines. Re-evaluate readiness with the same frozen rule (a new pre-registration for the revised negative criterion if the operator wants weak negatives counted).
- **Do not run a neural B/C-v2 pilot yet**, and do not run full multi-seed retraining. If a pilot is later authorised: one or two difficult folds among the six residual folds (D07 is the cleanest, since it is unaffected by every label arm), one seed per architecture, frozen CPU limit, source-backed evaluation, trained on the strong+weak labels. Full three-seed all-fold retraining would be triggered only by a pilot in which B or C-v2 beat model A on those folds by more than the 0.0146 noise floor on ER.

## Disclosures

Controls are random-input policies; they can stay shallow (here 56-129 % of the TAS's executed PCs), and the result is acquisition feasibility, not ML benefit and not a fair gameplay comparison. A first acquisition attempt was run with two games in parallel and was **voided** before any analysis because the two lanes shared the BizHawk job directories (one NHL '94 emulator process aborted); all results above come from a clean sequential rerun under the same frozen plan (void attempt CPU < 2 h, outside the per-game ledgers). The model-A diagnostic ran once after three games qualified; its final run is above. The six residual folds mostly record that more data does not move those lineages; they do not prove that an architecture would. Lineage-max ER is outlier-driven. Shingle containment approximates but does not measure engine ancestry. Private retention: `private-corpus/seg083-v1` (Shadowrun, Doraemon, The Terminator; NHL '94 and Shove It! are not retained).
