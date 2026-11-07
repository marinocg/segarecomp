# SEG-126 performance baseline and measurement method

This is the reproducible baseline for the profile-guided optimization study (SEG-126). Every number comes from the SEG-125 reports
(`--performance-report`, `SEGARECOMP_PERF_REPORT`, read by `tools/perf_report.py`) or the wall clock around the whole command. The only addition is
`tools/perf_baseline_bench.py`, a thin interleaving runner (no instrumentation). Raw per-run samples: `docs/testing/data/seg126/*.baseline.json`.
Commit under measurement: product `main` at `132d64b` (SEG-125 merged), built Release.

## Method

- **Interleaving.** Variants run round-robin (`v1 v2 v3 v1 v2 v3 ...`), so host drift hits every variant alike. Builds: 5 reps per variant (7 for the 4-second synthetic
  image). Runtime: 5 reps. Each result keeps every sample, the median, min/max and the coefficient of variation (CV).
- **Build vs runtime** are separate commands and separate results. A build uses `--jobs 8` (the CLI's own default cap) and is **not** pinned; a runtime run is pinned to
  one CPU (`taskset -c 2`) to remove scheduler migration noise.
- **Cold vs repeated.** `segarecomp build` removes its object directory at the start and keeps no object cache across builds, so a repeated build into a fresh or an
  existing output directory does the same compile work (verified: two consecutive synthetic builds both compile 25 units, reuse 0). Only the operating system's file
  cache differs between the first and later runs; the first sample of each variant is in the raw data (`samples[0]`). A true cold-disk build was not measured.
- **Runtime scenarios** use a fixed instruction budget (30 M M68K dispatches ~ 3,100 guest frames, unthrottled; 10 M ~ 1,050 frames, paced), the same guest and the
  same stop class in every variant. They start at the cartridge reset, so they cover boot, the title sequence and early attract/gameplay, not a hand-picked scene.
- **Reading the reports.** `emulation`, `vdp_compose`, `rgb_convert`, `texture_upload` and `present` are SEG-125 spans. `vdp_compose` is *inside* `emulation`, and
  `texture_upload` is inside `present`; they are never added. `present` is host submission time, not GPU execution time. Unthrottled results measure processing
  capacity; paced results measure smoothness (speed 100% with few missed deadlines means the host keeps up).
- **Images** are legally held local images from the ignored `games/` set, referred to only by label: `genesis_512k` (a 512 KiB Genesis image), `sms_256k` (a 256 KiB Master
  System image), and the project-authored `synthetic` spin image from `tools/perf_overhead_bench.py`. No title, path, byte or trace is recorded.

Ranking and the pre-registered experiment: `docs/decisions/0274-seg-126-bottleneck-ranking-and-pre-registration.md`. Candidate vs baseline results and decision: `docs/decisions/0275-seg-126-plane-run-results-and-decision.md`.

## Environment

| item | value |
|---|---|
| CPU | AMD Ryzen 7 5800X, 8 cores / 16 threads |
| Memory | 16 GB (15.6 GiB visible), WSL2 (Linux 6.18, microsoft-standard) |
| Compilers | GCC 13.3.0 (`cc`) and clang 18.1.3 (the packaged product compiles with `zig cc`, i.e. clang/LLVM; Zig 0.15.2 itself could not be run in this session, so clang 18 is the proxy) |
| SDL3 | 3.4.16; renderers `software` (dummy video driver), `opengl` (WSLg window, Mesa d3d12) |
| Noise | build CV 0.5-2.2% for images >= 20 s, 6-16% for the 4 s synthetic image; runtime CV 0.0-3.7% (headless 1.4-3.7%); load average at start 1.7-7.7 (the previous run's tail) |

## Build baseline (GCC 13.3, `--jobs 8`, median of 5; synthetic of 7)

| image | AOT policy | `-O` | wall | compile batches | aggregate job time | superseded (wasted) | generated C | executable |
|---|---|---|---|---|---|---|---|---|
| synthetic | compatibility | 0 / 1 / 2 | 4.68 / 3.72 / 4.08 s | 0.55 / 0.85 / 1.25 s | 2.1 / 3.2 / 4.6 s | 0 | 0.6 MiB | 0.3-0.5 MiB |
| synthetic | optimized | 2 | 3.95 s | 1.21 s | 5.1 s | 0 | 0.4 MiB | 0.3 MiB |
| genesis_512k | compatibility | 2 | **82.0 s** (CV 0.7%) | 74.1 s | 583 s | 26.6 s (4.6%) | 177.7 MiB | 38.1 MiB |
| genesis_512k | compatibility | 1 | 51.3 s (1.0%) | 43.3 s | 340 s | 14.8 s | 177.7 MiB | 39.3 MiB |
| genesis_512k | optimized | 2 | 28.8 s (2.2%) | 23.8 s | 182 s | 26.1 s (14%) | 54.7 MiB | 12.3 MiB |
| genesis_512k | optimized | 1 | 19.8 s (0.9%) | 14.7 s | 112 s | 15.0 s | 54.7 MiB | 12.5 MiB |
| sms_256k | compatibility | 2 | 59.8 s (0.5%) | 58.7 s | 450 s | 0 | 89.0 MiB | 28.8 MiB |
| sms_256k | compatibility | 1 | 39.8 s (1.6%) | 38.8 s | 298 s | 0 | 89.0 MiB | 26.7 MiB |
| sms_256k | optimized | 2 | 39.0 s (1.0%) | 37.9 s | 294 s | 0 | 58.8 MiB | 19.2 MiB |
| sms_256k | optimized | 1 | 25.4 s (0.5%) | 24.3 s | 189 s | 0 | 58.8 MiB | 17.8 MiB |

clang 18.1.3 (genesis_512k, optimized, 3 reps): `-O2` 18.4 s, `-O1` 17.9 s (-2.5%). One unrepeated clang compatibility build: 59.9 s (GCC `-O2`: 82 s).

Per-build structure of `genesis_512k` compatibility, GCC `-O2` (stage exclusive times, one profiled build): `compile_batch` 92% of wall; `m68k_emit` 3.9 s (4%); the Z80
materialization observation 2.6 s (2.5%); link 0.9 s; the rest is orchestration. Of the 754 s aggregate job time: generated M68K C 675 s (89%, 166.8 MiB of source), generated
Z80 C 75 s (10%; 25 s of it superseded), the handwritten runtime 1.5 s, the vendored/sound device sources 2.2 s, the viewer unit 0.3 s. Effective parallelism 7.9x of 8 jobs.

Compiler parallel scaling (genesis_512k, optimized, GCC, 3 interleaved reps each): `--jobs 8` 30.1/30.6/28.1 s, `--jobs 12` 29.2/29.1/29.2 s, `--jobs 16` 28.9/28.8/29.0 s. Peak
system memory 3.0 / 3.5 / 3.8 GB. Going from 8 to 16 jobs buys 3-4% at +0.8 GB.

## Runtime baseline (genesis_512k, compatibility AOT, `-O2`, median of 5, CPU-pinned)

| scenario | speed (% of native) | CV | avg frame interval | p99 | missed deadlines | per-frame spans (ms) |
|---|---|---|---|---|---|---|
| headless, unthrottled | 869% | 2.5% | 1.92 ms | 2.8 ms | - | emulation 1.92 |
| viewer, software renderer, unthrottled | **323%** | 0.2% | 5.17 ms | 5.95 ms | 0 | emulation 4.25 (**vdp_compose 2.17**), rgb_convert 0.36, texture_upload 0.03, present 0.53 |
| viewer, software renderer, paced | 100.1% | 0.0% | 16.66 ms | 17.75 ms | 1 | emulation 4.36 (vdp_compose 2.22), rgb 0.47, present 0.62, pacing_wait 11.2 |
| viewer, OpenGL renderer (WSLg), unthrottled | 215% | 0.8% | 7.76 ms | 9.5 ms | 0 | emulation 4.39 (vdp_compose 2.19), rgb 0.36, upload 0.12, **present 2.93** |
| viewer, OpenGL renderer (WSLg), paced | 100.0% | 0.0% | 16.67 ms | 18.5 ms | 2 | emulation 4.38, present 3.30, pacing_wait 8.4 |

Master System (sms_256k, compatibility `-O2`, viewer, software renderer, unthrottled, 3 reps, 1,500 frames): 1,294-1,316% of native, 1.27-1.29 ms per frame (emulation 0.60, present
0.60 ms). It is far from any limit and was not part of the interleaved matrix.

All paced sessions hold 100% of native speed on this host; the 1-2 missed deadlines are the start of the session. The unthrottled numbers show the headroom: the software
viewer runs at 3.2x real time, so a host ~3x slower than this one would just hold 60 fps with the current Genesis frame cost.

Hardware renderers were also probed on a 5 M-instruction run (single run each, not part of the matrix): `opengl` 242%, `gpu` 202%, `vulkan` 233% of native, present 1.6-2.7 ms per frame
against 0.53 ms for the software renderer. Hardware-backed renderers are not faster at the *host* side here: SDL's present call is dominated by synchronization with the
compositor, which the report cannot see inside.

### Instruction-level view (valgrind callgrind, viewer, software renderer, 3 M-instruction run)

Callgrind counts are **instruction counts, not time**; they locate work, they are not durations. Shares of all executed instructions:

| function (inclusive of its static callees where noted) | share |
|---|---|
| `genesis_vdp_plane_candidate_at` + `genesis_vdp_fetch_cell_pixel` + `genesis_vdp_render_row` + `genesis_vdp_select_cram_index` (per-pixel plane/tile composition) | 36.2% |
| `SDL_StretchSurface` (software renderer scaling, dummy driver only) | 9.2% |
| `genesis_sha256_block` (called from `genesis_vdp_frame_digest`: one SHA-256 of the frame artifact per produced frame (273 in this run)) | 8.4% |
| `genesis_vdp_decode_cram_entry` (called once per pixel by `genesis_vdp_frame_row_rgb888`) | 7.8% |
| M68K retirement + bridge dispatch + scheduler | ~19% |
| Z80 + YM2612 | ~3.5% |

## Comparability limits

1. One host, one CPU model, WSL2 (a virtualized scheduler and a Windows-side compositor). Linux/macOS/Windows hosts, ARM CPUs and bare-metal GPUs were not measured; relative
   effects within one host are the claim, absolute numbers are not portable.
2. The packaged product compiles with `zig cc` 0.15.2. That compiler could not be executed here; clang 18.1.3 is a same-family proxy and the GCC-specific `-O1` result must not be
   read as a statement about the packaged compiler (the clang row shows only -2.5%).
3. Commercial images are local and unpublished; the scenario is "reset to N instructions", which depends on the image's boot path. Results for another image are comparable only
   in kind.
4. GPU execution time is unmeasurable (present is host submission time); M68K and Z80 time inside `emulation` are not separable without per-instruction clocks (declared
   `unavailable` by SEG-125); hence the callgrind instruction counts above, which are a different quantity.
5. Builds are not pinned and use all cores; their run-to-run CV is small (0.5-2.2%) for >= 20 s builds but 6-16% for the 4 s synthetic build, so the synthetic result is a
   structure check, not a precise number.
6. Sega CD was not baselined (it reuses the Genesis runtime, so a Genesis result would apply).

## Reproducing

```
cmake -S . -B build/perf -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/perf --target segarecomp -j8
python3 tools/perf_baseline_bench.py build   --cli build/perf/apps/segarecomp/segarecomp --root . --synthetic --image 'genesis_512k=<image>' --reps 5 --opt-levels 1 2 -o build.json
python3 tools/perf_baseline_bench.py runtime --cli build/perf/apps/segarecomp/segarecomp --root . --image 'genesis_512k=<image>' --sdl3 <sdl3 prefix> --renderers software opengl --reps 5 -o runtime.json
python3 tools/perf_baseline_bench.py compare before.json after.json
```
