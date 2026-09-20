# Bounded Genesis and SMS/Game Gear Header Classification

**Status:** research contract for SEG-000-T001 (2026-08-05).  This document defines the
input-classification boundary consumed by later ingestion work.  It is not an implementation,
fixture, executable-map, mapper, checksum, or boot policy.

## Sources and supported facts

These are community technical references, not primary sources.  They are sufficiently specific
for this deliberately narrow contract; claims not listed below are excluded from it.

| ID | Title, publisher, and platform | URL and access date | Precise locator | Supported claim |
| --- | --- | --- | --- | --- |
| G1 | *ROM header reference*, PlutieDev; Mega Drive/Genesis family | <https://plutiedev.com/rom-header> (accessed 2026-08-05) | “ROM header format” | The header occupies `$100`–`$1FF`; its system type is the 16-byte field at `$100`, and its domestic title is the 48-byte field at `$120`; the table gives the remaining field offsets, sizes, and reserved ranges. |
| G2 | *ROM header reference*, PlutieDev; Mega Drive/Genesis family | <https://plutiedev.com/rom-header#system> (accessed 2026-08-05) | “System type” | The system-type field is space-padded and lists the nine known strings used below.  It also says a console with TMSS requires initial `SEGA` text; that boot observation is **not** used as a project bootability claim. |
| S1 | *ROM Header*, SMS Power! Development; Master System/Game Gear | <https://www.smspower.org/Development/ROMHeader> (accessed 2026-08-05) | “Location and size” | A 16-byte header can begin at raw-ROM offsets `$1FF0`, `$3FF0`, or `$7FF0`. |
| S2 | *ROM Header*, SMS Power! Development; Master System/Game Gear | <https://www.smspower.org/Development/ROMHeader#TMRSEGA7ff08Bytes> (accessed 2026-08-05) | “TMR SEGA ($7ff0, 8 bytes)” | The first eight bytes are ASCII `TMR SEGA`. |
| S3 | *ROM Header*, SMS Power! Development; Master System/Game Gear | <https://www.smspower.org/Development/ROMHeader#RegionCode0x7fff05Bytes> (accessed 2026-08-05) | “Region code (0x7fff, 0.5 bytes)” | The high nibble of the final header byte has codes `$3` SMS Japan, `$4` SMS Export, `$5` GG Japan, `$6` GG Export, and `$7` GG International. |
| S4 | *ROM Header*, SMS Power! Development; Master System/Game Gear | <https://www.smspower.org/Development/ROMHeader#ROMSize0x7fff05Bytes> (accessed 2026-08-05) | “ROM size (0x7fff, 0.5 bytes)” | The low nibble is a declared ROM-size code, may guide BIOS checksum range, and is often inaccurate or smaller than the actual ROM.  This contract therefore does not validate it. |
| S5 | *Memory Map*, SMS Power! Development; Master System/Game Gear | <https://www.smspower.org/Development/MemoryMap> (accessed 2026-08-05) | “Master System/Mark III/Game Gear” | `$0000`–`$BFFF` is cartridge space; this corroborates that header offsets are target/raw-ROM addresses, not host pointers or an executable-map proof. |
| S6 | *BIOS*, SMS Power! Development; Master System/Game Gear | <https://www.smspower.org/Development/BIOS> (accessed 2026-08-05) | introductory paragraphs | BIOS presence differs by model and can be active in the ROM area on boot.  This corroborates that header recognition cannot establish boot behavior. |

## Cited field layout and accepted bytes

All offsets below are zero-based raw-image offsets.  Ranges are half-open: `[start, end)`.
No target address is inferred from a host pointer; a recorded offset is the provenance for every
candidate.

### Genesis

G1 describes a header range `[0x100, 0x200)`: system type `[0x100, 0x110)`, copyright/release
`[0x110, 0x120)`, domestic title `[0x120, 0x150)`, overseas title `[0x150, 0x180)`, serial
`[0x180, 0x18E)`, checksum `[0x18E, 0x190)`, device support `[0x190, 0x1A0)`, ROM range
`[0x1A0, 0x1A8)`, RAM range `[0x1A8, 0x1B0)`, extra memory `[0x1B0, 0x1BC)`, modem support
`[0x1BC, 0x1C8)`, reserved `[0x1C8, 0x1F0)`, region `[0x1F0, 0x1F3)`, and reserved
`[0x1F3, 0x200)`.  This contract uses the complete 16-byte system field and complete 48-byte
domestic-title field only; it neither reads nor validates the other fields.

The following are G2-listed system types, encoded as ASCII and followed by exactly enough ASCII
spaces (`0x20`) to fill `[0x100, 0x110)`.  This milestone recognizes **only** the first two exact
values.  No NUL terminator, alternate padding, prefix, or trailing non-space byte is accepted.

| Field bytes before padding | Required padding | This milestone's classification |
| --- | ---: | --- |
| `SEGA MEGA DRIVE` | 1 space | `mega_drive` |
| `SEGA GENESIS` | 4 spaces | `genesis` |
| `SEGA 32X` | 8 spaces | rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED` |
| `SEGA EVERDRIVE` | 2 spaces | rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED` |
| `SEGA SSF` | 8 spaces | rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED` |
| `SEGA MEGAWIFI` | 3 spaces | rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED` |
| `SEGA PICO` | 7 spaces | rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED` |
| `SEGA TERA68K` | 4 spaces | rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED` |
| `SEGA TERA286` | 4 spaces | rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED` |

The rejected values are cited canonical-like system-field values, not supported Genesis variants.
Any complete field that begins ASCII `SEGA` but is not either accepted exact value is likewise
rejected as `HDR_GENESIS_SYSTEM_UNSUPPORTED`; it is not an 8-bit or generic no-header result.
Other complete nonmatching system fields make no Genesis candidate.  The recognized family result
is `genesis`; `system_type` is either `mega_drive` or `genesis`.

### Master System and Game Gear

For each candidate start `H` in `{0x1FF0, 0x3FF0, 0x7FF0}`, S1 requires the full range
`[H, H + 0x10)`.  S2's signature is `[H, H + 8) == ASCII "TMR SEGA"`.  The region/size byte is
at `H + 0xF`; its high nibble alone is classified, while its low nibble is recorded but not
validated (S4).  The accepted high nibbles and metadata are:

| High nibble | Result family | `region_system` |
| ---: | --- | --- |
| `0x3` | `master_system` | `sms_japan` |
| `0x4` | `master_system` | `sms_export` |
| `0x5` | `game_gear` | `gg_japan` |
| `0x6` | `game_gear` | `gg_export` |
| `0x7` | `game_gear` | `gg_international` |

All other high nibbles are rejected.  Product-code, version, reserved bytes, checksum, and
low-nibble ROM-size value are deliberately outside classification.

## Project classification policy

The following limits, order, diagnostics, and outcome rules are project policy, not hardware
facts asserted by the sources.

### Bounded reads and limits

- Reject an input whose byte length exceeds `0x400000` (4 MiB) before examining headers, with
  `IMG_SIZE_LIMIT`.  This is a conservative project ceiling: G1 lists common Genesis sizes only
  through 4 MiB, while S4 lists codes only through 1 MiB and warns their values are unreliable.
  A single 4-MiB ceiling avoids asserting mapper support or rejecting a cited header merely for
  an unreliable size nibble.  It is not a statement of either console's maximum cartridge size.
- A candidate read is permitted only after checking `start <= size` and `length <= size - start`;
  this subtraction form avoids overflow.  No byte outside the established range is read.
- Genesis first requires complete `[0x100, 0x110)` before classifying its system field. An image
  ending in `[0x104, 0x110)` is a system-field truncation candidate only when its available bytes
  at `0x100` exactly match a prefix of an accepted system field; fewer than four bytes or any
  other prefix makes no Genesis candidate. A complete accepted system field then requires the
  complete domestic-title range `[0x120, 0x150)`; ending at or after `0x110` but before `0x150`
  is a domestic-title truncation candidate. A complete `SEGA`-prefixed unaccepted system field is
  an unsupported-system candidate and does not require a title read.
- For a recognized Genesis candidate, retain the exact 48 domestic-title bytes and their
  `[0x120, 0x150)` provenance. `domestic_title_display` removes only trailing `0x20` bytes and
  renders each remaining `0x20`–`0x7E` byte as that ASCII character and every other byte as an
  uppercase `\xHH` escape. Its `domestic_title_encoding` is `ascii_with_hex_escapes`. This is a
  deterministic and lossless project display policy because the raw bytes remain metadata; it is
  not a claim that all title bytes are ASCII.
- An 8-bit candidate requires complete `[H, H + 0x10)`.  If the complete `TMR SEGA` signature is
  present but the image ends before `H + 0x10`, it is a truncation candidate.  A start with fewer
  than eight bytes, or a non-matching prefix, is not a candidate.  These rules make short,
  unrelated images `HDR_NOT_RECOGNIZED`, rather than falsely attributing a missing header to a
  platform.

### Outcomes, precedence, and stable diagnostics

The classifier returns exactly one of `recognized`, `rejected`, or `unrecognized`, and exactly
one diagnostic identifier.  It scans locations in this deterministic order: Genesis `0x100`,
then 8-bit `0x1FF0`, `0x3FF0`, and `0x7FF0`.  It records all candidates before selecting an
outcome, except that the size limit is an immediate rejection.

| Priority | Condition | Outcome and diagnostic |
| ---: | --- | --- |
| 1 | `size > 0x400000` | `rejected`, `IMG_SIZE_LIMIT` |
| 2 | Any truncation candidate | `rejected`; select the first truncation candidate in scan order and use its own diagnostic: `HDR_GENESIS_SYSTEM_TRUNCATED`, `HDR_GENESIS_DOMESTIC_TITLE_TRUNCATED`, or `HDR_8BIT_HEADER_TRUNCATED` |
| 3 | Any complete `SEGA`-prefixed unaccepted Genesis system field | `rejected`, `HDR_GENESIS_SYSTEM_UNSUPPORTED` from the Genesis candidate at `0x100` |
| 4 | Any complete 8-bit signature with an unsupported high nibble | `rejected`, `HDR_SMS_GG_REGION_INVALID` from the first invalid-region candidate in scan order |
| 5 | Exactly one valid candidate | `recognized`, `HDR_RECOGNIZED_GENESIS`, `HDR_RECOGNIZED_SMS`, or `HDR_RECOGNIZED_GG` by that candidate |
| 6 | Two or more valid candidates, all with one result family | `rejected`, `HDR_HEADER_COLLISION` |
| 7 | Two or more valid candidates with different result families | `rejected`, `HDR_HEADER_CONFLICT` |
| 8 | No candidate | `unrecognized`, `HDR_NOT_RECOGNIZED` |

Within a priority, scan order selects diagnostic provenance: Genesis `0x100`, then 8-bit `0x1FF0`,
`0x3FF0`, and `0x7FF0`. Thus a selected truncation diagnostic is intrinsic to its source candidate,
never a generic “earliest” label. Priorities 2 through 4 intentionally reject even if another
location is valid: a complete or partial header-like claim must not be silently ignored. Priority 6
rejects duplicate headers rather than choosing one arbitrarily; priority 7 covers Genesis/8-bit
and SMS/Game Gear conflicts.

For every result, metadata must include `input_size`, `size_limit`, `outcome`, `diagnostic`, and
an ordered `candidates` list. Each candidate records `kind` (`genesis` or `sms_gg`), `offset`,
required ranges, status, and field provenance. Genesis status is `system_truncated`,
`title_truncated`, `unsupported_system`, or `valid`; its records name `system_range`
`[0x100, 0x110)` and `domestic_title_range` `[0x120, 0x150)` when applicable. An
unsupported-system candidate retains the exact 16 system bytes; a valid one retains both exact
field byte sequences. An 8-bit status is `truncated`, `invalid_region`, or `valid` and retains its
header range `[H, H + 0x10)`. A recognized result additionally records one `family` and either
Genesis `system_type`, `domestic_title_bytes`, `domestic_title_display`, and
`domestic_title_encoding`, or 8-bit `region_system` plus `rom_size_code` (the low nibble,
informational only). Rejected results retain every candidate, so a diagnostic never discards source
offsets.

Recognition is only classification of bounded header bytes.  It does **not** prove executable
mapping, reset-vector validity, mapper behavior, checksum validity, BIOS acceptance, or
bootability.

## Synthetic project-owned fixture matrix

No binary fixture is created by this research task.  A later implementation may create only
synthetic, project-owned inputs: begin an all-zero byte array of the stated length, write exactly
the listed ASCII/byte values at the listed raw offsets, and add no commercial, firmware, key, or
derived content.  The fixture manifest must record, for **each** named binary, its ID, purpose,
construction, byte length, and SHA-256 of its exact bytes.  The expected diagnostic is part of the
test oracle, so every row maps to exactly one outcome.

`G(M,T)` writes the exact padded `SEGA MEGA DRIVE` field and `T` at `[0x120,0x150)`; `G(G,T)`
uses the exact padded `SEGA GENESIS` field; `G(U)` writes the exact padded cited-unaccepted
`SEGA 32X` field only. `T(V)` writes the indicated bytes then `0x20` through byte 47; quoted text
is ASCII and `\xHH` denotes that exact byte. `T47(V)` is its first 47 bytes. `S(H,R,L)` writes
`TMR SEGA` at `H` and byte `(R << 4) | L` at `H + 0xF`. Unlisted bytes remain zero.

| Fixture ID | Length and construction | Purpose | Exact outcome / diagnostic |
| --- | --- | --- | --- |
| `empty` | `0x000`; no writes | No candidate baseline | `unrecognized` / `HDR_NOT_RECOGNIZED` |
| `genesis-before-field` | `0x100`; no writes | Genesis required-range start boundary, with no field byte | `unrecognized` / `HDR_NOT_RECOGNIZED` |
| `genesis-short-prefix` | `0x104`; write `SEGA` at `0x100` | Genesis prefix truncation boundary | `rejected` / `HDR_GENESIS_SYSTEM_TRUNCATED` |
| `genesis-one-byte-short` | `0x10F`; first 15 system bytes of exact padded `SEGA MEGA DRIVE` | Genesis end boundary, one byte short | `rejected` / `HDR_GENESIS_SYSTEM_TRUNCATED` |
| `genesis-title-after-system` | `0x110`; write the system field of `G(M,T("SYNTHETIC TITLE"))` | Accepted-system completion boundary, before the title field | `rejected` / `HDR_GENESIS_DOMESTIC_TITLE_TRUNCATED` |
| `genesis-title-before-field` | `0x120`; write the system field of `G(M,T("SYNTHETIC TITLE"))` | Complete accepted system but no domestic-title byte | `rejected` / `HDR_GENESIS_DOMESTIC_TITLE_TRUNCATED` |
| `genesis-title-one-byte-short` | `0x14F`; write its system field and `T47("SYNTHETIC TITLE")` | Domestic-title end boundary, one byte short | `rejected` / `HDR_GENESIS_DOMESTIC_TITLE_TRUNCATED` |
| `genesis-valid` | `0x150`; `G(M,T("SYNTHETIC TITLE"))` | Complete required Genesis fields | `recognized` / `HDR_RECOGNIZED_GENESIS` |
| `genesis-title-escaped-display` | `0x150`; `G(G,T(A\x01))` | Deterministic title display/encoding policy | `recognized` / `HDR_RECOGNIZED_GENESIS`; display `A\x01` |
| `genesis-unsupported-system` | `0x110`; `G(U)` | Complete cited `SEGA`-prefixed but unsupported system field | `rejected` / `HDR_GENESIS_SYSTEM_UNSUPPORTED` |
| `sms-1ff0-short-signature` | `0x1FF8`; `TMR SEGA` at `0x1FF0` | First 8-bit location, signature/full-header boundary | `rejected` / `HDR_8BIT_HEADER_TRUNCATED` |
| `sms-1ff0-one-byte-short` | `0x1FFF`; `TMR SEGA` at `0x1FF0` | First 8-bit region-byte boundary | `rejected` / `HDR_8BIT_HEADER_TRUNCATED` |
| `sms-3ff0-short-signature` | `0x3FF8`; `TMR SEGA` at `0x3FF0` | Second 8-bit location, signature/full-header boundary | `rejected` / `HDR_8BIT_HEADER_TRUNCATED` |
| `sms-3ff0-one-byte-short` | `0x3FFF`; `TMR SEGA` at `0x3FF0` | Second 8-bit region-byte boundary | `rejected` / `HDR_8BIT_HEADER_TRUNCATED` |
| `sms-7ff0-short-signature` | `0x7FF8`; `TMR SEGA` at `0x7FF0` | Third 8-bit location, signature/full-header boundary | `rejected` / `HDR_8BIT_HEADER_TRUNCATED` |
| `sms-7ff0-one-byte-short` | `0x7FFF`; `TMR SEGA` at `0x7FF0` | Third 8-bit region-byte boundary | `rejected` / `HDR_8BIT_HEADER_TRUNCATED` |
| `sms-japan` | `0x2000`; `S(0x1FF0, 0x3, 0xC)` | Accepted SMS Japan code | `recognized` / `HDR_RECOGNIZED_SMS` |
| `sms-export` | `0x2000`; `S(0x1FF0, 0x4, 0x0)` | Accepted SMS Export code | `recognized` / `HDR_RECOGNIZED_SMS` |
| `gg-japan` | `0x2000`; `S(0x1FF0, 0x5, 0xF)` | Accepted GG Japan code | `recognized` / `HDR_RECOGNIZED_GG` |
| `gg-export` | `0x2000`; `S(0x1FF0, 0x6, 0x1)` | Accepted GG Export code | `recognized` / `HDR_RECOGNIZED_GG` |
| `gg-international` | `0x2000`; `S(0x1FF0, 0x7, 0x2)` | Accepted GG International code | `recognized` / `HDR_RECOGNIZED_GG` |
| `invalid-region-{R}` for each `R` in `{0,1,2,8,9,A,B,C,D,E,F}` | `0x2000`; `S(0x1FF0, R, 0x0)` | Deterministic parameterized family: one fixture named by each listed high nibble | `rejected` / `HDR_SMS_GG_REGION_INVALID` for the named `R` |
| `genesis-plus-8bit-partial-magic` | `0x1FF7`; `G(M,T("SYNTHETIC TITLE"))`; write `TMR SEG` at `0x1FF0` | Partial 8-bit magic is not a candidate or collision | `recognized` / `HDR_RECOGNIZED_GENESIS` |
| `genesis-plus-8bit-nonmatching-magic` | `0x2000`; `G(M,T("SYNTHETIC TITLE"))`; write `TMR SEGX` at `0x1FF0` | Nonmatching 8-bit magic is not a candidate or collision | `recognized` / `HDR_RECOGNIZED_GENESIS` |
| `collision-sms` | `0x4000`; `S(0x1FF0, 0x4, 0x0)`, `S(0x3FF0, 0x3, 0x0)` | Same-family multiple headers | `rejected` / `HDR_HEADER_COLLISION` |
| `conflict-sms-gg` | `0x4000`; `S(0x1FF0, 0x4, 0x0)`, `S(0x3FF0, 0x5, 0x0)` | Conflicting 8-bit families | `rejected` / `HDR_HEADER_CONFLICT` |
| `conflict-genesis-sms` | `0x2000`; `G(M,T("SYNTHETIC TITLE"))`, `S(0x1FF0, 0x4, 0x0)` | Genesis/8-bit conflict | `rejected` / `HDR_HEADER_CONFLICT` |
| `valid-plus-invalid` | `0x2000`; `G(M,T("SYNTHETIC TITLE"))`, `S(0x1FF0, 0x2, 0x0)` | Invalid candidate precedence over valid one | `rejected` / `HDR_SMS_GG_REGION_INVALID` |
| `over-limit` | `0x400001`; `G(M,T("SYNTHETIC TITLE"))` | Project size-limit precedence | `rejected` / `IMG_SIZE_LIMIT` |

The matrix deliberately has no executable fixture expectation: classification alone does not
license a later implementation to map, decode, or boot any listed input.
