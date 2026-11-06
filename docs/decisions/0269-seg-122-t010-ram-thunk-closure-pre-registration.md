# ADR 0269: SEG-122-T010 selective Optimized AOT for RAM-thunk Genesis cartridges - pre-registration

- Status: **Pre-registration, frozen.** Committed and pushed on the SEG-122-T010 branch before any candidate build or measurement on a commercial image.
  Results and the decision go in ADR 0270. This record is not edited afterwards, except to fix typos that don't change meaning.
- Scope: evaluates whether the existing report-only `close_genesis_region_admission` (ADR 0156) may keep a thunk-bearing Genesis Optimized build selective. It changes
  nothing by itself. Production behaviour changes only if ADR 0270 records `PROMOTE`.
- Related: ADR 0049 (alias descriptors, runtime guards), 0080 (validator), 0094 (prune), 0097 amendment (RAM thunks), 0103 (gen2 route), 0110 (noise floor), 0111 and 0114
  (alias rule, RAM thunks left fail-closed pending a separate pre-registered evaluation), 0156 and 0157 (closure as a diagnostic), 0268 (lock-on declaration).

## Question

The first emission of a thunk-bearing build is admitted. Preparation then materializes a work-RAM thunk, the prune rejects the next emission, and the program is broad.
Does applying the existing closure to the same proposal give a substantially cheaper program that behaves the same on measured routes, without weakening any fail-closed rule?

ADR 0114's recorded follow-up names a different mechanism (`O3`: mandatory identities as members of K0, then a downward prune). This evaluation is the upward closure. It does
not answer `O3`, and `O3` is not an arm.

## Frozen evaluation (reproduced unchanged from the task record, merged to the private backlog before this task started)

**Workloads** (local authorized inputs, gen2 `--aot-policy optimized --aot-model gen2`, launcher-equivalent `segarecomp build`). Targets: T1 standalone S&K (`--mapper lock_on`); T2 combined S3&K (`--mapper lock_on`); T3 combined S3&K with the native patch (`--patch-package`/`--patch-identity`, harness `exports/segapatch/sonic3k-native-widescreen.segapatch`). Controls: Sonic 3, Golden Axe, Flicky.

**Arms.** `BROAD`: `--aot-policy compatibility`. `INC`: Optimized on product `main` as deployed, including its fallback. `CLOSE`: the same binary plus only this change: for a gen2 emission, run the unchanged producer and prune; if the prune accepts, nothing changes; if it rejects and the emission's universe holds at least one materialized RAM-thunk identity, call `close_genesis_region_admission` on the same proposal with the default cap and pass the plan to the unchanged validator; any failure is the broad fallback. No retraining, region-budget change, seed change or second closure algorithm. Downward-prune variants (ADR 0111 `O3`) are not arms.

**Cost measurement.** End-to-end `segarecomp build` including every preparation round, one image at a time on an otherwise idle host, same compiler, flags and `--jobs`, fresh work and object-cache directories, total CPU (user + system, children included), reusing the timing approach of `tools/segarecomp_oracle_bench.py`. Three cold builds per arm and workload, compared by median, because ADR 0110 measured about +/-25 % single-sample CPU noise. Generated C bytes come from the final emission. `segarecomp_oracle_bench.py` strategies are not a substitute: they do not pass `--genesis-lock-on` or run preparation.

**Behaviour measurement.** Bounded generated-native runs with an explicit finite `--instruction-budget` and wall timeout, on one route per workload fixed before the first candidate run, identical across arms: T2/T3 the SEG-122-T005 pad route and 9000-frame capture point (11301 virtual frames); T1 a route reaching standalone gameplay with the T005 tooling (harness `tools/seg122_t005_s3k`, `sk` image; no new gameplay harness); controls the longest existing route, or a bounded input-less run. Compare runtime stop classification, instruction and virtual-frame progress, final architectural state where observable, frame and audio digests, captures, and unadmitted-PC events.

**Gates; all must pass for `PROMOTE`.**

- **G1 reproduction:** `INC` on T1 and T2 reports `prune_rejected` / `outside_region` / `proposal_rejected` with at least one thunk, and the cause is explained from code and sanitized reports. Failure: stop, record the difference, `RETAIN_BROAD`.
- **G2 structure:** on T1, T2 and T3 `CLOSE` reports validator accepted, `effective=ml_region_gen2`, no fallback, no cap exhaustion. The proposal digest equals `INC`'s for the same emission inputs. Preparation termination is complete, and the alias and thunk sets equal those of `BROAD`'s preparation; a selective preparation that discovers less fails.
- **G3 economics:** on each of T1 and T2, `CLOSE` is at least 20 % below `BROAD` in both generated C bytes and median cold build CPU. Its peak compiler RSS and executable bytes do not exceed `BROAD` by more than 10 %. T3 is reported and subject to G2, G4 and G5. Early stop: K/U of at least 0.90 on T1 or T2 ends the evaluation as `RETAIN_BROAD` without compiling further.
- **G4 reproducibility:** two independent `CLOSE` builds of each target give identical `k_sha256` and generated C digests, and the same route run twice gives identical digests.
- **G5 behaviour:** on T1, T2 and T3, `CLOSE` equals `BROAD` of the same image and patch state on every compared field, with zero unadmitted-PC events. Any difference is diagnosed; an unexplained difference or a stop on code that broad compiled fails the gate.
- **G6 controls:** Sonic 3 and Golden Axe keep the `INC` `k_sha256` and generated C digest (candidate path not taken) and an equal route. Flicky either stays broad, or, if selectively admitted, passes the same conditions as G2, G4 and G5 applied to Flicky (its own `BROAD` and route as the reference) with median cold CPU at most 110 % of `INC`. Record against ADR 0111's Flicky figures (14 thunks, mixed code/data copy, exploratory 52.2 %).

Decision: all gates pass gives `PROMOTE`; anything else gives `RETAIN_BROAD`. Thresholds are not revised after results. A passing short route is evidence for that route only, not game compatibility or dynamic-target completeness.

## Measurement detail (additions only; no gate is changed)

- **Compiler and flags:** `--cc /usr/bin/cc` (GCC 13.3), `--optimize 1 --jobs 4`, the settings of the existing lock-on build tooling; the packaged launcher's toolchain is not installed on this
  host, so that is a stated limit. Every arm and workload uses the same settings, one build at a time.
- **Binaries:** `INC` is the Release CLI of the task's base commit plus a measurement-only patch that prints one sanitized extra stderr line (proposal digest and identity counts) and changes nothing else. `CLOSE` is that binary plus the frozen trigger. The two patches' SHA-256 values are recorded in ADR 0270.
- **CPU:** user + system from `wait4` on `segarecomp build`, which includes its waited children.
- **Peak compiler RSS:** per-child `ru_maxrss` from a minimal `--cc` wrapper used identically in every arm.
- **Generated C:** bytes and SHA-256 of the final emission's units.
- **Routes:** every workload uses one pad script, the T005 run route (Start presses, then running; ends with its quit event), frame exact (`SEGARECOMP_VIEWER_SLICE=1`), with `--instruction-budget 4294967295` and a wall-clock timeout, and frame captures at fixed polls. T1 and the controls use the same script. Where it does not reach gameplay on a title, that is recorded and the run is an evidence-limited bounded route for that title only.
- **Control images:** Sonic 3 is the local exact 2 MiB image; Golden Axe and Flicky are the local images. Identities are recorded as SHA-256 only.
- **Order:** `INC` and `BROAD` first, then `CLOSE`. The K/U early stop is read from the first `CLOSE` build of each of T1 and T2.
- **Privacy:** durable results hold aggregates, classes and digests only. Work directories, plans, proposals, generated C, captures and logs stay in ignored paths.
