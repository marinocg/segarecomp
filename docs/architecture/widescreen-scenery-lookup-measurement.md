# Widescreen scenery lookup: pre-registered measurement (SEG-139-T005)

Status: **measured; no production change**. The success criterion (thresholds) was committed in `ceed3fa`, before any measurement of the widescreen
producer; the metric correction and the results were committed together afterwards (`ffd6335`), so git shows the thresholds' order but the correction's timing is self-reported.

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
  (**as pre-registered; corrected to `emulation`, see below**) and `texture_upload`, read with `perf_report.py` (`pr.load`, `pr.runtime_derived`,
  `perf_report.py compare`).
- Configurations (settings file only): no extension (`video.widescreen=original`), Level 2 at the legacy 16:9 width, and Level 2 with
  `video.widescreen_ratio` = `16:9`, `21:9`, `32:9`. Producer cost per frame = the average with the extension minus the no-extension average.
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

## Correction to the metric (self-reported: made after the first run, before any threshold was evaluated)

The first run showed the `vdp_compose` span does not contain the extension producer: it times only the native raster producer
(`perf_raster_producer`, `platforms/genesis/viewer/viewer_main_hook.c`), while the extension runs in `genesis_run_frame_extension` inside the guest slice. The
delta of `vdp_compose` was therefore ~0 for every aspect (a property of where the span is placed, not a finding about cost). The metric is corrected to the
per-frame `emulation` span (all guest execution of a slice, inclusive of the extension producer); **the thresholds above are unchanged**. The uncorrected first
run is kept in the results for transparency.

## Results

> **Reading note.** "Producer cost" below is a **proxy**: the difference in the per-frame `emulation` span (all guest execution of a slice) between a configuration with the extension and the one without. It includes other emulation work the extension perturbs (for example cache effects) and is not an isolated timer around `genesis_vdp_produce_extension`. The `vdp_compose` span does not contain the extension producer. The two attribution methods below (gprof 40.4%, ablation up to 69%) disagree and are both kept with their limits; the pre-registered rule, with its thresholds unrevised, decides: no lookup optimization is warranted.

Host: Linux 6.6 aarch64 (4 cores), GCC 14.2.0, `-O2`; viewer build of this delivery (`tools/widescreen_perf_matrix.py --runs 7`, 156 virtual frames per run,
median of 7 interleaved runs). All figures are `[measured]` host times in nanoseconds per virtual frame; the frame budget is 16,666,667 ns.

| Configuration | `emulation` | `vdp_compose` | `texture_upload` | Producer cost (emulation - none) | Share of 60 Hz budget |
| --- | ---: | ---: | ---: | ---: | ---: |
| no extension | 1,426,697 | 393,580 | 30,825 | 0 | 0.00% |
| legacy 16:9 width (unset ratio) | 1,774,095 | 395,674 | 89,171 | 347,398 | 2.08% |
| exact 16:9 | 1,764,088 | 392,913 | 87,162 | 337,390 | 2.02% |
| exact 21:9 | 2,219,107 | 391,614 | 140,253 | 792,409 | 4.75% |
| exact 32:9 | 3,188,581 | 392,571 | 251,073 | **1,761,883** | **10.57%** |

(`vdp_compose` is flat across rows, which is the correction above made visible: the span does not contain the extension.) The producer cost is linear in the planned
side (101 -> 238 pixels per side: x2.2 pixels, x2.3 cost), as the per-pixel loop predicts.

**16:9 regression check (before/after).** Same driver, product `main` at `ce49e73` (before SEG-139) versus this delivery, 7 interleaved runs each:

| Configuration | before (`ce49e73`) producer ns | after producer ns | change |
| --- | ---: | ---: | ---: |
| legacy 16:9 | 342,859 | 347,398 | +1.3% (noise level; the criterion allows 3%) |

The first uncorrected run (`vdp_compose` metric, 5 runs) gave producer deltas of 1,295 / 1,389 / -628 / -1,777 ns for legacy 16:9 / 16:9 / 21:9 / 32:9, i.e. noise,
which is what exposed the span placement.

### Criterion 1 (cost): met, narrowly

The 32:9 producer cost is 10.57% of the budget (threshold 10%); 16:9 is 2.08% (threshold 5%, not met). Re-running the whole matrix gave 10.57% twice (5 and 7 runs: 1,761,386 and
1,761,883 ns), so the figure is stable on this host; it is not stable across hosts and is a synthetic-scene measurement (a blank display ROM: the per-pixel lookup cost does
not depend on plane content, but cache behaviour on real tile data may differ).

### Criterion 2 (attribution): not met

`gprof` of the same build with `-pg` (`cc -pg` wrapper, 6,000,000 instructions, 32:9, 467 frames; call counts 99,586,816 plane lookups = 467 x 224 rows x 2 sides x 238 x 2 planes,
exactly the extension pixels):

| Part of `genesis_vdp_produce_extension` (self + children = 1.09 s) | seconds | share |
| --- | ---: | ---: |
| `genesis_vdp_plane_candidate_at` (the plane lookup) | 0.44 | **40.4%** |
| producer loop body (self) | 0.33 | 30.3% |
| `genesis_vdp_select_cram_index` (extension share) | 0.21 | 19.3% |
| `genesis_vdp_decode_cram_entry` | 0.06 | 5.5% |
| native row copy `genesis_vdp_frame_row_rgb888` | 0.04 | 3.7% |

The lookup is 40.4% of the producer, below the pre-registered 50%. `-pg` instrumentation adds a call-count hook to each of the ~10^8 tiny calls, which inflates the
callee's self time, so the true lookup share is, if anything, lower than this figure.

**Cross-check by ablation (not pre-registered; reported for transparency).** In a scratch copy outside the repository, the two lookups in the extension loop were replaced by
"transparent" and the same matrix re-run (7 runs): the 32:9 producer cost fell from 1,761,883 to 550,220 ns, i.e. the lookups account for **up to 69%** of the producer. This is an
upper bound (with constant inputs the compiler can also simplify the CRAM selection that follows), and it disagrees with gprof's 40%. The two methods bracket the lookup share
between about 40% and 69%; the pre-registered rule names gprof.

### Decision

By the pre-registered rule the optimization is **not warranted** (criterion 1 met by a hair, criterion 2 not met), so this task makes **no production change**: no lookup cache, no span
cache. The disagreement between the two attribution methods is recorded rather than resolved: a follow-up that wants to optimize should first obtain a function-level profile that does not
perturb ~10^8 tiny calls (hardware-counter sampling, which this host did not have) and should register a new criterion before coding. The measured dominant costs are:

- the lookup itself (between 40% and 69% of a producer that costs 10.57% of a 60 Hz frame at 32:9 on this host);
- `texture_upload` + side treatment, which grows with the surface (31 -> 251 microseconds per frame from no extension to 32:9, 1.5% of the budget at 32:9), not a bottleneck.

Nothing here changes the correctness evidence: `genesis_extension_test` compares every produced pixel to an independent reference at 101, 238, 266, 636, 668 and 672 pixels per side.
