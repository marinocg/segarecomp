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
| `full` | `z80_owner_group_differential_test` | grouped/shared emissions identical to the one-function-per-start reference on conformance scenarios and seeded random programs |
| `extended` | `z80_owner_group_differential_test_forms` | the same comparison over the complete legal-form matrix |
| `extended` | `sms_build_benchmark_test` | a real compile of a seeded 128 KiB image through the CLI: structural shape, per-process compiler RSS inside the ADR 0058 budget, executable size, the program starts; wall time only against a very generous ceiling |

No test compares a wall-clock time with a tight threshold; host variance makes that fragile. The structural invariants fail on a
regression to one function per start, which is the defect that made the build slow.

## Reference mode

`z80_image_emitter` accepts the spec verbs `group <n>` (entries per owner function) and `share <0|1>` (shared effect bodies);
`sms_image_emitter` accepts `--owner-group N` and `--share-bodies 0|1`. `group 1` with `share 0` is byte-identical to the
emission before SEG-033 and is the oracle of the differential gate. `SEGARECOMP_Z80_OWNER_GROUP=<n>` pins the group size of every
`z80_conformance.py` toolchain, so any existing Z80 test can be rerun under another bound.
