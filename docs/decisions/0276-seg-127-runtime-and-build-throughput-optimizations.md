# ADR 0276: SEG-127 runtime and build throughput optimizations (combined results and decisions)

Status: accepted. Baseline: product `main` 6ff4010 (post SEG-126), `docs/testing/performance-baseline-seg127.md`. Method: `tools/perf_baseline_bench.py` (interleaved, medians, CV
recorded), SEG-125 reports; raw data `docs/testing/data/seg127/`. One host (Ryzen 7 5800X, WSL2), GCC 13.3 for the matrix, the pinned `zig cc` 0.15.2 (downloaded and SHA-256-verified
against `.github/workflows/release.yml`) for the policy study. Delivered as ONE PR with one checkpoint commit per task; each change is independently revertible.

## Decisions

| task | bottleneck (measured) | change | isolated result | decision |
|---|---|---|---|---|
| T002 | `rgb_convert` 0.36 ms/frame: CRAM decoded per pixel | `genesis_vdp_frame_row_rgb888` decodes the 64 entries once per row into a stack LUT | rgb_convert -81%; viewer 422.0 -> 457.1% (+8.3%) | **accepted** |
| T003 | frame-artifact SHA-256 on every viewer frame (~0.2 ms) | `genesis_vdp_produce_frame_raster_undigested` (explicit zero digest + `digest_omitted`); only the SDL viewer uses it; `genesis_frame_artifact_is_populated` accepts it | vdp_compose 0.898 -> 0.698 ms; 457.1 -> 484.0% (+5.9%, cumulative +14.7%) | **accepted** |
| T004 | Z80 fixed point recompiled 12 of 31 units that later rounds replaced; 0 objects reused (owners were interleaved modulo a count that grows with each image) | `EmitOptions::stable_unit_layout` (Genesis registry only): contiguous 2048-owner blocks | superseded job time 27.5 -> 0.2 s, Z80 81 -> 58 s, 8 units reused; wall compat -2.5%, optimized -7.3% | **accepted** |
| T005 | 14% of M68K C bytes were a no-op retirement scaffold on a 640-byte struct | terminal retirement is `return genesis_runtime_retire_m68k_instruction(...)` | C 177.7 -> 161.3 MiB (-9.2%), M68K compile -8.5%, wall compat -7.5% / optimized -3.7%, headless runtime +2.9% | **accepted** |
| T006 | GCC `-O1` -31..-38% in SEG-126, clang -2.5% | re-measured with the pinned zig 0.15.2 | `--optimize 1/2`, `--runtime-optimize 1/2` all 71.5-72.2 s, aggregate 500-507 s, executables identical in size (zig cc maps -O1 and -O2 to the same optimization level) | **no-go, defaults unchanged** |
| T007 | after T002/T003: `genesis_vdp_plane_run_pixel` 23.7% of instructions, an out-of-line struct-returning call per pixel | `always_inline` (GCC/clang attribute, plain `inline` elsewhere) | vdp_compose 0.699 -> 0.582 ms; 479.6 -> 494.0% (6 of 7 reps 493-499 vs 474-484) | **accepted** |

## Combined result (baseline 6ff4010 -> final, interleaved)

| metric | baseline | final | change |
|---|---|---|---|
| viewer, software renderer, unthrottled | 424.5% | 509.4% | **+20.0%** (3.93 -> 3.28 ms/frame) |
| viewer, OpenGL (WSLg), unthrottled | 256.4% | 287.1% | +12.0% |
| headless, unthrottled | 818.0% | 861.3% | +5.3% |
| paced (software / OpenGL) | 100.1% / 100.0%, missed 1 / 2 | 100.1% / 100.1% | unchanged; paced emulation 3.10 -> 2.68 ms/frame |
| per-frame spans (software, unthrottled) | vdp_compose 0.90, rgb_convert 0.36, emulation 2.97, present 0.56 | 0.58, 0.07, 2.58, 0.59 | |
| build wall, GCC, compatibility AOT | 85.3 s | 76.2 s | **-10.7%** |
| build wall, GCC, optimized AOT | 30.0 s | 25.8 s | **-14.0%** |
| build wall, pinned zig 0.15.2, compatibility AOT | 76.8 s | 71.5 s | -6.9% (aggregate job time 539 -> 500 s) |
| aggregate compiler job time (GCC compat / optimized) | 605 / 191 s | 537 / 158 s | -11% / -17% |
| generated C (compat / optimized) | 177.7 / 54.7 MiB | 161.3 / 50.5 MiB | -9.2% / -7.7% |
| executable (compat / optimized) | 38.1 / 12.3 MiB | 36.8 / 11.9 MiB | -3.4% / -3.3% |
| superseded Z80 compile (compat) | 27.5 s | 0.2 s | -99% |

The effects compose without hiding one another: T002/T003/T007 act on the viewer-only path (headless programs are unchanged by them: 816.8 vs 815.1% after T002, 811.2% after
T003), T004/T005 act on build categories (z80_generated vs m68k_generated) that are separate rows of the job-time table, and the combined build wall (76.2 s) equals the sum of
the isolated savings within noise (85.3 - 2.1 - 6.4 = 76.8). The T005 change adds a small runtime gain that the final headless number includes.

## Correctness evidence

* RGB LUT: exact equality with the per-pixel decode over seven shapes (H32/H40, interlace, 1-256 palettes, mid-frame slot changes, random palettes/indices) and the invalid-input
  behavior (index >= 64, row, slot, width, NULL) - `genesis_raster_display_modes_tests`.
* Digest-free producer: public digest still equals `genesis_vdp_frame_digest`; undigested artifact has an explicit all-zero digest and `digest_omitted`, no stale bytes, same pixels/
  geometry/palettes, fail-closed untouched, NULL rejected; capture, coverage, Sega CD, evidence and export keep the digest-producing functions. `genesis_write_checkpoint_evidence_summary` additionally requires `digest_omitted == 0`, so a digest-free artifact can never be checkpoint evidence even when a caller claims every category matched (regression test in `genesis_checkpoint_evidence_runtime_test.py`: zero-digest + `digest_omitted` + lying caller = `fail`, a genuinely digested frame = `pass`; mutation-verified).
* Stable Z80 layout: 8 of 8 owner units recur byte-identically in a larger registry (0 of 8 with the option off); the sorted-line multiset of all generated Z80 C is identical to
  the baseline (11,408,886 bytes), identical `status.json` z80 block (images, epochs, runs, frames), and the Z80 build-pipeline, owner-group differential/mutation and registry tests pass.
* M68K scaffold: 64 captured frames over 30 M dispatches, sound summary, stop class and exit code identical to the baseline (twice: T004+T005 build, and final vs baseline);
  generated-code scalability tool updated; `ctest --preset fast` 213/214 then 214/214 after fixing a header-vocabulary guard, 43 focus suites (z80, raster, viewer, native compat,
  codegen) pass.
* Commercial data: only labels, medians, spans and counts are recorded; no titles, paths, bytes or traces.

## Not done / remaining bottlenecks (for later work)

* Per-access `GenesisRuntimeStop m = {0}` (624 B) in generated M68K code and the 640-byte `GenesisControlTransfer`: a runtime/semantic change (helpers fill stops partially) - not attempted.
* The instruction-provenance compound literal (~8% of M68K C bytes): text-only saving, no measured compile benefit.
* `genesis_vdp_plane_run_pixel` + `render_row` still ~33% of viewer instructions; `SDL_StretchSurface`/present (0.6 ms software, 2.95 ms OpenGL/WSLg compositor sync).
* M68K C is still 86% of compatibility-mode compile time; reducing the number of emitted instructions needs the optimized AOT policy or a shared-body design (new architecture).
* The 177 KB frame-artifact memset/copy per published frame (~1.5% of instructions) was measured as below the detection threshold and not changed.
