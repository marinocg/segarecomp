# ADR 0109: SEG-049-T008 adoption decision for gen2 M3 + B35 - OPT-IN RETAINED (no recommendation, no default change)

- Status: decision under the gate pre-registered in ADR 0108 (commit 7923aed, before any measurement). Classification: **ADOPTION_OPT_IN_RETAINED**.
- Measurements: `tools/genesis_ml_gen2_differential.py` unchanged, release CLI built from this branch, six commercial images from the ignored `games/` directory that are in no gen2
  training/validation/calibration/selection set and not in the ADR 0107 table. 600 frames, explicit instruction budget, clang. Only counts, ratios and booleans are recorded.

## 1. G1 table (gen2 vs broad; v1 shown for context)

| title (rom MiB) | broad terminal (frames) | gen2 admitted | gen2 == broad | PCs outside / missing | gen2 K/U | gen2 C vs broad | gen2 CPU vs broad | v1 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Alien Soldier (2.0) | frames_reached (600) | yes | yes | 0 / 0 | 0.339 | 0.53x | 0.47x | accepted, == broad (0.29x C) |
| Land Stalker (2.0) | frames_reached (600) | yes | yes | 0 / 0 | 0.339 | 0.49x | 0.35x | FALLBACK (prune_rejected), 1.00x |
| Batman (0.5) | guest_stop (230) | yes | yes | 0 / 0 | 0.341 | 0.46x | 0.44x | accepted, == broad (0.33x C) |
| Golden Axe (0.5) | guest_stop (55) | yes | yes | 0 / 0 | 0.335 | 0.41x | 0.41x | accepted, == broad (0.30x C) |
| Moonwalker (0.5) | guest_stop (0 frames, 6 ROM PCs) | yes | yes | 0 / 0 | 0.315 | 0.42x | 0.41x | accepted, == broad (0.30x C) |
| Sonic 3 (2.0, 2,097,664 B) | NOT MEASURABLE: the broad immutable-ROM AOT rejects the image (`C3 block lacks terminal branch`), independent of ML | admitted (K/U 0.341) but no baseline | n/a | n/a | n/a | n/a | n/a | admitted, no baseline |

Aggregates: escapes 0 of 5 measurable titles (weak: Moonwalker stops after 6 ROM PCs/0 frames, Golden Axe at 55 and Batman at 230 frames, so only the two 600-frame titles carry substantial equivalence weight); fallbacks 0 of 6 (gen2); economics pass on all five measurable titles (C 0.41-0.53x <= 0.80x; CPU 0.35-0.47x <= 1.00x);
determinism: a second full gen2 run of Golden Axe and Batman reproduced identical admission record, generated-byte count, equivalence booleans, executed-PC count and outcome (the largest title was not re-run; the gate had already failed on criterion 5).
Runtime depth: only **2 of 6** titles (Alien Soldier, Land Stalker) reach `frames_reached`; the other four stop early in broad and gen2 alike (three on pre-existing hardware frontiers after 0-230 frames, Sonic 3 before running because of an
unrelated broad-AOT rejection). Pre-registered criterion 5 requires >= 4 of 6.

## 2. Gate result

- G0 (fresh independent source-backed truth for frozen gen2 M3 + B35): **FAIL / absent.** No title outside the known five has source-backed executable truth; SEG-048's unsuccessful formal blind gates are not reclassified and no new result is invented. `default` is impossible.
- G1: criteria 1-3 pass on the five measurable titles, criterion 4 is only partially met (smallest-size titles re-run, the largest not; not decisive); **criterion 5 (runtime depth) fails (2 of 6 < 4)**, and one title (Sonic 3) cannot be compared at all. G1 therefore FAILS.
- G2: delivered (ADR 0103-0106); not re-opened (T009 re-checks).

Outcome: **opt-in retained.** Compatibility stays the default, `optimized` stays v1, gen2 stays the explicit experimental opt-in on both CLI and launcher. No presentation or code change is made; the policy owner, CLI and launcher already agree
(ADR 0103-0105) and the existing hermetic tests (`genesis_ml_admission_lifecycle_test`, `segarecomp_aot_model_lifecycle_test`, `segarecomp_build_command_test`) keep Compatibility as the default and explicitly selectable.

## 3. What the data does say (bounded)

- No divergence from broad on any of the five measurable unseen titles; economics are consistent with the nine-title table. gen2 admits Land Stalker where v1 falls back (a robustness advantage, not evidence of recall).
- Runtime-only, shallow traces (three of five stop within 230 frames) cannot establish recall for unexecuted code; this is exactly why runtime-only evidence cannot justify `default` and, here, does not meet the pre-registered bar for `recommended`.
- Unrelated pre-existing findings kept as inputs: broad AOT rejects the Sonic 3 image shape; Moonwalker, Golden Axe and Batman end early on hardware frontiers independent of ML; alias/RAM-thunk builds (Flicky) still fall back.

## 4. Path to a later promotion (not in scope)

A `recommended` re-run needs >= 4 deep (600-frame) unseen titles; `default` additionally needs fresh independent source-backed truth for frozen gen2 M3 + B35 on titles never used to train, select or calibrate them.
