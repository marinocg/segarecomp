# ADR 0184: SEG-081 - three-game TAS acquisition and cross-engine validation: pre-registration (checkpoint C0)

- Status: Accepted (SEG-081-T001, checkpoint C0). Follows ADRs 0181-0183 (unchanged: REPLAY_INVALID, LABELS_UNSAFE, INCONCLUSIVE).
- Privacy: aggregate only. ROMs, movies, traces, labels, signatures, screenshots and per-address data stay in the ignored private directory.

## What is frozen

Plan `tools/segarecomp_genesis_tasbatch.plan.json`, digest `a51546ad2c1b2c62ab329f0299d7504548c7c7a9db0aa1b77ef781333d652845`, frozen before any commercial
trace, label or model run. It binds the batch tooling, the unchanged SEG-080 modules, the model-A trainer, the SEG-072 corpus manifest/plan and the offline
tests. Games and order: Vectorman 2 (TASVideos 8668S), The Adventures of Batman & Robin (6248M), Earthworm Jim 2 (5952M / 9027S); one movie each, full length,
two traced TAS replays, an untraced playback, a seed-1 primary and a seed-2 sensitivity control. Ceilings: 8 CPU-hours per game, 24 in total, enforced by a
launch guard.

- **Identity:** ROM size and SHA-256, movie SHA-256 and input-log digest, movie-header hash (an MD5) equal to the local ROM MD5, core, frame count, no
  savestate, pinned BizHawk 2.9.1 files. The three local ROM MD5s match their movie headers. A mismatch rejects the game (`INPUT_REQUIRED`).
- **Corpus isolation:** none of the images is in the SEG-072 sealed/blind manifest (checked by SHA-256/SHA-1 in code) or the development registry. Vectorman 2
  and Earthworm Jim 2 are sequels of the SEG-077 development candidates Vectorman and Earthworm Jim: engine-adjacent, not identical, disclosed.
- **Replay evidence (generic):** five checkpoints at floor(k*N/6) plus the final state, each with work-RAM, VRAM, CRAM and VSRAM digests; playback must reach
  FINISHED at N frames; at least five distinct (RAM, VRAM) states; first-executed PCs (>= 0.5 % of the run's distinct PCs) in at least three of five
  movie-time fifths; playback and traced digests identical; two traced runs identical; an unreadable domain invalidates the gate.
- **Control:** the SEG-080 seeded policy ported verbatim (tested against the Lua), delivered as a `.bk2` that reuses the TAS movie's header, controller and sync
  settings (SEG-080 used BizHawk defaults), so ROM, core configuration, frame count, tracing and labelling are identical by construction.
- **Labels:** `seg080.label_policy.v1` (E/R/H/M/W/U, strong and strong+weak variants). No source truth exists: consistency means decode audit, ROM mapping,
  two-decoder agreement, and E-code inside DMA sources <= 0.5 % of E bytes. Weak labels never count towards the strong-code rule.
- **Per-game class:** `QUALIFIED_USEFUL` needs extra strong code cells vs the seed-1 control >= 100, or (>= 25 and >= 20 % of the control's), and at least one
  structural signature (instruction form or labelled-target control-flow pattern) absent from the registry and from the control; 500 extra cells is an
  ambitious, non-binding target. Other classes: `QUALIFIED_LOW_YIELD`, `REPLAY_INVALID`, `TRACING_UNAVAILABLE`, `LABEL_POLICY_CONFLICT`, `INPUT_REQUIRED`,
  `BUDGET_EXHAUSTED` (precedence in that order, input first).
- **Batch decision:** `CROSS_ENGINE_ACQUISITION_SUPPORTED` (>= 2 useful games, no unresolved major inconsistency), `_MIXED`, `_NOT_SUPPORTED` (>= 2 qualified,
  none useful), `EVIDENCE_INSUFFICIENT` (none qualified). Independent of the model-A verdict.
- **Model A:** only with >= 2 qualified games; frozen HGB; leave-one-lineage-out over all 11 qualified SEG-072 lineages; arms B0, C, T, TW and a matched-volume
  sensitivity arm; shingle-containment isolation; fixed operating point at B0 recall 0.90; verdict `ML_GAIN_SUPPORTED` / `ML_HARM_SUPPORTED` /
  `ML_INCONCLUSIVE` with a saturation precondition and a volume-confound rule.

## Pre-freeze reviews and rehearsal

Two independent reviews (plan, then delta) found and drove fixes before the freeze: a novelty rule satisfiable by partial coverage, unreadable-domain digests,
failed runs that could not be classified, unenforced ceilings and wall-clock-dependent cost, a missing sealed-manifest check, unbound inputs, a trivial
progression rule, and ML rule asymmetries. A rehearsal on the existing SEG-080 Sonic 2 data (not one of the three games) reproduced the SEG-080 coverage
(758 extra strong code cells) and the model-A code path (all arms within 0.0001 AP: the saturated baseline gives `ML_INCONCLUSIVE`, as the plan anticipates).
The rehearsal used no result from the three games. Post-freeze corrections, if any, are recorded as amendments with what had been observed.

## Disclosures

Player-2 inputs: the control moves player 1 only. The native CDL data flag is blanket (boot checksum sweeps read nearly every ROM byte), so E bytes also read as
data are counted but never gating. A title-screen-only control can fail the vblank/reset fidelity check and then yields `TRACING_UNAVAILABLE`.
