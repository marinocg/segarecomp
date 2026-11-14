# Widescreen aspect matrix: 4:3, 16:9, 21:9, 32:9 (SEG-139-T006, ADR 0302)

Status: synthetic matrix committed and run in CI; real-game aggregates are recorded in the private harness only. No frame, capture, trace or commercial byte is in this repository.

## 1. Synthetic matrix (CI, `sv_widescreen_matrix_test`)

One fictional 320 x 224 square-pixel scene; the planner, a fictional provider declaring `max=21:9;scene=synthetic:hud+scenery`, the shared host, an integer 2x window. Verdicts:
**pass** = composed frame equals the independent construction and the committed golden hash, and the declared coverage holds; **partial** = the frame is correct but the host fell back to the
native picture only because the aspect is outside the declared range; **fail** = any mismatch or an exceeded budget.

| Aspect | Side / surface | Surface bytes | Composed frame (golden FNV-1a) | Coverage (declared 21:9) | Verdict |
| --- | ---: | ---: | --- | --- | --- |
| 4:3 | 0 / 320 | 215,040 | `a659fbcb2bce5a83` | native picture, nothing to extend | pass |
| 16:9 | 39 / 398 | 267,456 | `2125da0c004f6f63` | scenery + hud covered | pass |
| 21:9 | 101 / 522 | 350,784 | `e86edf64ba877763` | scenery + hud covered | pass |
| 32:9 | 238 / 796 | 534,912 | `9a168ef87d987283` | **outside declared range**: native picture only (no provider scenery, no relocated HUD) | partial |

The report that backs the coverage column is asserted in the test (30 covered scenery frames for 16:9 and 21:9, 15 outside-range frames for 32:9). The scene generator is part of the test, so the
hashes pin it; they are only meaningful together with the independent construction, which is why both checks exist.

## 2. Cost against 16:9

Two different things are measured and must not be confused.

**Producer (Genesis, optimized build, from [the lookup measurement](widescreen-scenery-lookup-measurement.md), `[measured]`, median of 7 interleaved runs, host Linux aarch64 4 cores).** Producer cost per frame is a proxy (the `emulation` span with the extension minus without it):

| Aspect | Producer ns/frame | Share of a 60 Hz frame | Against 16:9 |
| --- | ---: | ---: | ---: |
| 16:9 (exact) | 337,390 | 2.02% | 1.00x |
| 21:9 | 792,409 | 4.75% | 2.35x |
| 32:9 | 1,761,883 | 10.57% | 5.22x |

**CI tripwires (Debug build, this delivery's run on the same host; coarse, not a benchmark).** `genesis_extension_test` times `genesis_vdp_produce_extension` on the synthetic scene: 16:9 1.94 ms,
21:9 4.45 ms (2.29x), 32:9 10.15 ms (5.23x) per frame; the gate is an absolute 250 ms per frame and "the 32:9 cost may not exceed 40x the 16:9 cost" (the pixel ratio is 6.1x, so a quadratic regression
fails). `sv_widescreen_matrix_test` times the host present of the synthetic frame (software renderer, dummy video): 16:9 0.29 ms, 21:9 0.38 ms (1.3x), 32:9 0.78 ms (2.7x), with an absolute gate of 250 ms.
The cost is linear in the planned side, as the per-pixel loop predicts; memory is the fixed maximum allocation (1664 x 448 RGB24 = 2,236,416 bytes) and no per-aspect or per-frame allocation exists
(checked: every planned surface fits the single allocation).

## 3. What 32:9 covers and what it does not

Covers (Genesis, the reference path):

- A planned, bounded surface up to 672 source pixels per side (ADR 0301), rejected rather than clamped; the producer, composer, texture and presentation modes (fit, integer, letterbox, centred crop) at 4:3 through 32:9, with pixel-exact producer output at 101, 238, 266, 636, 668 and 672 pixels per side.
- The generic Level 2/3 extension: the VDP's own plane scenery and the sprites the game already submitted over the extra columns.
- Honest declared-versus-delivered reporting per scene and component, and a visible fallback to the native picture outside a package's declared range.

Does not cover:

- **Gameplay.** Background coverage is not gameplay support. A game that does not create or update objects outside its original window does not gain them at 32:9; only a package that declares and delivers `actors` is reported as covering them, and the host measures that, it does not infer it.
- **Scenery the game never prepared.** Beyond the VDP's bounded plane window the extension is wrapped or stale scenery, wider at 32:9 than at 16:9. An authored provider covers only what it supports: a provider declaring 16:9 is shown only the native picture at 32:9.
- Master System (80-pixel producer bound) and Sega CD coverage declarations (SEG-147-T009, SEG-148-T010); the matrix is Genesis.
- Any real-game claim: the synthetic scene proves geometry, hashing, declaration and fallback behaviour, not that a title looks right at 32:9. Real-game evidence is the harness aggregate below and is reported as such.
- Per-scene observation of a real program: no provider or Luau API reports a scene yet (ADR 0302 consequences), so a shipped mod's frames account in the unscoped scene; only the synthetic tests drive named scenes. The Luau HUD path is the only schema 3 build that reaches a viewer, so only `hud` is observable end to end (`mod_luau_executable_test`).

## 4. Local real-game runbook (aggregate results only)

Legally held images stay under the ignored `games/` directory; nothing from a run is committed to this repository.

1. Build the Genesis viewer variant for each image with its widescreen package (`segarecomp build --patch-package <pkg> --patch-identity <id> ...`), once.
2. For each of 4:3, 16:9, 21:9, 32:9 write a one-line settings file (`video.widescreen=enhanced_gameplay`, `video.widescreen_ratio=<N:M>`) and run the built program **always with an explicit `--instruction-budget <N>` and a wall-clock `timeout`**, dummy video and audio drivers, `SEGARECOMP_VIEWER_SETTINGS=<file>`, `SEGARECOMP_PERF_REPORT=<json>` and `SEGARECOMP_COVERAGE_REPORT=<json>`; set `SEGARECOMP_PATCH_GEOMETRY` from `mod inspect` when the package declares geometry.
3. Keep only aggregates: frames run, extension surfaces accepted/rejected (`sv_host_extension_stats` through the report), the coverage counters, the `emulation` span per frame and the process peak memory. Do not keep frames, captures or traces.
4. Record the table in the harness backlog record of the task, never here. A configuration that was not run is stated as not run, not as pass.

The 4 MiB Sonic 3 & Knuckles lock-on image is not part of the local `games/` set on every machine; a row for an image that is absent is "not run".
