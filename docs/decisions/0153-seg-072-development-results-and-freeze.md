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

## T004 - adaptive versus fixed budgets on frozen-M3 out-of-fold predictions

Policies exactly as pre-registered (ADR 0151): fixed B35/B45/B50/B60 and the single ADAPT policy (`e` = fraction of cells with probability
>= 0.5; margin = max over the OTHER lineages' images of (residual FRF - e) + 0.05; budget ceil_to_0.01(e + margin); abstain to broad above
0.60); broad AOT as the reference (retained 1.0). Identical folds, seeds, rounding and recall accounting for every policy.

| policy (M3_FROZEN) | complete | missed starts | mean retained (lineage mean) |
|---|---|---|---|
| B35 | no | 8,066 | 0.357 |
| B45 | no | 637 | 0.456 |
| B50 | no | 2 | 0.505 |
| B60 | no | 2 | 0.604 |
| ADAPT | no | 2 | 0.947 (15 of 50 images abstain) |
| broad AOT | yes | 0 | 1.000 |

ADAPT behaviour: the residual-minus-density excess is dominated by one record (the Sonic CD Work RAM copy module with residual FRF 0.949), so
every lineage other than D01 inherits a margin of 0.826 and abstains entirely; D01's own margin, fitted without that module, is 0.153, which
gives 35 of its 39 images budgets of 0.28-0.54 and still misses the module's fragment (2 starts). The label-free density estimate does not
predict the residual tail: images with e from 0.011 to 1.0 occur, and the hard fragment sits in an image whose e is far below its residual
FRF. Neural confidence did not bound full recall.

**T004 classification: adaptive budgets are not justified for frozen M3** (ADAPT is neither complete nor cheaper than fixed budgets that miss
the same fragment, and its abstentions retain ~95%). No fixed budget is complete either; the smallest fixed budget that would cover every
development image is 0.95 (descriptive only - not a policy). The ADAPT definition is reused unchanged for every T005 candidate.

## T005 - training backend, candidate comparison and freeze

### Training backend (pre-registered benchmark)

Final M4-Dense architecture (52,901 parameters per network) and batch, all 50 development images, warm-up 50 steps + a 500-step epoch, 3
repeats. CPU: the actual farm (8 concurrent 2-thread processes): median 217 ms/step per process (194-241; one probe digest across all 24 runs).
ROCm (PyTorch 2.6.0+rocm6.4.2, Radeon RX 9070 XT `gfx1201`, WSL2; the PyTorch 2.13+rocm7.2 environment sees no device): median 30 ms/step
(24-30), peak 41 MB VRAM, the three repeats reproduce identical outputs. Data loading ~1 ms per batch on both. Estimated M4-Dense
cross-validation (33 fits x 4000 steps): CPU farm 4,344 s vs ROCm (one GPU process) 3,958 s - **8.9% improvement < 25%: parallel CPU
selected**. Frozen environment: Python 3.13.14, numpy 2.5.3, scikit-learn 1.9.1, torch 2.14.1 (the `+cu130` wheel, run on CPU only), float32,
`torch.use_deterministic_algorithms(True)`, threads M3-R 4 / M4-Dense 2. Every candidate was trained on the same backend.

### Development comparison (11 lineages, 50 images, leave-one-lineage-out)

Missed labelled starts / mean retained fraction (lineage mean; seeds and integer rounding included). Worst-lineage FRF is 1.0 for every
configuration (all-positive tiny Sonic CD modules), so it never separates candidates.

| configuration | B35 | B45 | B50 | B60 | ADAPT |
|---|---|---|---|---|---|
| M3_FROZEN | 8,066 / 0.357 | 637 / 0.456 | 2 / 0.505 | 2 / 0.604 | 2 / 0.947 |
| M1-HGB | 7,929 / 0.357 | 537 / 0.456 | 1 / 0.505 | 1 / 0.604 | 1 / 0.949 |
| **M3-R** | 8,121 / 0.357 | 435 / 0.456 | **0 / 0.505** | 0 / 0.604 | 32 / 0.585 |
| M4-Dense@128 | 5,603 / 0.357 | 511 / 0.456 | 2 / 0.505 | 1 / 0.604 | 7 / 0.923 |
| M4-Dense@64 | 5,530 / 0.357 | 460 / 0.455 | **0 / 0.505** | 0 / 0.604 | 69 / 0.870 |

Complete pairs: M3-R + B50/B60 and M4-Dense@64 + B50/B60. Structural closure (unchanged prune + validator, 11 broad-accepted Genesis images):
all accepted with C outside K = 0 for M3-R B50 (mean K/U 0.437), M3-R B60 (0.514), M4-Dense@64 B50 (0.475), B60 (0.562); D07 is now accepted
(rejected under M3_FROZEN B35). At B35 every model misses D07's dense code (density/budget failure: 194-248 missed cells, coverage ~0.96);
M3-R/M4-Dense remove the Sonic CD fragment that defeats M3_FROZEN and M1-HGB at B50/B60 (D01 residual FRF 0.443-0.448 vs 0.949/0.815);
M4-Dense@128 and @64 are weakest on Shining Force (D03 max FRF 0.608 / 0.477). ADAPT is never complete: with expanded-corpus models the
density estimate under-predicts the residual tail on D03 (M3-R 32 misses) while abstaining on most homebrew.

### Selection (frozen rule)

1 complete: {M3-R+B50, M3-R+B60, M4-Dense@64+B50, M4-Dense@64+B60}; 2 worst-lineage FRF: all 1.0 (tie); 3 mean retained: tie set {M3-R+B50
(0.5048), M4-Dense@64+B50 (0.5047)}; 4 mean K/U: M3-R 0.437 vs M4-Dense 0.475 (> 0.02 apart) -> **M3-R + B50**. Incumbent check: M3_FROZEN+B35
is incomplete (8,066 misses), so the complete alternative replaces it (rule `selection_order`, not `incumbent_retained`).

### Freeze

`tools/segarecomp_ml_v3.frozen.json` (committed in this checkpoint, before any sealed truth is opened): configuration **M3-R**, policy **B50**,
no adaptive margin; final refit on all 11 development lineages with the unchanged M3 recipe (seed 49, 30 epochs, 4 threads, 574 s); weights digest
`20bfa46f0ec2e4278d75fb5880381f55f58fe204a23577dfabcd5d60b5f6f005`, reproduced bit for bit by a second independent refit; CPU backend and
the environment above; the selection-table digest binds the development evidence. From this commit on no model, policy, threshold, rounding,
seed or label rule may change; the sealed corpus is opened once by `segarecomp_ml_v3_eval.py sealed` (marker written before any sealed truth is
read).

### Review notes (T007, no change to any frozen result)

- Retained fraction is computed in bytes (|R| bytes / image bytes) where ADR 0151 states |R| / cells; they differ only by the short last cell
  (at most ~1e-4 here) and change no selection or tie set.
- `residual_frf` counts seed cells that appear in the ranked prefix; ADR 0151's "seeds excluded from the count" is ambiguous on this. It only
  enters the ADAPT margin, which was never complete or selected.
- The selection margin is thin: worst-lineage FRF is degenerate and mean retained ties, so step 4 (K/U 0.437 vs 0.475) decided between M3-R and
  M4-Dense@64; within the tie band M3-R would also win as the simpler model.
- The historical reproduction overwrote the private gen2 fold files in place; the claim rests on the plan-committed digests.
