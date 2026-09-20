# ADR 0036: `platforms/genesis/compat/` file-level record count is a defensive ceiling, not a curated-set limit

- Status: Accepted
- Date: 2026-09-13
- Amends: ADR 0034 (corrects the record-count rationale/limit; changes nothing else in ADR-0034 --
  the schema surface, the two already-established records, the `compose-hints` union path, and every
  other prohibition remain exactly as ADR-0034 defined them)
- Related: ADR 0023 (the `logical_table_descriptor` schema and runtime consumer this correction does
  not touch), SEG-007-T206, SEG-007-T218

> **Path amendment (SEG-018-T002):** SEG-018-T002 later relocated this unchanged compatibility-metadata
> surface from `compat/genesis/` to `platforms/genesis/compat/` as part of the monorepo physical-layout
> migration. Schema, trust, safety, and consumption semantics are unchanged; current-path references
> below use the new location.

## Context

ADR-0034 introduced `platforms/genesis/compat/<rom-sha256>.json` to give a `logical_table_descriptor` produced by
a bounded, one-time, development-time semantic-analysis judgment a durable, version-controlled home.
At the time, only two such judgments existed (SEG-007-T164's `base_address = 2926` and SEG-007-T206's
`base_address = 94834`), so `tools/compat_genesis_schema.py` fixed `MAX_RECORDS_PER_FILE = 16` and
described the file as "a small, curated set of individually-justified scalar assertions."

SEG-007-T218 canonicalizes a whole-ROM static harvest of the same canonical PC-relative word-offset
dispatch idiom ADR-0023 already governs: 198 unique `(base_address, entry_width_bytes)` table
identities for the authorized Sonic REV00 ROM, all sharing the identical `entry_width_bytes = 2`,
`stride_bytes = 2` shape ADR-0023 already validates. `MAX_RECORDS_PER_FILE = 16` was never a technical
bound imposed by ADR-0023's runtime consumer (`parse_genesis_external_hints` /
`M68kGeneralStartupEnvironment::logical_table_descriptor_hint` / `m68k_fold_immutable_offset_table`),
which already looks up an arbitrary number of `logical_table_descriptor` records by
`(base_address, entry_width_bytes)` identity, nor by `tools/ghidra.py compose-hints`'s union logic,
which already unions an arbitrary number of structured records. It was a governance guard sized to the
number of individually curated assertions that existed at ADR-0034's own writing time -- an
accidental byproduct of that count, not a deliberate design limit on how many legitimate
`logical_table_descriptor` facts one ROM may have.

## Decision

A ROM-bound `platforms/genesis/compat/<rom-sha256>.json` file may legitimately contain the **complete set** of
bounded scalar `logical_table_descriptor` assertions required for that ROM. Record count alone is not
a semantic restriction on this file's legitimate content: a ROM with 198 legitimately identified
tables of the same governed shape is not a policy violation merely because it has more records than a
ROM with two.

`tools/compat_genesis_schema.py`'s per-file record-count check remains, but is re-scoped and
re-documented as a **defensive corruption/runaway-generation ceiling**, not a "small curated set"
limit: `MAX_RECORDS_PER_FILE` is raised from `16` to `1024` -- comfortably above the 198 records this
correction commits, while still catching a truly malformed or runaway-generated file (e.g. an
accidental dump of tens of thousands of entries) long before it could plausibly represent a legitimate
per-ROM table inventory. Its accompanying comment is corrected to describe this defensive-ceiling
purpose instead of "small curated set."

This correction changes **only** the file-level record-count rationale and limit. It explicitly does
**not**:

- widen ADR-0023's per-table `entry_count` cap (`1..256`), the `0xFFFE` maximum-implied-position
  bound, finite-selector cardinality bounds, target-admission checks, runtime membership guards, or
  any mapping/alignment/decode proof obligation -- every one of these remains completely unchanged;
- create a second descriptor `kind`, a second compatibility directory, a second consumer, a
  generated-C special case, a Sonic-specific runtime heuristic, or a runtime discovery mechanism;
- loosen any other `platforms/genesis/compat/` prohibition ADR-0034 established (no byte arrays, no
  disassembly-shaped fields, no `code_entry_candidate` / `address_table_candidate` /
  `code_pointer_table_descriptor` records, no bulk raw Ghidra dumps);
- change how a record is validated, admitted, or consumed once inside the file: every
  `logical_table_descriptor` record, whether one of two or one of two hundred, is still validated,
  admitted, and consumed by the exact same unweakened ADR-0023 parser/consumer path, and honestly
  marked `human_reviewed = false` unless a human genuinely, independently reviewed it.

`docs/testing/commercial-games.md`'s existing narrow `platforms/genesis/compat/` carve-out is amended in the
same spirit: it no longer reads as an implicit "handful of records" bulk-inventory prohibition on
record count, while every other forbidden-content boundary it already states (ROM bytes, disassembly,
raw instruction words, extracted assets, raw traces, bulk address-inventory/raw-Ghidra-dump record
kinds) remains completely unchanged and explicitly restated as still forbidden.

## Consequences

- A complete, whole-ROM static harvest of one governed `logical_table_descriptor` shape now has the
  same durable, reviewable, version-controlled home ADR-0034 already gave the first two individually
  curated assertions, instead of requiring an artificial one-file-per-few-records split or a quiet
  future widening with no recorded rationale.
- `platforms/genesis/compat/`'s schema guard still fails the build on a genuinely malformed, oversized, or
  otherwise policy-violating file; the corrected ceiling is defensive, not permissive of a different
  kind of content.
- Neither `tools/ghidra.py compose-hints` nor the ADR-0023 C++ consumer requires any change: both
  already treat `platforms/genesis/compat/` as an unbounded-by-count, per-identity union source. This correction
  only removes an artificial file-level ceiling that never matched their own already-supported
  contract.
