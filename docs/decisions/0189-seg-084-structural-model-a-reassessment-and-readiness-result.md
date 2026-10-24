# ADR 0189: SEG-084 - structural model-A reassessment, negative-supervision reading, failed-game diagnostics, label preservation and the new readiness decision (checkpoint C3)

- Status: Accepted (SEG-084-T001; retention is SEG-084-T002, harness PR #102). Follows ADR 0188 (frozen plan `f93e4023919b476ad169794088255923f1ed8ec9a2e6db832779b42e5e55f18e`, which supersedes the plan `bfb996e9...` named in ADR 0188: the plan was re-frozen after a first fit set under `bfb996e9...` was stopped and voided, so the order freeze -> fits is evidenced by file timestamps and the score stamps, not by separate git commits; no bound file changed after the first real fit under `f93e4023...`). ADRs 0181-0188 are not amended; SEG-083's plan, decisions and metrics are untouched.
- Privacy: aggregate only. Derived labels are in the private harness; nothing label-, ROM- or trace-derived is in this repository.

## Decision

| Question | Result |
|---|---|
| New readiness decision (independent of SEG-083) | **`MODEL_OR_EVALUATION_LIMITATION`** |
| Recommendation | **Improve evaluation and source-backed evidence before more acquisition. Do not run a B / C-v2 pilot now.** |
| Does the true structural model A benefit from SEG-081/083 labels? | Not beyond the noise reference: Genesis mean ER 0.1052 (A0) -> 0.0996 (A_all), change -0.0055 against a mean noise of 0.0107 |
| Do low-code-yield games contribute useful negatives? | Legitimate and non-trivial, but no measurable effect on any held-out negative slice |
| Shove It! / NHL '94 | Both contain recoverable valid labels under a prospective rule (retained, ADR 0187 unchanged) |

## Corrected baseline

The SEG-083 diagnostic was the SEG-072 **M1-HGB**, not the SEG-076 structural model A. The true model A, refitted by this study's pipeline, **reproduces the stored SEG-076 scores bit for bit on all 11 folds** (Genesis mean ER 0.105171; the SEG-076 figure is 0.105). ER 0.177 (M1) and 0.105 (A) are therefore different models, not a conflict.

## Model-A results (11 leave-one-lineage-out folds, 143 fits, 2.2 CPU-hours of the 8 allowed; one fit costs ~0.9 CPU-minutes; inference over all held-out lineages ~2-3 s per arm)

| Arm | Genesis mean ER | worst-fold ER | B35 misses | B50 misses | pooled AP |
|---|---|---|---|---|---|
| A0 (registry only) | 0.1052 | 0.211 | 8,060 | 0 | 0.99790 |
| A0 on 90 % of cells (noise, 3 seeds) | 0.1056 / 0.1151 / 0.1057 | 0.213-0.216 | 8,007-8,148 | 0 | 0.9977-0.9980 |
| A81 (+ Vectorman 2, Batman & Robin, Earthworm Jim 2) | 0.1037 | 0.218 | 7,771 | 0 | 0.99825 |
| A83 (+ Shadowrun, Doraemon, The Terminator) | 0.1061 | 0.212 | 8,168 | 0 | 0.99794 |
| **A_all** (all six TAS strong) | **0.0996** | 0.215 | 7,802 | 0 | 0.99817 |
| A_control (the six control strong labels) | 0.1052 | 0.205 | 8,169 | 0 | 0.99851 |
| A_matched (TAS at the control's volume) | 0.1022 | 0.210 | 8,001 | 0 | 0.99856 |
| A_all_W (strong+weak, weights 1.0 / 0.25; `seg084.a_weighted.v1`, not the original model A) | 0.1019 | 0.216 | 7,801 | 0 | 0.99851 |
| A_control_W | 0.1006 | 0.206 | 8,095 | 0 | 0.99842 |
| A_term (+ The Terminator only) | 0.1048 | 0.206 | 8,080 | 0 | 0.99808 |
| A_noterm (A_all without The Terminator) | 0.1034 | 0.220 | 7,643 | 0 | 0.99839 |

- **Noise.** The 10 % subsample moves a fold's lineage-max ER by up to 0.057 (D01) and 0.033 (D09); the mean noise is 0.0107. A_all's change (-0.0055) is half of it. A_all improves ER beyond the fold noise on 4 folds (D01, D07, D11, D12) and worsens it on 2 (D04, D10); the gain is dose-dependent (A_all 0.0996 < A_matched 0.1022, same labels at the control's volume), but the mean does not exceed the noise reference, so the pre-registered response rule fails.
- **Where the errors are.** B50 misses are 0 for every arm: at the production budget model A already retains every executable start of every development lineage. B35 misses are 97 % from one lineage (D07: 7,837 of 8,060; ER 0.193 in every arm: the ER is unmoved by any label set, although its B35 misses vary between 7,473 and 7,911 across arms). The persistent high-ER folds are the SGDK homebrew builds D05 (0.21), D07 (0.19), D11 (0.19), D09 (0.13), D06 and D10 (~0.10); the new commercial TAS labels do not move them. D03 improves slightly (0.029 -> 0.020; A_all) and D01 improves most (0.153 -> 0.056) but D01's own noise (0.057) is as large as the change.
- **TAS versus control.** A_all is better than A_control on 4 folds and worse on 2; the mean difference (-0.0056) is half of the noise: control-derived labels at similar volume do about as well. This is consistent with a volume / diversity effect rather than TAS-specific content, and it is not distinguishable here.
- **Weak labels.** Adding weak labels at weight 0.25 changed ER by +0.0023 (TAS) and -0.0046 (control), both inside the noise; neither helps nor harms detectably. AP / AUC are near 0.998 / 0.9997 for every arm and decide nothing. A "residual fold" as pre-registered also counts folds where A_all got worse from a near-zero baseline (D04: 0.0002 -> 0.053), so the residual count of 5 overstates headroom; it did not matter because the response rule failed first.

## Negative supervision from low-code-yield games

- **Legitimate.** For all six games the strong data bytes are exactly the retained header + DMA-source tier bytes (The Terminator: 304,640 = 512 H + 304,128 M), so the strong-tier definition is respected; no data-read tier and no reference truth is used.
- **Non-trivial in the registry's own sense.** 89 % of The Terminator's 2,377 strong negative cells are "hard" under the existing definition (adjacent to code or >= 50 % ordinary decodes), 99.95 % by ordinary decodes only, mean ordinary-decode fraction 0.88; the registry's own negatives are 84 % hard (0.72). They are DMA-fed data blocks that decode as plausible instructions, the same character as the registry's hard negatives. The definition is loose (most data is "hard"), so the count is a weak measure of difficulty.
- **Value.** A_term (A0 + The Terminator) shows no response beyond noise on ER, on hard-negative false positives (0.00136 against 0.00137) or on negatives admitted at B35 / B50 (-2.5 cells per fold, one or two folds). The hard-negative false-positive rate of every arm is ~0.0014, almost all from D07 (0.0149, unchanged by every arm). The held-out slices cannot currently see a benefit of extra negatives. The independent diversity count (4 lineages above 300 strong hard negatives: Vectorman 2, Batman & Robin, Earthworm Jim 2, The Terminator) meets the pre-registered negative criterion; SEG-083's frozen count (20) mixed code-yield with data-negative usefulness and is not reinterpreted.
- Yields kept separate (strong positive / strong hard-negative / weak hard-negative cells): Vectorman 2 1,647 / 1,843 / 1,843; Batman & Robin 1,040 / 3,235 / 3,235; Earthworm Jim 2 829 / 6,528 / 9,718; Shadowrun 772 / 4 / 3,452; Doraemon 465 / 16 / 1,389; The Terminator 194 / 2,117 / 3,950. Independent lineages: 5 useful for code; 4 for strong negatives.

## Shove It! and NHL '94 (existing artifacts, no replay)

- **Shove It!** The replay is faithful: identity holds, 66,331 frames played to the end, no perturbation, two traced runs identical, six distinct (RAM, VRAM) states, and a visual check of the retained checkpoint screenshots shows the game's stage counter advancing from stage 7 to stage 14. 98.4 % of its 2,451 executed ROM PCs are first executed in the first 30 seconds and 40 afterwards (34 in the second 30 seconds, 3 + 3 later), so only one movie fifth exceeds 0.5 %. The failure is the continued-discovery gate, a property of a 128 KB puzzle game, not desynchronisation. Proposed prospectively: separate replay fidelity, continued discovery (reported), label confidence and acquisition yield; under that separation Shove It! is a faithfully replayed `QUALIFIED_LOW_YIELD` (5 extra strong cells). ADR 0187's `REPLAY_INVALID` stands.
- **NHL '94.** All 204 executed starts rejected as `E_DISASM_NO_DECODE` (accepted fraction 0.9807) are decoded by the CPU decoder as ordinary straight-line instructions, 203 of 204 are opcode line B (0xB: the CMP / CMPA / EOR group) and 177 of them one form (family 2, source addressing class 10, width code 2); m68k-disasm printed raw data for all of them in both modes. For every one the emulator-observed trace is consistent with the CPU length: no discontinuity leaves the instruction (interrupt entries ignored) and pc + length was itself executed. No odd, out-of-image or header-area PC. By the frozen rules the class is **`DISASSEMBLER_LIMITATION`**: an instruction-form gap of the independent disassembler, not unsound execution labels; the rejected starts were already masked `U` (never labelled), so the labels are conservative. **Disclosure:** the first run of this diagnostic used a mistaken corroboration definition (it assumed the edge log records every transition; it records only non-sequential steps) and returned `UNRESOLVED`; the definition (not a threshold) was corrected, the plan re-frozen before the first fit, and the corrected run gives the class above. The 0.99 threshold was not lowered.
- **Retention rule (post-hoc, retention-only):** it was written after the NHL '94 and Shove It! outcomes were known and it admits exactly those two games, so "prospective" means only that it is applied forward and changes no earlier class. It is retention-only and does not feed any study arm. Rule in the plan: replay fidelity + label consistency + sound tracing, yield and discovery fifths irrelevant, the accepted-fraction failure waived only for a `DISASSEMBLER_LIMITATION` finding. Under it all five SEG-083 games are retention-eligible. The NHL '94 corroboration is one-sided evidence: it shows that no discontinuity leaves the instruction and that pc + the CPU decoder's length was itself executed, so a true length shorter than the CPU's would pass without a recorded edge; a Musashi or Ghidra length check of the 204 starts would settle it. It does not change retention, because those starts are masked `U`. NHL '94 and Shove It! are retained in the private snapshot `seg084-tas-prospective-v1` without any claim that the original experiment passed.

## Corpus preservation (SEG-084-T002, private harness PR #102)

Durable in private Git after merge: `seg072-dev-v1` (50 images), `seg075-xplat-dev-v1` (36 images, with relocations and source-map spans), `seg077-exploratory-v1` (1 sample, partially observed, no tiers), `seg084-tas-prospective-v1` (NHL '94, Shove It!), with `seg081-v1` / `seg083-v1` unchanged. Every snapshot verifies without a ROM (also from a fresh clone) and restores byte-identically (classes, tokens, seed reports, relocations, seeds: 197 + 144 + 4 + 48 files, 0 mismatches), and the existing training loader and model A's features read all 86 restored SEG-072 / SEG-075 samples. Still external: the images (7 commercial ROMs, SGDK builds, Sega CD disc files, toolchain-built cross-platform images), the pinned decoder CLI, and the sealed / reserved cohorts (custodian-controlled; an encrypted separate backup is recommended and not built). The SEG-075 handoff (3.6 GB on disk; the brief's historical figure was 4.64 GB) and the SEG-072 handoff (590 MB, of which a 308 MB bundle tar) stay local and at risk until backed up outside the agent workspace; the T002 policy lists it.

## Readiness: why `MODEL_OR_EVALUATION_LIMITATION`

Diversity is not the bottleneck: code diversity (5 independent useful lineages; the useful games add 32 % to the base positives) and negative diversity (4 lineages) both meet their pre-registered criteria, as the plan stated in advance. The structural model A does not respond beyond noise, and the evaluation itself is thin: eleven development folds, one lineage-max statistic dominated by single images, B50 saturated at zero misses, hard-negative error rates near zero except D07. A B / C-v2 pilot on these folds could not be adjudicated: SEG-076 already found B and C-v2 no better than A on this evaluation, and the new labels do not change model A's behaviour on the folds where headroom remains.

## What should happen next

1. **Improve evaluation and source-backed evidence first**: add held-out source-backed lineages that resemble the hard folds (more independent SGDK-style homebrew builds with source truth), report pooled and per-image ER with intervals instead of only lineage-max, and add a negative-sensitive held-out slice (D07-like data in code-looking regions).
2. **Do not acquire another TAS batch now** and do not train B / C-v2 (neither a pilot nor multi-seed): the TAS labels do not move the metric that matters and control-derived labels do about as well.
3. **Back up the local handoffs and the sealed cohorts** outside the agent workspace.
4. If evidence and evaluation improve and model A then responds, a bounded B / C-v2 pilot can be re-assessed under this same readiness rule.

## Disclosures

The noise reference is the largest of three deviations on a 10 % base perturbation, so the rule is conservative against finding a response. A_all raises the negatives admitted at B35 / B50 by +4.5 / +6.5 cells per fold (D10 beyond noise) and including The Terminator helps A_all by -0.0037 ER (A_all vs A_noterm); both are inside the noise. `corpus_snapshot_sb.py` refuses sealed / reserved paths by exact path-part names only (not symlinks or reserved ids). The tools default `HARNESS` to the local harness path as the SEG-076 tools do; set `SEGARECOMP_HARNESS_ROOT` elsewhere.


Development folds are reused throughout; nothing here is confirmatory. The noise reference perturbs only 10 % of the base cells (a smaller change than adding ~5,000 labelled cells), so the dose-response and A_matched comparisons were required as well. Lineage-max ER is outlier-driven. D07's lineage-max ER has zero sub-fit noise (it is set by one image), so its 0.0007 gain counts as 'beyond noise': a degenerate zero-noise case that inflates the response-fold count (it still fails the mean rule). The residual-fold rule counts regressions from a near-zero baseline. Restored TAS samples carry registry-style seeds regenerated by the pinned CLI (6 of 6 ROMs accepted; 358-533 seed addresses), not the retained reset-vector-only seeds. Rehearsals on three folds under draft plans were discarded (ADR 0188). A first full set of fits was started under plan `bfb996e9...` and stopped after a few minutes when a defect in the NHL diagnostic (a class-scope name error, before any output) required a re-freeze; its two score files were set aside unread.
