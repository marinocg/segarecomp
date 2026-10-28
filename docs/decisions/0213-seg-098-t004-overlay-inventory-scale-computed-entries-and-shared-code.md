# ADR 0213: SEG-098-T004 - overlay-inventory scale, computed entries and shared code

- Status: Accepted (pre-registration, measurements, decisions). Related: ADR 0140 (a real-title inventory of 36 modules and 18 copy programs at one Work RAM address exceeded the then bounds), 0206 decision 5 (executable subrange), 0207/0209 (source-relative bodies), 0211 (static admission), 0212.
- Privacy: project-authored fixtures; no commercial data.

## Pre-registered questions

1. **Scale.** What does an N-module inventory cost in the supported BIOS-image build, as a function of module bytes and overlay depth, and which bounds (64-slot disc-image limit shared by
   static and resolved extents: 16; static admission: 12; scan operations: 64; scan modules: 32) can be raised with measured, deterministic, typed-exhaustion behaviour?
2. **Computed entries.** When a proven module's code is reached through a computed transfer whose destination lies beyond the traversed executable hull, what happens, how is the stop
   classified, and is broad compilation of every loaded byte a sound baseline?
3. **Shared code.** Does the BIOS partitioned emitter need source-relative bodies for this inventory shape? Require measured benefit and behavioural equivalence before enabling.

Measurement protocol (fixed before the runs): `tools/sega_cd_scale_measure.py` over the `many` fixture (N distinct ~1.6 KiB modules of ~400 seeded `MOVE.W #imm,D1` instructions, reached by constant
selectors through the offset-table dispatcher; 4 bases or 1 base = overlay depth N/4 or N); a clean object cache per size; generated-C bytes, wall time of the whole multi-pass build (4 rounds),
spot-run of the first/middle/last module. (An earlier run with identical NOP bodies showed no growth: identical bytes at one address share one compiled variant - a property of the emitter, not
of the inventory; the fixture therefore uses distinct bodies.)

## Measurements (generated C and wall time, 16-job host; the module-free baseline is about 10.1 MB of C)

| N modules | C bytes (4 bases) | build s (4 bases) | C bytes (1 base) | build s (1 base) |
| --- | --- | --- | --- | --- |
| 3 | 12.3 MB | 11 | 12.5 MB | 11 |
| 12 | 19.8 MB | 12 | 19.2 MB | 16 |
| 24 | 28.7 MB | 15 | 28.2 MB | 28 |
| 36 | 38.8 MB | 18 | 38.2 MB | 42 |
| 48 | 47.8 MB | 23 | 47.2 MB | 58 |

Reading: generated C grows linearly with admitted code bytes (~0.8 MB per 1.6 KiB module, i.e. about 2 KB of C per guest instruction) and does not depend on overlay depth; build time grows
mildly with bytes and faster with depth at one base (the per-pc variant choice), 58 s for 48 modules stacked on one base. All spot checks ran correctly. The bounds, not the cost, were the limit.

Reproduce: `python3 tools/sega_cd_scale_measure.py --segarecomp <cli> --cc <cc> --root <repo> --work <dir> --nops 400 --groups 4 --jobs 16 3 6 12 24 36 48` (and `--groups 1` for the one-base column); `--nops` is the instruction count per module.

## Decisions

1. **Bounds** (measured, deterministic, typed exhaustion kept): disc-image slots 16 -> **64**; static admission 12 -> **48** (the rest of the slots stay for evidence-resolved extents);
   resolved extents 16 -> **64**; scan load operations 64 -> **128**; scan modules **64** (`kLoaderScanMaxModules`). The synthetic module-list bound (32) is unchanged: a scan with more than 32
   modules proposes them but writes no list text. One module over the static bound is `static_extent_bound_exceeded` and admits none (tested at 49).
2. **Computed entries.** Pre-registered fixture `computed_entry`: B's second routine is reached only by `LEA`-loaded `JMP (A0)`. Result: with the default `hull` static range the routine is not
   compiled: the build pass (and the run) stops typed `entry_not_compiled`, executing nothing wrong; with `--static-range loaded` (every loaded byte of a proven module, all decodable aligned
   starts) it runs. Both are sound - the executed bytes are the disc's bytes, guarded per instruction either way. **The default stays `hull`**: ADR 0206 decision 5 (header and trailing bytes are
   never executable authority) and the T002 contract are unchanged; `loaded` is an explicit option (`segarecomp sega-cd build --static-range loaded`). Whether Compatibility should default to
   `loaded` is an open policy decision (cost: at most one sector per module; benefit: computed destinations inside loaded modules) recorded for T005/the operator, not taken silently.
3. **Frontier classification.** `status.json` gains `final_stop` {class, cpu, location} with location `inside_admitted_extent` (the stopping instruction is admitted code: for
   `entry_not_compiled` a computed destination whose start is not compiled; for `code_bytes_mismatch` changed live bytes), `inside_static_module_window` (the pc lies in a proven module's loaded window outside its admitted range) or `outside_admitted_images` (an unknown source or a not yet
   admitted overlay). For the computed-transfer stop of the fixture the runtime's `address` field was 0 and `pc` was the transfer instruction (the destination is not reported there), so "destination inside the loaded window but outside the range" cannot be
   told apart from "destination elsewhere" without a runtime change; this is stated, not guessed.
4. **Shared code (source-relative bodies in the BIOS emitter): not enabled.** The measured cost is bytes-proportional and depth-independent; the only inventory shape that source-relative
   bodies help (one source image at several bases) occurs once in the fixture (C at two bases) and is not what the authentic route showed (4 overlay groups of *distinct* sources). The fixture uses distinct sources by construction, so it cannot show a benefit; the decision rests on the cost model (bytes-proportional) and on the authentic overlay groups being distinct modules, as ADR 0210's survey counted them (4 groups with more than one alternative; not whether sources repeat across bases - that was not measured). With no
   measured benefit on a real inventory shape there is no reason to put the PI mechanism into the partitioned emitter; the question reopens if a survey finds multi-base sources.

## Not claimed

A real title's inventory beyond what static analysis proves (ADR 0210/0212: 0 on the authentic disc); a benefit of the larger bounds on a commercial title; hardware or audio coverage.

## Review notes

- `discover-modules` reports `module_list_omitted` when modules exist but the list text is not written (more than 32); `--out` is removed in that case, never left stale. The Python build driver's extent bound follows the C++ one (64).
- `loaded` extents of a window near the end of sub RAM can fail the emit with `disc_image_invalid` (fail closed, untyped for that case).
