# ADR 0153: SEG-072 development results - frozen M3 reproduction, adaptive budgets, candidate comparison and freeze

- Status: Accepted (results appended per task; the protocol is ADR 0151, plan digest `48da926f…`). All numbers are development
  out-of-fold (leave-one-lineage-out over the 11 qualified lineages, ADR 0152). No sealed truth was read for anything in this record.
- Tooling: `tools/segarecomp_ml_v3_train.py` (frozen-M3 scoring, M3-R / M1-HGB / M4-Dense folds, hardware benchmark),
  `tools/segarecomp_ml_v3_eval.py` (policies, metrics, diagnosis, structural K stage, economics, selection).

## T003 - frozen M3 historical reproduction and expanded development baseline

### Historical reproduction (exact)

The five gen2 title-held-out M3 folds were refitted with the unchanged trainer (`segarecomp_ml_region_gen2_train.py loto --model M3`, frozen
environment: Python 3.13.14, numpy 2.5.3, scikit-learn 1.9.1, torch 2.14.1 CPU) now that Shining Force and Phantasy Star II truth is available
(ADR 0152). **Every fold's prediction digest equals the frozen `outer_score_digests`** of the gen2 rank plan, bit for bit:

| held out | prediction digest | FRF | fit seconds (5 concurrent) |
|---|---|---|---|
| s1 | `5d1e2407…` = frozen | 0.211 | 846 |
| flicky | `365afbf5…` = frozen | 0.177 | 870 |
| sf1 | `45468830…` = frozen | 0.208 | 776 |
| ps2 | `22a62d7b…` = frozen | 0.112 | 832 |
| sk | `e94106c8…` = frozen | 0.269 | 729 |

This settles ADR 0124's `M68K_ADAPTIVE_EVIDENCE_UNAVAILABLE` cause: the frozen folds are reproducible; only the inputs were missing.

### Expanded development baseline (M3_FROZEN)

M3_FROZEN = the historical outer folds for the five original titles, the unchanged production weights for every other image (zero-shot for
D05-D12; lineage-correlated for s2, the Sonic CD modules and sf2 - optimistic for D01/D03). B35 = production budget; seeds and integer rounding
included; R = ranked cells UNION seed cells.

| lineage | max FRF | B35 retained / missed starts | B45 | B50 | B60 |
|---|---|---|---|---|---|
| D01 (39 images) | 1.000* | 0.422 / **306** (3 modules) | 0.510 / 2 | 0.553 / 2 | 0.644 / 2 |
| D02 | 0.177 | 0.351 / 0 | 0.450 / 0 | 0.500 / 0 | 0.601 / 0 |
| D03 | 0.208 | 0.350 / 0 | 0.450 / 0 | 0.500 / 0 | 0.600 / 0 |
| D04 | 0.112 | 0.350 / 0 | 0.450 / 0 | 0.500 / 0 | 0.600 / 0 |
| D05 | 0.286 | 0.351 / 0 | 0.450 / 0 | 0.500 / 0 | 0.601 / 0 |
| D06 | 0.163 | 0.350 / 0 | 0.450 / 0 | 0.500 / 0 | 0.600 / 0 |
| D07 | 0.471 | 0.351 / **7,760** | 0.450 / **635** | 0.500 / 0 | 0.600 / 0 |
| D09 | 0.247 | 0.351 / 0 | 0.450 / 0 | 0.500 / 0 | 0.601 / 0 |
| D10 | 0.186 | 0.350 / 0 | 0.450 / 0 | 0.500 / 0 | 0.600 / 0 |
| D11 | 0.263 | 0.350 / 0 | 0.450 / 0 | 0.500 / 0 | 0.600 / 0 |
| D12 | 0.194 | 0.350 / 0 | 0.450 / 0 | 0.500 / 0 | 0.600 / 0 |

\* Four tiny Sonic CD sub-CPU modules (3-5 cells) consist only of positive cells, so FRF = 1.0 for any ranking; their seeds already cover them
(residual FRF 0). Consequence (no rule change): the pre-registered "worst-lineage FRF" criterion is 1.0 for every configuration and therefore
never separates candidates; the selection falls through to the next criterion.

**M3_FROZEN is not complete under any fixed policy**: misses 8,066 starts at B35, 637 at B45 and 2 at B50/B60.

Failure classes (missed positive 128-B cells, `diagnose`): at B35, 209 cells (D07 198, D01 11): mean code coverage 0.986, 1.4% mixed
code/data, 38.6 starts per cell - **code-density versus budget failures** (D07's application plus SDK code exceeds 35% of its cells; the
D01 misses are ADR 0140's three Work RAM copy modules). At B50/B60 one cell remains: a **small code/data-boundary fragment** (2 starts, 6%
code coverage) in a Sonic CD Work RAM copy module with residual FRF 0.949.

Structural closure (unchanged production prune + validator, 11 Genesis images that broad AOT accepts): 10 accepted with **C outside K = 0**,
K/U 0.334-0.355 (mean 0.345); D07 rejected (`machine_root_not_admitted` - its missed code leaves a machine root unclosed; production would fall
back to broad). Candidate false admissions (ordinary-decoding even offsets inside R that are labelled non-start, masked bytes excluded):
1,099,443 at B35, of which 256,637 inside labelled instructions - a property of admitting whole cells, not of the ranking; the validator stays the
correctness authority.

Economics (broad vs M3_FROZEN+B35 explicit admission plan, 120 frames, explicit instruction budget, clang; five images): generated C
-38.5% mean (Flicky -51%, D05 -14%, D09 -36%, D06 -31%, Sonic 1 -61%), bridge CPU -40.7%, bridge wall -46.4%; outcome, frame count, final
state, coverage and frame-stream digests equal to broad on all five. The broad-AOT route rejects D10-D12, E06 and E07
(`effective_address_not_24bit`): no generated-native comparison exists for them.
