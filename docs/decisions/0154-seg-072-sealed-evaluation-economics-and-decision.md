# ADR 0154: SEG-072 one-shot sealed evaluation, compilation economics and M68K ML v3 decision

- Status: Accepted. **Final classification: `RECALL_FAILED`** (ADR 0151 precedence: a confidently labelled sealed start outside R). Secondary
  facts, recorded and not hidden under the primary class: the corpus minimums were not met (`CORPUS_INSUFFICIENT` for the full gate:
  development 11/12, sealed 5/7) and the frozen pair fails the economics gate against the incumbent. Independent adversarial review: T007
  (appended below).
- Scope: offline research only. **No production change**: production gen2 M3 + B35, Optimized v1, the structural prune, the validator, the
  launcher defaults and the Sega CD admission policy are unchanged. No model weights trained on private inputs are published.
- Inputs: frozen record `tools/segarecomp_ml_v3.frozen.json` (committed in `3f7d08b`, ADR 0153); sealed binding ADR 0152. Aggregates:
  `tools/segarecomp_ml_v3.sealed_results.json`.

## One-shot sealed evaluation (T006)

`segarecomp_ml_v3_eval.py sealed` verified the committed frozen record and wrote the one-shot marker before reading any sealed truth; every
sealed image and class digest matched the committed binding. Frozen pair: **M3-R + B50** (final refit on all 11 development lineages, weights
`20bfa46f…`), R = top ceil(0.50 x cells) 128-byte cells UNION production seed cells.

| sealed lineage | labelled starts | missed | retained | FRF (descriptive) | false-admission candidates | deterministic |
|---|---|---|---|---|---|---|
| E01 Alien Soldier | 72,444 | **0** | 0.500 | 0.151 | 132,181 | yes |
| E02 Landstalker | unqualified (ADR 0152) | - | - | - | - | - |
| E03 Kid Chameleon | 33,508 | **0** | 0.500 | 0.130 | 147,353 | yes |
| E04 QuackShot | unqualified (ADR 0152) | - | - | - | - | - |
| E05 Sonic Spinball | 49,140 | **40** | 0.500 | 0.748 | 19,641 | yes |
| E06 Tank Battle | 2,784 | **0** | 0.500 | 0.343 | 7,300 | yes |
| E07 The Weave | 24,952 | **0** | 0.500 | 0.125 | 163,298 | yes |

**Zero-miss gate: FAILED** (E05). Descriptive post-unseal diagnosis (no change to anything frozen): the 40 starts lie in two 128-byte cells
(18 and 22 starts, ~30% code coverage each, adjacent in one area of the image) - mixed code/data fragments that the model ranks below the
50% budget (E05 full-recall fraction 0.748). Stated limitation: Spinball's rows come from an IDA-derived disassembly whose readme warns that some
data was interpreted as code; the pre-registered rule (ADR 0151 `labels.ida_derived`) counts its instruction rows as confident labels, and no
row was reclassified after unsealing. The other four qualified lineages hold full recall with wide margins (FRF 0.13-0.34 against a 0.50
budget). Per the brief, nothing was refitted, recalibrated or widened after the miss.

Structural closure (unchanged prune + validator; broad-accepted sealed images): E01 accepted, K/U 0.499, C outside K = 0; E03 accepted, K/U
0.486, C outside K = 0; E05 accepted, K/U 0.483, **C outside K = 104** (the 40 R misses plus starts the prune then drops). E06/E07: the production
broad-AOT enumerator rejects the images (`effective_address_not_24bit`), so there is no structural stage or generated-native comparison.

Bounded generated-native execution (broad vs frozen pair explicit admission plan vs the incumbent native gen2 route; 120 frames, explicit
instruction budget, clang): outcome, frame count, final-state, coverage and frame-stream digests equal to broad for both the frozen pair and the
incumbent on E01, E03 and E05 - including E05, whose short trace does not reach the missed code. Runtime-equivalent short traces are not
evidence of static recall.

TMSS: `TMSS_DIAGNOSTIC_UNAVAILABLE` (no authorized firmware input; ADR 0152). No firmware claim is made; no claim about authentic Sega CD BIOS
optimization follows from game or homebrew results.

## Compilation economics (per platform)

**Genesis immutable ROMs** (sealed E01, E03, E05; bridge = emission + strict-C11 compile + bounded run):

| image | generated C vs broad | bridge CPU vs broad | generated C vs incumbent M3+B35 | bridge CPU vs incumbent |
|---|---|---|---|---|
| E01 | -58.6% | -61.7% | -22.1% | -28.8% |
| E03 | -41.4% | -42.6% | +33.9% | +36.1% |
| E05 | -43.1% | -43.7% | +34.1% | +32.1% |
| mean | **-47.7%** | **-49.3%** | **+15.3%** | **+13.1%** |

Gate versus broad (generated C <= -30%, compile CPU <= -25%): met. Gate versus the incumbent (-10% generated C and -10% CPU, or a reliability
improvement at <= +10% generated C): **not met** (+15.3% mean; the development reliability improvement does not offset it, and E05's sealed miss
removes the reliability claim on sealed evidence). ML inference cost (research PyTorch, CPU, whole image): 0.45-7.1 s per sealed image versus
86-442 s bridge CPU saved against broad - net positive against broad; the incumbent already has a native inference path. Cache-reuse effects were
not measured separately (each bridge run is a cold, single-image build). Development baseline (ADR 0153): incumbent -38.5% generated C on five
images.

**Sonic CD verified application modules** (36 modules, development out-of-fold; R-only because no Sega CD R-admission path exists - ADR
0131/0133 - and none was built): incumbent M3 + B35 reproduces ADR 0140's 306 missed starts in three Work RAM copy modules (mean retained 0.428);
M3-R + B50 misses none (mean retained 0.557; per class: Word RAM in place 0.500, Work RAM copy 0.501, PCM driver 0.500, sub program 0.726). No
generated-C or compile-time reduction can be measured or claimed for Sega CD.

**Broad materialized M68K images**: not comparable - no existing safe admission contract admits partial ML proposals for materialized
images, and none was added (brief).

## Decision (pending T007 review concurrence)

| criterion (ADR 0151 precedence) | result |
|---|---|
| 1 complete development pair exists | yes (M3-R+B50/B60, M4-Dense@64+B50/B60) |
| 1 sealed zero-miss | **no - E05 misses 40 starts** -> `RECALL_FAILED` |
| 2 corpus minimums 12 / 7 | not met (11 / 5) - would be `CORPUS_INSUFFICIENT` |
| 3 economics vs incumbent | not met (+15.3% generated C) - would be `ECONOMICS_NOT_JUSTIFIED` |

**Classification: `RECALL_FAILED`.** M3+B35 stays the production experimental `gen2` policy; Compatibility (broad AOT) stays the default and
the unconditional fallback. What this means and does not mean:
- The expanded corpus does improve development recall: with ten more lineages, retraining the frozen architecture (M3-R) or the byte-dense
  M68K model (M4-Dense@64) reaches complete leave-one-lineage-out recall at B50, which the frozen M3 cannot at any budget up to B60 - but only at
  a ~43% larger budget than B35, and the improvement did not survive the sealed test (E05).
- B35 is not a safe budget for dense-code images: every model misses D07's dense code at B35 (density/budget failure), and frozen M3's B35 already
  misses 306 starts in Sonic CD Work RAM copy modules and 7,760 in D07 on development.
- Adaptive budgets did not help: the single label-free density policy never reached complete recall, and abstained on most images.
- The byte-dense M4 model is not justified over the simpler M3-R: equal mean retention, worse K/U and weaker on Shining Force.
- No production integration, model replacement, launcher default change or Sega CD admission-policy change follows. Any further attempt needs a
  new pre-registered protocol with fresh sealed lineages (E05 and the other four are now exposed), and should treat small mixed code/data
  fragments and dense-code images explicitly.
