# ADR 0181: SEG-080-T001 - Sonic 2 TAS-versus-control acquisition experiment (decision: REPLAY_INVALID, a gate-design artifact)

- Status: Accepted (SEG-080-T001). Follow-up to SEG-077; ADR 0174 (`STOP_ACQUISITION` for the earlier Chuck Rock protocols) is unchanged.
- Privacy: aggregate only (counts, rates, digests, hours). No address, byte, per-PC list, screenshot, trace, label or movie input appears here or in the
  committed results file. All raw artifacts stay in the ignored private workspace.
- Disclosure: Sonic 2 was used in SEG-072 and in SEG-077 rule-selection work. This is a controlled known-truth engineering test, not a blind
  generalisation study, and one game, one movie, one control policy and one window cannot support a claim about Genesis games in general.

## Decision

**Primary result under the frozen decision logic: `REPLAY_INVALID`.** The cause is the pre-registered Gate A *progression* rule, not any observed
desynchronisation: the first 15 minutes of this 100% movie are mostly special stages, so only one of the five frozen checkpoints is in level mode and
the rule (at least two level-mode checkpoints in two zones, in play order) cannot be satisfied. Every other Gate A condition passed, and Gate B and the
Gate C thresholds passed. The result is **not** converted to a code-yield verdict, and the rule was not changed after the outputs were seen.

This ADR therefore reports both facts without merging them: (1) the frozen primary result is `REPLAY_INVALID`; (2) every measured quantity that would
have been decisive had Gate A passed is on the favourable side of its frozen threshold (below). Per the task's interpretation rules `REPLAY_INVALID`
does not falsify TAS-based code acquisition, and the post-hoc observation in (2) is not an `ACQUISITION_PROMISING` finding.

## Pre-registration and amendments (all before any result was used)

Plan `tools/segarecomp_genesis_bizhawk.t001.plan.json`, digest `c206922d...6234`, bound to the adapter, the unchanged SEG-077 label pipeline, the decoder
CLI, the source-truth file digest and the registry digest; `run`, `label` and `evaluate` verify it. Two independent reviews of the plan (pre-run and
delta) drove amendments A1-A3 (enforced freeze and pinned inputs, control-policy frame alignment with the SEG-077 Lua, Gate A expected zone order and
end state, vblank/self-fetch checks, a complete CPU ledger, extra cells excluding cells the control labelled wrongly, a seed-2 sensitivity control).
A2 fixed a tool defect (`movie.*` Lua calls raise when no movie is loaded). A4 was requested by the operator: BizHawk's native Code/Data Logger.

**Disclosure on A4.** A first batch of traces (without the CDL) completed before A4 and its aggregate ROM-PC counts (TAS 14,944 vs control 15,023)
were seen before A4 was written. Those traces are superseded and archived privately; A4 changed only the evidence tiers (identically for both
conditions), not the thresholds or the decision logic.

## Inputs and tracer

World REV01 image (1,048,576 bytes; SHA-1 `8bca5dce...`, SHA-256 `193bc406...`); movie tasvideos 5640M `.bk2` (SHA-256 `85403cb6...`; core
Genplus-gx, BizHawk 2.9.1, two-player, six-button sync, no savestate, 115,665 input frames, ROM MD5 matches the movie header's hash field); the official
BizHawk 2.9.1 linux-x64 package (SHA-256 `5afb39af...`) under Mono 6.8, run as an unprivileged user under Xvfb. The operator replayed the movie
manually first. Traces use BizHawk's own M68K bus execute and read callbacks plus the native CDL (started from Lua, log read by reflection; no
BizHawk change). A synthetic fixture (one 64-byte ROM-to-VRAM DMA, word/odd-byte/even-byte reads) reproduces executed PCs, the DMASource extent and the
data-read extents exactly, with no exec/data overlap. Reset and state-load behaviour were not exercised (power-on movie, no loads, no savestates).
The platform differs from the movie author's (Linux/Mono instead of Windows); the waterbox core is deterministic.

## Gate A (synchronisation and tracing)

- Identity checks passed; the full movie replays to its end in the original emulator, final state is the last zone (Death Egg) in level mode; lives,
  act and zone progression over the whole movie are consistent with the published goal. The frozen window plays seven special stages first, then
  level play.
- Frozen checkpoints (2, 5, 8, 11, 15 min): level mode at 2 min only; special-stage mode at the other four (act counter advances 0 to 1). The frozen
  progression rule requires two level-mode checkpoints: **failed** (`progression=false`).
- Determinism: TAS repeat identical in PC-set, read-set, edge-set, CDL and checkpoint-state digests; control repeat identical in PC/read/edge/CDL.
- Fidelity: ROM mapping mismatches 0; odd PCs 0; accepted-E fraction 0.99993; CDL exec flags agree with callback PCs on every byte; self-fetch
  fraction 1e-5; reset and vblank PCs executed; fixture passed (both TAS and control).
- No reference TASVideos video was compared frame by frame; the original-emulator replay is the reference, as pre-registered.

## Gate B (independent source-truth evaluation; truth opened only by `evaluate`)

TAS labels: 18,523 instruction starts, 46,885 continuation bytes, 252,692 data bytes, 730,476 unknown; accuracy 100% for each class, 0 false starts, 0
code-as-data and 0 data-as-code conflicts, 0 error-bearing cells; the TAS-minus-control new labels (5,250 starts, 13,414 continuation, 124,010 data)
are likewise exact. The control has one wrong data byte over code (1 error-bearing cell). The truth is complete for this image, so no label lies over
reference-unknown bytes. **Gate B: pass** (thresholds >= 99.9%, zero code-as-data).

## Gate C measurements (non-binding because Gate A failed; thresholds unchanged and not moved)

| measure (TAS vs seed-1 control, equal 54,000 frames) | value |
|---|---|
| executed ROM PCs: TAS / control / intersection / union | 14,944 / 15,023 / 9,747 / 20,220 |
| PCs only in TAS / only in control | 5,197 / 5,276 |
| extra correct code-positive 128-byte cells (threshold 24) | 168 (bootstrap by address block: 38 to 349) |
| extra correct mixed code/data cells (threshold 8) | 35 (7 to 72); 31 of them are also extra code-positive cells |
| extra correct nontrivial negative cells (secondary 200) | 915 (407 to 1,532) |
| new hard boundary windows / new indirect targets | 856 / 180 |
| seed-2 control (sensitivity only): extra pos / mixed / negative | 184 / 38 / 1,135 |
| 4-gram instruction signatures new to the TAS vs control, absent from SEG-072 registry (excluding Sonic 2) | 1,911 of 2,786 |
| 3-cell class-pattern windows only in the TAS, absent from registry | 0 of 1 |

The control, not a historical trace, is the comparator: it also reaches 15,023 PCs, so the equal-duration control is strong; the TAS's marginal yield
comes from different code, not from a larger executed set. The novelty numbers are descriptive and have no threshold.

## Resources

CPU ledger 2.25 CPU-hours (limit 8) across the playback, five traces, fixture, three labellings and evaluation; BizHawk wall clock was about twice the CPU
time because runs were parallel. Engineering investigation stayed inside the 3-hour limit; no synchronisation-correction attempt was needed. The
optional model-A sanity check was not run (Gate A did not pass).

## Interpretation and limits

- `REPLAY_INVALID` here does not mean the movie desynchronised: the full-movie replay, the repeat determinism and the fixture-validated tracer all
  hold. It means the pre-registered evidence of progression could not be produced inside the frozen window because the gate counted only level mode
  while the window is mostly special stages (a mode set that I guessed from game-logic knowledge, not from the movie).
- The measured marginal yield is far above the engineering thresholds on this one game and is robust to the second control seed, but it is one movie,
  one game, one control policy family and one window. It does not establish a general acquisition result.
- Honest follow-up options (not started; each needs its own pre-registration and the operator's decision): re-pre-register Gate A progression with
  special-stage-aware checkpoints, and/or a window that follows the special stages; then repeat on an independent title. No 16-game expansion follows
  from this ADR.

## Evidence index

Aggregate results: `tools/segarecomp_genesis_bizhawk.t001.results.json`; plan and digest beside it; tests `tests/segarecomp_genesis_bizhawk_test.py`.
