# ADR 0114: SEG-051 Z80 executable-region ranking experiment (SMS pilot) and decision record

- Status: PROTOCOL FROZEN (this commit precedes every label-sensitive result); results are appended below by later commits.
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
