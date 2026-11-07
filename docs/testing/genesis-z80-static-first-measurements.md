# Genesis Z80 static-first image production: measurements (ADR 0277)

Baseline: product `main` 5547e8d (post SEG-127, stable Z80 unit layout and object cache). Same binary, same host (Ryzen 7 5800X class, WSL2, GCC 13),
legacy (`--z80-static 0`) and static-first (`--z80-static 1`) builds alternated. Sizes, counts, milliseconds only: no byte, offset, address or hash of
any image. Locally held authorized images; nothing here is required by or visible to CI. Raw aggregates: `docs/testing/data/seg128/aggregates.json`.
Timings are kept separate from correctness: *Output* is the correctness column (generated Z80 C + registry and the linked program compared byte for byte).

Flags: all rows except the first use `--optimize 0 --runtime-optimize 1` (the CTest flags); the first row uses the default (`-O2`), median of 3 interleaved
pairs (legacy walls 83.3-84.3 s, static 81.5-83.9 s overlap: the wall difference is within the noise, the materialization difference is not).

## Differential builds

| image | static outcome | images proven of final | fallback | Z80 generations | discovery runs | verification runs | units compiled / reused | Z80 C bytes | Z80 compile ms | materialization ms | build wall s | program bytes | output | static ms | refusals |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Sonic 1 (default -O2; 3 interleaved pairs) | complete | 2 of 2 | none | 3 -> 1 | 3 -> 1 | 1 -> 1 | 23 / 8 -> 19 / 0 | 11,408,660 | 7462 -> 7354 | 11416 -> 10499 | 84.25 -> 82.35 | 38,571,592 | identical | 9 | - |
| Sonic 1 (-O0, runtime -O1) | complete | 2 of 2 | none | 3 -> 1 | 3 -> 1 | 1 -> 1 | 23 / 8 -> 19 / 0 | 11,408,660 | 1788 -> 1605 | 6013 -> 5062 | 52.93 -> 51.29 | 40,538,216 | identical | 9 | - |
| Sonic 2 | partial | 1 of 2 | none | 3 -> 2 | 3 -> 2 | 1 -> 1 | 23 / 8 -> 22 / 8 | 11,202,102 | 1565 -> 1700 | 5830 -> 5751 | 88.45 -> 88.58 | 65,356,600 | identical | 8 | decompression_unproven=1 |
| Streets of Rage | complete | 2 of 2 | none | 3 -> 1 | 3 -> 1 | 1 -> 1 | 23 / 8 -> 19 / 0 | 11,583,369 | 1767 -> 1670 | 5878 -> 5118 | 48.92 -> 48.94 | 37,466,296 | identical | 37 | - |
| Streets of Rage 2 | partial | 2 of 2 | none | 3 -> 1 | 3 -> 1 | 1 -> 1 | 23 / 8 -> 19 / 0 | 11,686,768 | 1744 -> 1603 | 6374 -> 5010 | 121.33 -> 121.01 | 86,880,312 | identical | 10 | window_control_unknown=1 |
| The Lion King | complete | 1 of 3 | sound_fault_requires_observed_registry | 4 -> 7 | 4 -> 4 | 1 -> 1 | 34 / 24 -> 34 / 81 | 16,541,771 | 2496 -> 2442 | 8018 -> 12841 | 175.68 -> 180.72 | 121,514,064 | identical | 15 | - |
| Thunder Force IV | unsupported | 0 of 0 | none | 1 -> 1 | 1 -> 1 | 1 -> 1 | 1 / 0 -> 1 / 0 | 659 | 13 -> 20 | 2325 -> 2340 | 81.06 -> 80.54 | 56,769,848 | identical | 104 | ambiguous_epoch_order=1, window_control_unknown=2 |
| Sonic 3 | partial | 1 of 2 | none | 3 -> 2 | 3 -> 2 | 1 -> 1 | 23 / 8 -> 22 / 8 | 11,547,863 | 1642 -> 1657 | 6743 -> 6307 | 144.51 -> 144.81 | 100,949,312 | identical | 20 | window_control_unknown=1 |
| Phantasy Star IV | partial | 1 of 2 | none | 3 -> 2 | 3 -> 2 | 1 -> 1 | 23 / 8 -> 22 / 8 | 10,713,148 | 1565 -> 1544 | 6885 -> 6160 | 272.44 -> 273.09 | 172,804,912 | identical | 2 | window_control_unknown=1 |
| OutRun | partial | 2 of 2 | none | 3 -> 1 | 3 -> 1 | 1 -> 1 | 23 / 8 -> 19 / 0 | 11,324,063 | 1677 -> 1572 | 5843 -> 4888 | 64.28 -> 63.41 | 48,001,344 | identical | 11 | decompression_unproven=1 |
| Mega Man: The Wily Wars | complete | 2 of 2 | none | 3 -> 1 | 3 -> 1 | 1 -> 1 | 23 / 8 -> 19 / 0 | 11,349,257 | 1587 -> 1585 | 6474 -> 5280 | 133.7 -> 132.79 | 88,314,504 | identical | 14 | - |

Reading it:

* **Fully proven (Sonic 1, Streets of Rage, Mega Man; Streets of Rage 2 and OutRun prove all images but log a refused construct on a side path):**
  one Z80 compilation generation instead of three, one discovery run instead of three, the same single verification run, no reused unit (everything compiled
  once), identical Z80 C, registry and program. Sonic 1's driver comes through the Kosinski call; Streets of Rage's through the two-stage loader (Kosinski into
  a work-RAM buffer, then a counted copy); the others through constant counted ROM copies.
* **Partially proven (Sonic 2, Sonic 3, Phantasy Star IV):** the boot stub is proven, the driver loader is not recognised (`decompression_unproven`) or sits behind
  a branch the walk cannot decide while the 68K owns the bus (`window_control_unknown`); the unchanged fixed point discovers the rest: 3 -> 2 generations, identical output.
* **Contradicted prediction (The Lion King):** this title isolates its sound CPU in the observation window (a structural Z80 mutation). Such a run cannot
  be verified against a static snapshot, so the build discards the static registry and runs the legacy fixed point (`sound_fault_requires_observed_registry`).
  The output is identical to legacy; the cost is honest and visible: 4 -> 7 generations, +5 s wall. **Static-first can cost more than it saves on such titles.**
* **Nothing proven (Thunder Force IV):** `unsupported` (divergent epoch orders), no static work reused, the legacy flow unchanged.
* **Wall time.** The saving is the avoided discovery programs and their emit/compile/link generations: materialization -0.8 to -1.4 s on the fully proven
  rows (-8% at `-O2` for Sonic 1: 11.4 -> 10.5 s), total wall -0.3 to -2%, in several rows inside run-to-run noise. The producer itself costs 2-104 ms. The
  remaining build time is the M68K C compile and the **M68K alias-preparation rounds, which this change neither removes nor shortens** (they re-run the fixed
  point with the same registry; with a static registry each such round still needs only its verification run, as before).
* Z80 compile milliseconds are unchanged because SEG-127 already eliminated the superseded object work: the same units are compiled once either way.

## Producer census over every locally held image (58 images <= 8 MiB, producer only, no build)

Outcomes: complete 7, partial 47, none 3, unsupported 1. Images proven: 0 for 4 images, 1 for 47, 2 for 7 (the boot stub alone for most titles).
Refusal classes (number of images showing it): decompression_unproven 17, window_control_unknown 12, length_unknown 9, state_bound 6,
z80_store_value_unknown 3, z80_store_form_unsupported 2, z80_store_unknown_address 2, ambiguous_epoch_order 1. Walk size: at most 19,188 steps;
per image median 82 ms, maximum 224 ms including process start and a ROM read (the producer alone is 2-104 ms inside a build).

## Synthetic fixtures (CI): 23 project-authored fixtures, legacy vs static

Every fixture's generated Z80 C, registry and linked program are byte-identical and the bounded-run sound/execution summary is identical, including
the fallbacks (never-activated image, region-gated upload, structural mutation) and the refused shapes. Fully provable fixtures: exactly one Z80
compilation generation, one discovery run, one verification run, no reused unit. Repeated builds with 1/4/8/4 jobs are byte-identical.

## Default enablement

Not enabled by default. The feature is correct on every comparison above and on the fixtures, but the wall-time benefit is small (about 1-2% of a
build, inside noise on several rows), a contradicted prediction costs extra work, and the default build would pay the producer's ~10-100 ms on every
Genesis build. It stays an explicit opt-in (`--z80-static 1`) until a wider qualification justifies more.
