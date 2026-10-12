# ADR 0114: SEG-051 Z80 executable-region ranking experiment (SMS pilot) and decision record

- Status: Accepted. Protocol frozen in commit `e7a7657` (before any model was fitted); results and decisions below. Outcome: Genesis `Z80_BROAD_AOT_CHEAP_ENOUGH`; SMS `Z80_RECALL_FAILED` as a strict-recall exploratory pilot; native integration and a Z80 Optimized mode `NOT_JUSTIFIED`.
- Related: ADR 0058/0071/0073 (Z80 AOT, owners, Genesis materialization), 0100-0103/0107/0109/0113 (M68K executable-region ML), `docs/architecture/executable-image-contract.md`.

## Context

SEG-049/050 showed a learned region ranking can reduce M68K generated-native compile cost. SEG-051 asks the same question for the Z80
without reusing M68K weights, encodings or the B35 budget. Two platforms are decided independently: Genesis (Z80 sound drivers materialized
at build time, ADR 0073) and the Master System (the whole program is Z80).

## Corpus qualification (aggregates only; truth stays private)

A lineage qualifies only if (1) an image is reproducibly built from pinned public assembler source, (2) the linked assembler listing
yields source-backed instruction starts, and (3) the product Z80 decoder (`z80-decode-token-report`, i.e. `decode_at`) agrees with the
listing on every instruction length (a 4 KiB block with any disagreement is quarantined). `.db` is **ambiguous** (hand disassemblies place real
code in `.db`), never a negative; only `.dw/.incbin/.dsb/.asc/.fill` are confident data. Unlisted and ambiguous bytes are unknown.

Nine independent SMS lineages qualify (pairwise 12-byte instruction-run overlap at most 3.3% of the smaller set): Alex Kidd, Columns,
Super Tennis, Sonic 1 (code from source, bulk data unlisted), Knights of the Square, 2048-SMS, Phantasy Star, Lord of the Sword, Solomon's Key.
Exact local-cartridge identity: Alex Kidd, Columns, Super Tennis, Sonic 1. Source-built reference images: the other five (see the plan for
identity caveats, notably Lord of the Sword's 194-byte divergence between assembler versions). Revisions of one game, Sonic 1 reconstructions
and Sonic 2 (different revision from the local cartridge, same engine family) are not additional lineages. Genesis has no source-backed
Z80 corpus of comparable size: its Z80 images are a handful of small drivers (see economics) and it is decided on cost, not truth.

Split (fixed by `sha256("seg051-sealed-v1:"+id)` order before any model exists): 6 development lineages, 3 sealed lineages. **The adoption
minimum (8 development + 3 sealed) is not met**, so this is an exploratory pilot; native integration and a Z80 Optimized mode are
`NOT_JUSTIFIED` irrespective of the pilot result.

## Protocol (frozen: `tools/segarecomp_ml_region_z80.plan.json`, digest `tools/segarecomp_ml_region_z80.plan.digest`)

32-byte aligned decision cells with 32 bytes of context per side; positive = cell holds a source instruction start, negative = all bytes
confident data, else masked. Candidates C1 (raw-byte dilated CNN) and C2 (C1 + Z80 decode tokens kind/length/form); controls: random order and a
byte-histogram logistic regression. Leave-one-lineage-out over the development lineages gives out-of-fold FRF; the fixed budget is
`ceil_0.05(max OOF FRF + 0.05)`; the adaptive budget is `min(1, max(0.2, m * fraction(p>=0.5) + 0.05))` with `m` the smallest multiple covering all
development OOF FRFs. The selected candidate is fitted once on all development lineages and the sealed lineages are scored exactly once.
Any uncovered source instruction-start cell is a full-recall failure.

## Z80 admission seam finding (T004 prerequisite)

`codegen::z80::CodeImage` carries bytes and windows only; the broad AOT emits every legal start. There is no admitted-start set, no typed
unadmitted-PC behavior and no validator contract on the Z80 side, so a selective producer is not a producer insertion: it needs a new Z80
codegen/runtime/validator contract. That is outside SEG-051 and would be justified only after the adoption evidence exists.

## Baselines (measured; `segarecomp build`, `-O2`, 8 jobs, one 16-thread host, cold; medians of 3 (Genesis Golden Axe 2))

| Platform | Image | Build CPU (s) | Wall (s) | Generated C | Z80 share |
| --- | --- | --- | --- | --- | --- |
| SMS | 32 KiB | 80 | 11 | 13 MB | 100% (all code is Z80) |
| SMS | 128 KiB | 314-322 | 42 | 48-52 MB | 100% |
| SMS | 256 KiB | 592-634 | 77-83 | 93-96 MB | 100% |
| SMS | 512 KiB | 1259 | 165 | 187 MB | 100% |
| Genesis | Sonic 1 | 865 (778-906) | 129 | ~178 MB | Z80 C 11.4 MB (6.4%); Z80 compile 12.5 s wall = 9.7% of wall |
| Genesis | Streets of Rage | 711 (697-713) | 108 | ~163 MB | Z80 C 11.6 MB (7.1%); 12.6 s = 11.6% |
| Genesis | Golden Axe | 1214 | 187 | ~163 MB | Z80 C 11.2 MB (6.9%); 10.5 s = 5.6% |

SMS build CPU is linear in ROM size (`-3.0 + 2.441 s/KiB`, R2 0.998; run-to-run spread up to ~25% when other jobs shared the host, <3% otherwise).
Genesis materializes 2 Z80 images per workload (3-4 epochs): Z80 is 6-7% of generated C and at most ~12% of wall, so even eliminating
**all** Z80 compilation cannot reach the 20% gate: Genesis is `Z80_BROAD_AOT_CHEAP_ENOUGH` and has no CNN track. The SMS opportunity is real: the whole
build is Z80 and costs 2.4 CPU-seconds per KiB.

## Pilot results (SMS, 6 development lineages, leave-one-lineage-out; FRF = fraction of ranked cells needed to cover every source instruction-start cell)

| Held-out lineage | cells | positive cells | C1 FRF | C2 FRF | histogram-LR | random |
| --- | --- | --- | --- | --- | --- | --- |
| 2048-SMS | 1024 | 88 | 0.105 | 0.105 | 0.440 | 0.996 |
| Knights of the Square | 4096 | 741 | 0.222 | 0.225 | 0.552 | 0.997 |
| Columns | 4096 | 431 | 0.245 | 0.366 | 0.367 | 0.997 |
| Solomon's Key | 4096 | 663 | 0.720 | 0.644 | 0.783 | 0.996 |
| Phantasy Star | 16384 | 966 | 0.977 | 0.976 | 0.994 | 0.997 |
| Super Tennis | 1024 | 429 | 0.990 | 0.985 | 0.991 | 0.999 |
| mean / median | | | 0.543 / 0.720 | 0.550 / 0.644 | 0.688 / 0.783 | 0.997 |

- Selection rule: C2 would replace C1 only with a 0.05 lower mean FRF; it did not (0.550 vs 0.543), so **C1 (raw bytes) is selected; the Z80 decode tokens add nothing measurable.**
- The CNN beats the context-free byte-histogram control on every fold and the random control by a wide margin: the ranking signal is real.
- Gates: median development FRF 0.72 > 0.60 **FAILED**; the fixed budget calibrates to `ceil_0.05(0.99+0.05) = 1.0` (no selectivity, 0% projected saving); the adaptive rule is **not calibratable** (no multiplier up to 4.0 covers the worst fold).
- The sealed cohort was **not evaluated** (deviation that only reduces claims): the policy was unselective and the development gate had already failed, so spending the cohort could not pass anything. It remains available for a future registered attempt.
- Descriptive, non-binding (C1 out-of-fold): 90% of positive cells are covered at 7.8-38% of the ranking, 99% at 10-57% (median ~26%), but the last cells push FRF to 0.72-0.99. The positive-cell share, the lower bound of any full-recall selection, is 6-42%.
- Diagnosis: Phantasy Star's 0.977 is two image-edge cells holding the reset/interrupt vectors (0.540 without them); the edge cells were never trained on by protocol (crop edges are masked) and are architecture-defined roots in any real policy. Super Tennis (0.99: a data-dominated cell with trailing instructions) and Solomon's Key (0.72: a progressive tail) are genuine ranking misses. These are hypotheses for a new registered attempt, not a retroactive pass.
- Caveat on labels: positives are a lower bound of real code (unlisted and ambiguous `.db` bytes are unknown), so even 100% labelled recall would not prove full static coverage.

## Decision

| Platform | Classification | Notes |
| --- | --- | --- |
| Genesis | `Z80_BROAD_AOT_CHEAP_ENOUGH` | Z80 <= ~12% of build wall; no source-backed Genesis Z80 corpus of comparable size; no CNN |
| Master System | `Z80_RECALL_FAILED` (exploratory pilot, strict full recall) | economics are large (all of the build, 2.4 CPU-s/KiB); ranking works for the bulk but a handful of cells defeat strict recall; also 6 < 8 development lineages |
| T004 native producer | `NOT_JUSTIFIED` | no recall pass, adoption minimums unmet, and no Z80 selective-admission seam (see above) |
| Launcher | unchanged | Optimized remains Genesis-only (`supports_optimized`, covered by `launcher_policy_test`); nothing nonfunctional is advertised |

Shared executable-region interface: **not extracted** (default decision). The second CPU shares the cell/FRF/budget vocabulary but not tokens, decoder, admission contract
or corpus mechanics; one implementation of the Z80 side exists only as offline research tooling.

Follow-ups (separate, not done here): (1) a new registered attempt with image-edge-aware training, architecture-defined vector/reset cells as structural roots, and a
budget defined with a structural closure instead of a strict ranking cut; (2) at least 2 more independent source-backed SMS lineages (8 development + 3 sealed),
for example SDCC-built homebrew with relocated listings or further disassemblies matching legally held cartridges; (3) a Z80 selective-admission contract
(admitted-start set, typed unadmitted-PC behavior, validator) before any native producer.
