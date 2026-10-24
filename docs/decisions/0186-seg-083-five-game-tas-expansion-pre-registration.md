# ADR 0186: SEG-083 - five-game TAS corpus expansion and neural-retraining readiness: pre-registration (checkpoint C0)

- Status: Accepted (SEG-083-T001, checkpoint C0). Follows ADRs 0181-0185 (unchanged: REPLAY_INVALID, LABELS_UNSAFE, INCONCLUSIVE, then SEG-081's CROSS_ENGINE_ACQUISITION_SUPPORTED and ML_INCONCLUSIVE).
- Privacy: aggregate only. ROMs, movies, traces, labels, signatures, screenshots and per-address data stay in the ignored private directory; derived training labels are retained
  only in the private harness (`private-corpus/seg083-v1`, SEG-082 policy), never in this repository.

## What is frozen

Plan `tools/segarecomp_genesis_tasbatch5.plan.json`, digest `228647dd43f4ba8b66fcfb2a6633a641a388b5a7052e1423cfb78c16cb2d4b61`, frozen before any commercial trace, label or model run. It binds the five-game layer, the unchanged
SEG-081 batch tool and its SEG-080 modules, the model-A trainer and the SEG-072 corpus manifest. Games and order: Shadowrun (TASVideos 8945S), NHL '94 (9294S), Shove It!
...The Warehouse Game (5781M), Doraemon: Yume Dorobou to 7-Nin no Gozans (8708S), The Terminator (8956S). The local ROM and movie metadata are authoritative; the
TASVideos ids are recorded as candidate identities. One movie per game, full length, two traced TAS replays, one untraced playback, one primary control and one comparability
control. Ceilings: 8 CPU-hours per game, 40 in total, enforced by a launch guard.

- **Identity:** unchanged SEG-081 procedure (ROM size, SHA-256/SHA-1/MD5, movie SHA-256 and input-log digest, movie-header hash equals the ROM MD5, core, frame count, no
  savestate, controller count, six-button setting, pinned BizHawk 2.9.1). All five local ROM MD5s equal their movie-header hashes. Shadowrun and Shove It! were recorded on BizHawk 2.8
  and are replayed on 2.9.1 (one bounded compatibility attempt each); no input or setting is altered.
- **Corpus isolation:** none of the images is a sealed/blind entry or a registry member (machine-checked). NHL '94 is a SEG-077 development candidate; its acquisition tasks were
  cancelled, so no registry or label data exist for it (valid new lineage, disclosed). Developer/engine adjacency is measured as 12-byte code-shingle containment against every
  registry lineage, every SEG-081 game and every earlier game of the batch (pairwise > 0.30 means one lineage cluster; > 0.30 against the registry means no independent diversity).
- **Control correction.** SEG-081's controls moved player 1 only, and sat near the title screen (26-36 % of the TAS's executed PCs). The **primary control now drives every
  controller present in the movie** with independent seeded streams (player 1 = the SEG-081 stream, player k a stream seeded `seed*7919 + 104729*(k-1)`), delivered as a `.bk2` that
  reuses the TAS header, core, controller and sync settings. The SEG-081 player-1-only control is kept as a secondary comparability baseline (`ctl2_a`); the primary class never depends
  on it. Both control/TAS executed-PC ratios are reported per game. No game-playing policy and no per-game script was added; if the controls remain shallow the result is
  disclosed as a limitation: it establishes executed-code yield, not optimality against another intelligent policy.
- **Per-game class** (unchanged SEG-081 rule, applied against the primary control): `QUALIFIED_USEFUL` needs extra strong code cells >= 100, or (>= 25 and >= 20 % of the control's), and
  at least one structural signature absent from both the registry and the control. Other classes: `QUALIFIED_LOW_YIELD`, `REPLAY_INVALID`, `TRACING_UNAVAILABLE`,
  `LABEL_POLICY_CONFLICT`, `INPUT_REQUIRED`, `BUDGET_EXHAUSTED`. Classes are per game: a failed movie never invalidates another game.
- **Acquisition class:** `EXPANSION_SUPPORTED` (>= 3 of 5 useful, no unresolved major inconsistency), `EXPANSION_MIXED`, `EXPANSION_NOT_SUPPORTED`, `EVIDENCE_INSUFFICIENT`.
  Independent of the model-A and readiness outcomes; `ML_INCONCLUSIVE` is not failed acquisition.
- **Labels:** `seg080.label_policy.v1` unchanged (E/R/H/M/W/U, strong and strong+weak variants); weak-read loss under RAM execution is reported per game.
- **Model-A diagnostic** (bounded; runs once >= 3 new games qualify and at the final checkpoint): frozen HGB, leave-one-lineage-out over the qualified SEG-072 lineages, arms B0, C, T, TW,
  T_matched, C81, T81, T_all, C_all. Primary reported quantities are not AP: Genesis mean and worst-lineage ER, retained-region recall and missed instruction starts at B35/B50,
  boundary-stratum misses, hard-negative errors, D01/D03/D07, and paired fold deltas against the control and volume-matched arms; AP/AUC are reported as historical context. The
  SEG-081 `ml_verdict` is printed for continuity. Evaluation targets are source-backed SEG-072 lineages only; sealed cohorts, calibration on targets and hyperparameter search are forbidden.
- **Readiness decision** (pure function `readiness_decision`, thresholds in the plan):
  All counts come from the **new batch alone** (SEG-081's games were judged against a weaker control and only enter the eight-candidate total).
  `NEURAL_RETRAINING_NOT_JUSTIFIED` if any of: new useful independent lineages < 2, new novel structural signatures < 100, no "residual" fold.
  `NEURAL_PILOT_JUSTIFIED` if no floor failure and new useful independent lineages >= 3, eight-candidate total >= 5, new novel signatures (versus the registry and SEG-081) >= 300, new strong
  hard-negative cells >= 300, at least two residual folds, and label provenance / snapshot verification complete. A residual fold is a development fold where the data-only arm T_all still has
  lineage-max ER >= 0.05 and its ER gain over B0 does not exceed the noise floor (mean |ER(B0 on 90 % of base cells) - ER(B0)|). B0's absolute ER is a historical constant and is not a criterion.
  Known weakness, disclosed before the run: lineage-max ER is outlier-driven, so the residual criterion can be easy to satisfy and discriminates weakly; the other criteria carry the decision. Otherwise `MORE_DIVERSITY_NEEDED`, naming the unmet criterion. A significant AP gain is not required (the baseline is
  near-saturated), and the SEG-076 figures (A 0.105, B 0.155, C-v2 0.127 ER; ~1.6 CPU-hours per C-v2 fit) are historical context, not thresholds. A justified pilot is a **separate, prospective**
  small experiment (one or two difficult development folds, one seed per architecture, frozen compute limits); it is not run in SEG-083.

## Pre-freeze review and rehearsal

An independent adversarial review of the tool and plan found no blocker and three major issues, all fixed before the freeze: readiness could be satisfied by SEG-081 evidence and a historical
headroom constant (now new-batch-only counts and an outcome-linked residual criterion), no noise floor for ER deltas (added the B0-on-90 % null arm and a floor that gates paired claims), and metric
code not bound by the plan (ER / B35 / B50 / boundary modules now bound). Minor fixes: removed image file names from the games table, corrected stale seed-2 wording, withhold rule uses control and TAS
shingles, paired deltas extended to missed starts and recall, disclosure of the unused X/Y/Z/Mode buttons and player-2 Start. A rehearsal of the diagnostic on the existing SEG-081 data (not one of the five
games; discarded) exercised the full code path, and the synthetic BizHawk fixture passed on the frozen plan (small and 2 MiB images).

## Disclosures

Random-input controls may remain shallow; yield over them measures executed-code acquisition feasibility, not ML benefit, and is not a fair gameplay comparison. The native CDL data
flag is blanket, so E bytes also read as data are counted but never gating. No source truth exists for these titles. Engine independence is approximated by shingle containment; true
ancestry is not measurable. Bound files are pinned to LF by `.gitattributes`, so a Windows checkout cannot change a verified digest.
