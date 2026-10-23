# ADR 0182: SEG-080-T002 - the TAS-versus-control experiment over the full movie replay (decision: LABELS_UNSAFE)

- Status: Accepted (SEG-080-T002). Supplements ADR 0181; ADR 0181's recorded result (`REPLAY_INVALID`, a gate-design artifact) is unchanged.
- Privacy: aggregate only. No address, byte, per-PC list, screenshot, trace or label appears here or in the committed results file.
- Disclosure: Sonic 2 was used in SEG-072 and SEG-077 rule selection (known-truth engineering test, not blind).

## Why this exists, and what it is not

ADR 0181 reported `REPLAY_INVALID` because the frozen Gate A progression rule could not be met inside the first 15 minutes of a 100% movie that
spends them mostly in special stages. The operator then directed the analysis to use the **full replay**. This is a **post-results amendment**:
the T001 playback timeline and results were seen before it was written, so it is not an independent replication of T001's pre-registration. What was
changed: the window (all 115,665 movie frames, control run for the same number of frames with the same policy and seed; the control policy is idle
for the last 45 frames because of the SEG-077 whole-second cutoff, as in T001), the five checkpoints (20/40/60/80/100 % of the movie: 23,133,
46,266, 69,399, 92,532, 115,665), and the Gate A progression rule (at least three level-mode checkpoints in at least three zones, in play order; zone
0x05, the third Metropolis act, was added to the play order after the playback timeline showed it). What was **not** changed: ROM, movie, BizHawk
2.9.1, tracer, native CDL tier, label pipeline, Gate B, Gate C thresholds (24 / 8 / 200) and the decision logic. A new plan (digest `8a235f9e...497c`)
was frozen and verified before any run; a pre-run review found no blocker. A wrapper (`tools/segarecomp_genesis_bizhawk_full.py`) imports the
frozen T001 adapter, so the T001 plan still verifies.

## Decision

**`LABELS_UNSAFE`** under the frozen decision logic. Gate A passes this time; Gate B fails its zero-conflict criterion on three bytes.

## Gate A (passes)

Identity, full-movie end state (the last zone in level mode), TAS repeat determinism (PC, read, edge, CDL and checkpoint digests identical), the
CDL fixture, ROM mapping, vblank/reset execution, self-fetch fraction 1e-5 and the control repeat all pass. Progression: the 20 % checkpoint is in the
first zone (level mode), 40 % in a special stage, 60 %, 80 % and 100 % in three further zones in play order, ending in the last zone. This is the
original emulator's replay used as the reference; no frame-by-frame comparison with the TASVideos video was made.

## Gate B (fails on the zero-conflict criterion)

TAS labels: 37,451 instruction starts, 102,477 continuation bytes, 655,289 data bytes, 253,359 unknown. Starts and continuation bytes are 100 %
correct (0 false starts, 0 data-as-code). **Three data bytes are labelled data where the source truth has code** (accuracy 99.9995 %, above the 99.9 %
bar, but the frozen rule also requires zero code-as-data conflicts). They lie in two 128-byte cells; all three come from the data-read tier (none from
the DMA tier or the header); the seed-1 control has one such byte, which is among the three. Cells containing a wrong label are excluded from every
cell count below. The mechanism is the known limit of a read-based data label (a code byte read by an executed instruction as data); it is not a
tracing or synchronisation defect. The rule was not relaxed.

## Gate C measurements (non-binding because Gate B failed; thresholds unchanged)

| measure (TAS vs seed-1 control, 115,665 frames each) | value |
|---|---|
| executed ROM PCs: TAS / control / intersection / union | 32,606 / 15,023 / 12,725 / 34,904 |
| PCs only in TAS / only in control | 19,881 / 2,298 |
| extra correct code-positive 128-byte cells (threshold 24) | 761 (bootstrap by address block: 381 to 1,182) |
| extra correct mixed cells (threshold 8) | 201 (101 to 314); 163 also extra code-positive |
| extra correct nontrivial negative cells (secondary 200) | 2,895 (2,110 to 3,724) |
| new verified instruction starts / continuation bytes | 20,991 / 60,065 |
| new hard boundary windows / new indirect targets | 2,366 / 930 |
| seed-2 control (sensitivity): extra pos / mixed / negative | 903 / 233 / 3,955 |
| 4-gram instruction signatures new to the TAS, absent from the SEG-072 registry (excluding Sonic 2) | 6,235 of 9,913 |

The control saturates: its executed-PC count (15,023) is the same as in the 54,000-frame T001 run, so after the first half of its random play it adds no
new executed code, while the TAS keeps adding code through the movie. These numbers are far above the engineering thresholds, but under the frozen
logic they do not decide: the experiment's decision is `LABELS_UNSAFE`, not `ACQUISITION_PROMISING`, and the three-byte conflict has to be resolved
by a decision about the data-read rule (for example excluding bytes inside any executed instruction span or admitting data only from CDL data flags
that never touch executed code) in a new pre-registration, not here.

## Resources and checks

CPU ledger 2.50 CPU-hours for this amendment (limit 8), separate from T001's 2.25; no synchronisation-correction attempt was needed. An independent
post-execution review reproduced the evidence (see the task record). Optional model-A sanity check not run (Gate B failed). One game, one movie, one control
policy; no claim about other titles.

## Evidence index

`tools/segarecomp_genesis_bizhawk_full.t002.results.json`, the plan and digest beside it, `tests/segarecomp_genesis_bizhawk_full_test.py`.
