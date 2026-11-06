# ADR 0273: Performance profiling and observability (SEG-125)

- Status: Accepted.
- Scope: build-time and runtime HOST performance measurement, one versioned report envelope, a local report/compare tool, an optional viewer overlay and a
  sanitized sharing form. It adds no interpreter, JIT, tracing framework, telemetry channel or third-party dependency, and changes no guest behavior.

## 1. Decision in one paragraph

Two small collectors write the same JSON envelope (`segarecomp.performance`, `schema_version` 1): a C++ `BuildProfile` bracketing the existing stages of
`segarecomp build` (`--performance-report <file>`), and a header-only, allocation-free C `SvPerf` struct (`libs/viewer/include/sv_perf.h`) owned by the
console adapter that runs the guest (Genesis headless hook, Genesis SDL3 viewer; the Master System and Sega CD viewers adopt the same struct at their existing
frame loop). Both are **off unless asked for**: without a report request or the overlay switched on, no collector is enabled and the only cost is one branch
at each frame/slice/stage boundary. `tools/perf_report.py` (Python standard library) validates, summarizes, compares, sanitizes and renders reports.

## 2. Reused facilities (nothing duplicated)

| need | reused |
|---|---|
| build stage boundaries | the `@stage <name> begin/done` markers of `apps/segarecomp/build_command.cpp` (`status.json`, launcher progress and `build.log` are unchanged) |
| object reuse / supersession | the existing content-keyed object caches (`m68k_objects`, `z80_objects`) and `plan_m68k`/`prepare`; the profiler *observes* them and never changes a key |
| frame boundaries and pacing | `GenesisRuntime::virtual_frame_count` (the pacing authority), `GenesisPacer`, the `GenesisViewerHost` callbacks (wrapped, not edited) |
| CPU / device counters | the runner's dispatch count, `scheduler.master_ticks`, Z80 epoch count, mixer frame/underrun/overrun counters |
| renderer name | `sv_host_renderer_name` |
| FPS box and settings | `video.show_fps` box and `sv_settings` of the shared viewer host; the overlay is one more value there, not a second UI |
| exit classification | the viewer's typed outcome (`guest_stop`, `window_closed`, ...) and the runner's `GENESIS_*` kinds |

## 3. Modes

| mode | how | what it records |
|---|---|---|
| build | `segarecomp build ... --performance-report <file.json>` | stages, operations (inclusive and exclusive), compiler jobs, object disposition, sizes, config |
| runtime *summary* (default) | `SEGARECOMP_PERF_REPORT=<file>` | frame-interval and frame-work histograms, span totals, counters, pacing |
| runtime *detailed* | `SEGARECOMP_PERF_MODE=detailed` | summary plus the last 1024 frames (frame-indexed timeline) in a **private** report |
| overlay | viewer menu *Video > Performance Overlay* or `F3` (Off / Player / Developer) | live view of the same collector; no report needed |
| privacy | `SEGARECOMP_PERF_PRIVACY=private|shareable`, `perf_report.py sanitize` | see section 8 |

Headless runtime profiling runs the same runner in bounded slices (the re-entrant contract the viewer already relies on; slice size is reported and part of
comparability). A headless frame-capture run is not measured (it has its own loop); that is reported on stderr, never silently.

## 4. Host time versus guest time

Every duration in a report is **host wall time** (monotonic clock) or **host CPU time**. Guest time is carried only as counters (`guest.master_ticks`,
`counters.m68k_dispatches`, virtual frames). The collector never feeds a host value into the machine: it wraps callbacks that already exist, reads the clock
at slice/frame/present boundaries, and writes only its own struct. Equivalence is tested: sound summary (audio digest, virtual frames, result kind, stop class)
is identical with the collector on and off.

*Emulation speed* = (virtual frames x the console's native frame period) / host wall time of those frames. It is derived by the consumer from `frames.virtual`,
`config.target_frame_ns` and `wall_ns`; a headless free-run reports how much faster than real time the host can run the guest, a paced viewer session ~100%.

## 5. Metrics (meaning, unit, owner)

All times are integer nanoseconds (`*_ns`) in runtime reports and milliseconds with three decimals (`*_ms`) in build reports; sizes are bytes.

Runtime (`runtime`):

| metric | meaning | owner |
|---|---|---|
| `wall_ns`, `paused_ns`, `pauses` | session length; time in the pause menu (excluded from frame intervals) | `SvPerf` |
| `frames.virtual / presented / coalesced` | guest frame boundaries crossed; host presents; boundaries that shared one slice (their interval is split evenly) | adapter |
| `frame_interval` | wall time between virtual-frame boundaries (includes pacing wait): count, min, avg, p50, p95, p99, max; histogram resolution 50 us, top bucket 64 ms (max stays exact) | `SvPerf` |
| `frame_work` | `frame_interval` minus pacing wait: the host headroom indicator | `SvPerf` |
| `pacing.waits / wait_ns / missed_deadlines / resyncs` | pacer sleeps; boundaries whose deadline had already passed; the pacer's late-stall resynchronizations | adapter + `GenesisPacer` |
| `spans[]` | `emulation` (all guest slices incl. CPU, Z80 sync, devices, sound synthesis; inclusive), `vdp_compose` (inclusive, contained in emulation), `rgb_convert`, `present` (inclusive: host submission, **not** GPU time), `texture_upload` (inclusive, contained in present), `audio_present`, `pacing_wait` (exclusive). Each row names its `class` and `parent`; inclusive rows must not be summed with their parent | adapter / viewer host |
| `guest.master_ticks` | deterministic guest time at the end | runtime |
| `counters{}` | existing counters: `m68k_dispatches`, `runner_slices`, `z80_epochs`, `audio_frames`, `audio_underruns`, `audio_overrun_dropped`, `frames_published`, `render_rejections` ... (platform-specific, explicit names) | runtime / mixer |
| `unavailable[]` | `{metric, reason}` for every metric the platform or entry cannot give | adapter |

Build (`build`): `wall_ms`; `stages[]` (the `@stage` markers; nested stages are children of their parent) and `operations[]` (`m68k_emit`, `z80_emit`,
`z80_materialize`, `alias_round`, `compile_batch`, `link_exec`, `sms_emit`), each with `count`, `inclusive_ms` and `exclusive_ms` (= inclusive minus direct
children, so the exclusive times plus `accounting.unattributed_ms` equal `wall_ms`); `counts` (`m68k_emissions`, `alias_rounds`, `materialize_rounds`,
`z80_prepare_iterations`); `sizes` (`generated_c`, `executable`, ...); `compile` (see section 6); `config` (optimize, runtime optimize, AOT policy, jobs,
hardware concurrency, viewer, compiler name and version line, host OS).

Not measured and said so in `unavailable[]`: M68K time and Z80 time separately (they interleave inside one retirement loop; a clock per instruction is forbidden),
DMA/interrupt counts (no existing inexpensive counter), GPU execution time (`SDL_RenderPresent` returning is submission time), Sega CD main/sub split (until
the machine exposes per-CPU counters).

## 6. Compiler jobs and object accounting

Each translation unit job yields `{category, wall_ms, source_bytes, object_bytes, round, disposition}`:

- `compiled` jobs are timed by the worker that ran them (each worker writes only its own slot; the main thread reads after joining), together with child CPU time
  and peak RSS where the OS reports them (POSIX `wait4`; Windows CPU time only) - otherwise `null` and listed under `unavailable`.
- `reused` units are those whose content key was already compiled (`reuse_saved_ms` = what their original compile cost).
- After the final link the profiler is told which objects the executable contains. A compiled object is **`final`**, **`pass_only`** (needed only by the build-time
  observation program, e.g. the pass hook - needed, not wasted), or **`superseded`** (a later alias-preparation round replaced the unit - discarded work, its time is
  `wasted_ms`). A build that fails before the final link reports `unit_disposition` unavailable.
- `batch_wall_ms` (wall time of the batches) and `aggregate_job_ms` (sum of the concurrent jobs) are separate numbers; their ratio is the achieved parallelism and
  the two are never added.
- `phases.initial` versus `phases.preparation` separate the first build from work repeated by alias-preparation rounds (`round` > 0).

## 7. Report envelope, status and failure semantics

```
{ "schema": "segarecomp.performance", "schema_version": 1,
  "kind": "build" | "runtime", "status": "complete" | "incomplete" | "failed",
  "privacy": "private" | "shareable", "platform": "genesis" | "master_system" | "sega_cd",
  "config": { ... comparability keys ... }, "build": { ... } | "runtime": { ... }, "unavailable": [ ... ] }
```

- `complete`: the session/build ended as requested (build succeeded; viewer window closed, budget reached or guest finished).
- `incomplete`: measurements are valid up to a typed guest stop, or the report is a start-of-session marker whose session never finished (a killed process leaves
  `outcome: running`).
- `failed`: the build failed (`failure.stage` names the innermost open stage) or the host failed. The report is still written.
- A schema or major-version mismatch is rejected by the reader; unknown additional keys are ignored (minor additions keep `schema_version`).
- Writing a report can fail (read-only target): the build/session result is unaffected and a diagnostic is printed.

Boundedness: histograms are fixed arrays, the frame history is a 1024-entry ring, span rows are a fixed enumeration, build unit lists are capped at 10 rows, so a
report is at most ~30 KB for runtime and ~100 KB for build however long the session or large the game.

## 8. Private versus shareable

- **private** (default, local): may include unit labels (generated-source file names), `slowest_units`/`largest_units`, the frame timeline and compiler version.
  It never contains ROM bytes, disassembly, addresses, PCs, ROM paths or hashes, or game titles - those are never collected - but it is still treated as local.
- **shareable**: aggregates and categories only - no unit names, no timeline, no compiler version string beyond its name, no host paths. Produced by
  `perf_report.py sanitize` (an allow-list rewrite: unknown keys are dropped, not copied) or `SEGARECOMP_PERF_PRIVACY=shareable`.
- Nothing is uploaded; there is no network code. Commercial-derived reports follow `docs/testing/commercial-games.md`: durable evidence uses the shareable form.

## 9. Benchmark comparability

Two reports are comparable only if `kind`, `platform`, `status = complete` and these config keys agree: build - `optimize`, `runtime_optimize`, `aot_policy`,
`jobs`, `viewer`, compiler name, `host_os`; runtime - `entry`, `paced`, `slice_dispatches`, `target_frame_ns`, `renderer`, `host_os`, `mode`. A difference is a
warning in `perf_report.py compare` (the numbers are still printed, labelled *not comparable*), and different frame counts or a non-complete status are called out.
Host noise is the consumer's responsibility: compare repeated runs; the tool reports run-to-run spread when given several reports of one side.

## 10. Platform fit

Genesis: both entries implemented. Master System and Sega CD: the same `SvPerf` struct is adopted at their viewer loops (`sms_viewer_run`,
`scd_viewer.c`) with their own counter names; platform sections never share fields that mean different things. No cross-platform abstraction layer is added: the
common part is the envelope and the collector struct only.

## 11. Rejected alternatives

A generic event/trace bus, per-instruction counters, sampling profilers, background threads, a database, uploading, and GPU timestamp queries were rejected
(cost, determinism, scope). Per-unit compile timing by parsing compiler `-ftime-report` was rejected (compiler-specific).

## 12. Consequences and measured cost

- The collector is compiled into every Genesis/Master System/Sega CD viewer and headless program (header-only, ~55 KiB of static data) but is idle unless a report or the
  overlay is requested; idle cost is one branch per slice/frame/present. Measured end-to-end overhead is below the run-to-run noise of the method (about 2%) and the direct
  collector cost is ~0.01% of a frame (method, table and limits: `docs/testing/performance-profiling.md`).
- Generated programs built by an older `segarecomp` do not read the new environment variables; a report is produced only by a program built after this change.
- `SvPerf` and `BuildProfile` are the only new stateful pieces; `perf_report.py` is stateless. Future consumers (GPU-acceleration studies, CI trend checks) read the
  versioned JSON; they do not get hooks into the collectors.
- Out of scope and unchanged: guest instruction-level profiling, execution-coverage integration (the existing coverage tooling is untouched), GPU timing.
