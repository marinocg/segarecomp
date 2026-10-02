# Z80 / Master System production build performance (SEG-033)

The host C compiler dominates the cost of `segarecomp build` for a Master System image (96-98% of wall time before SEG-033).
ADR 0071 records the structural fix (bounded multi-entry owners, shared effect bodies) and the build policy; this page says how
the cost is measured and what protects it.

## Measuring: `tools/sms_build_benchmark.py`

Runs the real consumer route (`segarecomp build`: analyze, generate, compile with the CLI's own flags, link) on one image and
reports generation/compile/link wall time, compiler CPU, per-process and aggregate compiler RSS (sampled process tree),
generated C bytes and files, host functions (owners + shared bodies), exact entries, unique owners, translation-unit size
distribution, executable size, startup and generated-native throughput (median of repeated runs at a fixed T-state budget) and
the final machine-state digest. Image shapes: `random:<KiB>`, `dense:<KiB>` (back-to-back legal instructions) and
`rom:<path>` for an authorized local image (bytes and names are never printed or recorded). `--platform genesis` measures a build only.

```sh
python3 tools/sms_build_benchmark.py --cli build/dev/apps/segarecomp/segarecomp --root . --image random:512 [--optimize 2] [--jobs 8] --json out.json
```

Wall times on a shared workstation vary by about 15%; compare CPU seconds and use repeated runs. Aggregates only go into evidence.

## What protects it

| tier | test | checks |
|---|---|---|
| `full` | `sms_emission_budget_test` (128 KiB) and `_512k_test` | exact entry count, owner count and grouping ratio, bound of 128 entries per owner, host function count, largest TU and total generated C, determinism, byte identity with the generic Z80 emitter |
| `full` | `z80_owner_group_differential_test` (+ `_slice2..4`, disjoint batch slices) | grouped/shared emissions identical to the one-function-per-start reference on conformance scenarios and seeded random programs |
| `extended` | `z80_owner_group_differential_test_forms` | the same comparison over the complete legal-form matrix |
| `extended` | `sms_build_benchmark_test` | a real compile of a seeded 128 KiB image through the CLI: structural shape, per-process compiler RSS inside the ADR 0058 budget, executable size, the program starts; wall time only against a very generous ceiling |

No test compares a wall-clock time with a tight threshold; host variance makes that fragile. The structural invariants fail on a
regression to one function per start, which is the defect that made the build slow.

## Reference mode

`z80_image_emitter` accepts the spec verbs `group <n>` (entries per owner function) and `share <0|1>` (shared effect bodies);
`sms_image_emitter` accepts `--owner-group N` and `--share-bodies 0|1`. `group 1` with `share 0` is byte-identical to the
emission before SEG-033 and is the oracle of the differential gate. `SEGARECOMP_Z80_OWNER_GROUP=<n>` pins the group size of every
`z80_conformance.py` toolchain, so any existing Z80 test can be rerun under another bound.

## Genesis: build-time Z80 materialization (SEG-032-T008)

`segarecomp build` of a Genesis image adds the materialization stage (ADR 0073, T008 record): one pass-program link and one bounded headless run
per discovered image plus the completing run and one confirming run, the Z80 C of the registry compiled with the same flags as the Master System
build, and the vendored ymfm objects (once per build). `status.json` carries the sanitized aggregates (`z80`: images, epochs, discovery runs,
runs, frames reached, units, units compiled/reused, generated/object/executable bytes, emit/compile/materialize milliseconds); no title, ROM
byte, address or image hash is recorded. Authorized-workload reference (default `-O2`, cold, one workstation): 2 images, 3-5 epochs, 4 runs,
materialization stage 11-18 s of a 60-115 s build, 10.2-10.7 MB of generated Z80 C, 3.8-4.1 MB of Z80 objects, 34-51 MB executable.
Protection: `genesis_z80_materialization_test` (the loop against scripted runners), `genesis_z80_build_pipeline_test` and its `_bound`/`_smc`/`_falsify`/`_prepare` siblings (case groups of one script; the real route with
project-authored ROMs: exact counts, determinism across repeats and worker counts, typed failures, removal/mutation falsification) and
`genesis_z80_forbidden_identifiers_test`.
