# ADR-0032: Human-reviewed immutable code-pointer-table descriptor

- Status: Accepted
- Date: 2026-09-10
- Related: ADR-0023, ADR-0025, SEG-007-T203

## Context

ADR-0023 established that a ROM-identity-bound, explicitly opted-in, human-reviewed
logical extent/type assertion may supply one trusted scalar domain while downstream
structural checks retain their existing authority. ADR-0025 established the distinct
authority path for code-entry proposals: only an address admitted by the ordinary
mapping/alignment/decode/discovery walk may enter generated code or the
`EmittedCodeAddressSet`.

The reached object-dispatch family uses a contiguous immutable cartridge table of
4-byte absolute code pointers rather than ADR-0023's 2-byte relative data offsets.
Public Sonic 1 community source identifies the table's contiguous semantic boundary;
a human-reviewed cross-check against the authorized, hash-pinned image confirmed that
the mechanically decoded PC-relative EA refers to the same table shape and that the
descriptor base is the first real pointer entry, not the earlier affine origin used by
the dispatcher's 1-based indexed consumer. The reviewed extent is recorded durably only
as bounded within the fixed schema cap. Automatic
analyzer derivation is neither required nor granted authority by this decision.

## Decision

Add an independent `code_pointer_table_descriptor` record to the heterogeneous
`--external-hints` artifact. It is empty by default and requires:

- an exact lowercase `rom_sha256` match and explicit opt-in;
- `source = "immutable_cartridge"`;
- `pointer_type = "absolute_code_address"`;
- `entry_width_bytes = 4` and `stride_bytes = 4`;
- `1 <= entry_count <= 256`, with checked `base_address + 4 * entry_count`;
- complete typed provenance with non-empty tool/version/timestamp and
  `human_reviewed = true`.

Records sharing a base address but disagreeing on count conflict: all records for that
identity are dropped. Semantically identical provenance variants are harmless and are
ordered deterministically; composition deduplicates exact records while preserving
distinct provenance variants. The narrow JSON reader rejects duplicate keys at every
object level, recognizes strict JSON number grammar, and requires complete finite
numeric conversion. The existing `logical_table_descriptor` parser and
semantics are unchanged.

At generation time, the Genesis frontend requires one structurally valid
`raw_cartridge_rom` mapping claim to cover the descriptor's complete half-open extent.
Partial, overlapping, ambiguous, overflowing, non-cartridge, or out-of-image coverage
contributes no candidates. A valid interval is read big-endian in fixed 4-byte steps.
Every extracted value is appended only as a `code_entry_candidate` proposal, sorted
and deduplicated with existing proposals, then sent through the exact ADR-0025
per-root validation/admission walk. There is no direct descriptor-to-emitted-set path.
The T179 `JSR (An)` runtime dispatcher and membership guard are unchanged.

`tools/ghidra.py compose-hints` recognizes this descriptor as structured data so it
preserves it alongside existing logical descriptors while unioning candidate records.
This recognition does not make Ghidra its producer or validator.

## Trust boundary

The table's type and declared extent are trusted human assertions. Mapping,
alignment, decode, and discovery checks validate consequences; they do not prove that
the extent is semantically correct. An under-count omits proposals, so a selected
omitted target reaches the existing honest Tier-2 nonmember stop. An over-count may
propose adjacent values, but cannot directly grant execution: each value must still
survive ordinary ADR-0025 admission. A structurally valid extra can nevertheless be
admitted, which is why the extent remains explicit reviewed trust rather than an
automatically proven fact.

## Consequences

Ordinary and candidate-only recompilation remain unchanged. No runtime table read,
opcode decoder, interpreter, JIT, selector-value proof, new dispatcher, or Sonic-
specific production branch is introduced. Synthetic tests cover positive admission
and strict-C11 execution through existing `JSR (An)` dispatch, biased-affine-origin
separation, fail-closed schema/provenance/identity/extent cases,
invalid proposal exclusion, under-count behavior, and heterogeneous composition.

Public corroboration: [Sonic Retro s1disasm, Sonic the Hedgehog disassembly]
(https://github.com/sonicretro/s1disasm), specifically its public object execution and
object-pointer source units (consulted as corroborating lookahead, never production
authority).
