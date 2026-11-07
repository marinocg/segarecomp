# ADR 0274: SEG-126 bottleneck ranking and optimization pre-registration

- Status: Accepted (recorded before any optimization code; results are recorded in ADR 0275).
- Evidence: `docs/testing/performance-baseline-seg126.md` and `docs/testing/data/seg126/*.baseline.json` (product `main` at `132d64b`).
- Scope: which one measured opportunity SEG-126 implements, and the exact experiment that decides whether it stays.

## 1. What the measurements say

**Builds** (compile is 87-92% of wall in every scenario; the stages around it are single-threaded):

| finding | evidence |
|---|---|
| Compile volume dominates: generated M68K C is 89% of aggregate compile time in the default compatibility policy (167 MiB of C for a 512 KiB image); with the optimized policy it is 54% and generated Z80 C is 41%. | `compile.categories`, genesis_512k |
| Parallelism is already saturated: 7.6-7.9x on 8 jobs; 16 jobs buy 3-4%. | `effective parallelism`, `--jobs` scan |
| Z80 fixed-point iterations compile units that a later iteration supersedes: 12 of 31 Z80 units, 4.6% of compatibility job time and **14% of optimized-policy job time** (about 2-3 s wall of 18-29 s). | `wasted_ms`, `superseded` |
| Single-threaded front: M68K emission 3.9 s, Z80 materialization observation 1.3-2.6 s, link 0.9 s = 7-9% of a compatibility build and ~19-30% of an optimized one. | stage/operation exclusive times |
| Invariant sources (handwritten runtime, vendored YM2612/PSG, viewer unit) cost 3.7-4 s of aggregate time: ~1% of a compatibility build, 2% of an optimized one, but 50-60% of the 4 s synthetic build. | `compile.categories` |
| `-O1` instead of `-O2`: **GCC 13: -31..-38% build time** (uniform `-O1` costs 6-8% of viewer capacity because the handwritten runtime slows too; Z80 units at `-O1` cost nothing at runtime). **clang 18: -2.5%.** The packaged product compiles with `zig cc` (LLVM). | `--optimize` scan, clang row |

**Gameplay** (Genesis viewer, software renderer, unthrottled: 323% of native, 5.17 ms per frame; paced sessions already hold 100%):

| finding | evidence |
|---|---|
| `vdp_compose` is 2.17 ms of the 5.17 ms frame (42%); it sits inside `emulation` (4.25 ms). | SEG-125 spans |
| Instruction-level: the per-pixel plane/tile composition (`plane_candidate_at`, `fetch_cell_pixel`, `render_row`, `select_cram_index`) is 36% of all executed instructions, about 280 instructions per output pixel. The nametable entry is re-fetched, re-decoded and the scroll arithmetic repeated for every pixel although it changes once per 8-pixel cell. | callgrind (instruction counts, not time) |
| Per-frame frame-artifact SHA-256: 8.4% of instructions; consumed only by capture/evidence tools, not by the viewer. | callgrind |
| Per-pixel CRAM decode in the RGB conversion: 7.8% of instructions (the palette has 64 entries). | callgrind |
| Audio presentation: 0.001 ms per frame. Pacing: 100% of native, 1-2 missed deadlines at session start. | spans, `pacing` |
| Hardware renderers: `present` costs 1.6-3.0 ms per frame (software 0.53 ms); texture upload is 0.03-0.27 ms. GPU execution time is not observable. | `present`, `texture_upload` |
| Master System viewer: 13x native, 1.3 ms per frame. | SMS probe |

## 2. Ranking of the candidates

Scores: expected end-to-end improvement / evidence quality / complexity / regression risk / applicability. "e2e" is the measured or bounded estimate in this scenario.

| # | candidate | e2e | evidence | complexity | risk | applicability | verdict |
|---|---|---|---|---|---|---|---|
| 1 | **VDP row composer: fetch/decode the nametable entry and tile row once per cell run instead of per pixel** | est. -1.0..-1.5 ms of 5.17 ms (viewer capacity +20-30%) | measured span + instruction profile | low-moderate, one function | moderate, but bit-exactness is checkable against the independent per-pixel reference and existing VDP and frame suites | every Genesis game and Sega CD, viewer and frame capture | **selected** |
| 2 | Lazy frame-artifact digest | -0.4 ms (est. 8%) | instruction profile | low, but changes a published producer contract (`frame_digest` set on success; consumers in capture/evidence) | moderate (evidence tools) | all Genesis | follow-up |
| 3 | Decode CRAM once per row in the RGB conversion | -0.25 ms (est. 5%) | instruction profile | very low | very low | all Genesis | follow-up (could be a separate PR; not bundled: the study allows one optimization) |
| 4 | Z80 supersession waste (avoid compiling units a later iteration replaces) | -2 s of 60 s (3-4%) in compatibility, -2-3 s of 18-29 s (11%) in optimized | measured | moderate: changes the materialization fixed-point loop | moderate (build output equivalence) | Genesis builds with Z80 | later study |
| 5 | `-O1` for generated C | GCC: -31..-38%; clang/LLVM: -2.5% | measured, repeated | low as a flag, moderate as a per-category policy (the existing split covers only Z80 units) | low-moderate, a measured 5% M68K slowdown on GCC | GCC users and CI only; the packaged zig/clang product gains ~2.5% | **not selected**: the benefit does not reach the packaged compiler, and Zig could not be run here to prove otherwise |
| 6 | Overlap single-threaded emit/materialization with compilation | up to 7-19% wall | measured shares, no prototype | high: a second scheduling mechanism | moderate | all builds | rejected by the study's own constraints |
| 7 | Cross-build object cache for invariant sources | <= 1-2% of real builds; large only for a 4 s synthetic build; `zig cc` already caches | measured | high (identity, flags, invalidation, parallel safety) | high (stale object risk) | all | rejected |
| 8 | More compile jobs (default cap 8) | -3..-4% for +0.8 GB | measured | trivial | memory exhaustion on small hosts | all | rejected |
| 9 | Smaller generated C (emitter volume) | the real build lever (89% of compile) | measured shares | high, per-emitter | high | Genesis/SMS | future study; not low complexity |
| 10 | GPU compositing | the replaceable CPU work is 2.2 ms (compose) + 0.4 ms (RGB); the hardware `present` path already costs 1.6-3.0 ms and is not shown to be GPU-bound | measured cost, **no** GPU evidence | very high (a renderer reproducing sprite limits, priorities, mid-frame state, plus a CPU fallback) | high | viewer only | rejected: the evidence does not support it |
| 11 | Audio, pacing, texture upload | none: 0.001 ms / 100% paced / 0.03 ms | measured | - | - | - | nothing to optimize |

Notes on interpretation: overlapping spans were never added (`vdp_compose` is inside `emulation`, `texture_upload` inside `present`); callgrind shares are instruction counts and
rank *where code runs*, not how long it takes, and were used only to choose where to look; the time estimates for #1-#3 are derived from the measured `vdp_compose` and
`rgb_convert` spans, and are therefore estimates until the A/B run confirms them. The headline user-visible effect on this host is capacity, not smoothness: paced play already holds
100% here, so the benefit is headroom for slower hosts and lower CPU use, and the study says so rather than claiming a smoothness change.

## 3. Pre-registered experiment (frozen before implementation)

**Target.** `genesis_vdp_render_row` and the helpers it calls in `platforms/genesis/runtime/vdp_render.c`, the native-frame row composer used by `genesis_vdp_produce_frame` and
`genesis_vdp_produce_frame_raster` (viewer frames, frame capture, evidence frames, Sega CD).

**Minimal change.** Inside the per-pixel loop, keep the per-plane nametable entry of the current plane cell (and the vertical-scroll group) and re-fetch it only when the
plane column or group changes; fetch the 4 pattern bytes of the current tile row once per cell instead of one byte per pixel; hoist the loop-invariant window/left-blank tests.
The sprite row builder and the priority selection are unchanged. No new data structure outside the function; no change to any header.

**Reference preserved.** The per-pixel path stays as the unmodified `genesis_vdp_compose_pixel` / `genesis_vdp_plane_candidate_at` pair (used by the extension and the public API) and
becomes the oracle of a new randomized differential test: the new row composer must be byte-identical to a per-pixel composition for random registers (all plane sizes, all
horizontal/vertical scroll modes, window, left-blank, H32/H40, interlace mode), random VRAM/VSRAM/CRAM and random sprite lists.

**Expected benefit.** `vdp_compose` -35% or better (2.17 -> <= 1.4 ms) and viewer unthrottled speed +10% or better (323% -> >= 355%). Headless runs, SMS and Sega CD main loops
are unchanged by construction (headless composes no frames without capture).

**Affected paths.** Genesis viewer; Genesis frame capture/checkpoint evidence frames; Sega CD (shares the Genesis VDP renderer). Not affected: the Level 3 extension producer,
the SDK compat host table, audio, CPU/device scheduling, the build.

**Risks.** Wrong wrap/flip/scroll-group boundary handling producing a pixel error that no single fixture covers (mitigated by the randomized differential test and the existing
12 VDP suites); a hidden dependence on per-pixel evaluation order (none exists: every pixel reads VRAM, never writes it).

**Procedure (identical for baseline and candidate).** Same CLI binary; baseline programs built with `--runtime-dir` of the unmodified `main` checkout, candidate programs with the
task worktree's runtime; `genesis_512k` plus two further local Genesis images (different mappers/screen modes where available); `tools/perf_baseline_bench.py runtime`
(software renderer unthrottled and paced, OpenGL unthrottled, headless as a no-effect control), 5 interleaved reps, then 5 more if any CV exceeds 3%; the same `--instruction-budget`.
Equivalence: the SEG-125 on/off tests plus a frame-digest stream and audio digest comparison between baseline and candidate programs for each image over the full budget, the identical
`runner_resource_limit` stop class and dispatch count, and the existing independent VDP validation suites. Linux/macOS/Windows CI jobs must pass.

**Acceptance (all required).**
1. Frame digest stream, audio digest, stop class and dispatch count byte-identical on every image; differential test and all existing VDP suites pass; CI green on Linux, macOS, Windows.
2. `vdp_compose` median per-frame time reduced by >= 25% (the 35% expectation is not the bar), and the viewer unthrottled speed improved by >= 8%, each larger than three times the
   larger of the two CVs.
3. No regression: paced speed stays 100% with missed deadlines <= baseline + 1; headless unchanged within noise; executable size change < 1%.

**Rollback.** Any digest or stop-class difference, a failing existing suite, or `vdp_compose` improvement < 10% or viewer speed gain < 3% => the candidate is reverted, the study
and measurements are delivered with an explicit no-go.

**Not done (explicitly out of scope of this experiment).** Lazy digest, CRAM-per-row decode, `-O1` policy, Z80 supersession, build scheduling, caching, GPU work. They are recorded
as future opportunities with the evidence above.
