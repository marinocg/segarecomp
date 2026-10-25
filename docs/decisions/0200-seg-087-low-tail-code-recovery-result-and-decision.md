# ADR 0200: SEG-087 - low-tail code recovery and static evidence feasibility: result and decision (checkpoints C2-C4)

- Status: Accepted (SEG-087, checkpoint C4). Follows ADR 0199; none of ADRs 0190-0199 is amended.
- Evidence: `tools/segarecomp_seg087_diag.results.json` (aggregate; produced by `segarecomp_seg087_diag.py run`, about 6 CPU-seconds on stored scores), ADR 0199.
- Scope reminder: development-lineage evidence only. Nothing here is independent qualification and no deployment, production or default-policy decision is made.

## Decision: `STRUCTURAL_EVIDENCE_INSUFFICIENT`

Checkpoint C1 (ADR 0199) applied the stop rule written for it: independent label-free witnesses (seed-rooted reachability, direct targets of reached sources, relocation fields, and - added after the adversarial review - reachability from the header exception-vector table) cover **51 of the 205** low-tail positives (24.9 %), but only **4 of the 158 historical cells (2.5 %)** (the 47 SGDK `d12` cells, an image without seed roots, are all vector-reachable), and **1 of the 17** cells that decide each image's oracle threshold, against a 50 % requirement; removing the
witnessed cells leaves `T0` unchanged and worsens the shortfall (3.32 to 3.97). Granting the non-independent heuristics (a direct target from any decodable word, a long decode run; they fire on 53 % and 60-70 % of verified-data cells) would pass G3 numerically but is not independent evidence, so the decision rests on G1, G2 and independence. Jump-table / indirect-target recovery as a witness was not tested (no such deterministic route is part of the evaluated tooling). Consequently:

- **C2 (witness-rescue experiment): not executed.** No candidate was implemented, no budget (B35/B45) was chosen, no synthetic rescue tests exist. A rescue that adds only what independent evidence supports would add at most the 51 witnessed cells to a ranked region that already contains every one of the 205 at B45.
- **C3 (recall and economics): not executed.** No recall table for a candidate and no compile benchmark exists (0 of 8 CPU-hours; diagnosis 6 CPU-seconds of 4 CPU-hours).
- **C4:** independent adversarial review of the C1 evidence (see below) and this record.

## Answers to the ten required questions

1. **Genuinely executable, or partly reconstruction ambiguity?** All 205 contain at least one labelled instruction start, none contains unknown/disputed bytes, and none is flagged by the direct-edge label audit (0 of 205, against 12 of 13,981 other positives). 57 are pure code. The remaining 148 are mixed: 26 are a single start among data, 96 are more than half data with two or more starts, 26 are other
   mixed cells. These 122 fragment cells are exactly where a reconstruction boundary decision (code or data-in-code) would matter, and the evidence available cannot rule that out; they are neither trimmed nor asserted correct. So: mostly real code by every available check, with an unquantified residue of label uncertainty confined to the fragment cells.
2. **Why do they score low?** Two distinct reasons. D01/D03 (158 cells, historical): brief code fragments inside data; cell-level aggregates are dominated by data, decode chains are short (median maximum run 27 and 20 words against 64) and 120 of the 158 begin after data. D12 (47 cells, one SGDK image without a seed universe): complete code cells with a distinct byte statistic (few zero bytes) compared with the
   training lineages, i.e. a held-out code-style shift. They are stable across 11 model fits (195 of 205 negative in at least half, 128 in all), and well ranked within their images (0.09-0.37 of cells above them).
3. **Primarily code and data mixed inside 128-byte cells?** For the historical tail yes: 148 of the 158 are mixed cells and only 6+4 are pure code. For the SGDK tail no (47 pure code cells). Overall 148 of 205 are mixed.
4. **Can deterministic witnesses find them without label leakage?** No. The extractor is label-free (asserted by test), but independent witnesses cover only 4 of the 158 historical tail cells (51 of 205 overall, because header vector roots reach all 47 `d12` cells); the heuristics that cover the historical tail are non-specific.
5. **Proportion rescued?** 51 of 205 (24.9 %; 4 of 158 historical, 47 of 47 SGDK) would be witnessed; no rescue was run. At the threshold level the rescue would move nothing: the D01 cell that sets `T0` is unwitnessed and witnessing D12 enlarges the shortfall (3.32 to 3.97).
6. **Recall preserved?** Not evaluated (no candidate). For reference the existing ranked budgets keep these cells: B35 204 of 205, B45 and B50 205 of 205; B50's zero known-start misses on the development corpus (ADR 0197) is unchanged.
7. **Actual total build cost versus B50 and deployed incremental?** Not evaluated: the pre-registered gate was not reached; no cost claim is made.
8. **Historical commercial versus modern SGDK?** All 158 historical low-tail cells are in D01 (Sonic 1/2/3&K and Sonic CD modules) and D03 (Shining Force) and are fragment/mixed cells; the SGDK tail is one image (47 pure-code cells, no seed universe, therefore outside the SEG-085/086 policy domain; seed-rooted witnesses are undefined there but header vector roots reach all 47 cells). Of the seven SGDK images the other six have no negative-logit
   positive. Independent witnesses cover 2.5 % of the historical tail and 100 % of the SGDK tail (vector-rooted only); the SGDK tail does not bind the threshold.
9. **Enough evidence to justify collecting new independent historical games?** Not for this line. There is no candidate to confirm, and new games would only add more lineages to estimate the tail margin (ADR 0197), not a witness for the fragments. New historical games would be justified by a different, positively-evidenced hypothesis.
10. **Continue with model A, targeted labels, smaller granularity, or stop?** Stop extending this selector (absolute-threshold dynamic budgets from model A plus structural witnesses) without qualitatively new evidence. Smaller granularity is not indicated: 147 of 205 cells still contain a mixed sub-cell at 64 bytes and 146 at 32 bytes (ambiguity moves, 189 mixed 32-byte sub-cells remain), and the M4-Dense 64-byte
   aggregate ranks the same cells no differently (203 of 205 in the top 35 % at both 128 and 64 bytes). Model A remains the ranker; B50 and the deployed incremental implementation remain the economic baselines. If the operator wants to continue anyway, the only narrowly-scoped data question suggested is corroboration of the boundaries of the 122 fragment cells and a sample of
   code in the D12 style; that is evidence acquisition, not a new selector.

## Procedure and integrity notes

- The go/no-go rule and its 50 % level were written after exploratory runs had shown the 205 count and the seed-reach coverage (disclosed in ADR 0199); the decision does not depend on the level (coverage is 2.0 % and 0 %).
- No SEG-085/086 frozen output, plan or digest was changed; no model was fitted; no sealed, reserved, evaluation-only or H01 material was read; the registry loader and guards are the unchanged ones. Public output contains aggregates and classifications only.
- Not a completeness statement: the witnesses are structural evidence of a synthetic kind (seed roots, direct edges); "uncovered" means uncovered by those, not unreachable.
