# Performance profiling and observability

Design: [ADR 0273](../decisions/0273-performance-profiling-and-observability.md). This page is the how-to. Everything here measures **host** time (how long this computer
takes); it never changes the game, and with nothing requested nothing is collected. Nothing is uploaded: reports are local files.

What it answers:

| question | where to look |
|---|---|
| Where is compilation time spent? | build report `stages` / `operations` (exclusive time), `compile.categories` |
| How much compile work is repeated or thrown away? | `compile.phases` (initial vs preparation rounds), `superseded`, `wasted_ms`, `reuse_saved_ms` |
| Which generated translation units are expensive? | `compile.slowest_units` / `largest_units` (private report) |
| Is gameplay limited by CPU, devices, rendering, audio or presenting? | runtime `spans` (emulation, vdp_compose, rgb_convert, present, audio_present, pacing_wait) and the *measured shares* lines |
| Does the game keep its intended speed? | emulation speed (derived), `frame_interval` p50/p95/p99/max, `pacing.missed_deadlines` |
| What changed between two builds or sessions? | `perf_report.py compare` |
| Can players see it without developer tools? | the in-game overlay (F3) |

## Commands

All paths are examples. `tools/perf_report.py` needs only Python 3.11+.

**1. Profile a compilation**

```
segarecomp build --rom game.bin --output out --cc cc --runtime-dir platforms/genesis --performance-report build.json
python3 tools/perf_report.py summary build.json
```

The report is written on every exit, marked `complete` or `failed` (with the failing stage). `status.json`, the launcher's `@stage` lines and the generated program are
unchanged. Add `--jobs N` to see the effect of compiler concurrency: batch wall time and the aggregate of the concurrent jobs are reported separately.

**2. Profile a headless run** (the generated program, no window)

```
SEGARECOMP_PERF_REPORT=run.json ./out/game --instruction-budget 50000000
SEGARECOMP_PERF_REPORT=run.json SEGARECOMP_PERF_MODE=detailed ./out/game --instruction-budget 50000000
```

`SEGARECOMP_PERF_MODE` is `summary` (default) or `detailed` (adds the last 1024 frames as a frame-indexed timeline). The run uses the same runner in bounded slices
(`SEGARECOMP_PERF_SLICE`, default 4096 dispatches); the guest result, sound digest and stop class are identical with and without it. Always pass a finite
`--instruction-budget` to a headless run.

**3. Profile in the viewer**

```
SEGARECOMP_PERF_REPORT=viewer.json SEGARECOMP_PERF_MODE=detailed ./out/game          # a viewer build; the report is written when the session ends
```

Without a report, switch the overlay on while playing: **F3** cycles Off - Player - Developer, or use *Pause menu > Video > Performance Overlay* (the setting is saved in
the viewer settings file as `video.perf_overlay`). *Player*: emulation speed, presented FPS, frame-time p95 and missed deadlines, the render backend and a graph of the
last 120 frames (green = on time, red = more than 12.5% over the console's frame time). *Developer* adds the average per-frame cost of emulation, VDP composition,
present, texture upload, audio and the pacing wait. Nothing is drawn unless you ask; the pause menu is excluded from frame times; Genesis, Master System and Sega CD
use the same shared viewer.

**4. Compare two builds**

```
python3 tools/perf_report.py compare before.json after.json
python3 tools/perf_report.py compare vanilla.json patched.json --mod     # a modded/patched build compiles a different program: expected, noted
```

**5. Compare runtime performance**

```
python3 tools/perf_report.py compare run-before.json run-after.json
```

Output shows `before -> after`, absolute and percentage change, and flags a change as significant only when it is at least 10% *and* above an absolute floor (50 ms for
builds, 0.1 ms for frame times). Reports that are not comparable (different platform, renderer, pacing mode, slice size, optimization level, jobs, compiler, ...)
produce `WARNING` lines; different kinds or platforms print no numbers and exit with status 3. Repeat each side a few times and compare similar medians: one run is one
sample of a noisy host.

**6. Offline HTML report**

```
python3 tools/perf_report.py html build.json -o build.html
python3 tools/perf_report.py html run-before.json run-after.json -o compare.html
python3 tools/perf_report.py compare before.json after.json --html compare.html
```

One self-contained file: inline CSS and SVG, no script, no network, no external font. Charts: build stages, operations, compile cost by category, slowest units,
kept-vs-discarded and compiled-vs-reused work, runtime frame timeline, subsystem breakdown and before/after differences. Every value is labelled *measured*,
*derived* (computed from measured values by a stated rule) or *unavailable*.

**7. Share safely**

```
python3 tools/perf_report.py sanitize build.json -o build-shareable.json
python3 tools/perf_report.py check-shareable build-shareable.json
SEGARECOMP_PERF_PRIVACY=shareable SEGARECOMP_PERF_REPORT=run.json ./out/game --instruction-budget 50000000   # runtime: shareable at the source
```

`sanitize` rebuilds the report from an allow-list: aggregates and categories only. It drops unit names, the compiler version line, the frame timeline and any text that
looks like a path; unknown fields are never copied. A **private** report (the default) is for you: it may name generated translation units and holds the per-frame
timeline. Reports never contain ROM bytes, addresses, PCs, ROM paths or hashes, or game titles, because those are never collected - but for commercial games follow
[`commercial-games.md`](commercial-games.md): commit nothing derived from a commercial game, and quote only the shareable form (class-level numbers) in durable evidence.

## Reading a report

- **Exclusive vs inclusive.** A stage or operation's *exclusive* time excludes the spans nested in it; exclusive times plus the unattributed remainder equal the wall
  time. Runtime `vdp_compose` is *inside* `emulation`; `texture_upload` is inside `present`. Do not add an inclusive row to its parent.
- **Wall vs aggregate.** Compile jobs run concurrently. `batch_wall_ms` is the time the build waited; `aggregate_job_ms` is the sum of the jobs' own times; their ratio is
  the achieved parallelism. A serial build has a ratio near 1.
- **Used vs discarded.** After the final link every compiled object is `final`, `pass_only` (needed by the build-time observation program) or `superseded` (replaced by a
  later alias-preparation round: discarded work, counted in `wasted_ms`).
- **Host time vs guest time.** `guest.master_ticks`, virtual frames and dispatch counts are deterministic guest quantities. Speed = virtual frames x the console's native
  frame period / host wall time. A headless or unthrottled run shows how much *faster* than real time this computer can emulate; a paced session shows ~100%.
- **Frequency vs cost.** Counters (`m68k_dispatches`, `z80_epochs`, `audio_frames`, ...) say how often something happened; spans say how much host time a stage took.
  They are different quantities and are reported separately. A per-address execution profile is not collected (the existing execution-coverage tooling is untouched and
  can be joined to the frame timeline by frame number in future work).
- **`present` is not GPU time.** It is the host submission call; the report lists `gpu_time` as unavailable.
- **Unavailable** measurements are listed with the reason: separate M68K/Z80 time (they interleave inside one retirement loop; a clock read per instruction is not
  acceptable), DMA/interrupt counts (no cheap existing counter), Sega CD main/sub split, Windows peak RSS.

## Status of a report

`complete`: the session or build ended as requested. `incomplete`: a typed guest stop ended the session (numbers are valid up to it), or the report is the start-of-session
marker of a session that never finished. `failed`: the build or the host failed (a build report names the stage). A report is still written for each.

## Measured cost of the collector

Method (`tools/perf_overhead_bench.py`, reproduces everything below): one synthetic spinning Genesis image built at `-O2`; every variant is run interleaved
(off, summary, detailed, ... rotated each round, 11 rounds for runs, 5 for builds) so host drift hits all variants alike; runtime children are pinned to one CPU; the median
and the min-max spread of each variant are reported, and a collector microbenchmark gives its direct cost. Linux x86-64, GCC, 16 hardware threads, otherwise idle;
the workload is a spinning guest, i.e. the *worst case* for a relative figure (a real game spends longer per frame in the same emulation).

| variant (median of 11 / 5) | time | vs off | run-to-run spread |
|---|---|---|---|
| headless, collector off | 5.876 s | - | 8.6% |
| headless, summary | 5.920 s | +0.75% | 5.4% |
| headless, detailed | 5.867 s | -0.15% | 8.8% |
| viewer (unthrottled, dummy driver), off | 1.553 s | - | 14.1% |
| viewer, report summary | 1.541 s | -0.79% | 10.3% |
| viewer, report detailed | 1.578 s | +1.60% | 11.5% |
| viewer, overlay player | 1.544 s | -0.60% | 12.1% |
| viewer, overlay developer | 1.542 s | -0.74% | 13.5% |
| `segarecomp build`, off | 4.012 s | - | 7.9% |
| `segarecomp build --performance-report` | 4.063 s | +1.27% | 6.9% |

Reading: every difference is smaller than the spread between repeated runs of the *same* variant, so this method cannot resolve an overhead below about 2%. The targets
(profiling disabled: no regression; summary <= 1%; detailed <= 3-5%) are therefore met within measurement resolution, and the resolution is stated rather than tuned away.
The direct cost is measured instead: on this machine a clock read is ~18 ns, a begin/end span pair ~38 ns, a frame boundary ~24 ns, and an *idle* collector (nothing requested)
~0.4 ns per frame-loop visit (one branch each). The headless run above makes 9,766 slices for 3,125 frames, i.e. ~142 ns of collector work per frame = **0.008%** of a
frame's host time (0.0076% reported by the bench). The viewer adds ~4 clock reads per slice (500 dispatches) plus the host's own present/upload spans; at the same arithmetic
that is ~0.1% of a 2 ms frame. "Disabled" means the wrapper callbacks are installed but idle: no clock is read and no memory is written (verified by `sv_perf_test`
and `sv_perf_overlay_test`: zero clock reads, bit-identical picture).

Not measured: Windows/macOS, a hardware (non-software) renderer, or a real game; none of those was available. Run the bench there before relying on the numbers.

## For developers

- Collector: `libs/viewer/include/sv_perf.h` (header-only C11, fixed-size, no allocation, idle = one branch). Console adapters wrap their existing host callbacks:
  `platforms/genesis/runtime/genesis_sound_hook.c` (headless), `platforms/genesis/viewer/viewer_main_hook.c`, `platforms/master-system/viewer/sms_viewer_main.c`,
  `platforms/sega-cd/viewer/scd_viewer.c`; the shared host (`libs/viewer/src/sv_host.c`) times its own present/upload and draws the overlay.
- Build profiler: `apps/segarecomp/build_profile.hpp` (+ the stage markers and `ProfileSpan` call sites in `build_command.cpp`; per-process CPU/RSS from `process_run.cpp`).
- Tests: `sv_perf_test`, `sv_perf_overlay_test`, `build_profile_test`, `perf_report_test`, `perf_integration_test`.
- Adding a metric: add it to the collector, give it a name in the adapter's `counter_names` (or a span id), list it in the ADR table, and make the reader validate it. If it
  cannot be measured cheaply on some platform, report it under `unavailable` with the reason instead of a zero.
