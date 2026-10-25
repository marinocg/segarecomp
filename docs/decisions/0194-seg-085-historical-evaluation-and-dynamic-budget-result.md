# ADR 0194: SEG-085 - historical M68K evaluation and dynamic retention budgets: result and decision (checkpoints C4-C5)

- Status: Accepted (SEG-085, checkpoints C4/C5). Follows ADRs 0190-0193; none is amended. Protocol frozen by ADR 0190 (`seg085.protocol.v1`) and plan `59813af7...` (ADR 0193) before any cohort image was scored.
- Aggregates: `tools/segarecomp_seg085_eval.cohort.json`, `tools/segarecomp_seg085_eval.economics.json`; supporting `segarecomp_seg085_{diag,dyn}.*.json`, cohort manifest.

## Decision: `EVIDENCE_INSUFFICIENT`

Applying ADR 0190 section 5 as written: the qualified independent historical cohort has **one lineage and one economics-measured image against a minimum of three of each**. That is the first row of the table and it takes precedence; no promotional or
exculpatory classification is available. Precedence: the insufficiency rows govern; the conditions of `STATIC_MODEL_OR_CALIBRATION_LIMITATION` also hold, and the limitation findings below are reported as secondary. DYN_SCORE's abstention is a limitation of this single pre-registered calibration, not evidence that dynamic budgets fail (the optional label-free normalisation was not tried). The ADAPT figures for Phantasy Star IV are single-image evidence, not a result. Two secondary findings:

- **DYN_SCORE has a concrete calibration limitation** (what `STATIC_MODEL_OR_CALIBRATION_LIMITATION` would name): the frozen, isolation-safe threshold (T = -13.66) retains more than the 0.60 cap on every one of the 50 development images and on the independent image, so it always abstains to broad AOT and delivers
  no saving (it costs the BROAD build, 1.48x the B50 CPU on the independent image). Cause: raw model-A logits are not comparable in the tail across lineages (per-lineage weakest-positive score ranges from -10.3 to +10.4). A narrow future study would test a *label-free* per-image score normalisation or a
  quantile calibration over the positive-score distribution, evaluated under the same inner-refit isolation; the truth-informed per-image headroom (oracle full-recall fraction 0.17 commercial-era, 0.24 SGDK, against B50 0.50) shows the ranking is not the limit.
- **Unchanged ADAPT on model A looks economically attractive but is not recall-safe under its own fitting rule.** On the independent Phantasy Star IV image: B50 and ADAPT both have 0 missed starts (before and after the production prune and validator, `c_outside_k` = 0, runtime digests equal to BROAD); total build CPU BROAD 1,872 s, INC (deployed
  incremental) 893 s, B50 1,268 s, ADAPT 790 s (0.62 of B50, 0.88 of INC, 0.42 of BROAD). On the seven commercial-era development images ADAPT pools to 0.66 of B50 (1,670 vs 2,528 CPU-s; INC 2,028), **but that pool includes s2, which has 116 starts outside K after the prune** (1 start at the proposal level); without s2 the pool is 1,520 vs 2,261 (0.67), and s2's own saving is not a valid saving: the lineage that sets the margin is unprotected when it is the target. An isolation-corrected margin removes that miss on all 50 images but retains 0.53 of cells (about B50). One independent image cannot adjudicate between these.

Not a completeness guarantee: a zero-miss result on one image, or on a few, is empirical recall only. A model score is not proof that a region is non-executable, and the validator cannot show that unidentified executable code was not excluded.

## Required report

1. **Representativeness of the development corpus.** By lineage count 7 of 11 Genesis lineages are SGDK/GCC; by positive cells 87 % are commercial-era; independent full commercial ROMs number seven (two engine families) plus 36 Sonic CD modules of one lineage (ADR 0191).
2. **Why SGDK dominated SEG-084's failures.** D07 (240p Test Suite) holds 97.2 % of Genesis B35 missed starts: density 0.41, SDK-masked cells consume the 35 % budget (239 of 407 masked cells admitted); at B50 every Genesis image has zero misses.
3. **Newly qualified programs.** One: Phantasy Star IV (English revision, 1995, hand-written MC68000), tier T-B. Toejam & Earl was found to be a SEG-077 reserved title and discarded (ADR 0192). No Atari ST / Amiga period lineage beyond the SEG-075 development ones was found.
4. **Label reliability.** Byte-identical rebuild; decoder agrees on all 102,494 starts; 4 of 27,916 direct targets contradict the labels (rate 1.4e-4) and 119 starts / 5 cells were masked. Boundaries remain the reconstruction authors'; not independently verified by an original build or by execution (the economics bridge shows equal digests with BROAD but gives no label verification).
5. **Why historical ADAPT underperformed.** A single additive margin set by the worst fitting image, fitted on records produced by models that had seen the target lineage; abstention whole lineages to 100 % under the frozen M3 and M1 scores (ADR 0191, 0193).
6. **Does DYN_SCORE give different per-image budgets?** No: it abstains everywhere (retained 1.0). ADAPT on model A does (0.27-0.36 of cells on the commercial-era images).
7. **Does DYN_SCORE retain every verified start?** Vacuously yes (it retains everything); the non-isolated variant misses 18 starts, which is why isolation matters.
8. **Does it reduce generated-native cost?** DYN_SCORE: no (0 % saving, an increase against B50 and INC). ADAPT on model A: yes on the one independent image (-37.7 % vs B50, -11.5 % vs INC) and on the development commercial pool (-33.9 % vs B50), at the recall risk above.
9. **Historical versus modern.** Commercial-era images are sparse (density 0.09-0.25) and almost perfectly ranked (lineage max ER 0.046 mean); SGDK are denser, SDK-masked and harder (0.139). ADAPT-on-A savings on SGDK are about zero (0.489 retained; pooled CPU 1.03 of B50). B35 fails only on dense images.
10. **Further development?** Not yet promotable. Dynamic budgets are worth a narrow follow-up only with (a) a label-free score normalisation study under isolated calibration, and (b) at least three more independent historical lineages with trustworthy labels, which this environment could not supply (the binding constraint is data, not method). More TAS labels or B/C-v2 retraining are not indicated by any result here; the existing deterministic structural model A with B50 remains adequate.

## Economics coverage and caveats

- Phantasy Star IV's broad build needs an unlimited stack (the default stack makes clang crash on its generated C, 839 MB); the startup bridge's single-unit path crashed and was not used for economics. The measurement used the SEG-076 CLI (`357f8a23...`) and SEG-076 benchmark settings; an earlier CLI without the closure flag produced argument-parse failures that were mistaken for validator rejections and discarded.
- Two concurrent runs sharing work directories contaminated six development images; they were killed, deleted and re-run as disjoint sets. All reported numbers come from the clean runs. Measurements ran two sessions in parallel; CPU seconds are the primary metric, wall time is not comparable.
- Model-inference overhead (1.55 s CPU for Phantasy Star IV; well under 2 s for each development image) is recorded but not added to the policy costs; it is below 0.2 % on the independent image and below about 5 % on the ~30 s SGDK builds, so no ratio above changes by more than that.
- The SGDK economics group has 4 of the 7 SGDK images (d05, d06, d07, d09 are the SEG-074 gate images; D10-D12 have no broad AOT, `effective_address_not_24bit`): an economics coverage gap for three lineages.
- Cross-platform images could not be run through the economics pipeline (not Genesis); they are an economics coverage gap and appear only in the error taxonomy.
- Runtime equivalence is bounded (120 frames, finite instruction budget) and covers startup only.
