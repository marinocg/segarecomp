# Widescreen scenery window sufficiency (SEG-152-T001)

Status: **measured and tested**. Disposition: **`T007_REQUIRED`**, scoped to the per-line fallback class (a real Sonic 2 conflict was reproduced); window geometry itself is sufficient. See "Disposition". Companion of [ADR 0320](../decisions/0320-abi-2-exact-frame-presentation-and-parity-contract.md) (decision 8) and the [parity matrix](abi2-abi1-parity.md) (row 6).

## Question

ABI 2 submits authored offscreen scenery as bounded name-table **windows**: at most two per plane, at most 128 x 64 cells and 4096 words each, in unwrapped plane-cell coordinates, looked up by the host with the line's actual scroll (`floor((x - hscroll) / 8)`, `floor((line + vscroll) / 8)`); a plane without a window is left to the generic extension (ADR 0309, 0313, 0314). ABI 1 instead answers every off-picture cell in provider code (`scenery_word(plane, row, x, hscroll, vscroll)`). Can the windows express **every** scenery answer an ABI 1 provider gives in the verified Sonic 1, Sonic 2 and Sonic 3 & Knuckles scenes, under the ABI 1 pairing of work RAM and raster, at 16:9, 21:9 and 32:9, at any placement of the native picture (ADR 0315), under both timings? The default hypothesis was "yes". A "no" needs a deterministic failing fixture first.

## The harness (one, shared with SEG-152-T008)

`tests/abi_parity_harness.hpp` is **the** ABI 1 / ABI 2 comparison harness; `tests/abi_parity_scenery_test.cpp` (CTest `abi_parity_scenery_tests`, label `full`) is its first consumer. SEG-152-T008 extends the same header (frame-exact snapshots, helpers, HUD bitmaps, refusals, sprites, placement, save/load, replay); it must not create another fixture format, digest or runner.

| Part | What it is |
|---|---|
| Production code under test | the **real** extension producer `genesis_vdp_produce_extension` (the viewer's own), and the **production** window contract `platforms/genesis/viewer/mod_scenery_window.hpp` (limits, range check and the lookup), which `mod_capture.cpp` now includes instead of owning, so the harness compares against the limits and the lookup the host runs, not a copy |
| Fixture format | `parity::Spec`: per plane and line the VDP hscroll (10-bit signed), a registered-origin multiple `k` (world x = x - hscroll + 512 k, the Sonic 3 & Knuckles `bg_register` shape), an ABI 1 fallback flag (the VDP's own wrapped word: the `own_plane` shape), an answer region (a plane coordinate answered differently under another raster condition); per plane the vscroll, the per-line speed (for the predicted timing) and an optional world clamp (level edges) |
| ABI 1 reference | a `GenesisNativeCompat` table answering from the world model for every off-picture pixel, exactly the Sonic providers' shape |
| ABI 2 twin | a window **builder** (the provider model) that submits at most two windows per plane for the placement it declares, answered by `segarecomp::scenery::lookup` |
| Digest | `row_digest` / `surface_digest`: one FNV-1a 64 over the produced RGB rows |
| Classifier | per row either identical pixels or the class of the difference: `LIMIT:window` (a requested cell is outside every window), `LIMIT:per-line-fallback`, `LIMIT:condition-dependent`, or `UNEXPLAINED` (a harness failure). `PROVIDER_ERROR:placement` marks a builder that used another placement than the one it declared; it is never a limit and never counts toward `T007_REQUIRED` |
| Builder strategy | one rectangle; else left | right rectangles; else a row split; else **column slicing** (`cover_columns`: full-height windows of at most `min(128, 4096 / rows)` columns covering the union of every requested span completely, however many windows one interval needs; overlapping and adjacent coverage merges; the greedy cover from the left is optimal, so the count it reports is the true minimum; with more than two needed the provider submits the first two and the case is a `LIMIT:window`). The runner counts `uncovered` requested cells independently of the pixels and asserts that a builder is feasible exactly when none is uncovered |

Timings: **Exact** builds the windows from the scroll the host applies to the produced frame (what the exact-frame mechanism of ADR 0320 provides); **Predicted** builds them from the scroll two frames earlier (`render(N)` feeds the frame produced when the runtime counts N + 2) and pads every span by the largest per-line displacement **without extrapolation**. The Sonic providers derive the scroll of the frame they serve from the same RAM snapshot ABI 1 uses, so "Predicted" is a lower bound for them (it is what a provider without a RAM scroll buffer could do), and a limit that exists only there never requires T007.

## Synthetic scenarios (original scenes, no commercial data)

Every scenario is run at 16:9 (side 39), 21:9 (101) and 32:9 (238) over the placement matrix `left_px` in {0, side/2, side, 3 side/2, 2 side} (left-aligned, intermediate, centred, intermediate, right-aligned), under both timings: 225 cases per timing, plus the 672-pixel maximum, the placement error cases and the cost report. Result per scenario and aspect (the verdict is identical at every placement; an all-left or all-right placement asks one side only):

| Scenario (ABI 1 shape) | Exact 16:9 / 21:9 / 32:9 | Predicted 16:9 / 21:9 / 32:9 | Why |
|---|---|---|---|
| `constant` (constant-scroll playfield) | PASS / PASS / PASS | PASS / PASS / PASS | one rectangle per plane |
| `parallax-few` (6 monotone bands) | PASS x3 | PASS x3 | one rectangle |
| `parallax-many` (28 bands, 24 px steps: the hull of both sides exceeds one window at 32:9) | PASS x3 | PASS x3 | left | right rectangles |
| `parallax-interleaved` (alternating cell rows at -500 / +500, the widest spread a 10-bit scroll reports) | PASS x3 | PASS x3 | two full-height column windows (no contiguous row split fits) |
| `split-scroll` (a different background camera in the lower region) | PASS x3 | PASS x3 | two row groups, or column slices |
| `fallback-cell-rows` (S3K `own_plane` lines on their own plane cell rows) | PASS x3 | PASS x3 | the VDP's own word is copied from the name table into the window |
| `fallback-mid-cell` (the fallback boundary falls inside a plane cell row) | **LIMIT:per-line-fallback** x3 (4 rows) | the same | one plane coordinate has the VDP's word on one line and the authored word on the next |
| `origin-multiple` (constant registered multiple, k = 1) | PASS x3 | PASS x3 | the window sits at the VDP coordinates; the words come from the shifted world |
| `origin-multiple-mid-cell` (the multiple changes inside a cell row) | **LIMIT:condition-dependent** x3 (4 rows) | the same | one plane coordinate maps to two world cells |
| `raster-condition` (the answer region changes inside a cell row: a water line) | **LIMIT:condition-dependent** x3 (4 rows) | the same | same plane coordinate, two answers |
| `raster-condition-cell-row` (the same boundary on a cell-row boundary) | PASS x3 | PASS x3 | every plane coordinate keeps one answer |
| `level-start`, `level-end` (the world clamps; picture at the first and the last camera position) | PASS x3 | PASS x3 | the edge column repeats outward by a pure function of the cell |
| `wrap-crossing` (hscroll wraps -512 -> 511 between N and N + 2) | PASS x3 | **LIMIT:window x3 (224 rows)** | exact-frame scroll is the host's; a stale scroll puts the window 128 cells away. **Prediction only: does not require T007** |
| `fast-camera` (12 px per frame) | PASS x3 | PASS x3 | the padded margin (4 cells) |

`placement-error`: a builder that computed its windows for another placement than it declared (centred declared, intermediate built; right-aligned declared, left-aligned built) is classified `PROVIDER_ERROR:placement` at all three aspects and differs in the produced rows; it is excluded from every T007 tally.

`wide-constant` (672 px per side, constant scroll, left-only, right-only, centred and intermediate placement): PASS with every requested cell represented; a one-sided 1344-column extension is 168 plane cells and needs **two** windows (the first version of the slicer made one slice per interval and left the remainder uncovered: now fixed and pinned by `cover_tests`, which also cover one, exactly-capped, two, three, overlapping, adjacent and disjoint intervals and a three-window case where only two can be submitted). `672 px per side` (the maximum planned side): `constant` and `parallax-few` PASS at all four placements with two windows per plane (a plane needs 2 x 84 columns, never a window wider than 128); `parallax-interleaved` is **LIMIT:window** (the widest scroll spread plus 1344 planned columns needs three 128-column windows). The window bound only bites above a total width of 1024 pixels (about 41:9 for this picture); 32:9 is 796.

Every claimed limit has a deterministic failing assertion in the test (the differing rows exist, the surface digests differ, and each differing row is attributed to the class); every PASS asserts identical rows and identical digests.

### Why two windows are enough up to 32:9 (a bound, not only a measurement)

The VDP reports `hscroll` as a 10-bit signed value, so every line's scroll lies in -512..511. The plane columns any plane needs on a line are `floor((x - hscroll) / 8)` for the off-picture `x`, so the union over **all** lines is at most `(picture + 2 * side) / 8 + 128` cells wide (one scroll range of 1024 pixels is 128 cells). With a full-screen vertical scroll the cell rows are 28 or 29, and 128 columns x 29 rows = 3712 <= 4096 words, so a full-height window can be a full 128 columns. Two such windows cover 256 columns, which covers the union whenever `(picture + 2 * side) / 8 <= 128`, that is a total width of 1024 pixels: every scene at 16:9, 21:9 and 32:9, whatever the parallax. The measurements below agree (at most 228 columns of hull at 32:9; never more than two windows). A frame with per-column vertical scroll is already degraded by the producer (`GENESIS_EXT_FLAG_COLUMN_VSCROLL` withholds authored credit) and declined by the ABI 1 providers (`BAD_SCROLL`).

## Aggregate measurement from the authorized local images (geometry only)

This first measurement is **geometry-only**: it records which plane cells the host asks for and says nothing about the providers' answers. The later real-provider experiment below instruments the answers and is the evidence for answer representability.

Method: the production extension producer (`genesis_vdp_produce_extension`) was run, in a scratch copy of the runtime outside the repository and outside the commit, on the generated native program of each authorized local image, with a recording table in place of a provider that answers every cell as "generic". For every sampled published frame (one in 30) and for the three planned sides (39, 101, 238; centred placement) it recorded the plane cells the host asks for, and reduced them **in the program** to class-level numbers per plane: hull columns, left / right columns, plane cell rows, the number of distinct hscroll values over the lines, the minimum number of full-height column windows (cap `min(128, 4096 / rows)`), and whether one window or a left | right pair would have fitted. Only these numbers are published: no cell, table, frame, address or per-frame figure leaves the run (ephemeral diagnosis under the AGENTS.md authorization). Scenes: Sonic 1 in Green Hill Zone Act 1, Sonic 2 (1 player) in Emerald Hill Zone Act 1, Sonic 3 & Knuckles (declared lock-on composite) in Angel Island Zone Act 1 with the camera moving, driven by a scripted pad (every other state, such as titles, menus and a two-player split screen, was excluded). Sampling is a lower bound on the scene classes, not exhaustive: one zone and act per game, one frame in 30.

| Game / scene | Aspect (side) | Plane | Frames | Max hull columns | Max left / right columns | Max cell rows | Max distinct hscroll values | Windows needed (max) | One window fits | Left \| right fits |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Sonic 1, Green Hill Zone Act 1 | 16:9 (39) | A | 70 | 51 | 6 | 29 | 1 | 1 | 70/70 | 70/70 |
|  | 16:9 (39) | B | 70 | 178 | 133 | 29 | 102 | 2 | 39/70 | 43/70 |
|  | 21:9 (101) | A | 70 | 67 | 14 | 29 | 1 | 1 | 70/70 | 70/70 |
|  | 21:9 (101) | B | 70 | 194 | 141 | 29 | 102 | 2 | 37/70 | 42/70 |
|  | 32:9 (238) | A | 70 | 101 | 31 | 29 | 1 | 1 | 70/70 | 70/70 |
|  | 32:9 (238) | B | 70 | 228 | 158 | 29 | 102 | 2 | 15/70 | 40/70 |
| Sonic 2 (1 player), Emerald Hill Zone Act 1 | 16:9 (39) | A | 41 | 105 | 60 | 29 | 2 | 1 | 41/41 | 41/41 |
|  | 16:9 (39) | B | 41 | 176 | 131 | 28 | 46 | 2 | 17/41 | 17/41 |
|  | 21:9 (101) | A | 41 | 121 | 68 | 29 | 2 | 1 | 41/41 | 41/41 |
|  | 21:9 (101) | B | 41 | 192 | 139 | 28 | 46 | 2 | 17/41 | 17/41 |
|  | 32:9 (238) | A | 41 | 155 | 85 | 29 | 2 | 2 | 34/41 | 41/41 |
|  | 32:9 (238) | B | 41 | 226 | 156 | 28 | 46 | 2 | 15/41 | 17/41 |
| Sonic 3 & Knuckles (lock-on), Angel Island Zone Act 1 | 16:9 (39) | A | 64 | 51 | 6 | 29 | 1 | 1 | 64/64 | 64/64 |
|  | 16:9 (39) | B | 64 | 171 | 127 | 29 | 3 | 2 | 63/64 | 64/64 |
|  | 21:9 (101) | A | 64 | 67 | 14 | 29 | 1 | 1 | 64/64 | 64/64 |
|  | 21:9 (101) | B | 64 | 187 | 135 | 29 | 3 | 2 | 63/64 | 63/64 |
|  | 32:9 (238) | A | 64 | 101 | 31 | 29 | 1 | 1 | 64/64 | 64/64 |
|  | 32:9 (238) | B | 64 | 221 | 152 | 29 | 3 | 2 | 63/64 | 63/64 |

Reading: plane A (the foreground) is a single window at every aspect in all three games except Sonic 2 at 32:9 (two, because of its two-value status split). Plane B (the background) with its per-line parallax needs **two** windows at every aspect in Sonic 1 and Sonic 2 (a hull of up to 228 columns at 32:9 fits neither one window nor a left | right pair), and in Sonic 3 & Knuckles' Angel Island background one window in 63 of 64 sampled frames and two in the frame where the 10-bit scroll wraps; **no sampled frame needs more than two windows**. Cell rows never exceed 29. The providers' own `own_plane` decisions (Sonic 1: only the special stage, plane level; Sonic 2: a per-line identity check; Sonic 3 & Knuckles: plane B only, per line) were **not** instrumented by this geometry sampling; they are measured by the real-provider experiment below for Sonic 3 & Knuckles (Angel Island Zone 1) and, as conflicts, for Sonic 2 (Emerald Hill Zone 1), and remain unmeasured for Sonic 1.

## Per-frame cost

Provider-side work per produced frame (deterministic counts, `PARITY cost` lines of the test; centred placement): the ABI 1 provider answers every off-picture cell of both planes in its own code, one call per pixel, **34,944 / 90,496 / 213,248 calls** at 16:9 / 21:9 / 32:9; an ABI 2 provider fills at most two bounded windows per plane (**2,856 / 3,724 / 5,628 words** for a constant scroll, **4,044 / 4,940 / 9,996** for 28-band parallax, never more than 4 x 4096 = 16,384), and the host answers the same pixels by lookup in those words. The host copies the words once per submission (at most 4 x 8 KiB). The wall-clock share of the producer itself is the existing measurement of [widescreen-scenery-lookup-measurement.md](widescreen-scenery-lookup-measurement.md) (10.57% of a 60 Hz frame at 32:9 on its host, the lookup being 40% to 69% of it); the lookup is unchanged, and what an ABI 2 provider adds is bounded by the word counts above, with no per-pixel callout.

## Real-provider answer-conflict experiment (the three conclusions kept apart)

Three different claims were conflated in the first version of this note; they are separate here.

1. **Geometry sufficiency**: can two bounded windows *cover* the plane cells a frame asks for? Established by the bound and by the measurements above, for the measured scenes and every aspect up to 32:9.
2. **Answer representability**: does the real provider give **one** answer per plane coordinate within a frame (a window holds one word per cell)? This is a property of the provider's answers, not of the geometry, and it is what the synthetic `LIMIT:` scenarios model.
3. **Production coverage**: which verified scenes were actually measured.

Experiment (scratch, outside the repository; authorized local images; aggregate counts only): the **real, unmodified ABI 1 provider source** (`segarecomp-patches` `32eb7d4`) was compiled into the scratch runtime and driven by the real producer through a recording wrapper with the exact ABI 1 pairing (raster N, work RAM of the end of N - 1, `prepare` verdicts honoured). For one frame in 5 and the 32:9 plane (side 238) it recorded every answer by plane coordinate `(plane, floor((x - hscroll)/8), floor((line + vscroll)/8))` and counted: frames accepted or declined (and why), the provider's own `own_plane` lines and where their boundaries fall relative to plane cell rows, registered-multiple (`kx`, `ky`) changes between lines, **coordinates answered differently by different lines (conflicts)**, those involving a fallback line, and unanswered cells. For the conflicts it also produced the frame a second time through an **ideal window** (a table holding exactly the first answer per coordinate, unlimited geometry: the best any window set can do) and counted pixel rows that differ from the ABI 1 frame.

| Scene (image, class) | Accepted frames | `own_plane` lines (max / frames > 0) | `own_plane` or multiple boundaries inside a plane cell row | Multiple changes | **Coordinates with two answers** (frames) | Unanswered cells | Pixel rows differing for an ideal window |
|---|---:|---|---|---|---|---:|---|
| Sonic 3 & Knuckles lock-on, Angel Island Zone 1, running level | 1032 | plane B: 224 / 356; plane A: 0 | 0 | 0 | **0 (0 of 1032)** | 0 | 0 |
| Sonic 2 (World), Emerald Hill Zone 1, 1 player | 269 | not instrumented (the conflicts below were measured directly): the conflicting lines are up to 2 fallback lines and 1 authored line | not instrumented | not instrumented | **up to 34 keys, on up to 3 lines, in 136 of 269 frames, all involving a fallback line, plane A only** | 0 | **up to 2 rows, in 136 of 269 frames** |
| Sonic 1 (USA, Europe image) | 0 (the provider declines every frame of this image; the scene scope of the image was not reachable) | not measured | not measured | not measured | not measured | not measured | not measured |

Differing scroll values alone are common (Sonic 3 & Knuckles: up to 18 line boundaries per frame inside a plane cell row) and are **not** conflicts: two lines with different scroll ask different plane cells, and the world is a pure function of the world cell. A conflict needs the same plane coordinate and two answers, which in these providers means a fallback line (the VDP's own wrapped word) sharing a plane cell with an authored line.

Reading:

- **Sonic 3 & Knuckles, Angel Island Zone 1**: answer representability holds across 1032 accepted frames: the 356 frames with `own_plane` lines have their boundary on a cell row boundary or share no coordinate with an authored line; the registered multiples never change.
- **Sonic 2, Emerald Hill Zone 1**: a **real, reproducible failing comparison**. In half of the accepted frames, up to three lines share a plane coordinate between a fallback line and an authored line, and even an ideal window (unbounded geometry) differs from the ABI 1 frame in up to two pixel rows. This is the synthetic `LIMIT:per-line-fallback` class occurring in a production scene.
- **Sonic 1**: not established (see the gate below).

## Conclusion per class

| Scene class | Geometry | Answer representability | Production coverage | T007 |
|---|---|---|---|---|
| Sonic 1 gameplay | sufficient (two windows; measured, bound) | **unresolved**: the provider was not reachable on the available image; it has per-line origin offsets that can differ by multiples of 512 (the Sonic 3 & Knuckles `kx` shape) | geometry: Green Hill Zone 1 | gate (below) |
| Sonic 1 special stage | sufficient (planes left to the generic extension) | holds (plane level) | not measured | no |
| Sonic 2 (1 player) | sufficient | **fails** for the per-line fallback class (measured conflict) | Emerald Hill Zone 1 | **required (per-line fallback)** |
| Sonic 3 & Knuckles | sufficient | holds in Angel Island Zone 1 (0 conflicts in 1032 frames); other zones unmeasured | Angel Island Zone 1 | not for AIZ1; the other zones fall under the gate |
| total planned width above 1024 px | no (three windows) | n/a | not a production aspect | no |
| a camera that crosses the scroll wrap under the predicted timing | no | n/a | exact-frame removes it | no |

Smallest bounded alternatives, named so that the implementation task has a design to start from: **a per-row generic flag** per plane (a 224-bit mask: a flagged line is answered by the VDP-derived generic extension regardless of windows; this exactly expresses `own_plane` lines, the measured Sonic 2 case, with no new geometry); per-line spans (a window answering only a screen-line range) for a multiple or condition that changes inside a plane cell row, to be added only if a real comparison needs it; a third window or a larger bound above 1024 px total width.

## Disposition

Geometry is sufficient and is **not** the open question. Answer representability is **required to be extended** for the per-line fallback class, on concrete evidence from the real Sonic 2 provider, and is **unresolved** for Sonic 1 and for Sonic 3 & Knuckles beyond Angel Island Zone 1.

`T007_REQUIRED - the per-line fallback class: real-provider comparison Sonic 2 Emerald Hill Zone 1 (conflicts in 136 of 269 accepted frames, up to 2 pixel rows differing even for an ideal window), synthetic fixture fallback-mid-cell; minimal bounded design: a per-row generic flag per plane (a 224-bit mask) in the scenery contract; the registered-multiple and raster-condition classes (origin-multiple-mid-cell, raster-condition) have synthetic failing fixtures only and stay watched, not implemented.`

Unresolved gate (recorded, not silent): the Sonic 1 answer-representability measurement (needs the provider to accept a Sonic 1 scene on an available image), Sonic 2 revision 1, and Sonic 3 & Knuckles zones other than Angel Island Zone 1. Any of them can only widen T007 (for example to per-line spans); none can cancel the per-line fallback work, because that one is reproduced.

Provenance and limits: the geometry was sampled (one frame in 30, one zone and act per game); the conflict experiment samples one frame in 5 of one zone and act per measured game; the synthetic harness models the providers' shapes over the real producer and the production window lookup and does not run guest code, snapshots, save/load or replay (the viewer tests and T008 do). An operator may instead accept up to two differing pixel rows in Emerald Hill Zone 1 as a recorded exception at T008/T010; that is a decision for the operator, not this note.

## Handoff: the canonical resolution step is mandatory

Merging F31 does not by itself change any backlog record. The canonical harness full-refinement step must be completed **before F32 starts**, in this order:

1. Record the T001 and T002 evidence (`task_pr.py complete-delivery F31`), including the exact line `- **Scenery disposition:** T007_REQUIRED - ...`.
2. Promote SEG-152-T007 from `draft` to `ready` with the **measured scope only**: the per-line fallback class, the smallest bounded per-row generic mechanism. Do not widen it to origin-multiple or raster-condition (synthetic fixtures only) without a concrete failing real-provider comparison; do not implement it in F31.
3. Resolve every relevant dependency edge: T007 keeps its `depends_on` and stays a dependency of T008 and T011; add it to the Sonic 2 migration and adaptive-placement tasks as the disposition names; re-check acyclicity.
4. Run `backlog.py validate` and `agent_verify.py control`.

Unmeasured scene classes are **follow-up verification gates**, not proof of compatibility: Sonic 1 real-provider answer representability (unresolved), Sonic 2 revision 1 (unmeasured), Sonic 3 & Knuckles zones other than Angel Island Zone 1 (unmeasured). Geometry sufficiency and answer representability are different claims; the first does not establish the second.
