# ADR 0275: SEG-126 per-cell plane evaluation: results and decision

- Status: Accepted: **GO** (the candidate meets every pre-registered acceptance criterion in ADR 0274 section 3 and stays).
- Change under test: `genesis_vdp_render_row` evaluates plane A/B per nametable cell run (`GenesisVdpPlaneRun` in `platforms/genesis/runtime/vdp_render.c`); the unused per-pixel
  `genesis_vdp_plane_candidate` wrapper was removed. No header, API or other path changed.
- Raw data: `docs/testing/data/seg126/runtime-ab-image-A.json`, `runtime-ab-images-B-F.json`, `equivalence.json` (baseline: `*.baseline.json`).

## 1. Method (as pre-registered)

Same CLI binary for both sides (`main` at `132d64b`). Baseline programs were built with `--runtime-dir` of the unmodified `main` runtime, candidate programs with the task worktree's runtime
(`tools/perf_baseline_bench.py runtime --image LABEL=PATH@ROOT`). Image A: compatibility AOT, `-O2`, GCC 13.3; images B and F: optimized AOT, `-O2`. Variants are interleaved round-robin,
5 reps each, CPU-pinned (`taskset -c 2`), `--instruction-budget` 30 M (unthrottled) / 10 M (paced), SDL3 3.4.16, dummy video driver for the software renderer, a WSLg window for OpenGL.
Same host as the baseline (AMD Ryzen 7 5800X, WSL2). Images are referred to by label only.

## 2. Measured improvement

Image A (512 KiB, compatibility AOT), median of 5 interleaved reps:

| scenario | baseline | candidate | change | frame interval avg (p99) | missed deadlines |
|---|---|---|---|---|---|
| viewer, software, unthrottled | 321.2% of native (CV 0.2%) | **425.0%** (CV 0.3%) | **+32.3%** | 5.20 (6.00) -> 3.93 (4.70) ms | 0 -> 0 |
| viewer, OpenGL (WSLg), unthrottled | 214.1% (CV 0.5%) | 256.9% (CV 0.4%) | +20.0% | 7.79 (9.45) -> 6.50 (8.05) ms | 0 -> 0 |
| viewer, software, paced | 100.1% | 100.1% | unchanged (paced) | frame work 5.50 -> 4.17 ms | 1 -> 1 |
| viewer, OpenGL, paced | 100.0% | 100.0% | unchanged (paced) | 16.67 ms | 2 -> 2 |
| headless, unthrottled (no frames composed: control) | 865.1% (CV 2.5%) | 869.0% (CV 1.5%) | +0.5% (noise) | 1.93 -> 1.92 ms | - |

Targeted subsystem (software, unthrottled): `vdp_compose` **2.168 -> 0.899 ms per frame (-58.5%)**; samples 2.165-2.177 vs 0.897-0.902 (no overlap); `emulation` (which contains it)
4.27 -> 2.97 ms; `rgb_convert` 0.360 -> 0.360; `present` 0.530 -> 0.564 ms (+0.03 ms, +6%: the next frame is presented sooner after the previous present; an observed small
regression of an unrelated span, immaterial next to the 1.27 ms gained). OpenGL: `vdp_compose` 2.191 -> 0.911 ms; its 2.9 ms `present` is unchanged, so the end-to-end gain there is
smaller (+20%): the host-side cost the candidate cannot reach.

Across images (software, unthrottled, optimized AOT, 5 reps): image B (1 MiB) 253.2% -> 356.1% (**+40.6%**, `vdp_compose` 3.34 -> 1.41 ms); image F (2 MiB) 330.1% -> 434.3% (**+31.6%**,
2.04 -> 0.83 ms). Every CV was below 0.9%; every paced run stayed at 100%; headless varied by at most 1.4% with CV up to 2% (unchanged by construction).

CPU time and memory (paced software session, same guest, 3 runs each; `/usr/bin/time`): user CPU **5.67 s -> 4.32 s (-24%)** for the 17.26 s session (identical session length and 1,035 frames on
both sides); peak RSS 21.4 MB -> 20.8-21.4 MB (unchanged). Executable size: viewer 40,168,536 -> 40,168,592 bytes (+56), headless 39,993,344 -> 39,993,400 (+56). Build time and build
output are unaffected (same sources, same flags; the runtime source is one unit of the handwritten set).

The pre-registered bar was `vdp_compose` >= -25% and viewer speed >= +8%; the pre-registration's expectation was -35% / +10%. The measured effect is larger than expected. The instruction-count
profile of ADR 0274 had suggested a smaller reduction (callgrind total instructions fell only 15.1 G -> 13.8 G, -9%): the old code's cost was latency (64-bit division/modulo per pixel and
dependent loads), not instruction count. Instruction counts therefore located the work correctly but did not predict its duration; the SEG-125 spans and the unthrottled speed are the
measurements this decision rests on.

## 3. Correctness evidence

- **Differential test** `genesis_vdp_render_plane_run_differential_tests` (fast label): 48 randomized frames, 3.9 M pixels, 0 mismatches against a per-pixel oracle built from the independent public
  primitives (`genesis_vdp_resolve_plane_pixel`, `genesis_vdp_decode_nametable_word`, a test-local window rule and the documented priority order), covering H32/H40, interlace mode 2, five plane
  sizes, full/cell/line horizontal scroll, full/2-cell vertical scroll, window H/V ranges with mirror bits, left-column blank, backdrop, flips, priorities, palettes and 143 mid-frame register/VSRAM
  changes (23 H32 frames, 12 interlaced). **Mutation check:** four deliberate faults (ignore the vertical-scroll group change, skip the v-flip, drop the column-change refetch, ignore the interlace tile size)
  were each detected on the first frame; the unmodified code was restored and re-verified.
- **Existing suites:** fast preset 214/214 passed; focused suites (`genesis_(vdp|frame|raster|extension|live|native_compat|viewer)`, `perf|sv_`, `build_profile`) passed (10 + 1 additional); `agent_verify.py fast`
  at the validated commit reported PASS. The existing VDP render, frame, raster display-mode, extension (Level 3 uses the unchanged per-pixel plane helpers), live-frame, native-compat, frame-capture and viewer suites are among them.
- **Program-level equivalence** (`tools/perf_baseline_bench.py equivalence`, headless programs built from the unmodified and the modified runtime, same image, same environment, 100 M-instruction budget): the
  captured frames (every 90th published frame, up to 64) have byte-identical SHA-256 on four images (A, B, C, F: 4 x 64 = 256 frames; interleaved image categories: compatibility and optimized AOT, 0.5-2 MiB),
  and the `SOUND_SUMMARY` (audio digest, result kind, stop class, dispatch/virtual frame counts), `RUNTIME_NOTES` and exit code are identical on all six images. Two further images (D, E) stop early with the
  identical stop class (5 and 1 frames captured) and are weak coverage by themselves.
- **Not checked / limits:** macOS and Windows execution (the change is portable C11 integer arithmetic; their results come from CI, see the PR); the viewer's on-screen output was not compared pixel-for-pixel (it consumes the
  same producer output that the capture compares); interlace mode 2 and H32 are covered by the randomized test, not by a commercial image.

## 4. Decision

**Keep.** All three acceptance criteria hold: (1) digests, stop class and exit codes identical, differential test and existing suites pass; (2) `vdp_compose` -58.5% (bar 25%) and viewer speed +32% (bar 8%), each
far outside the CVs; (3) paced speed and missed deadlines unchanged, headless unchanged, executable size +56 bytes. The rollback criteria were not approached.

## 5. Remaining opportunities (measured, not done)

Callgrind on the candidate (instruction shares; not times): the per-pixel plane work is still the largest single function (`genesis_vdp_plane_run_pixel` 19.8% of instructions but a small share of time now), then
the frame-artifact SHA-256 (9.3%, needed only by capture/evidence tools), per-pixel CRAM decode in the RGB conversion (8.6%, the palette has 64 entries), and the SDL software scaler (10.2%, dummy driver only).

| opportunity | estimate | note |
|---|---|---|
| lazy frame digest | ~0.4 ms of ~3.9 ms (viewer) | changes the `frame_digest`-on-success producer contract |
| CRAM decode once per row in the RGB conversion | ~0.25 ms | local, very low risk |
| GCC `-O1` for generated C | -31..-38% GCC build time, +5% M68K time | clang/LLVM (the packaged `zig cc` family) gains only 2.5%; re-measure with the pinned Zig before any change |
| Z80 supersession waste | 4% (compatibility) .. 14% (optimized) of compile job time | alters the materialization fixed point |
| smaller generated M68K C | the real build lever (89% of compile) | emitter work |
| GPU compositing | not supported by the evidence (hardware `present` is 1.6-3 ms and not GPU-bound) | |

## 6. Measurement limitations

One host (WSL2, one CPU model); relative within-host changes only. Hardware-renderer results include WSLg compositor synchronization. The packaged Zig 0.15.2 could not be executed in this session, so no
packaged-compiler build was measured; the runtime change is compiler-independent C and was measured with GCC 13.3. CI (Linux, macOS, Windows) is the cross-platform gate.
