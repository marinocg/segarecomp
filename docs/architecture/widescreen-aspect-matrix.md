# Widescreen aspect matrix: 4:3, 16:9, 21:9, 32:9 (SEG-139-T006, ADR 0302)

Status: synthetic matrix committed and run in CI; real-game aggregates (section 5, `widescreen-32x9-classification.json`) are non-reconstructable classes only. No frame, capture, trace, address or commercial byte is in this repository.

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
- Scene names are a provider/mod claim (a native provider's `GenesisCompatDecision.scene`, a Luau mod's `sega.scene`), checked against the declaration; the host does not infer or verify the scene from the guest. The Luau HUD path is the only schema 3 build that reaches a viewer, so only `hud` is observable end to end (`mod_luau_executable_test`, which asserts per-scene `covered` for a mod that names its scene).

## 4. Local real-game runbook (aggregate results only)

Legally held images stay under the ignored `games/` directory; nothing from a run is committed to this repository.

1. Build the Genesis viewer variant for each image with its widescreen package (`segarecomp build --patch-package <pkg> --patch-identity <id> ...`), once.
2. For each of 4:3, 16:9, 21:9, 32:9 write a one-line settings file (`video.widescreen=enhanced_gameplay`, `video.widescreen_ratio=<N:M>`) and run the built program **always with an explicit `--instruction-budget <N>` and a wall-clock `timeout`**, dummy video and audio drivers, `SEGARECOMP_VIEWER_SETTINGS=<file>`, `SEGARECOMP_PERF_REPORT=<json>` and `SEGARECOMP_COVERAGE_REPORT=<json>`; set `SEGARECOMP_PATCH_GEOMETRY` from `mod inspect` when the package declares geometry.
3. Keep only aggregates: frames run, extension surfaces accepted/rejected (`sv_host_extension_stats` through the report), the coverage counters, the `emulation` span per frame and the process peak memory. Do not keep frames, captures or traces.
4. Record the per-run numbers in the harness backlog record of the task; only the non-reconstructable classes of section 5 are committed here. A configuration that was not run is stated as not run, not as pass.

The 4 MiB Sonic 3 & Knuckles lock-on image is not part of the local `games/` set on every machine; a row for an image that is absent is "not run".

## 5. Real-game classification (SEG-140-T006 / T007, aggregate only)

Source of truth: [`docs/product/widescreen-32x9-classification.json`](../product/widescreen-32x9-classification.json), checked by `tests/widescreen_classification_test.py` (every studied scene class has a result for each of 4:3, 16:9, 21:9 and 32:9; no presentation-only claim without submitted actors, resident art, a verified-scope scenery source and an observed run; every NOT RUN names its blocker; no address-, digest- or byte-like text). This section is a reading aid and must agree with it.

**Method (private, bounded, repeatable).** Images: the exact Sonic 1 REV00, Sonic 2 World REV00 and Sonic 3 & Knuckles lock-on composite images (matched by digest to the packages). The Sonic 2 REV01 image is **not in the local set: NOT RUN, nothing is substituted**. The existing 16:9 packages (`sonic1-native-widescreen`, `sonic2-native-widescreen`) are the baseline and were not changed; they were rebuilt from the maintained sources into generated-native viewer programs. Each class was entered by a scripted pad route (`SEGARECOMP_VIEWER_PAD_SCRIPT`: title, the game's own level-select entry for Sonic 1, hold right with a periodic jump for about 40 s), always with `--instruction-budget 45000000`, a 420 s wall timeout, dummy video and audio, and the runbook of section 4. Three kinds of run, all on the same route:

1. the *real package* at 16:9, 21:9 and 32:9 (and 4:3 for the cost table): provider acceptance, the coverage report and the performance report;
2. a *measurement provider on the vanilla image* and 3. the same on the *16:9-package image*: a private, never-published read-only provider (`frame.work_ram`, `frame.vdp`, it declines every frame) that only aggregates, per scene class, the live object-table entries by distance from the picture and their on-screen flag, and the hardware sprite table (sprites fully beside the picture, patterns that are all zero, per-line sprite counts by Y exactly as the hardware limit counts them). The object-table study covers Sonic 1 only; the Sonic 2 and Sonic 3 & Knuckles object-table layouts are not verified here, and the Sonic 3 & Knuckles sprite table is a permanently full chain that this probe cannot read meaningfully, so their actors, art and budget are `unverified`, never guessed. The Sonic 3 & Knuckles program had to be built unoptimized (`-O0`, one job) because the sandbox ran out of memory otherwise, so its timings are not comparable with the other two.

Repeatability: the 32:9 run of each title was repeated and gave identical frame, authored, declined and coverage counters; the counters are identical at 16:9, 21:9 and 32:9 because the schema 2 packages accept frames independently of the aspect.

**Reading the results.**

- `authored-verified-scope` is the package's own validated 16:9 scope (maintainer-attested; this delivery observed the provider accepting the frames). At 21:9 and 32:9 the provider still accepts the same frames and the host presents them (these packages declare no geometry, so the host does not restrict them), but nothing established that the reconstructed columns are correct that far out: `authored-unverified`, never promoted.
- The schema 2 packages declare no `geometry`, so the coverage report shows their delivered scenery and HUD as `undeclared` (presented, never counted as support), `actors` as not offered, and nothing is rejected. A package that declared `max=16:9` would be shown the native picture only at 21:9 and 32:9 (ADR 0302); these declare nothing and were not changed.
- Object-table entries include non-drawing logic objects, so `present-not-submitted` means "objects exist beyond the submission window with the on-screen flag clear", not "visible enemies are missing".
- Sprite counts per line are by Y (the hardware rule): 20 per line in H40. A peak above 20 means the hardware drops sprites in that frame.

Per scene class (aggregates of the runs above; each pair is vanilla -> 16:9 package):

| Class | Route frames | Provider authored frames | Objects per frame beyond 64 px left / right | Flagged % to 39 px | Flagged % 40-101 px | Flagged % 102-238 px | Sprites fully beside the picture per frame | Blank-pattern % of those | Peak sprites on a line | Frames >=16 / >=20 on a line (package) |
| --- | ---: | ---: | --- | --- | --- | --- | --- | ---: | --- | --- |
| `sonic-1-ghz` | 4933 | 3767 | 1.95 / 3.99 | 28.1 -> 81.1 | 3.6 -> 48.0 | 0.2 -> 2.1 | 1.07 -> 3.93 | 0.1 | 14 -> 16 | 5 / 0 |
| `sonic-1-lz` | 5052 | 4037 | 0.82 / 6.14 | 11.2 -> 38.9 | 0.0 -> 23.7 | 0.0 -> 0.0 | 0.83 -> 1.86 | 0.0 | 14 -> 13 | 0 / 0 |
| `sonic-1-mz` | 4809 | 2872 | 0.55 / 3.59 | 44.9 -> 92.0 | 14.7 -> 60.7 | 4.9 -> 5.8 | 3.8 -> 7.65 | 0.0 | 16 -> 19 | 103 / 0 |
| `sonic-1-slz` | 4934 | 3855 | 0.88 / 4.74 | 0.5 -> 1.4 | 0.0 -> 0.4 | 0.1 -> 0.1 | 0.97 -> 1.08 | 0.0 | 15 -> 15 | 0 / 0 |
| `sonic-1-syz` | 5364 | 4246 | 2.22 / 6.13 | 12.7 -> 47.8 | 0.0 -> 30.9 | 0.0 -> 0.0 | 0.46 -> 1.94 | 0.0 | 16 -> 18 | 65 / 0 |
| `sonic-1-sbz` | 4856 | 3717 | 2.27 / 8.39 | 2.8 -> 9.4 | 0.7 -> 11.1 | 0.1 -> 0.4 | 0.84 -> 2.98 | 0.4 | 14 -> 14 | 0 / 0 |
| `sonic-2-ehz1` | 4893 | 3722 | not measured | not measured | not measured | not measured | 0.68 -> 2.51 | 1.2 | 23 -> 23 | 168 / 16 |
| `sonic-3-and-knuckles-aiz1` | 7691 | 6704 | not measured | not measured | not measured | not measured | invalid probe | invalid probe | invalid probe | invalid probe |

Per class and aspect (T007; 4:3 is the native picture for every class and is `native-only`). `kind` is the feasibility of the claimed component: `gameplay-changing-required` = the margin actors exist, or must exist, beyond what the game submits and widening it changes activation or the sprite budget; `inconclusive` = the evidence cannot name the cause; `unsupported` = the provider declines the scene and only the generic path exists:

| Class | Aspect | Scenery | HUD | Actors | Art | Original sprite budget | Result | Rejection / fallback reasons |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `sonic-1-ghz` | 16:9 | authored-verified-scope | relocated | submitted-partial | resident-for-submitted | within | **gameplay-changing-required** | - |
| `sonic-1-ghz` | 21:9 | authored-unverified | relocated | submitted-partial | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-ghz` | 32:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-lz` | 16:9 | authored-verified-scope | relocated | submitted-partial | resident-for-submitted | within | **gameplay-changing-required** | - |
| `sonic-1-lz` | 21:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-lz` | 32:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-mz` | 16:9 | authored-verified-scope | relocated | submitted-partial | resident-for-submitted | within | **gameplay-changing-required** | - |
| `sonic-1-mz` | 21:9 | authored-unverified | relocated | submitted-partial | unverified | constrained | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-mz` | 32:9 | authored-unverified | relocated | present-not-submitted | unverified | constrained | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-slz` | 16:9 | authored-verified-scope | relocated | present-not-submitted | resident-for-submitted | within | **gameplay-changing-required** | - |
| `sonic-1-slz` | 21:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-slz` | 32:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-syz` | 16:9 | authored-verified-scope | relocated | submitted-partial | resident-for-submitted | within | **gameplay-changing-required** | - |
| `sonic-1-syz` | 21:9 | authored-unverified | relocated | submitted-partial | unverified | constrained | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-syz` | 32:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-sbz` | 16:9 | authored-verified-scope | relocated | present-not-submitted | resident-for-submitted | within | **gameplay-changing-required** | - |
| `sonic-1-sbz` | 21:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-sbz` | 32:9 | authored-unverified | relocated | present-not-submitted | unverified | unverified | **gameplay-changing-required** | no_virtual_sprite_consumer |
| `sonic-1-special-stage` | 16:9 | generic-wrapped | none | unverified | unverified | within | **inconclusive** | - |
| `sonic-1-special-stage` | 21:9 | generic-wrapped | none | unverified | unverified | unverified | **inconclusive** | no_virtual_sprite_consumer |
| `sonic-1-special-stage` | 32:9 | generic-wrapped | none | unverified | unverified | unverified | **inconclusive** | no_virtual_sprite_consumer |
| `sonic-1-non-running-screens` | 16:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene |
| `sonic-1-non-running-screens` | 21:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene, no_virtual_sprite_consumer |
| `sonic-1-non-running-screens` | 32:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene, no_virtual_sprite_consumer |
| `sonic-2-ehz1` | 16:9 | authored-verified-scope | relocated | submitted-partial | resident-for-submitted | constrained | **gameplay-changing-required** | - |
| `sonic-2-ehz1` | 21:9 | authored-unverified | relocated | unverified | unverified | constrained | **inconclusive** | no_virtual_sprite_consumer |
| `sonic-2-ehz1` | 32:9 | authored-unverified | relocated | unverified | unverified | constrained | **inconclusive** | no_virtual_sprite_consumer |
| `sonic-2-non-running-screens` | 16:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene |
| `sonic-2-non-running-screens` | 21:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene, no_virtual_sprite_consumer |
| `sonic-2-non-running-screens` | 32:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene, no_virtual_sprite_consumer |
| `sonic-3-and-knuckles-aiz1` | 16:9 | authored-verified-scope | none | unverified | unverified | unverified | **inconclusive** | - |
| `sonic-3-and-knuckles-aiz1` | 21:9 | authored-unverified | none | unverified | unverified | unverified | **inconclusive** | no_virtual_sprite_consumer |
| `sonic-3-and-knuckles-aiz1` | 32:9 | authored-unverified | none | unverified | unverified | unverified | **inconclusive** | no_virtual_sprite_consumer |
| `sonic-3-and-knuckles-non-running-screens` | 16:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene |
| `sonic-3-and-knuckles-non-running-screens` | 21:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene, no_virtual_sprite_consumer |
| `sonic-3-and-knuckles-non-running-screens` | 32:9 | generic-declined | none | unverified | unverified | unverified | **unsupported** | provider_declined_scene, no_virtual_sprite_consumer |

`no_virtual_sprite_consumer` is not a runtime rejection: it records that no virtual-sprite path could contribute to that row (section 6). Not run (explicit, with blockers in the JSON): `sonic-1-bosses-final-zone-ending`, `sonic-1-later-acts`, `sonic-2-other-zones-and-special-stage`, `sonic-2-presentation-only-variant`, `sonic-3-and-knuckles-other-zones-and-bosses`, `sonic-3-and-knuckles-object-table-and-sprite-study`, and the REV01 image of Sonic 2 (image absent). **T006 is not fully performed: the Sonic 3 & Knuckles object, art and budget study is NOT RUN, so its actors are unverified.**

**Findings that follow from the numbers.**

- **Actors.** In every studied Sonic 1 running class, object-table entries exist beyond the 64 px the package submits (Green Hill: about 2 per frame on the left and 4 on the right beyond 64 px) while the share carrying the on-screen flag falls from tens of percent inside 39 px to a few percent at 102-238 px. At 21:9 the package covers part of the margin, at 32:9 almost none of it is submitted. The package states that this flag is also what lets objects act, which is why its 16:9 result is itself a gameplay-changing modification and a wider one is not presentation-only. Whether objects are *loaded* on the left while moving left (the object manager's own window, documented as about 128 px) was **not exercised**: every route moves right, so left-side loading is inconclusive here.
- **Art.** Sprites the game did submit beside the picture are drawn from resident patterns (blank-pattern share at most 1.2% across classes). That is the only residency evidence; it proves nothing about the art of objects the game never submitted (`unverified` at 21:9 and 32:9), and drawing never proves artwork correct.
- **Budget.** The 64 px widening already raises the peak per-line sprite count (Marble 16 -> 19 of 20, Spring Yard 16 -> 18); the Sonic 2 package exceeds 20 on 16 frames of Emerald Hill (vanilla: 5). A wider window adds more sprites to lines already near the cap, so the budget constrains 21:9/32:9 actors wherever it was measured near the limit.
- **Scenery.** Authored scenery is accepted on running levels only (the provider declines the title, level select, title card and fades by design) and the Sonic 1 special stage uses the VDP's own wrapped plane. Nothing here shows the authored columns are right at 21:9 or 32:9.
- **Sonic 3 & Knuckles (Angel Island act 1).** The provider accepts 6,704 of 7,691 route frames at 16:9, 21:9 and 32:9 alike and serves foreground scenery (unverified beyond 16:9), but offered **no HUD on this route** (cause not diagnosed). Its actors, art and budget are unverified (see method), so the class is `inconclusive`, not a gameplay-changing finding.
- **No virtual-sprite path exists end to end** (section 6).

**Cost (same route, four aspects run in sequence with no other load; host: Linux aarch64, 4 cores, dummy video, software renderer, optimized generated program).** Emulation span per virtual frame (the producer cost is the difference to 4:3) and process CPU for about 4,900 frames:

| Title route | 4:3 | 16:9 | 21:9 | 32:9 | CPU seconds 4:3 / 16:9 / 21:9 / 32:9 | Peak RSS |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| Sonic 1 Green Hill 1 | 3.58 ms | 4.07 ms (+0.49) | 4.86 ms (+1.28) | 6.32 ms (+2.74) | 24.2 / 26.2 / 29.1 / 35.8 | 44-51 MB |
| Sonic 2 Emerald Hill 1 | 3.59 ms | 4.15 ms (+0.56) | 4.81 ms (+1.22) | 6.35 ms (+2.76) | 23.9 / 26.3 / 28.7 / 35.6 | 50-57 MB |
| Sonic 3 & Knuckles Angel Island 1 (`-O0`, not comparable) | 5.99 ms | 9.68 ms (+3.69) | 13.34 ms (+7.35) | 21.34 ms (+15.35) | 53.9 / 82.9 / 110.6 / 173.2 | 56-63 MB |

The producer delta at 32:9 is 5.6x the 16:9 delta (the lookup measurement gave 5.2x) and the 32:9 frame stays well inside the 60 Hz budget. One timed run per configuration: indicative, not a benchmark. The unoptimized Sonic 3 & Knuckles program exceeds the 16.7 ms frame budget at 32:9 (p99 frame interval 28.5 ms); that is a property of the `-O0` build, not a finding about an optimized one.

## 6. What the current system supports and refuses (limitation summary)

| Question | Answer | Evidence |
| --- | --- | --- |
| Virtual-sprite compositor status | A tested C11 module (command model, VDP decode, priority, clip, original budget), linked by no shipped program. | ADR 0303; `genesis_vsprite_decode_tests`, `genesis_vsprite_layers_tests` |
| Presented in the live viewer? | **No.** The viewer never calls the compositor and sets no `actor_*` field. | ADR 0303 Limitations; `viewer_sdl3.c` builds `SvExtendedSurface` without them; only `sv_scene_rejection_test` fixtures set them |
| Can a mod author virtual sprites through a supported executable path? | **No.** Neither Luau nor a native provider can submit them. | ADR 0303 Limitations |
| Side-column sprite drawing? | **No.** The compositor works in native-picture coordinates only. | ADR 0303 decision 3 |
| Which actor-coverage claims are available? | **None.** A declared `actors` component reports `missing` in every shipped program; the accounting contract (ADR 0304) exists and is tested with synthetic fixtures, which prove nothing about gameplay. | `sv_scene_rejection_test`, `sv_coverage_test` |
| Which Sonic scenes need game-specific gameplay changes? | Actor visibility beyond 64 px at 21:9/32:9 in every studied Sonic 1 running class (Green Hill, Labyrinth, Marble, Star Light, Spring Yard, Scrap Brain, act 1 each). Sonic 2 and Sonic 3 & Knuckles: not established (their object tables are unverified). | section 5 |
| Findings needing follow-up | A and B below; per-title window/object-loading changes in `segarecomp-patches` only if wider actor visibility is wanted; the Sonic 2 REV01 run, the Sonic 3 & Knuckles object/art/budget study and its missing HUD on the route; routes for other zones, acts and special stages; a verified Sonic 2 object table; a left-moving object-loading measurement. | section 7 |

## 7. Post-F15 handoff: missing product capabilities (not tasks created by F15)

Recommended for a post-F15 harness refinement; F15 deliberately builds neither.

- **A. Live virtual-sprite consumer and mod-authoring integration.** Owners: `platforms/genesis/viewer/viewer_sdl3.c` (the per-frame surface hand-over, the one place that builds `SvExtendedSurface`), `platforms/genesis/runtime/virtual_sprite.{h,c}` (the compositor), the Luau host and the native provider seam for authoring (ADR 0267, 0296, 0303). Reusable: the `genesis_vsprite_*` composition and validation, `sv_actor_signal` and the `actor_*` fields (ADR 0304), `GenesisVirtualReason`. Acceptance tests: a built program composes a fixture set into the presented frame (golden hash); the actor counters in `SEGARECOMP_COVERAGE_REPORT` become `covered` only for a composed, presented set in a declared scene and stay `missing` otherwise; Vanilla is byte-identical; a rejected set shows its reason and the native picture. Dependencies: the authoring API decision (the ABI 2 route SEG-138-T011) and the F14/F15 contracts.
- **B. Virtual-sprite presentation in the widescreen side columns.** Owners: `virtual_sprite.c` (coordinate space), `sv_host.c` and `sv_layout.c` (surface geometry and the planner). Reusable: `sv_geom_plan` and the layered-overlay placement model (anchors, bounds, per-layer rejection). Acceptance tests: sprites straddling the picture edge are drawn into the side at 16:9, 21:9 and 32:9 with the same priority, clip and budget rules; nothing is drawn outside the declared region; the 4:3 output is unchanged. Dependency: A.

SEG-140 must not be described as fully delivered while A is unavailable: its promised live virtual-sprite presentation does not exist yet.
