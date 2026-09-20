# ADR-0034: Committed Genesis compatibility metadata for scalar table descriptors

- Status: Accepted
- Date: 2026-09-11
- Amended by: ADR-0036 (corrects `MAX_RECORDS_PER_FILE`'s rationale from a "small curated set" limit
  to a defensive corruption/runaway-generation ceiling and raises it from 16 to 1024; every other
  decision below is unchanged)
- Related: ADR-0023, ADR-0025 (§10), ADR-0032, ADR-0033, SEG-007-T164, SEG-007-T203, SEG-007-T204,
  SEG-007-T206

> **Path amendment (SEG-018-T002):** SEG-018-T002 later relocated this unchanged compatibility-metadata
> surface from `compat/genesis/` to `platforms/genesis/compat/` as part of the monorepo physical-layout
> migration. Schema, trust, safety, and consumption semantics are unchanged; current-path references
> below use the new location.

## Context

Every ADR-0023 `logical_table_descriptor` -- including the one manually reviewed table SEG-007-T164
originally resolved -- lives only in the fully ignored `.tools/analysis-hints/<rom-sha256>.json` file:
never committed, never shared across environments or contributors, and lost the next time someone
regenerates their local hints from scratch. SEG-007-T206 extends the same trusted-annotation pattern to
a second table shape (a 2-byte PC-relative offset-word table) via a bounded, one-time, development-time
semantic-analysis judgment (see its own task record's Scope Phase 3). That judgment's only lasting
output is a small `entry_count` fact; without a committed home it would inherit the exact same
fragility ADR-0023's own manually-reviewed table has always had, defeating the whole point of doing the
bounded analysis once.

`docs/testing/commercial-games.md`'s existing policy excludes every commercial-derived value from
committed evidence, including scalar addresses and counts. A blanket carve-out ("commit whatever
analysis produces") would be far too broad and would reintroduce exactly the raw-disassembly/bulk-dump
risk that policy exists to prevent.

## Decision

Introduce `platforms/genesis/compat/<rom-sha256>.json`: a per-ROM, Git-tracked (not ignored) JSON array holding
ONLY curated, individually-justified scalar table-descriptor assertions -- narrow by schema/content
shape, not by an expectation of a small record count (ADR-0036 corrects the latter). Today the only
recognized record `kind` is `logical_table_descriptor` (ADR-0023's own schema), carrying exactly
`rom_sha256`, `kind`, `base_address`, `entry_width_bytes`, `stride_bytes`, `entry_count`, and
`provenance` (`tool`, `tool_version`, `timestamp`, `human_reviewed`) -- the identical minimal scalar
surface an `.tools/analysis-hints/` record already carries, and nothing else. No byte array, no
disassembly-shaped field, and no bulk record kind (`code_entry_candidate`,
`address_table_candidate`, `code_pointer_table_descriptor`) is ever eligible for this location, no
matter how small a subset.

`tools/ghidra.py compose-hints` unions `platforms/genesis/compat/<sha>.json` into the composed artifact exactly
like the existing `.tools/analysis-hints/<sha>.json` base-hints source: same ROM-identity precondition,
same conflict-detection rule for a disagreeing duplicate identity, same normalization. An absent or
empty compat file is a complete no-op. This wiring is a pure **consumption** path -- `compose-hints`
never invokes, re-derives, or re-judges an extent; it only reads whatever a prior bounded,
development-time analysis already committed.

`tests/compat_genesis_schema_test.py` structurally validates every file under `platforms/genesis/compat/`:
exactly the permitted scalar keys, a well-formed `rom_sha256`, a recognized `kind`, and a defensive
corruption/runaway-generation ceiling on file-level record count (ADR-0036) -- a complete per-ROM
inventory of individually-justified scalar assertions is permitted content; only a raw dump, a
disassembly-shaped field, or a bulk record kind (`code_entry_candidate`, `address_table_candidate`,
`code_pointer_table_descriptor`) is ever forbidden here, no matter how small or large the file. This
guard fails the build if a future change ever widens `compat/`'s committed content beyond this narrow
schema.

`docs/testing/commercial-games.md` is amended with a narrowly scoped carve-out describing exactly this
surface, while every other existing prohibition (ROM bytes, disassembly, instruction words, assets,
traces, bulk address inventories/Ghidra dumps) is explicitly restated as still forbidden under
`compat/`.

## Consequences

- A `logical_table_descriptor` produced by a bounded, one-time, development-time semantic-analysis
  judgment (SEG-007-T206 Phase 3, or any future equivalent for a different table) now has a durable,
  reviewable, version-controlled home instead of silently disappearing on hint regeneration.
- `platforms/genesis/compat/` never becomes a general commercial-data escape hatch: its schema guard, not
  reviewer discipline alone, keeps the committed surface to individually-justified scalars.
- Ordinary recompilation (`compose-hints`, the one-shot route) never re-derives or re-judges a
  committed assertion; consumption remains exactly as deterministic as reading a manually authored
  `.tools/analysis-hints/` record today.
- This ADR does not create a new trust primitive: a `platforms/genesis/compat/` `logical_table_descriptor` record
  is validated, admitted, and consumed by the exact same unweakened ADR-0023 parser/consumer path as
  any other `logical_table_descriptor`, honestly marked `human_reviewed = false` unless a human
  genuinely, independently reviewed it.
