# Genesis Reset Image Contract

**Status:** research contract for SEG-001-T001 (2026-08-05).  This is a bounded input and
reset-vector contract for a later Genesis ingestion/analysis slice.  It defines neither a
general Genesis emulator nor a claim that an accepted image boots on hardware.

## Evidence and scope of claims

| ID | Source | URL, access date, and precise locator | Claim used here |
| --- | --- | --- | --- |
| M1 | *M68000 Family Programmer's Reference Manual*, Motorola, **MC68000**, M68000PM/AD Rev. 1 (1992) | [archived manual](https://archive.org/details/m68000familyprog0000unse), accessed 2026-08-05; §1.1.1 “Data Organization”, printed pp. 1-2–1-3 (PDF pp. 20–21) | Multi-byte operands are stored most-significant byte first (big-endian); the MC68000 processor address space is 24 bits. |
| M2 | *M68000 Family Programmer's Reference Manual*, Motorola, **MC68000**, M68000PM/AD Rev. 1 (1992) | [archived manual](https://archive.org/details/m68000familyprog0000unse), accessed 2026-08-05; §6.2 “Exception Vector Assignments”, printed p. 6-4 (PDF p. 166), Table 6-1, rows `0 Reset: Initial SSP` (`$000000`) and `1 Reset: Initial PC` (`$000004`) | Reset vector 0 supplies the initial supervisor stack pointer and vector 1 supplies the initial program counter. |
| M3 | *M68000 Family Programmer's Reference Manual*, Motorola, **MC68000**, M68000PM/AD Rev. 1 (1992) | [archived manual](https://archive.org/details/m68000familyprog0000unse), accessed 2026-08-05; §6.3.1 “Address Error”, printed pp. 6-7–6-8 (PDF pp. 169–170), paragraph “Instruction Fetch” | An **attempted** instruction fetch at an odd address causes an address-error exception. |
| S1 | *Genesis Technical Overview*, Sega Enterprises, Inc. | [Sega Retro archive entry](https://segaretro.org/Genesis_Technical_Overview), accessed 2026-08-05; PDF p. 3, “68000 Memory Map” table, row `$000000–$3FFFFF` / `Cartridge ROM` | The 68000 cartridge-ROM window is `$000000`–`$3FFFFF`. This is the primary console-source basis for the window. |
| S2 | *Memory map*, PlutieDev | <https://plutiedev.com/memory-map#68000-address-space>, accessed 2026-08-05; “68000 address space” table | Independently corroborates cartridge slot `$000000`–`$3FFFFF`. |

M1–M3 are a primary CPU reference for the original MC68000. S1 is the primary console document; S2 independently
corroborates the cartridge range. The sources establish the CPU and hardware facts in this
section only. All acceptance, range, diagnostic, and report rules below are project policy.
The public S1 archive may require a browser challenge; its PDF page, title, and table-row locator are retained
so the claim remains auditable. No emulator source or proprietary image is used.

## Typed domains and notation

All intervals are half-open, and all hex literals denote non-negative mathematical integers.
`size` is the byte length of the untrusted raw input.

| Name | Domain | Meaning |
| --- | --- | --- |
| `ImageSize` | `0 <= size <= 0x400000` | Accepted raw-image byte length. |
| `ImageOffset` | `0 <= o < size` | A zero-based byte index into the supplied raw image. It is never a host pointer. |
| `ImageRange` | `[start, end)` where `0 <= start <= end <= size` | A checked raw-image range. |
| `M68kAddress24` | `0 <= a <= 0xFFFFFF` | A numeric 24-bit 68000 bus address. |
| `VectorWord32` | `0 <= v <= 0xFFFFFFFF` | The four-byte, big-endian value stored in a reset-vector slot. |
| `CartridgeAddress` | `0x000000 <= a <= 0x3FFFFF` | The cited Genesis cartridge CPU-ROM window. |

`be32(o)` is defined only when `[o, o + 4)` is an `ImageRange`, and is:

```
((VectorWord32)image[o] << 24) | ((VectorWord32)image[o + 1] << 16) |
((VectorWord32)image[o + 2] << 8) | (VectorWord32)image[o + 3]
```

The casts occur before shifting. Thus the source bytes at offsets `0`, `1`, `2`, and `3` are the
most- through least-significant bytes of the initial SSP, and offsets `4` through `7` are those of
the initial PC. This representation rule follows M1; it is not dependent on host endianness.

## Bounded raw-image and reset policy

This contract accepts a **finite, non-mirrored** raw mapping only. It deliberately does not infer
a mapper, bank register, SRAM, expansion device, TMSS behavior, or any wraparound rule.

1. Reject `size > 0x400000`. The limit is the end-exclusive length of the S1/S2 cartridge window,
   not a statement that every historical cartridge is no larger or needs no mapper.
2. Define `mapped_length = size` after the size check. Define `map_offset_to_address(o) = a` only
   if `o` is an `ImageOffset`; then `a = o` and `a` is a `CartridgeAddress`. Define the inverse
   `map_address_to_offset(a) = o` only if `0 <= a < mapped_length`; then `o = a`.
3. No other address maps to an image byte. In particular, `0x400000` never aliases offset `0`, and
   an address below `mapped_length` is the only address that maps. This is the non-mirroring rule.
4. The one region is `raw_cartridge_rom`, with CPU address range `[0x000000, mapped_length)`, raw
   image range `[0, mapped_length)`, and permissions `read | execute`; it has no `write`
   permission. “Execute” permits a later static decoder to source instruction bytes; it does not
   establish that a CPU, console, or loader can execute them.
5. Reset reads require `[0, 4)` for SSP and `[4, 8)` for PC. `initial_ssp = be32(0)` and
   `initial_pc_word = be32(4)`. The SSP is reported verbatim as a `VectorWord32`; this task does
   not validate a Genesis stack-memory map.
6. Convert the PC word to `M68kAddress24` only if `(initial_pc_word & 0xFF000000) == 0`; then
   `initial_pc = initial_pc_word`. Do not mask or truncate a nonzero high byte. Requiring a zero
   high byte is a conservative **project static-image acceptance policy**, not a claim that reset
   hardware itself rejects every other 32-bit PC representation.
7. A converted PC is eligible only when `(initial_pc & 1) == 0` and
   `0 <= initial_pc < mapped_length`. The latter checked inequality is the same as requiring a
   mapped `CartridgeAddress`; it permits the reset PC to point to the last byte only in principle,
   while a later instruction fetch must separately bounds-check its full instruction length.

The conversion formulas above use mathematical integers. An implementation using fixed-width types
must reject before narrowing and must use a checked range predicate, such as
`start <= size && length <= size - start`, before every raw read. It must not compute an unchecked
`start + length`, cast a host pointer to an address, or modulo-reduce an address.

## Deterministic result and diagnostics

The validator emits exactly one result. It evaluates the following table in order and stops at the
first matching row. IDs are stable API strings.

| Priority | Predicate | Result / diagnostic |
| ---: | --- | --- |
| 1 | `size > 0x400000` | `rejected` / `GENESIS_IMAGE_SIZE_LIMIT` |
| 2 | `[0, 4)` is not an `ImageRange` | `rejected` / `GENESIS_RESET_SSP_TRUNCATED` |
| 3 | `[4, 8)` is not an `ImageRange` | `rejected` / `GENESIS_RESET_PC_TRUNCATED` |
| 4 | `(be32(4) & 0xFF000000) != 0` | `rejected` / `GENESIS_RESET_PC_NOT_24BIT` |
| 5 | `(be32(4) & 1) != 0` | `rejected` / `GENESIS_RESET_PC_ODD` |
| 6 | `be32(4) >= size` | `rejected` / `GENESIS_RESET_PC_UNMAPPED` |
| 7 | otherwise | `accepted` / `GENESIS_RESET_IMAGE_ACCEPTED` |

This precedence makes malformed inputs deterministic: size takes precedence over unavailable
vectors, an unavailable PC takes precedence over its value, width takes precedence over alignment,
and alignment takes precedence over map membership. It also means a zero-length image reports the
SSP truncation rather than a generic format error. M3 establishes the consequence of an attempted
odd instruction fetch; rejecting an odd reset-PC word before attempting a fetch is this contract's
fail-closed project policy, not an asserted reset-hardware sequence.

## Required report, provenance, and serialization

Every result reports the following fields. `unexamined` means priority 1 stopped validation;
`unavailable` means the validator examined the range and found it incomplete; `not_checked` means
an earlier non-size diagnostic made the check inapplicable. These three values are not synonyms.

| Field | Normal value / provenance | `GENESIS_IMAGE_SIZE_LIMIT` exact value |
| --- | --- | --- |
| `outcome`, `diagnostic`, `input_size`, `size_limit`, `exit_class` | Result-table values; `exit_class` is `success` only for accepted and otherwise `input_rejected`. | `rejected`, `GENESIS_IMAGE_SIZE_LIMIT`, actual size, `0x400000`, `input_rejected`. |
| `raw_region` | Object named `raw_cartridge_rom`, with CPU range, raw range, permissions, and `mapping = identity_non_mirrored`. | `null`; `raw_region_status = not_constructed`. |
| `reset_ssp_range`, `reset_pc_range` | Fixed requested raw ranges `[0,4)` and `[4,8)`, each with `status = available` or `unavailable`. | The same requested ranges, each with `status = unexamined`; no vector bytes were read. |
| `initial_ssp`, `initial_pc_word`, `initial_pc`, `entry_address`, `entry_image_offset` | Values defined by the checked stages, otherwise `null`. `entry_*` are non-null only on acceptance. | All `null`. |
| `pc_alignment`, `pc_mapping` | `not_checked`, `even`/`odd`, and `not_checked`/`mapped`/`unmapped` under diagnostic precedence. | Both `unexamined`. |
| `source_provenance` | Object defined below. | Same object, with both vector provenance statuses `unexamined`. |

For diagnostics 2–3, `raw_region` is constructed, `raw_region_status = constructed`, and a vector
range is `unavailable` exactly when its complete four bytes cannot be read. For diagnostics 4–6 and
acceptance, both vector ranges are `available`; `initial_ssp` and `initial_pc_word` are non-null.
For diagnostics 4–5, `pc_mapping = not_checked`; for diagnostic 4, `pc_alignment = not_checked`;
for diagnostic 5, `pc_alignment = odd`; for diagnostic 6 and acceptance, `pc_alignment = even`.
`initial_pc` is non-null after the width check (diagnostics 5–6 and acceptance) and null otherwise.

`source_provenance` is always an object with `input = raw_image`, `byte_order = big_endian`, and
`source_ids = ["M1", "M2", "M3", "S1", "S2"]` in exactly that order. It contains `ssp` and `pc`
objects, each with fixed `offset` (`0` or `4`), `length = 4`, and the range status above. An
available object contains its four source bytes and decoded word; unavailable and unexamined
objects use `bytes = null` and `word = null`. Thus a rejected report retains every safely read
value but never invents provenance.

The exact nested key order is: `raw_region` uses `name, cpu_address_range, image_range,
permissions, mapping`; each range uses `start, end, status`; `source_provenance` uses `input,
byte_order, source_ids, ssp, pc`; and each `ssp`/`pc` object uses `offset, length, status, bytes,
word`. Address and image ranges are two-element integer arrays `[start, end]`; `permissions` is
the ordered array `["read", "execute"]`; source bytes are a four-element integer array in raw
offset order. A null `raw_region` has no nested keys.

If byte-identical reports are required, serialize this report as canonical UTF-8 JSON: one object,
no insignificant whitespace, keys in this exact order:

```
outcome, diagnostic, exit_class, input_size, size_limit, raw_region_status, raw_region,
reset_ssp_range, reset_pc_range, initial_ssp, initial_pc_word, initial_pc, entry_address,
entry_image_offset, pc_alignment, pc_mapping, source_provenance
```

Nested object keys use the order stated in their definitions above; arrays preserve listed order.
All integers are base-10 JSON numbers with no leading zero (except `0`); all unavailable values are
the JSON literal `null`; strings are ASCII where this contract supplies them. The public command's
process exit code is `0` for `success` and `2` for `input_rejected`. These serialization and exit
rules are project interface policy, not hardware facts.

## Synthetic, project-owned fixture matrix

No fixture binary is created by this research task. A later test suite may create only the inputs
below: allocate an all-zero array of the stated length and make exactly the listed byte writes.
All unlisted bytes remain zero. These arrays are project-authored synthetic data, not ROM,
firmware, key, SDK, or derivative content.

`W32(o, v)` writes the four bytes of `v` in the `be32` order defined above. Each eventual fixture
manifest **must** record fixture ID, construction recipe, exact byte length, SHA-256 of the exact
resulting bytes (lowercase hexadecimal), expected outcome/diagnostic, and expected report fields.
The manifest must calculate rather than hand-wave the hash; a changed byte, construction, or length
requires a new hash record. For each row, its exact report is derived by applying the diagnostic
table, the status rules in “Required report”, and the listed recipe; omitted writes are zero. Every
row therefore has the fixed source-ID list and requested vector ranges stated there, rather than
an implementation-defined report. The matrix is complete for this contract's decision rows and
checked truncation boundaries.

| Fixture ID | Length and writes | Purpose | Expected result and selected report facts |
| --- | --- | --- | --- |
| `empty` | `0`; none | Empty-input boundary | `rejected` / `GENESIS_RESET_SSP_TRUNCATED`; both ranges unavailable. |
| `ssp-one-byte` | `1`; byte `12` at `0` | SSP truncation equivalence representative | `rejected` / `GENESIS_RESET_SSP_TRUNCATED`; both ranges unavailable. |
| `ssp-two-bytes` | `2`; bytes `12 34` at `0..1` | SSP truncation equivalence representative | `rejected` / `GENESIS_RESET_SSP_TRUNCATED`; both ranges unavailable. |
| `ssp-three-bytes` | `3`; bytes `12 34 56` at `0..2` | SSP word one byte short | `rejected` / `GENESIS_RESET_SSP_TRUNCATED`. |
| `ssp-only` | `4`; `W32(0, 0x12345678)` | PC range begins exactly at end | `rejected` / `GENESIS_RESET_PC_TRUNCATED`; report SSP `0x12345678`. |
| `pc-one-byte` | `5`; `W32(0, 0x12345678)`, byte `00` at `4` | PC truncation equivalence representative | `rejected` / `GENESIS_RESET_PC_TRUNCATED`; report SSP `0x12345678`. |
| `pc-two-bytes` | `6`; `W32(0, 0x12345678)`, bytes `00 00` at `4..5` | PC truncation equivalence representative | `rejected` / `GENESIS_RESET_PC_TRUNCATED`; report SSP `0x12345678`. |
| `pc-three-bytes` | `7`; `W32(0, 0x12345678)`, bytes `00 00 00` at `4..6` | PC word one byte short | `rejected` / `GENESIS_RESET_PC_TRUNCATED`. |
| `valid-be32-entry` | `8`; `W32(0, 0x00FF0000)`, `W32(4, 0x00000006)` | Big-endian vector decoding and in-image even entry | `accepted` / `GENESIS_RESET_IMAGE_ACCEPTED`; SSP `0x00FF0000`, PC word/address `0x00000006`, entry offset `6`. |
| `pc-high-byte` | `8`; `W32(0, 0)`, `W32(4, 0x01000000)` | No silent 24-bit truncation | `rejected` / `GENESIS_RESET_PC_NOT_24BIT`; `initial_pc = null`. |
| `pc-high-byte-and-odd` | `8`; `W32(0, 0)`, `W32(4, 0x01000001)` | Width precedes alignment | `rejected` / `GENESIS_RESET_PC_NOT_24BIT`. |
| `pc-odd` | `8`; `W32(0, 0)`, `W32(4, 0x00000007)` | 68000 PC alignment | `rejected` / `GENESIS_RESET_PC_ODD`; PC address `7`, mapping not checked. |
| `pc-odd-unmapped` | `8`; `W32(0, 0)`, `W32(4, 0x00000009)` | Alignment precedes mapping | `rejected` / `GENESIS_RESET_PC_ODD`. |
| `pc-at-image-end` | `8`; `W32(0, 0)`, `W32(4, 0x00000008)` | End-exclusive mapping boundary | `rejected` / `GENESIS_RESET_PC_UNMAPPED`. |
| `pc-even-unmapped` | `8`; `W32(0, 0)`, `W32(4, 0x0000000A)` | Finite mapping, no fabricated bytes | `rejected` / `GENESIS_RESET_PC_UNMAPPED`. |
| `four-mebibyte-last-even` | `0x400000`; `W32(0, 0)`, `W32(4, 0x003FFFFE)` | Largest accepted image and highest even mapped address | `accepted` / `GENESIS_RESET_IMAGE_ACCEPTED`; entry offset `0x3FFFFE`. |
| `four-mebibyte-window-end` | `0x400000`; `W32(0, 0)`, `W32(4, 0x00400000)` | Cartridge-window end and no-mirror rule | `rejected` / `GENESIS_RESET_PC_UNMAPPED`. |
| `over-limit` | `0x400001`; `W32(0, 0)`, `W32(4, 0x00000000)` | Size limit precedence | `rejected` / `GENESIS_IMAGE_SIZE_LIMIT`; vector availability is not examined. |
| `over-limit-unexamined-vectors` | `0x400001`; no writes | Size limit precedes vector examination; the zero bytes physically exist but are deliberately unread | `rejected` / `GENESIS_IMAGE_SIZE_LIMIT`; both vector ranges are `unexamined`, `raw_region = null`. |

All rows in this matrix are **reset-validator unit inputs only**. They intentionally omit the
Genesis header and therefore are not inputs to the public `analyze` command, whose first gate is
recognized Genesis classification. A later public-analysis fixture must be a separately named,
synthetic composite: start with one of the accepted vector recipes above, enlarge it as necessary,
and add the recognized Genesis header bytes required by
`header-classification-contract.md` (SEG-000-T001). It must record its own construction and
SHA-256. This requirement composes with, and does not reinterpret or redefine, SEG-000's
classification policy.

## Explicit non-goals and limitations

- Acceptance does not classify a ROM header, validate a checksum, identify region, validate an
  initial SSP, decode an instruction, or prove any vector is semantically executable.
- No bootability claim is made. TMSS, BIOS/reset sequencing, VDP, Z80, I/O, timing, interrupts,
  peripheral behavior, SRAM, expansion hardware, and bus contention are outside this contract.
- Mapper and bank-switch behavior are unsupported. Inputs whose execution needs a mapping other
  than the finite identity mapping must be rejected or handled by a later explicitly cited policy.
- Self-modifying code and writes to `raw_cartridge_rom` are unsupported; the region has no write
  permission. A later analysis/runtime must fail closed with source provenance if reached.
- The contract does not specify instruction-fetch length, exception behavior after reset, stack
  access, or 68000 variants beyond the cited reset/vector, byte-order, address-width, and
  instruction-address alignment facts.
