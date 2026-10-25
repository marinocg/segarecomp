# ADR 0199: SEG-087 - low-tail code recovery: scope, pre-registered stop criteria, low-tail diagnosis and independent-witness coverage (checkpoints C0-C1)

- Status: Accepted (SEG-087, checkpoints C0/C1). Follows ADRs 0190-0198; none is amended. SEG-085 (`EVIDENCE_INSUFFICIENT`) and SEG-086 (`CALIBRATION_INEFFECTIVE`) stand.
- Code and aggregates: `tools/segarecomp_seg087_diag.py`, `tools/segarecomp_seg087_diag.results.json` (aggregate only), tests `tests/segarecomp_seg087_diag_test.py`.
- Scope reminder: offline development-lineage evidence. It authorises no production, runtime or default-policy change, trains nothing, and is not independent qualification.

## C0: question, frozen inputs, data isolation, stop criteria

Question: can independently established, label-free structural evidence identify the rare executable 128-byte cells that structural model A scores poorly (the cells that force the SEG-085/086 absolute threshold to be conservative), so that a small witness union could replace a global margin?
If not, this line stops (`STRUCTURAL_EVIDENCE_INSUFFICIENT`) without a rescue mechanism.

- Frozen inputs, read not modified: the stored out-of-fold model-A scores (SEG-076/084), the SEG-085 inner-refit scores, the unchanged CPU decode tokens and seed roots, the development labels. Model A is not refitted. Registry loading uses the unchanged SEG-084 loader with the lineage and reserved-identity
  guards used since SEG-086; H01 (Phantasy Star IV) and every evaluation-only, sealed or reserved sample are not loaded, and nothing here is fresh evidence.
- Labels are used only to describe the cells being diagnosed and to score a witness afterwards. Every witness is computed from tokens, image words, seed roots and relocation fields (the test file asserts the witness extractor has no label parameter).
- Witnesses examined (definitions in the tool header): seed-rooted reachability by fall-through and direct edges over decoder-ordinary words (`W_REACH_ROOTED`, already an input feature of model A), direct targets of seed-reached sources (`W_REACH_TARGET`), trusted relocation fields (`W_RELOC`), reachability from the program-counter entries of the exception-vector table in the image header (`W_VECTOR_ROOTED`; **added after the adversarial review**, which showed that the three unseeded SGDK images have no seed roots and so made seed-rooted reach undefined there), and two measured-but-**non-independent**
  heuristics: a direct target from any decoder-ordinary word (`W_UNROOTED_TARGET`) and a long ordinary decode run (`W_DECODE_CHAIN`). Only these four can be a code root; arbitrary decodable bytes are not trusted.
- **Go/no-go rule for a rescue experiment (T002), written down for this ADR.** Proceed only if all hold: (G1) independent witnesses cover at least 50 % of the non-seed low-tail cells in images that have a seed universe **and** at least 50 % of the image-weakest negative-logit cells (the cells that decide each image's oracle threshold);
  (G2) independent witnesses fire on at most 1 % of verified-data cells; (G3) removing the independently witnessed cells from the lineage minima moves the SEG-085 decomposition (`T0`, shortfall) by at least 2 logits. Otherwise record `STRUCTURAL_EVIDENCE_INSUFFICIENT`, close T002/T003 unexecuted and go to the decision.
  *Disclosure:* the 205 low-tail count and the seed-reach coverage were seen in exploratory runs while the tool was being built, before this rule was written; the 50 % level is deliberately generous (a rescue that leaves the weakest binding cell unrescued gains nothing, because the threshold is set by the weakest unrescued positive) and was not tuned to that outcome.
- Compute: the whole diagnosis runs in about 6 CPU-seconds on stored scores (ceiling 4 CPU-hours).

## C1: results (50 development images, 14,186 non-seed positive cells; aggregates)

**Population.** 205 non-seed positive cells have a negative logit: D01 132 (Sonic 1/2/3&K and Sonic CD modules), D03 26 (Shining Force), D12 47 (SGDK). 158 are historical commercial, 47 are modern SGDK. All 47 SGDK cells are in one image (`d12`) that has no independently established seed universe (like `d10`/`d11`); the SEG-085 policy treats such images as outside its domain.
Seed cells are already retained: of 838 seed-positive cells only 4 have negative logits.

**What sets the SEG-085/086 threshold.** `T = T0 - shortfall = -10.339 - 3.316 = -13.655`. `T0` is the weakest verified positive of D01 (one single-start cell, 122 data bytes, entered after data); the shortfall is the leave-one-lineage-out miss against the weakest D12 positive (-7.02, a pure-code cell). Two cells in two lineages fix the whole
calibration. Other lineage minima range from -6.4 (D03) to +10.4 (D09); the other eight lineages have no non-seed positive below zero (SEG-086's "seven" counted one fewer).

**They are well ranked inside their own images.** The share of an image's cells scoring above a low-tail cell is 0.09 to 0.37 (median 0.20). Ranked budgets keep them: B35 retains 204 of 205 (the one exception is a D01 cell), B45 and B50 retain all 205. The binding limit of the earlier work was the *absolute* threshold, not the ranking. The M4-Dense 128- and 64-byte aggregates
of the same cells (a different model, descriptive only) place 203 of 205 in the top 35 % at both units. B35's large known-start losses (ADR 0197) are on SGDK images and not due to these cells.

**Stable across fits.** Over the out-of-fold model and the ten SEG-085 inner refits that exclude the cell's lineage, 128 of 205 cells are negative in all 11 fits and 195 in at least half; none is negative only in the out-of-fold fit. They are not a single-fit artefact.

**Code/data structure (label-based, offline description).**

| category | all 205 | D01 | D03 | D12 |
|---|---|---|---|---|
| pure code | 57 | 6 | 4 | 47 |
| single-start fragment (one start, data elsewhere) | 26 | 22 | 4 | 0 |
| data-dominated fragment (more than half data, 2+ starts) | 96 | 84 | 12 | 0 |
| other mixed code/data | 26 | 20 | 6 | 0 |
| contains unknown/disputed bytes | 0 | 0 | 0 | 0 |

Entry: 120 cells begin after data, 2 after a terminating instruction, 83 by fall-through from code (all 47 D12 cells). Label audit (a direct edge from a labelled start landing on a labelled continuation or data byte, ADR 0191): **0 of 205** flagged, against 12 of 13,981 other positives, so the available label audits give no indication that the tail is a boundary artefact.
That does not prove the 26 single-start and 96 data-dominated cells have correct boundaries: a reconstruction that rebuilds byte-identically can still carry a data-in-code decision; these 122 cells are the ones where a labelling error would matter most and they are reported as such, not trimmed.
Why they score low (median label-free cell character, `feature_character_medians`): D01/D03 tail cells have short decode chains (maximum run 27 and 20 words, against 64 for other positives) and a lower ordinary-word share for D01 (0.70; D03 0.83 is about equal to other positives), i.e. brief code fragments inside data that the cell-level aggregate cannot separate from data. D12 is different: a pure-code image whose cells have a
distinct byte statistic (few zero bytes, 0.07 against 0.17-0.22; 0.46 ASCII-range bytes against 0.37-0.39) relative to the training lineages: a code-style distribution shift on a held-out lineage. Two distinct causes, not one.

**Independent witness coverage** (the headline):

| set | cells | usable independent witness | seed-reached | reached-source target | vector-rooted | relocation | unrooted target (not independent) | long decode run (not independent) |
|---|---|---|---|---|---|---|---|---|
| low-tail positives | 205 | **51 (24.9 %)** | 4 | 3 | 50 | 0 | 80 (39 %) | 188 (92 %) |
| low-tail, historical D01/D03 (seeded images) | 158 | **4 (2.5 %)** | 4 | 3 | 3 | 0 | 80 | 141 |
| low-tail, SGDK `d12` (no seed universe) | 47 | 47 (100 %) | 0 | 0 | 47 | 0 | 0 | 47 |
| image-weakest negative cells (decide each image's threshold) | 17 | **1 (5.9 %)** (`d12`) | 0 | 0 | 1 | 0 | 8 | 15 |
| lineage-weakest cells | 11 | 4 | 1 | 1 | 4 | 0 | 7 | 9 |
| all non-seed positives | 14,186 | 1,744 (12.3 %) | 1,326 | 1,227 | 1,321 | 0 | 13,122 | 14,126 |
| verified-data cells, historical | 73,870 | 2 (0.003 %) | 0 | | 0 | 0 | 53.2 % | 59.5 % |
| verified-data cells, SGDK | 9,334 | 0 | 0 | | 0 | 0 | 18.5 % | 70.3 % |

Rooted evidence is precise (it fires on 2 verified-data cells in 83,204) but sparse: seed roots reach 9 % of all non-seed positives (631 seed cells in 51,295 D01 cells; none in D10-D12) and vector roots 9 % as well. The vector-table root changes the SGDK picture and not the historical one: the 47 pure-code `d12` cells are all reached from the header vectors (SGDK code is generated
and reachable), while the 158 historical cells are witnessed in only 4 cases (2.5 %). The two heuristics that do cover the historical tail are non-specific: the long-decode-run heuristic fires on 60-70 % of data cells and on nearly every positive, the unrooted-target heuristic on 53 % of historical data cells, and arbitrary decodable bytes are not a trustworthy root. By category, an independent witness exists for 1 of 96 data-dominated fragments, 0 of 26 mixed,
49 of 57 pure-code and 1 of 26 single-start cells. Not tested: jump-table / indirect-target recovery as a witness (120 of 205 cells begin after data); no such deterministic route exists in the evaluated tooling and building one is the speculative-disassembly infrastructure the brief excludes.

**Effect of a perfect rescue on the calibration.** Removing the independently witnessed cells (seed, vector, relocation) from the lineage minima leaves `T0 = -10.339` (the D01 single-start cell is unwitnessed) and *worsens* the shortfall from 3.316 to 3.965, because the D12 minimum disappears and D03 becomes the comparison. Removing only the seed-rooted cells changes nothing. Only if *every* cell flagged by any of the
five witnesses is granted, i.e. the non-independent heuristics are treated as roots (they also admit about half of all historical data cells), does `T0` move to -6.14 with a residual shortfall of 0.20; that upper bound passes G3 numerically but is not independent evidence, so the conclusion rests on G1, G2 and independence, not on G3 alone.

**Granularity (descriptive; nothing trained).** 147 of 205 cells still contain a mixed sub-cell at 64 bytes and 146 at 32 bytes; only 58 and 59 cells separate completely, 47 of them the trivially pure D12 cells. Of the 158 historical cells, 11 resolve at 64 bytes and 12 at 32. At 32 bytes 189 mixed sub-cells remain; finer units move the ambiguity to a new boundary
(inline data adjacent to short code runs) instead of removing it. D01 cells are short fragments of 1-2 start-bearing sub-cells (mean 1.2 at 64 bytes), so a finer unit would retain fewer bytes only if a finer unit could be *decided*, which is the unsolved part.

**Gate evaluation.** G1: 2.5 % of the seeded-image (historical) tail and 5.9 % of the image-weakest cells (24.9 % of all 205 only because the unseeded SGDK image is entirely vector-reachable) against 50 % - **fail**. G2: 0.003 % - pass. G3 for independent witnesses: `T0` moves 0 logits (and the shortfall grows) - **fail**.

## Decision at C1: `STRUCTURAL_EVIDENCE_INSUFFICIENT`

The binding low-tail cells are real code in mixed 128-byte cells (D01, D03) and a code-style shift (D12); the historical cells are covered by the deterministic witnesses that exist (seed and vector roots, direct edges, relocations) in only 4 of 158 cases, while the SGDK cells are all vector-reachable but do not bind the threshold (the D01 single-start cell sets `T0`). The only evidence that covers them is the same arbitrary-decodability the brief excludes and that admits half of all data. A witness-union rescue on top of B35/B45 would add
no cells that a stable ranking does not already keep (B45 already retains all 205) and cannot move the absolute threshold (witnessing every independent cell leaves `T0` at -10.34). The brief's stop rule applies: **no rescue mechanism is implemented (T002 not executed), no recall or economics run (T003 not executed)**. This decision is development evidence only; it authorises nothing.

## Consequences

SEG-087 proceeds to the adversarial review and the synthesis ADR (T004). Further work on dynamic budgets from model A would need qualitatively new evidence (for example targeted labels for short code fragments in mixed cells, or independently verified entry points beyond the broad-AOT seeds), not another threshold or witness variant.
