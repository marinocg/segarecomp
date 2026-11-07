# SEG-127 baseline (post SEG-126) and measurement method

Frozen baseline for the SEG-127 multi-optimization work package. Same method and tools as `performance-baseline-seg126.md` (no new framework):
`tools/perf_baseline_bench.py` (an interleaving runner over the SEG-125 `--performance-report` / `SEGARECOMP_PERF_REPORT` data, `tools/perf_report.py`).
Commit under measurement: product `main` at `6ff4010` (SEG-126 merged, i.e. including the per-cell VDP plane evaluation of ADR 0275), built Release.
Raw samples: `docs/testing/data/seg127/runtime-base-6ff4010.json` (5 interleaved reps, CPU-pinned runtime runs) and `build-base-6ff4010.json` (3 interleaved reps).
Candidate-vs-baseline data files in the same directory are named `*-ab-*.json`; every A/B result in ADR 0276 compares against these programs/binaries.

## Environment and variability

| item | value |
|---|---|
| CPU / memory | AMD Ryzen 7 5800X (8C/16T), 16 GB, WSL2 (Linux 6.18) |
| Compilers | GCC 13.3.0 (`cc`), clang 18.1.3. The packaged `zig cc` is **not installed** on this host (see T006) |
| SDL3 | 3.4.16, `software` renderer on the dummy video driver (host-side presentation without a compositor) and `opengl` on WSLg |
| Runtime runs | pinned to one CPU (`taskset -c 2`), 30 M M68K dispatches unthrottled (~3,100 guest frames) or 10 M paced, from the cartridge reset |
| Builds | `--jobs 8`, not pinned; images are legally held local files from the ignored `games/` set, referred to only by label (`genesis_512k`) |
| Observed noise | runtime CV 0.0-0.7% (viewer), 0.7-2.5% (headless); build CV 0.1-0.3% (3 reps, genesis_512k), ~0.5% synthetic |

Cold vs repeated builds: `segarecomp build` keeps no object cache across builds (objects are removed at start), so a repeated build does the same compile work; only
the OS file cache differs. Paced vs unthrottled: paced runs measure smoothness (100% of native speed with few missed deadlines), unthrottled runs measure capacity.

## Runtime baseline (genesis_512k, compatibility AOT, -O2, median of 5)

| scenario | speed (% of native) | CV | per-frame spans (ms) |
|---|---|---|---|
| headless, unthrottled | 810% | 0.7-2.5% | emulation 2.06 (no VDP composition) |
| viewer, software renderer, unthrottled | **425.7%** | 0.3% | emulation 2.96 (**vdp_compose 0.90**), **rgb_convert 0.36**, texture_upload 0.03, present 0.56 |
| viewer, software renderer, paced | 100.1% | 0.0% | emulation 3.05, rgb_convert 0.47, present 0.62, pacing_wait 12.5 |
| viewer, OpenGL (WSLg), unthrottled | 257.6% | 0.1% | emulation 3.08 (vdp_compose 0.91), rgb_convert 0.36, texture_upload 0.12, **present 2.94** |
| viewer, OpenGL (WSLg), paced | 100.0% | 0.0% | present 3.34, pacing_wait 9.71 |

Ranked remaining per-frame costs of the unthrottled software viewer (3.92 ms per guest frame): non-VDP emulation ~2.06 (M68K retirement + bridge, Z80, YM2612),
`vdp_compose` 0.90 (of which the frame-artifact SHA-256 is ~0.2 - measured by T003), `present` 0.56 (SDL presentation; 2.9 ms on OpenGL/WSLg, dominated by compositor
synchronization), `rgb_convert` 0.36 (the CRAM decode repeated for every pixel), `texture_upload` 0.03.

## Build baseline (genesis_512k, GCC -O2, `--jobs 8`, median of 3)

| AOT policy | wall | aggregate compiler job time | generated M68K C | generated Z80 C | superseded (wasted) Z80 job time | generated C | executable |
|---|---|---|---|---|---|---|---|
| compatibility | **85.3 s** | 605 s | 519 s (86%) | 81.3 s | **27.5 s** (4.5% of aggregate, 34% of the Z80 work) | 177.7 MiB | 38.1 MiB |
| optimized | **30.0 s** | 191 s | 105 s (55%) | 81.8 s (43%) | **27.7 s** (14.5% of aggregate) | 54.7 MiB | 12.3 MiB |
| synthetic (compat / optimized) | 4.0 / 3.9 s | 4.7 / 5.1 s | - | - | 0 | 0.6 / 0.4 MiB | 0.3 MiB |

Z80 materialization (Genesis): 2 images, 3 discovery runs + 1 confirming run, 31 Z80 units compiled, **0 reused** (19 are the final program's units, 12 are superseded
by later rounds); `genesis_z80_materialize` observation runs ~15 s. M68K C: 78,108 retired-instruction entries, ~1,560 bytes of C per instruction; the largest repeated
scaffolding per instruction is the retirement call (`m68k_retirement_pc`, `if (retired...) return retired; return retired;` ~14% of the bytes of an owner unit), the
instruction-provenance compound literal (~7%) and the routed-access scaffolding (~15%).

## Limits

One host; WSL2 virtualized scheduler; GCC/clang proxies for the packaged `zig cc`; commercial images are local and unpublished (scenario = reset to N instructions); GPU execution
time is unmeasurable (present is host submission time); relative effects within the host are the claim, absolute numbers are not portable.

## Reproducing

```
cmake -S . -B build/perf -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/perf --target segarecomp -j8
python3 tools/perf_baseline_bench.py runtime --cli <cli> --root <product root> --image 'genesis_512k=<image>' --sdl3 <sdl3 prefix> --renderers software opengl --reps 5 --work-dir <fresh dir> -o runtime.json
python3 tools/perf_baseline_bench.py build   --cli <cli> --root <product root> --synthetic --image 'genesis_512k=<image>' --reps 3 --opt-levels 2 -o build.json
python3 tools/perf_baseline_bench.py compare before.json after.json
```
`--image LABEL=PATH@ROOT` builds one runtime variant from another checkout, which is how the per-task checkpoints are compared inside one interleaved run.
