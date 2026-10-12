# ADR 0117: SEG-056 second-generation SMS Z80 executable-region CNN (structural roots, dense byte model) and decision record

- Status: Accepted. Protocol frozen in commit `aa68a95` (plan digest `21db1afb...a852`) before any SEG-056 model was fitted. Outcome: **`CNN_GENERALIZATION_IMPROVED`** (research improvement, not adoption-qualified). The literal frozen sealed gate was missed by an arithmetic margin of 3e-5; native selective admission and a launcher SMS Optimized mode are **not built**.
- Related: ADR 0116 (SEG-051, frozen history, not reopened), 0058/0071/0073 (Z80 AOT/owners), 0100-0103/0113 (M68K executable-region ML).

## Context

ADR 0116 classified the SMS pilot `Z80_RECALL_FAILED`: the raw-byte dilated CNN C1 ranked most code well but a handful of cells forced a fixed budget of 100% of the image. SEG-056 asks whether (1) architectural entry roots, (2) byte-level supervision with edge-aware windows, (3) a larger context, and (4) smaller decision cells make strict source-backed full instruction-start recall achievable at a meaningful retained fraction. Genesis Z80 remains `Z80_BROAD_AOT_CHEAP_ENOUGH` and is out of scope.

## Corpus

The nine SEG-051 lineages were recovered from ignored private research outputs (reference images, truth, token reports, source checkouts, out-of-fold C1 scores) and every image SHA-256 re-verified against the frozen SEG-051 plan; sealed truth was not opened. Two new independent development lineages were qualified from SDCC-built homebrew (Duckslayer Adventures, Stalactites): libraries (SMSlib, PSGlib, crt0) rebuilt from devkitSMS source, relocated ASxxxx listings (`sdldz80 -u`, library objects linked explicitly), rows byte-checked against the ROM, shared-library and runtime bytes masked (`U`, so they cannot create cross-lineage leakage), compiler-emitted constant tables `D`, a lone `.db` wedged between instructions `A`. Decoder agreement 100% (0 of 2504 and 0 of 4646 starts disagree). Maximum 12-byte instruction-run overlap with any development lineage 1.05% (the two SDCC games with each other); 0% against the raw bytes of the sealed images. Corpus now meets the adoption minimum: **8 development + 3 sealed** (sealed unchanged). Caveat: the two SDCC lineages share a compiler and library code (masked). Discarded: Sonic 2 (same engine family, different local revision), smslib/devkitSMS examples (one library, tiny), Tunnel (Game Gear, no asset tooling); Castle of Illusion, Taz-Mania and Wonder Boy III have no source-backed disassembly.

Reproduction: `tools/segarecomp_ml_region_z80_sdcc_build.sh` (sdcc 4.2.0; devkitSMS 533ae573, duckslayer-adventures 7ddffba6, stalactites-sms e3064b0b); the two rebuilds were bit-identical across two runs.

## Failure diagnosis (development lineages only; `tools/segarecomp_ml_region_z80_gen2.diagnosis.json`)

- Labels are sound: for every development lineage the listing/decoder length audit has 0 mismatches and 0 orphan continuation bytes.
- Phantasy Star (0.977): the two worst cells are the image-start vector cells. Cell 0 is never trained by the SEG-051 protocol (crop edges are masked) and the left padding used byte 0, which collides with real `NOP`. They are architectural roots anyway: with roots the residual FRF is 0.540. The SEG-051 post-hoc set `{0,1,2}` was wrong: the architectural cells are the ones holding 0x0000, 0x0038 and 0x0066, i.e. `{0,1,3}` at 32 bytes.
- Super Tennis (0.990): the worst cell is data-dominated (29 `D` bytes, 3 instruction starts): a short fragment after a table that 32-byte mean/max pooling at radius 30 cannot see; roots do not help (0.990).
- Solomon's Key (0.720): a progressive tail of 7 cells that are pure instruction runs (32 starts, repetitive 1-byte forms), each adjacent to other positive cells: a context problem, not a label problem.
- Supervision: Phantasy Star contributed 74 confident negative cells of 16384 and Super Tennis 57 of 1024 under cell labels, while byte-level labels give 3,958 and 2,573 negative bytes (and 29,653 / 12,741 positive bytes).

## Protocol (frozen)

`tools/segarecomp_ml_region_z80_gen2.plan.json`: R = architectural root cells UNION top-`ceil(B*n)` ranked cells (retained fraction includes root overhead); dense byte-level candidates B1 (C1 trunk, dense head), D256 / D512 (local path + stride-4 residual dilated context path, radius 250 / 504 bytes), D512HP (single hard-positive reweighting); PAD token and 512 bytes of real context either side; training windows reach the image edges; lineage-uniform sampling; 3,000 steps, 3 seeds, logits averaged; reductions {max, top2} and cell sizes {32,16} evaluated as aggregation only. Selection: lowest development mean retained fraction at full recall. Fixed budget `B = ceil_0.05(max development residual FRF + 0.05)`; no adaptive estimator. Gates: development strict (B <= 0.65, zero uncovered positive cells), sealed opened only if it passes, sealed success = zero missed positive cells and mean retained <= 0.65.

## Development results (8 lineages, leave-one-lineage-out; `...dev_results.json`)

Mean retained fraction at strict full recall, roots included (worst lineage in brackets):

| Model | mean | worst | 6 SEG-051 lineages mean |
| --- | --- | --- | --- |
| A: historical C1, refit on these folds | 0.471 | 0.972 (Super Tennis) | 0.508 |
| B: C1 + structural roots | 0.451 | 0.972 | 0.481 |
| B1 (byte labels, edge-aware, PAD) | 0.423 | 0.980 | |
| D256 | 0.384 | 0.854 | |
| D512 | 0.302 | 0.580 | |
| D512HP, cell 32, top2 (selected) | **0.301** | **0.592** | 0.288 |

Attribution (same folds, cell 32): roots -0.020 (nearly all Phantasy Star: 0.762 -> 0.603); byte labels + edge-aware windows + PAD -0.028; larger context/two-scale -0.040 (radius 250) and -0.121 (radius 504); hard-positive reweighting -0.001 (none); max versus top2 reduction ~0.000; 16-byte cells **worse** (+0.03: mean 0.332 versus 0.301 and worst 0.756 versus 0.592) because the root set and fragment ranking do not benefit. Seed variance is large: single-seed worst lineage ranges 0.574-0.899 and the Super Tennis seed std-dev is 0.168; the 3-seed logit ensemble is part of the method. Residual FRF per lineage for the selected model: Columns 0.159, Duckslayer 0.315, 2048 0.110, Knights 0.202, Phantasy Star 0.153 (was 0.977), Solomon 0.513 (was 0.720), Stalactites 0.361, Super Tennis 0.592 (was 0.990). Fixed budget `ceil_0.05(0.592+0.05) = 0.65`; zero missed positive cells on every development fold at that budget. Development gate: passed, **narrowly** (0.65 equals the gate).

Improvement criterion (>= 0.10 lower mean than B on the same folds, no worse worst lineage): 0.451 -> 0.301 and 0.972 -> 0.592: **met**.

## Sealed evaluation (once, `...sealed_results.json`)

The selected candidate was refit on all 8 development lineages (3 seeds; weights SHA-256 `6bee7772...4f11`) and scored once on the three sealed lineages at B = 0.65 (cell 32, top2, roots):

| Sealed lineage | positive cells | missed | full-recall FRF (descriptive) | retained at B |
| --- | --- | --- | --- | --- |
| Alex Kidd | 679 | **0** | 0.324 | 0.6501 |
| Lord of the Sword | 954 | **0** | 0.138 | 0.6500 |
| Sonic 1 | 1204 | **0** | 0.214 | 0.6500 |

Strict source-backed full recall held on all three sealed lineages with a wide margin (they would be fully covered at 14-32% retained). The frozen success rule additionally requires mean retained <= 0.65; the unrounded mean is 0.650033 because the three structural-root cells outside the top-K add 3e-5 to a 0.65 budget on one lineage. By the literal rule `sealed_success = false`. The rule is **not** relaxed after the fact; the sealed cohort is now spent for this model, so any re-registration (for example budgeting `B` so that root overhead is included) would need new, unseen lineages for blind evidence. Nothing about the failure indicates missed code; the budget is conservative because it is set by the weakest development fold (Super Tennis, high seed variance) with a +0.05 margin.

## Economics and cost

Projection (ADR 0116 model, `-3.0 + 2.441 s/KiB`, a **projection**, no native producer exists): retaining 65% saves 35-36% of SMS build CPU (32-512 KiB images). Model: 43,809 parameters (175 KB fp32); PyTorch single-thread inference 0.15 CPU-s per 128 KiB image, 250 MB peak RSS (research tooling). No native C inference, size or real build savings were measured because native integration was not justified.

## Decision

| Item | Result |
| --- | --- |
| Primary classification | `CNN_GENERALIZATION_IMPROVED`: strong development improvement, zero sealed misses, adoption not qualified |
| `Z80_ML_ADOPTION_GATES_PASSED` | No: the literal sealed selectivity gate missed by 3e-5, no native producer, no real economics, no Z80 selective-admission contract |
| `Z80_ML_RECALL_BLOCKED` | No: no code missed; budget economic |
| `Z80_ML_CORPUS_BLOCKED` | No: 8 + 3 lineages, two new independent |
| Native Z80 selective admission, launcher SMS Optimized | **Not implemented** (gate not passed; ADR 0116 admission seam finding stands: no admitted-start set, typed unadmitted-PC failure or validator on the Z80 side) |

Follow-ups (not done): a new registered attempt whose budget accounts for root overhead and whose blind evidence comes from further unseen lineages; a Z80 selective-admission contract before any native producer.

## Environment and post-freeze notes

- Execution: CPU (PyTorch 2.14 CPU build, deterministic algorithms, 2 threads per fit) produced every result. A ROCm PyTorch 2.6.0+rocm6.4.2 (HIP 6.4.43484, Radeon RX 9070 XT `gfx1201`, WSL2) path was added (`--device cuda`, never a silent fallback) and benchmarked: deterministic run-to-run bit-identical, but at this model size the GPU step costs 27 ms (batch 8, GEMM-fallback convolution kernels, linear in batch) versus about 33 ms for one 2-thread CPU process, so it did not beat the 8-process CPU farm and was not used for registered results. CPU and GPU results are not bit-identical (B1 / 2048-SMS, seed 1: residual FRF 0.091 versus 0.094, max logit difference 6.9): do not mix devices within a candidate.
- Post-freeze edits that do not change the plan or any decision: `--device`, validated atomic result publication, efficient recall-budget search in `metrics` (re-verified identical on the earlier diagnosis), single-shot sealed script.
- The plan's `unit.context` text says "MARGIN=512"; the receptive-field radius of D512 is 504 bytes (test-asserted).
