# ADR 0107: SEG-049 (T003-T007) final verification record - native gen2 M3 + B35, authority, CLI, launcher, packaging

- Status: delivery record of the combined T003-T007 product PR. **No default change, no universal-support claim, no recommendation.** Compatibility (broad AOT) stays the default,
  `optimized` stays v1, gen2 is an explicit experimental opt-in. T008 (adoption decision) and T009 are untouched and still need fresh independent source-backed evidence.
- Predecessors: ADR 0100-0106.

## 1. Evidence statement (unchanged limits)

The five source-backed titles are a known, previously exposed corpus (ADR 0100/0101); B35 is an empirical generalization hypothesis chosen from a five-point grid on them. The structural validator
does not prove that no unselected code can ever execute. A source-backed game may contain executable code ranked below the B35 cutoff. Runtime traces below are bounded (600 frames or an earlier
guest stop) and cannot establish completeness for unexecuted code; the no-render frame-stream digest is not visual equivalence.

## 2. Native vs research (ADR 0102, repeated here as aggregates)

Exact selected-cell, ML-only region and (through the unchanged prune + production validator) admitted-set parity with the research PyTorch final fit on all five source-backed ROMs; repeated-run
identity; logits within 1.53e-5 of PyTorch float32 (tolerance 1e-4; smallest cut-boundary gap 6.6e-4); bit-identical logits across GCC/clang, -O0..-O3, `-march=native`; bitwise goldens asserted on
Linux, macOS and Windows CI; evaluator 0.29-5.3 s single-thread for 128 KiB-2 MiB, peak 9-19 MiB.

## 3. Nine-title bounded differential through the native production route (`tools/genesis_ml_gen2_differential.py`, release build of the CLI)

Each title: the unchanged bridge, explicit instruction budget, 600 frames (no-render coverage), clang; variants broad / v1 (`--ml-admission`) / gen2 (`--ml-gen2-admission`, native M3 + B35 + unchanged prune
and validator). "== broad" = outcome, frame count, final-state digest and coverage digest equal (the frame-stream digest of a no-render run is vacuous). The three chains ran in parallel on one 16-core host:
CPU seconds are comparable, wall seconds are not and are not reported.

| title | broad terminal (frames) | broad executed ROM PCs | v1 == broad | gen2 == broad | gen2: PCs outside broad / broad PCs not executed | gen2 admitted K/U | generated C vs broad (gen2 / v1) | CPU vs broad (gen2 / v1) | gen2 peak RSS vs broad |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Sonic 1 | frames_reached (600) | 2417 | yes | yes | 0 / 0 | 0.352 | 0.39x / 0.26x | 0.37x / 0.23x | 0.56x |
| Flicky | guest_stop (26) | 214 | yes | yes | 0 / 0 | 0.346 | 0.66x / 0.51x | 0.75x / 0.56x | 0.87x |
| Shining Force | frames_reached (600) | 1836 | yes | yes | 0 / 0 | 0.345 | 0.47x / 1.00x | 0.34x / 0.78x | 0.56x |
| Phantasy Star II | frames_reached (600) | 1205 | yes | yes | 0 / 0 | 0.336 | 0.45x / 0.41x | 0.42x / 0.38x | 0.59x |
| Sonic & Knuckles | guest_stop (9) | 270 | NO | yes | 0 / 0 | 0.355 | 0.42x / 0.37x | 0.42x / 0.36x | 0.61x |
| Sonic 2 | frames_reached (600) | 1804 | yes | yes | 0 / 0 | 0.348 | 0.40x / 0.29x | 0.38x / 0.26x | 0.59x |
| Cool Spot | frames_reached (600) | 1203 | yes | yes | 0 / 0 | 0.329 | 0.48x / 0.19x | 0.48x / 0.14x | 0.61x |
| Streets of Rage | frames_reached (600) | 2153 | yes | yes | 0 / 0 | 0.349 | 0.43x / 0.30x | 0.41x / 0.25x | 0.58x |
| OutRun | frames_reached (600) | 2620 | yes | yes | 0 / 0 | 0.323 | 0.46x / 0.26x | 0.63x / 0.22x | 0.65x |

- **Zero** broad-executed ROM PCs outside the gen2 admitted set and zero unexplained divergence on all nine titles (executed-PC sets of gen2 and broad are identical). A pre-existing hardware frontier ends Flicky
  (26 frames) and Sonic & Knuckles (9 frames) in broad and gen2 alike; that is not an ML result. Sonic & Knuckles under v1 diverges from broad (15 broad-executed PCs are not executed; known SEG-048 gap, ADR 0099),
  gen2 does not.
- Economics (single samples): gen2 generated C is 0.39-0.66x of broad and CPU 0.34-0.75x (0.63x OutRun); v1 is smaller/cheaper where it is accepted (generated C 0.19-0.51x of broad on eight titles; Shining Force is a v1 fallback to the broad program, 1.00x; CPU 0.14-0.78x), so gen2 keeps more
  code than v1 on seven titles and buys correct coverage on Sonic & Knuckles at that price (as ADR 0101 section 10 already stated). Model inference is not the dominant cost (ADR 0102/0104).
- Not claimed: gameplay compatibility, equivalence beyond the observed window, any title outside these nine, visual equivalence.

## 4. Packaged builds and cross-platform

The release workflow (Linux, macOS arm64, Windows; bundled Zig) builds and extracts each package, runs `dependency_check.py` (no Python/ML runtime or research file; native dependencies packaged or OS libraries; models embedded)
and the package smoke test in a scrubbed environment (Compatibility, Optimized v1, Optimized + gen2, cache miss/hit separation, golden fractions/admitted-set digests/identities). Recorded as workflow results of the final head
in the harness evidence.

## 5. What is not done (T008 / T009)

- The adoption decision (opt-in vs recommended vs default) with fresh, genuinely independent source-backed executable truth for the frozen gen2 model and B35 (SEG-048's formal blind gates did not succeed and are not
  evidence for gen2) plus broader runtime/validator/economics evidence; alias/RAM-thunk builds (Flicky) currently fall back (ADR 0104 section 3) and are an input to that decision.
- The final adversarial production-feature decision.
