# Widescreen scenery lookup: pre-registered measurement (SEG-139-T005)

Status: **criterion pre-registered** (this section was committed before any measurement of the widescreen producer was taken; the results
section is added by a later commit, so `git log -p` shows the order).

## Question

At wide aspects (16:9, 21:9, 32:9) the Genesis extension producer (`genesis_vdp_produce_extension`, `platforms/genesis/runtime/vdp_render.c`)
evaluates, for every extra source pixel on every row, a Plane A and a Plane B candidate (`genesis_vdp_plane_candidate_at`), selects the
CRAM index and decodes the colour. Cost therefore grows linearly with the planned side. Is that lookup a bottleneck that justifies a
cell/span cache (or another change), or is "no change" the correct outcome?

## Method (reuses `tools/perf_report.py`; no new profiling stack)

- Subject: the packaged generated viewer, built with `segarecomp build ... --optimize 2 --runtime-optimize 2` for the synthetic display ROM of
  `tests/perf_viewer_integration_test.py` (H40, display on, scroll planes enabled, no commercial content), software renderer, dummy video/audio drivers,
  `SEGARECOMP_VIEWER_UNTHROTTLED=1`, `SEGARECOMP_VIEWER_MUTE=1`, `--instruction-budget 2000000`.
- Runtime report per configuration: `SEGARECOMP_PERF_REPORT` + `SEGARECOMP_PERF_MODE=detailed`; the metric is the `vdp_compose` span
  (frame composition including the extension producer, ADR 0273) and `texture_upload`, read with `perf_report.py` (`pr.load`, `pr.runtime_derived`,
  `perf_report.py compare`).
- Configurations (settings file only): no extension (`video.widescreen=original`), Level 2 at the legacy 16:9 width, and Level 2 with
  `video.widescreen_ratio` = `16:9`, `21:9`, `32:9`. Producer cost per frame = `vdp_compose` average with the extension minus the no-extension average.
- Repetition: 5 runs per configuration, interleaved; the median of per-run averages is the figure.
- Before/after: the same driver is run against a build of product `main` before SEG-139 (`ce49e73`) for the no-extension and legacy 16:9 configurations
  (the only ones that exist there), to check there is no 16:9 regression from the planner change.
- The native-rate frame budget is 1/60 s = 16,666,667 ns (NTSC); the figure is a host-time measurement on the CI-class host that ran it and is labelled
  `[measured]`; GPU time is not measured (declared unavailable by the report).

## Pre-registered success criterion (fixed before measuring)

An optimization of the lookup is **warranted** only if BOTH hold:

1. **Cost.** The median producer cost per frame at **32:9** (320 x 224, Level 2) is **at least 10% of the frame budget** (>= 1,666,667 ns), OR the producer cost at
   16:9 is at least 5% of the budget (>= 833,333 ns).
2. **Attribution.** A function-level profile (`gprof` of the same build, if criterion 1 holds) attributes **at least 50%** of the extension producer's
   time to the plane lookup (`genesis_vdp_plane_candidate_at` and callees).

If criterion 1 fails the outcome is **no change**: the task delivers the measurement and this document only (no code change), as the task acceptance requires.
If criterion 1 holds but 2 fails, the dominant cost is elsewhere and a lookup cache is not evidence-backed: record the negative result for the lookup and
name the measured dominant cost without implementing it in this task.

If an optimization is implemented, success additionally requires: producer cost at 32:9 reduced by **at least 25%** (median of 5), golden hashes of the
producer tests (`genesis_extension_test`, which compares every pixel to an independent reference) unchanged, and no 16:9 regression greater than 3%.

## Results

(pending - filled in by the measurement commit)
