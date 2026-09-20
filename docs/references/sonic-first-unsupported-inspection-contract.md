# Sonic first-unsupported inspection contract (SEG-006-T001)

**Status:** research finding for SEG-006-T001; it supplies no implementation, fixture, or Sonic
content. **Platform/CPU:** Genesis/Mega Drive, original MC68000 only. **Accessed:** 2026-08-09.

This records a single hash-only, locally reproducible run of the existing SEG-005/SEG-012 shared
static route (`genesis-rom-startup`) against one authorized local Sonic the Hedgehog Genesis image,
and the one deterministic first-unsupported stop it reaches. It does not implement, render, or claim
any Sonic-specific capability; it only observes the existing bounded pipeline's fail-closed boundary,
locally establishes the observed primary's semantic classification, proposes exactly one next bounded
reusable CPU capability for a later task, and separately records a known subsequent architecture
boundary (the current startup analyzer's fixed SEG-005 graph shape) that recognizing that one
capability alone does not resolve.

## Authorized input identification (hash-only, no path)

The authorized local input is identified **only** by its SHA-256 digest, per
`docs/testing/commercial-games.md`. No local filesystem path, filename, or ROM byte appears in this
document or in the linked backlog evidence.

```text
sha256: 46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6
```

This is the digest of the entire input file as read from the user's local, gitignored `games/`
tree (see `docs/testing/commercial-games.md`). The digest, not any path or filename, is the sole
identifier used for reproduction below.

## Existing route (SEG-005/SEG-012 shared ingress and selected subset, inherited verbatim)

This task reuses the shared SEG-005/SEG-012 static route's selected memory map, selected
instruction subset, and its exact evaluation order verbatim; it defines no new stage, map entry, or
instruction form. See `docs/references/genesis-rom-startup-contract.md` for the full contract,
summarized here only to the extent needed to interpret the finding:

- **Selected map:** `raw_cartridge_rom` at `[0x000000, image_length)` (instruction/data read, no
  write); `synthetic_work_ram` at canonical `[0x00ff0000, 0x01000000)` (data read/write only).
- **Selected instruction subset:** `MOVEQ #s8,D0` (`70 ii`); `MOVE.L D0,(xxx).L` (`23 C0 ...`);
  `MOVE.L (xxx).L,D1` (`22 39 ...`); direct `JSR (xxx).L` (`4E B9 ...`); selected `RTS` (`4E 75`),
  with `RTS` accepted only as the statically paired return of a selected direct `JSR`.
- **Fail-closed evaluation order** for a mapped source: (1) wrong typed space/CPU, (2) odd
  instruction address, (3) unmapped instruction address, (4) primary truncation, (5) source-defined
  `ILLEGAL` primary `4AFC`, (6) primary classification, (7) selected-instruction extension
  validation. Primary classification (using only the complete two-byte primary) is: a primary in a
  selected opcode family but not this slice's selected form is `unsupported_instruction_form`; an
  exact selected form proceeds to extension validation; every unrelated complete primary is
  `valid_but_unsupported_instruction`.

This task adds no new region, instruction form, or ordering rule; it exercises the existing rule
against one previously untested authorized commercial input.

## Reproduction procedure (hash-only)

A validator with independent legal access to a Genesis Sonic the Hedgehog image whose SHA-256
matches the digest above can reproduce this finding without any additional information:

```sh
cmake --preset dev
cmake --build --preset dev
sha256sum <local-path-to-authorized-image>   # must equal the pinned digest above
./build/dev/segarecomp genesis-rom-startup <local-path-to-authorized-image>
```

The command is expected to exit nonzero and print a single JSON error object to stderr whose
`category` and `source_address` match the recorded finding below. `image_offset` is a numeric
target-address-derived fact (not ROM content) and is reproducible from `source_address` alone once
the same reset-vector-derived entry point is reached; it is recorded here for convenience.

### Independent oracle identity (pinned, not invoked in this task)

This task pins, but does not itself invoke, the same independent MC68000 execution oracle SEG-005
established:

```text
oracle: Karl Stenerud, Musashi, commit 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
```

(See source O1 below and `docs/references/genesis-rom-startup-contract.md`'s "Independent Musashi
oracle" section and `tests/genesis_rom_startup_static_slice_test.py`'s `ORACLE_REVISION` /
`ORACLE_ID`, which pin the identical revision.) A later validation task would reproduce this finding
hash-only by: (1) confirming the authorized local image's SHA-256 equals the digest above, (2)
confirming a local Musashi checkout's `git rev-parse HEAD` equals the pinned commit, (3) running the
same `genesis-rom-startup` CLI command shown above, and (4) confirming the returned `category` and
`source_address` match the recorded finding, without ever needing the ROM's path, bytes, or any
Sonic-derived artifact to travel through version control.

## First unsupported observation (category/address/image offset, no raw bytes)

Running the shared static route's reset ingress against the pinned image deterministically halts at
the very first non-reset instruction boundary the route attempts to decode, before any selected
MOVEQ/MOVE/JSR/RTS form is reached:

```text
category:        valid_but_unsupported_instruction
source_address:  0x00000206
image_offset:    518
```

Per the reused stop rule above, `valid_but_unsupported_instruction` at primary classification means:
the reset vector is a valid, even, mapped ROM address; the two-byte primary opcode at that address
is completely decodable (no truncation, not `ILLEGAL`); but that primary does not belong to any of
this slice's selected opcode families (`MOVEQ`, the two selected `MOVE.L` absolute-long forms, or
the `JSR`/`RTS` family). This is the earliest possible non-reset stop the route can reach: Sonic's
real reset code does not happen to open with any of SEG-005's five narrowly selected forms, so the
route fails closed on its very first decoded instruction. No raw instruction bytes, mnemonic-level
disassembly, or other ROM content is recorded here; only the numeric stop category and the numeric
source address/image offset, which describe the shape of the boundary and are not copyrightable
expression.

## Semantic classification: two distinct evidence owners

Because the earlier draft of this record blurred two different facts together — implying the
production decoder itself understood the primary as `TST` — this section makes the provenance of
each fact explicit and keeps them separate.

### Fact owner 1: the production `segarecomp` decoder (generic boundary only)

`decode_m68k_instruction`'s `genesis_startup` profile (`src/m68k_pipeline.cpp`) contains **no**
semantic instruction classifier for `TST` or any other unselected family. It only establishes,
generically, that the primary at `0x00000206` is: a complete two-byte word (no truncation), not the
source-defined `ILLEGAL` primary `4AFC`, not a match for any of this slice's five `select(...)`
branches (`moveq`, `move_l_d0_absolute_long`, `move_l_absolute_long_d1`, `jsr_absolute_long`, `rts`),
and not a match for any of the `unsupported_instruction_form` bitmask patterns
(`(word & 0xFFC0) == 0x4E80`, `(word & 0xF000) == 0x2000`, `(word & 0xF100) == 0x7000`). The
production decoder's sole output is therefore the generic category already recorded above:
`valid_but_unsupported_instruction` at `0x00000206` / image offset `518`. It does not itself decide,
and this document does not claim it decides, that the primary is `TST`.

### Fact owner 2: independent local research classification (public Motorola predicates applied to a private primary)

Separately from the production decoder, this task's local research reads the same already-located
16-bit primary at the same already-established offset and interprets it, off-tree, against the
publicly documented Motorola MC68000 encoding rules for `TST` (see M1/M3 below), yielding:

```text
observed_instruction_family:    TST
observed_size:                  long
observed_addressing_mode:       absolute_long
```

This classification is derived, not guessed, and derived by research applying public predicates to a
private value — not by the shipped decoder. See "Reproducible local-only semantic classification
procedure" and "Public encoding predicates" below for the exact, independently auditable steps and
bit-field rules used to reach it. No opcode word, byte pair, disassembly mnemonic, or ROM-derived
operand value is reproduced in this document or in the linked backlog evidence; only the three
semantic facts above are recorded, which are the minimum needed to select the next reusable
capability and are not themselves copyrightable expression.

## Reproducible local-only semantic classification procedure

A validator with independent legal access to the exact SHA-256-matching image can reproduce the
`TST`/`long`/`absolute_long` classification above, entirely locally, without any additional
information beyond this document and the public Motorola manual:

```text
1. Confirm the entire input's SHA-256 matches the pinned digest above.
2. Run: segarecomp genesis-rom-startup <authorized-local-image>
3. Confirm the failure is category valid_but_unsupported_instruction at source_address
   0x00000206 / image_offset 518 (the production decoder's generic boundary; see "Fact owner 1"
   above).
4. Locally read only the 16-bit big-endian primary word already located at that established
   offset (any local tool suffices; the tool used is not standardized and need not be recorded).
5. Apply the public Motorola MC68000 TST encoding predicates below to that private word:
   a. verify the primary belongs to the TST family (not an unrelated line-4 form such as TAS or
      ILLEGAL);
   b. decode the size field and require it selects "long";
   c. decode the effective-address mode/register fields and require they select "absolute long".
6. Record only the three semantic facts:
   observed_instruction_family: TST
   observed_size: long
   observed_addressing_mode: absolute_long
7. Do not print, commit, log, or paste: the primary/opcode value itself, any ROM byte, any
   disassembly, any extension word, any operand/address value, or any surrounding code.
```

## Public encoding predicates (auditable against the primary Motorola source, no opcode value inserted)

The predicates below are the exact, symbolic bit-field rules a reviewer applies to some private
16-bit word `W` from a legally held image to independently prove `family(W) == TST`,
`size(W) == long`, and `EA(W) == absolute_long`, per the MC68000 `TST` instruction format published
in M1/M3. They are stated symbolically; no observed opcode value is inserted into them.

The published `TST` instruction word format is `0100 1010 SS MMM RRR` (bits 15-0, most significant
first): a fixed 8-bit family prefix `0100 1010` in bits 15-8, a 2-bit size field `SS` in bits 7-6, a
3-bit effective-address mode field `MMM` in bits 5-3, and a 3-bit effective-address register field
`RRR` in bits 2-0. The manual assigns `SS = 00` to byte, `01` to word, and `10` to long (`11` in this
same bit position instead selects the unrelated `TAS` instruction, per M3, and must be excluded from
the `TST` family test). The manual assigns effective-address mode `111` with register `001` to
absolute-long addressing (the same mode/register pair the existing pipeline already validates for
`MOVE.L`'s absolute-long forms).

```text
TST_FAMILY_PREFIX_MASK   = 0xFF00
TST_FAMILY_PREFIX_VALUE  = 0x4A00
SIZE_FIELD_MASK           = 0x00C0   (bits 7-6)
LONG_SIZE_VALUE            = 0x0080  (0b10, shifted into bits 7-6)
EA_FIELD_MASK              = 0x003F  (bits 5-0: mode || register)
ABSOLUTE_LONG_EA_VALUE     = 0x0039  (mode 0b111, register 0b001)
TAS_SIZE_FIELD_VALUE       = 0x00C0  (0b11, shifted into bits 7-6; excluded from TST)

family(W)  == TST         iff (W & TST_FAMILY_PREFIX_MASK) == TST_FAMILY_PREFIX_VALUE
                           AND (W & SIZE_FIELD_MASK) != TAS_SIZE_FIELD_VALUE
size(W)    == long        iff (W & SIZE_FIELD_MASK) == LONG_SIZE_VALUE
EA(W)      == absolute_long iff (W & EA_FIELD_MASK) == ABSOLUTE_LONG_EA_VALUE
```

These masks were checked directly against the cited manual's `TST` instruction format (M1) and
opcode map (M2/M3) before being documented here, and were not invented ad hoc. They are the same
mode/register absolute-long encoding the existing pipeline already implements and validates for
`MOVE.L D0,(xxx).L` / `MOVE.L (xxx).L,D1` (`src/m68k_pipeline.cpp`), so this document introduces no
new effective-address decoding rule, only the additional `TST`-specific family-prefix and size-field
predicates. Applying these three predicates to the private primary located at `0x00000206` is what
this task's local research did to reach the semantic classification above; the private word itself
is not recorded anywhere in this document, the linked backlog evidence, or version control.

## Current architecture boundary: `analyze_startup_profile` remains bounded to the exact SEG-005 graph

`analyze_startup_profile` (`src/m68k_pipeline_frontend.cpp`) decodes the entry instruction and
immediately requires `first->kind == M68kInstructionKind::moveq`; any other decoded kind — including
a hypothetically newly selected `TST` form — is rejected as `startup_graph_mismatch`, not accepted
as forward progress. The same function's subsequent roles are equally fixed: the second decoded
instruction must be `move_l_d0_absolute_long`, the third a validated `jsr_absolute_long` whose
statically paired return is exactly `rts` (via `discover_m68k_static_call_return`), and the fifth
`move_l_absolute_long_d1`. `emit_m68k_frontend_c`'s `genesis_rom_startup` branch
(`src/m68k_pipeline_frontend.cpp`) is equally bounded: it requires `analysis.decoded.size() == 5`,
`analysis.ir.size() == 5`, and the exact IR-kind sequence `write_moveq`, `write_d0_absolute_long`,
`call_absolute_long`, `return_from_subroutine`, `read_absolute_long_d1`, in that order, or it rejects
the translation.

Therefore, recognizing `TST` absolute-long as a selected decode form, by itself and without any
change to `analyze_startup_profile` or `emit_m68k_frontend_c`, would **not** let the shared startup
route advance further into Sonic's real reset code. Because Sonic's first decoded instruction is
`TST`, not `MOVEQ`, adding only `TST` support would change the observed category at
`0x00000206` from `valid_but_unsupported_instruction` to `startup_graph_mismatch` — a different,
still-rejecting stop at the identical address, not "Sonic executes one instruction further." This is
an accurate compatibility finding about the current fixed-shape startup analyzer, not a defect in
SEG-005, which intentionally proved one bounded synthetic five-operation vertical slice
(the SEG-005 backlog records) rather than general startup discovery.

## Public hardware locators for the observed stop category and classification

| ID | Public source, URL, access date, and locator | Fact used here |
| --- | --- | --- |
| M1 | Motorola, *M68000 Family Programmer's Reference Manual* (1988), [public scan](https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf), accessed 2026-08-09; §2 **Addressing Modes**; §6, entries for **MOVEQ**, **MOVE**, **JSR**, **RTS**, **TST**, and **ILLEGAL**, under "Instruction Format" and "Operation". | The MC68000 opcode map partitions two-byte primary opcodes into distinct instruction-family regions (e.g. the `MOVEQ` family, the `MOVE`-derived families, the line-`4` miscellaneous/`JSR`/`RTS`/`TST`/`ILLEGAL` family), each with its own operand/addressing-mode encoding and, for `TST`, an explicit size field (byte/word/long) and effective-address field (including absolute-long); a primary opcode belonging to one family is not a member of, and cannot be reinterpreted as, an unrelated family such as `MOVEQ` or a selected `MOVE.L`/`JSR`/`RTS` form. |
| M2 | Motorola, *M68000 Family Programmer's Reference Manual* (1988), same scan as M1; Appendix B, **Instruction Set Summary**, opcode map table. | The published opcode map is the authoritative public catalog of which two-byte primary values belong to which instruction family; SEG-005's five selected forms occupy only a small, disjoint subset of that map, so any reset code whose leading instructions are drawn from the remaining, much larger map necessarily first hits an unrelated family under this slice's fail-closed classification. |
| M3 | Motorola, *M68000 Family Programmer's Reference Manual* (1988), same scan as M1; §6, **TST** and **TAS** entries, "Instruction Format" bit-field diagrams (opcode bits 15-0: `0100 1010 SS MMM RRR` for `TST`; the same `0100 1010 11 MMM RRR` region for `TAS`). | The exact bit-field layout used by the "Public encoding predicates" section below: the fixed 8-bit `0100 1010` family prefix in bits 15-8, the 2-bit size field in bits 7-6 (`00`=byte, `01`=word, `10`=long, `11`=reserved for the distinct `TAS` instruction rather than `TST`), and the 3+3-bit effective-address mode/register fields in bits 5-0 (mode `111`/register `001` selects absolute-long addressing), which are the public predicates a validator applies to a private primary word to classify it, without needing to disclose that word. |

No hardware claim beyond public MC68000 opcode-map structure and the public `TST`/`TAS` instruction
format is made. This document does not reproduce the opcode word, any disassembly, or Sonic's
source code; it records only the semantic classification and the public predicates used to reach it.

## Next bounded reusable capability

**Capability:** add the `TST` instruction family's **absolute-long, long-size addressing form** to
the shared selected-instruction subset (decode → lift → shared `M68kIrOperation` execution effect →
structured-C lowering via `emit_m68k_operation_c` → synthetic fixture/unit tests → Musashi
differential), following the same typed provenance/EA-validation sequence
(`effective_address_not_24bit`, `odd_effective_address`, `unmapped_data_access`) SEG-005 already
defined for absolute-long data operands. None of that decode/lift/emission/test work is performed by
this task; T001 is inspection-only.

This is bounded to "one instruction family" per the milestone's ceiling (no broader than one
instruction family, two addressing forms, one analysis/flow capability, or one memory/device
boundary): it adds exactly one instruction family (`TST`) using exactly one addressing mode
(absolute-long) and one size (long) that this slice's operand-validation contract already fully
specifies for a different instruction (`MOVE.L`). It requires no new memory region, no new
call/return machinery, and reuses the existing absolute-long EA-resolution and validation order
verbatim. It is justified directly by this task's locally established semantic classification above,
not by convenience: the observed primary is `TST` absolute-long long, so that is the capability
selected, not a broader or narrower one.

Adding this capability alone does **not** advance Sonic past `0x00000206` under the current startup
analyzer (see "Current architecture boundary" above); its value is expanding the shared, reusable
MC68000 capability set, verifiable independently of Sonic via synthetic fixtures and the pinned
Musashi oracle, matching this project's general recompiler mission rather than a Sonic-specific
carve-out.

## Known subsequent boundary (explicitly outside this task and outside the next CPU-capability task)

Separately from the next CPU capability, `analyze_startup_profile`'s `genesis_rom_startup` profile
remains fixture-shaped: it accepts only the exact five-operation SEG-005 graph
(`MOVEQ` → `MOVE.L D0,(xxx).L` → `JSR (xxx).L` → `RTS` → `MOVE.L (xxx).L,D1`) in that fixed order.
Recognizing `TST` as a decodable form will surface `startup_graph_mismatch` at `0x00000206` (in place
of today's `valid_but_unsupported_instruction`) until startup analysis itself supports bounded
general static discovery beginning at the reset entry point, rather than one hardcoded graph shape.
This boundary is a known future compatibility frontier; it is **not** part of the next CPU-capability
task's scope (adding `TST` decode/lift/emission/tests does not require or imply rewriting startup
discovery), and it is not designed here. A sufficient forward statement: future startup compatibility
must eventually replace exact SEG-005 fixture-shape matching with bounded static program discovery
that begins at the reset entry, consumes only selected shared MC68000 operations and the existing
typed static edges (`M68kStaticEdge`, `discover_m68k_static_call_return`), and fails closed at the
first unsupported operation, flow edge, or memory boundary — reusing SEG-003's static direct-flow
machinery and SEG-005-T004/T006's call/return and static-frame discovery as the most likely existing
architecture rather than a new CFG engine. This document does not propose supporting Sonic's full
control-flow graph, and does not design the discovery engine.

## Unknown following boundary

What the shared route would encounter after `0x00000206` — whether the addressed `TST` operand
falls inside the currently selected `synthetic_work_ram`/`raw_cartridge_rom` map, what instruction
follows, or what the next unsupported boundary would be — is intentionally **not** claimed by this
task. Determining the operand's target region, or any instruction beyond this one, would require
either committing further commercial-derived detail that this record avoids, or capability/analysis
work reserved for later tasks. Whether the observed instruction's operand resolves inside the
currently selected data map is intentionally not claimed by T001; this becomes the next fail-closed
boundary to evaluate once instruction recognition and bounded startup discovery permit the access to
be evaluated at all.

## Explicit non-goals

- This task does not implement, emit, or validate `TST` or any other new instruction form, and does
  not implement bounded startup discovery; both are reserved for later tasks (e.g. a future
  SEG-006-T002 for the CPU capability) if selected.
- This task does not claim Sonic the Hedgehog boots, executes correctly, advances past
  `0x00000206`, or is broadly supported by this project in any capacity beyond the one deterministic
  first-unsupported stop and its semantic classification recorded above.
- This task commits, prints, or logs no ROM bytes, opcode words, disassembly, extracted assets,
  screenshots, traces, or other Sonic-derived artifact, and no local filesystem path.
- This task does not invoke the Musashi oracle; it pins the oracle's identity/protocol only, exactly
  as SEG-005 already established, so a later task can reproduce this finding hash-only.
- This task does not modify, rename, move, or duplicate any file under `games/`.
- This task does not add, change, or reinterpret any SEG-005 map entry, instruction form, or
  evaluation order; it reuses them unchanged.
- This task does not propose a general Sonic CFG engine, a Sonic-specific decoder/executor/emitter,
  or any bypass of the shared decode/lift/analysis/emission pipeline.
