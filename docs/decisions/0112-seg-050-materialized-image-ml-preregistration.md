# ADR 0112: SEG-050-T003 frozen pre-registration of the materialized-image ML experiment (E0 / E1 / E2)

- Status: **FROZEN PRE-REGISTRATION (checkpoint C3).** This commit contains the protocol, the measurement tool and its tests. It is committed BEFORE the tool is run on any truth-bearing image, before any E1 number exists and before any E2 training. Results go to ADR 0113 in a later, separate commit; this ADR is not edited after the freeze except by a dated erratum that cannot change a threshold.
- Question: *does learned executable-region ranking generalize to already-established copied/decompressed executable images, reducing compilation work beyond the savings of ordinary proven-alias integration (ADR 0111)?*
- Pipeline preserved: known executable image -> ML proposal -> existing structural admission / unchanged validator -> generated-native AOT. ML never establishes that an image exists, proves a copy, authorizes a runtime target or overrides the validator.
- Independence from Problem A: the outcome of ADR 0111 does not decide whether this experiment is worth running; it is only an input to the economics gate (section 6).

## 1. Arms (exactly these)

- **E0 broad AOT**: every supported candidate instruction start of the image (the T001 baseline; no model).
- **E1 frozen M3 + B35, image arm**: the shipped native evaluator (`segarecomp ml-gen2-cell-logits`, model SHA-256 `11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1`, 77,441 parameters, B35 = `N = ceil(cells * 35 / 100)` in integers, score descending then cell ordinal ascending) WITHOUT retraining, calibration, new features or any change to B35. Image contract:
  - the image bytes are exactly the verbatim copy `ROM[source : source + length]` (ADR 0049); cells are the frozen 128-byte decision cells starting at IMAGE offset 0; the 1,024-byte context of a cell is taken from the image alone with the frozen symmetric word-block reflection at both image ends (period 2n); a short final cell is scored with reflected padding;
  - decode tokens are recomputed by the CPU-owned token export on the image bytes alone (an instruction truncated by the image end is a real boundary effect and is reported, not hidden);
  - the execution base, the source address, the image length and the lineage are NEVER inputs (the model only sees bytes and tokens; execution at a different base changes no input byte). Identical bytes copied several times are one lineage and one image for selection purposes;
  - B35 is applied PER IMAGE (the image is the ranking universe).
- **E1-direct (reported reference, not an arm of the gates)**: the same bytes' scores from scoring the whole ROM (context = surrounding ROM); this is what ADR 0111 O1 consumes. The per-image difference to the image arm is the "direct-ROM versus materialized-image" difference.
- **E2 small materialized-image-specific model**: run ONLY if section 4 (corpus gate) passes for E2 AND E1 failed a model-quality gate (R1-R3). Fixed candidate list, no search: exactly one candidate `E2a` = the frozen M3 architecture retrained on materialized-image lineages with image-boundary reflection, selection percent chosen from `{35, 50, 65, 80}` by leave-one-lineage-out folds on the training lineages only, one seed. E2 is skipped when E1 already passes, when the corpus is insufficient, or when no independently justified reason exists. E2 is never trained on, selected with or evaluated against the E1 test lineages, never on M3's five training titles as held-out, and never on the SEG-053 reserved corpus.

## 2. Leakage and lineage rules

- Lineage = original game / source project. Copies, aliases and multiple images of the same bytes are ONE lineage; engine-family titles (e.g. the Sonic games) are one lineage for splitting.
- **Known-corpus (no gate weight):** the five titles M3/B35 was fitted/selected on (Sonic 1, Flicky, Shining Force, Phantasy Star II, Sonic & Knuckles; ADR 0100/0101). Any E1 number on them is reported as `EXPLORATORY_KNOWN_CORPUS` and can neither pass a gate nor justify T005.
- **Gate-weighted lineage ("blind qualifying lineage")** requires all of: class A or B (ADR 0110), independent source-backed instruction-start truth derivable for the copied image by byte identity (exact byte-identical rebuild of a public source; observed runtime coverage is NEVER truth), not a known-corpus title, not consumed by SEG-053 (its reserved independent corpus is untouched).
- T001 numbers never feed any selection.

## 3. Metrics (all pre-registered; per image and aggregated over gate-weighted lineages)

`C outside R` (truth starts whose cell is not selected); full-recall (`C outside R == 0`) and the full-recall FRACTION of images; positive-cell fraction `p` and the density limit flag (`p > 0.35`: B35 cannot reach full recall for ANY ranking, a code-density/budget limitation, never presented as transfer failure or success); FRF (smallest score-prefix containing every positive cell); proposed fraction (selected cells / cells); R-stage and K-stage attribution (`C outside R` is a classifier miss; structural-prune and validator losses are separate and reported as `C outside K` and rejection class); validator acceptance of the plan built from the proposed subset by the UNCHANGED prune and validator; image-boundary sensitivity (mean |logit difference| image arm vs direct, edge cells (4 per side) vs interior; selected-set Jaccard; Spearman); generated-C bytes and compile CPU (T001 method, `seg050_materialized_cost.py`); whole-build CPU/wall; determinism (two logit evaluations bit-identical, identical selected sets); direct-ROM vs materialized-image difference.

## 4. Corpus gate (STOP class `CORPUS_BLOCKER`)

- A gate-weighted E1 claim requires **>= 3** blind qualifying lineages (exploratory frozen-M3 transfer target in the operator amendment: 3-5). Fewer => the E1 gates are not evaluated and the classification is `CORPUS_BLOCKER`; known-corpus and no-truth images are still measured and labelled exploratory.
- E2 requires **>= 8** qualifying lineages for training with a SEPARATE held-out set of **>= 3** (>= 11 total) and exact truth for every one.
- Decompression arm: no M68K deterministic-decompression producer exists (ADR 0110); it is not evaluated and no producer is built.
- As of the T001 screening the number of blind qualifying lineages is **0** (A with exact truth: Flicky, a known-corpus title; Golden Axe has no truth). The expected terminal class is therefore `CORPUS_BLOCKER`; this prediction is stated before the exploratory measurement.

## 5. Gates (evaluated only when section 4 passes; every gate is on gate-weighted lineages only)

- R1: full-recall fraction of images = 1.0 (`C outside R == 0` for every image). R2: proposed fraction <= 0.60 per image. R3: no image density-limited.
- K1: for every image the plan built from the proposed subset is accepted by the unchanged validator AND `C outside K == 0`. NOTE (code fact, section 7): the validator makes every `execution_alias` identity a mandatory root, so a proper subset of an alias image is rejected as `materialized_image_not_admitted`; K1 is evaluated with the unchanged validator and is not relaxed. A subset-based selection of the interior of a materialized image therefore cannot pass K1 without changing the validator, which this milestone forbids.
- E1 (economics): >= 25 % reduction of the image-attributable generated C versus E0, AND >= 10 % of the whole-build generated C beyond the ADR 0111 O1 build (savings already obtained by alias integration are not counted).
- D1: determinism (bit-identical logits, identical selection, identical plan) across two runs.

## 6. Classification (precedence in this order)

`CORPUS_BLOCKER` (section 4 not met) > `MATERIALIZED_ML_RECALL_FAILED` (R1-R3) > `MATERIALIZED_ML_ADMISSION_FAILED` (K1) > `MATERIALIZED_ML_ECONOMICS_INSUFFICIENT` (E1 or the T001 finding `NOT_ECONOMICALLY_IMPORTANT`) > `MATERIALIZED_ML_GATES_PASSED`. Any non-pass ends the ML track: T005 is closed with a NOT_JUSTIFIED statement and no production ML code is added. No threshold, budget, policy, feature or model may change after a result is seen; deviations are listed and the affected claim withdrawn.

## 7. Code facts the protocol relies on (verified, ADR 0110 section 6)

`apply_genesis_hybrid_admission` and `prune_genesis_region_admission` reject any proposal that does not admit every `execution_alias` identity (`materialized_image_not_admitted`); copy-alias descriptors are maximal verbatim runs derived from an observed build-time stop (not minimal code spans); the alias identity count of a mixed code/data copy includes decoded data (Flicky: 23,601 identities for a copy holding a minority of true starts); there is no M68K decompression producer.

## 8. Exploratory arm (declared now, no gate weight)

Flicky's single 49,152-byte copy (known-corpus, exact-copy truth derivable from the pinned source rebuild) is measured with `tools/seg050_materialized_e1.py` as `EXPLORATORY_KNOWN_CORPUS`, together with the budget arithmetic and boundary statistics of the Golden Axe copy (no truth: structure only, no recall claim, source not cell-aligned so no direct-ROM mapping). These numbers cannot satisfy any gate.

## 9. Frozen artifacts

`tools/seg050_materialized_e1.py` (SHA-256 `0f48fcb31d2163f47ba1cd373ae689cfdc80e2e66e67573d871cd39f6b978e9d`), `tests/seg050_materialized_e1_test.py` (`58f2ecceb37b34fa66aaffd299b3879509c699ee52e1a4b99595a73ea8f37751`), `tools/seg050_materialized_cost.py` and `tools/seg050_materialized_screen.py` (ADR 0110), the frozen gen2 model/representation/token/rank-plan/weight identities embedded in the native producer (ADR 0102/0103), truth artifacts identified by ROM SHA-256 and truth SHA-256 only (private, ignored `.cache` inputs; SEG-048/049 digests re-verified on load).
Command line (private inputs by path, nothing printed): `seg050_materialized_e1.py --segarecomp <cli> --rom <image> --truth <.u> --source <hex> --length <hex> --work <ignored-dir>`; the descriptor values come from the local build-time preparation and are never committed.

## 10. Dated erratum (C3b), after the independent pre-registration review and BEFORE any truth-bearing run

No threshold of sections 4-6 is changed; no result existed when this was written (the tool had only been smoke-tested on random synthetic bytes). Changes are implementation/definition clarifications; the tool is re-frozen.

1. **Joint unsatisfiability stated.** For an ADR 0049 alias image the unchanged validator requires every identity (code 0110 section 6), so K1 is satisfied only by proposing the WHOLE image, which contradicts R2 (proposed fraction <= 0.60) and yields 0 % reduction versus E0 (E1 gate). Therefore, after R1-R3 the only reachable terminal classes on an alias image are `CORPUS_BLOCKER`, `RECALL_FAILED`, and `ADMISSION_FAILED` by construction; `MATERIALIZED_ML_GATES_PASSED` is unreachable unless an executable image class without the mandatory-root rule exists (none does). The experiment is therefore effectively a recall-and-transfer study; this is stated, not hidden, and K1 is not relaxed. A report-only diagnostic (no gate weight) is declared: the hypothetical `C outside K` of the model subset, evaluated by the structural closure alone, is NOT computed in this milestone.
2. **Density flag** is defined in integer arithmetic: `density_limited = positive_cells > ceil(cells * 35 / 100)` (the earlier float form `p > 0.35` was wrong for small images; example 2 cells / 1 positive). Tool re-frozen.
3. **Minimum gate weight:** an image carries gate weight only with >= 8 cells (1,024 bytes) and a lineage only with >= 100 exact truth starts across its images. Smaller images are measured and labelled structure/exploratory (R2 is arithmetic-impossible for 1- and 3-cell images). R2 applies to the ML selection only; native production unions seed cells, which T005 would have to add.
4. **Structure-only mode** (`--truth` omitted) is declared for no-truth images (e.g. an unaligned source): it reports cells, B35 budget, determinism and image digest, and makes NO recall claim. With `--truth` the source must be 128-byte aligned; an unaligned-source lineage is therefore unmeasurable for recall and is not counted.
5. **Definitions:** "image-attributable generated C / compile CPU" = difference of two emissions with and without the descriptor (`seg050_materialized_cost.py`, ADR 0110 section 9); "10 % of whole-build generated C beyond O1" = (C of the O1 build - C of the O1+ML build) / C of the O1 build, measured with the shipped build; if no O1+ML build can be produced the economics gate is not met. D1 determinism = two evaluations of the image give bit-identical logits and an identical selection (implemented). Non-finite logits abort. FRF counts ties pessimistically (`>=` the lowest positive score) and is reported only. Validator acceptance / `C outside K` / generated-C measurements are made with the unchanged validator tests and the cost tool, not by the E1 tool. The image SHA-256 is part of the durable evidence.
6. **E2 clarifications (E2 is not executed in this milestone unless section 4 passes):** candidate percents are `{35, 50}` (65/80 can never meet R2 and are dropped); the training recipe (epochs, learning rate, sampling, augmentation) must be pinned in a further pre-registration committed before any training; E2 judged on the same gates R1-R3/K1/E1; the minimum E2 corpus is 8 training + 3 held-out lineages disjoint from the E1 test lineages (so 14 in total if those E1 lineages are distinct); using a percent other than 35 is a declared E2-only deviation from B35.
7. **Re-frozen artifacts:** `tools/seg050_materialized_e1.py` SHA-256 `0b682b625cefdc46fa8c78b6593bd989b741b8345744f45f1787f92da61e6450`; `tests/seg050_materialized_e1_test.py` SHA-256 `88c3fa8dd93620ca858f6efc8b2d87625234909779671c69df6d65c6775e6ce8`.
