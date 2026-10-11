# ADR 0113: SEG-050-T004 offline evaluation of E0 / E1 on materialized images (results of the frozen ADR 0112 protocol)

- Status: results (checkpoint C4). Protocol, thresholds and classification precedence are those of ADR 0112 as re-frozen by its dated erratum (`cb88bc0`); nothing was changed after seeing a number. Classification: **`CORPUS_BLOCKER`**. E2 was not run.
- Tool: `tools/seg050_materialized_e1.py` SHA-256 as pinned in ADR 0112 section 10, binary: native `segarecomp ml-gen2-cell-logits` (frozen gen2 model `11258708...a98e1`). Product base `7814e35`, protocol `cb88bc0`.

## 1. Corpus gate (ADR 0112 section 4): not met

Blind qualifying lineages (class A/B, exact-copy truth, not a known-corpus title, not SEG-053-reserved): **0** of the required 3 (E1) / 14 (E2). The only A lineage with exact truth is Flicky, a title the frozen M3/B35 was fitted/selected on (known-corpus, no gate weight); Golden Axe has no independent truth; there is no B lineage (no M68K decompression producer). No labels were manufactured and no observed runtime coverage was substituted. R1-R3, K1, E1 and D1 gates are therefore NOT evaluated for the classification; the measurements below are `EXPLORATORY_KNOWN_CORPUS` / structure-only and carry no gate weight.

## 2. Exploratory measurement: Flicky's single copy (known-corpus, exact-copy truth by byte identity)

Image: one 49,152-byte verbatim copy (384 cells; image digest recorded in the ignored local output, not here), 4,602 derived truth starts, 151 positive cells.

| quantity | E1 image arm | direct-ROM scores (context = ROM) |
| --- | --- | --- |
| selected cells (B35 = ceil(384 x 35 / 100)) | 135 (35.2 %) | 135 (35.2 %) |
| positive cells / positive-cell fraction | 151 / 39.3 % | 151 / 39.3 % |
| `C outside R` (truth starts in unselected cells) | 332 of 4,602 (7.2 %) | 332 (7.2 %) |
| positive cells missed | 16 | 16 |
| full recall | **no** | **no** |
| FRF (smallest score-prefix holding every positive cell) | 0.3932 (151 cells = exactly the positive count) | 0.3932 |
| density-limited (`positive_cells > N`: 151 > 135) | **yes** | yes |

- **Transfer to the image is exact, not lossy.** The image arm and the direct-ROM scoring select the *identical* 135 cells (Jaccard 1.000, Spearman 0.999998). Image-boundary effect: mean |logit difference| 0.115 on the 4 edge cells per side (reflection padding and truncated tokens) and exactly 0.000 in the interior. Position/base/length are not inputs.
- **The ranking is perfect for this copy (known-corpus!)**: FRF equals the positive-cell fraction, i.e. every positive cell outranks every negative cell. The failure to reach full recall is therefore a **code-density / budget limitation of B35, not a ranking failure**: 39.3 % of the cells hold code, B35 can hold only 35.2 %. Per ADR 0112 this is reported as a density limit and is NOT presented as transfer or as pass. A different selection unit or calibrated percent (here at least 39.3 %, i.e. about 40 %) would be a separate pre-registered E2 hypothesis requiring independent lineages; it was not tried.
- Determinism D1: two evaluations bit-identical, identical selection.

Golden Axe's 432-byte copy (structure-only, no truth, source not cell-aligned): 4 cells, B35 budget 2 cells (50 % of the image); R2 (<= 0.60) is met only because 2/4 = 0.5 and gate weight is excluded by the minimum size (8 cells). Determinism holds. No recall claim.

## 3. Structural admission and economics (stated, not gated here)

- **Admission:** the unchanged validator makes every identity of an ADR 0049 image a mandatory root (ADR 0110 section 6, ADR 0112 erratum 1). Any proper subset of Flicky's copy (the 135 selected cells would admit 35 % of 23,601 alias identities) is rejected as `materialized_image_not_admitted`; the unit test `alias_source_mapping` ("partially covered copy") exercises exactly this rejection with the production validator. The interior of a materialized image therefore has no admissible partial selection without changing the validator or the descriptor semantics (both forbidden in this milestone).
- **Economics:** the image-attributable cost is large only for Flicky (ADR 0110: 80.6 % of M68K compile CPU) and the alias integration of ADR 0111 already gives all the savings the unchanged validator admits (Golden Axe: -58.8 % generated C). The remaining ML-selectable share (non-alias ROM) is unaffected by an image arm. `NOT_ECONOMICALLY_IMPORTANT` (ADR 0110) stands for conclusion 2.

## 4. Leakage / integrity statement

Split by lineage; the five M3 titles are known-corpus; copies of one title are one lineage; SEG-053's reserved corpus (Alien Soldier, Land Stalker sources) was neither read nor measured; truth is private and identified by ROM/truth digests; no runtime coverage was used; no gate, budget or threshold was altered; E2 was skipped (corpus gate, ADR 0112 section 1).

## 5. Classification and consequences

`CORPUS_BLOCKER` (primary; precedence). Independent structural facts recorded for the decision: B35 cannot reach full recall on a code-dense copy (39.3 % > 35 %), and even a perfect recall could not pass K1 because of the mandatory-root rule. **T005 is closed NOT_JUSTIFIED: no production ML code for materialized images is added.** Acquisition follow-up (exact inputs): see ADR 0110 section 11.
