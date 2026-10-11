# ADR 0108: SEG-049-T008 adoption gate for the gen2 M3 + B35 optimized-AOT policy (PRE-REGISTRATION)

- Status: pre-registered BEFORE any broader-set run (committed ahead of the measurements recorded in ADR 0109). Thresholds below are not changed after the data is seen.
- Predecessors: ADR 0099-0107. Frozen: gen2 M3 + B35, the production prune and validator. No retraining, threshold or budget change is in scope.

## 1. Decision space

`opt-in` (status quo: Compatibility default, `optimized` = v1, gen2 explicit experimental) | `recommended` (gen2 named as the suggested optimized mode on CLI and launcher, Compatibility still default) |
`default` (universal default becomes ML-selective AOT). Broad AOT / Compatibility stays explicitly selectable and the fallback in every outcome.

## 2. Gates

**G0 - source-backed static truth (required for `default` only).** Fresh, genuinely independent source-backed executable truth for the frozen gen2 M3 and B35 (C subset R, C subset K) on
titles/builds never used to train, select or calibrate them. SEG-048's formal blind gates did not succeed and are not evidence; the five-title B35 result is known-corpus evidence; the nine-title
runtime differential is runtime-only. No new source truth is invented in this task. If no such truth exists in the repository/authorized inputs, G0 FAILS and `default` is impossible.

**G1 - broader runtime set (required for `recommended`).** Run on commercial images present in the ignored `games/` directory that were in no gen2 training/validation/calibration/selection set and
not in the ADR 0107 nine-title table: Alien Soldier, Batman, Golden Axe, Land Stalker, Moonwalker, Sonic 3 (six titles; sizes 0.5-2 MiB, five distinct publishers/engines). Per title, via
`tools/genesis_ml_gen2_differential.py` (release CLI, 600 frames, explicit instruction budget, clang):

1. Equivalence: gen2 outcome, frames, final-state digest, coverage digest and executed ROM-PC set all equal broad (0 PCs outside, 0 missing). ANY divergence on a title is an "escape": zero escapes allowed.
2. Acceptance: gen2 admits (does not fall back to the broad program). Fallback budget: at most 1 of 6 titles may fall back (a fallback is safe, but it is not evidence for the model).
3. Economics, on accepted titles: generated C <= 0.80x broad and CPU seconds <= 1.00x broad (single sample; wall time not used).
4. Determinism: a second gen2 run of the smallest and the largest title yields identical outcome/final-state/coverage digests and admitted fraction.
5. Runtime depth: at least 4 of 6 titles must reach `frames_reached` (600) rather than an earlier guest stop; otherwise the evidence is too shallow and G1 FAILS (a pre-existing hardware frontier that ends broad and gen2 alike is not an ML failure, but it carries no weight).

**G2 - lifecycle/UX parity (required for `recommended`).** Already delivered by ADR 0103-0106: one admission owner, CLI/launcher parity, Python-free packages. Re-checked by T009; any T009 blocker voids a recommendation.

## 3. Outcome rule

- `default` iff G0 and G1 and G2 pass. G0 cannot pass without new source truth.
- `recommended` iff G1 and G2 pass and G0 does not (runtime-only evidence). Because bounded traces cannot establish recall on unexecuted code, `recommended` must be worded as a runtime-validated
  suggestion with unconditional Compatibility fallback, never as a correctness claim.
- otherwise `opt-in`, unchanged. A single escape on any title is a hard stop for `recommended` and is reported as a finding.
- Any policy/presentation change is applied once through the shared policy owner so the CLI and the launcher cannot disagree, and tests keep Compatibility selectable on both.
