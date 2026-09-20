# MC68000 `MOVE <ea>,CCR` contract (SEG-007-T118)

**Status:** implementation contract for the `general_startup` decode profile's
one selected System Control Group *condition-code-register write* form.
**CPU covered:** original MC68000 (the semantics are identical on every
68000-family part). **Accessed:** 2026-08-30.

## Sources and the boundary of this contract

### Source facts

1. Motorola, *M68000 Family Programmer's Reference Manual* (1988) -- the
   project-wide `M1` citation defined in
   [`genesis-rom-startup-contract.md`](genesis-rom-startup-contract.md) and
   already used by SEG-007-T059/T085/T088/T114/T116;
   [public scan](https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf),
   accessed 2026-08-30. §6 instruction entry **MOVE to CCR -- Move to Condition
   Code Register**, System Control Group, under "Operation", "Description",
   "Instruction Format", and "Condition Codes":
   - **Operation:** `Source -> CCR`.
   - **Size:** word only. The source operand is read as a word; only its
     low-order byte is moved to the CCR and the upper byte is ignored. (Note
     `ANDI`/`ORI`/`EORI` to CCR are byte operations, but `MOVE to CCR` is a
     word operation.)
   - **Condition Codes:** X, N, Z, V, C are **all set** to the corresponding
     bits of the source operand (bits 4..0). Every condition code is replaced
     wholesale.
   - **Privilege:** the entry has no privilege line. `MOVE to CCR` is
     **unprivileged on every 68000-family part** (unlike `MOVE to SR`, which is
     privileged on all parts, and `MOVE from SR`, which is privileged from the
     MC68010). It executes identically in user and supervisor mode and raises
     no privilege-violation exception.
   - **Source `<ea>`:** the data addressing modes -- data register direct plus
     every memory mode plus the two PC-relative modes plus immediate; address
     register direct is not a legal source.
   - **Instruction Format:** the opcode word is `0100 0100 11 mmm rrr`
     (`0x44C0`-`0x44FF`), where `mmm`/`rrr` are the standard effective-address
     mode/register subfields. A register source consumes no extension word.
   - Only the CCR sub-field (low byte) of the 16-bit SR changes; the upper
     (system) byte of SR -- supervisor bit, trace bit, interrupt-mask bits --
     is untouched.
2. Motorola, *M68000 Family Programmer's Reference Manual* (1988), §3 **Program
   Counter** and the §2 word-alignment material (same scan as source 1):
   instruction words are word aligned.
3. Karl Stenerud, **Musashi**, commit
   `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, `m68kdasm.c` `g_opcode_info`
   table row `{d68000_move_to_ccr, 0xffc0, 0x44c0, 0xbff}`.
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd>
   (accessed 2026-08-30). Consulted **only** as a structural encoding-table
   cross-check -- the same style SEG-007-T025/T088/T116 already use -- that the
   opcode word is `mask 0xffc0 / base 0x44c0` and that its legal-EA legend
   `0xbff` is the data addressing-mode set (Dn plus every memory mode plus the
   PC-relative modes plus immediate; An-direct excluded). The `d68000_`
   name-prefix and the absence of a `LIMIT_CPU_TYPES` guard in that row's
   handler independently corroborate that the instruction is present and
   unprivileged on the base MC68000. Musashi is never an authority for this
   instruction's hardware semantics, privilege class, or timing.
4. prb28, *m68k-instructions-documentation*, `instructions/move.md` "MOVE to
   CCR" section, <https://github.com/prb28/m68k-instructions-documentation>
   (accessed 2026-08-30). Independently states the same operation
   (`[CCR] <- [source]`), word size ("The source operand is a *word*, but only
   the low-order *byte* contains the condition codes. The upper byte is
   neglected."), all five condition codes set from the source (`* * * * *`),
   and the data addressing-mode source row (An-direct excluded). Used only as a
   second independent statement of the already-primary `M1` facts, per this
   project's console-research triangulation discipline.

The source facts are deliberately separate from the **project policy** sections
below. This contract infers no console mapping, reset-vector layout, timing,
prefetch effect, exception behavior, interrupt effect, or supervisor/user
mode-state model from `MOVE to CCR`.

## Hardware contract

For an instruction word, in big-endian byte order:

```text
15                                              0
 0 1 0 0  0 1 0 0  1 1 m m  m r r r     = 0x44C0 | (mode << 3) | reg
```

`MOVE <ea>,CCR` is a word operation whose source is any data effective address
and whose destination is the implied condition code register (the low byte of
SR). Its only architectural effects are (a) replacing all five condition codes
(X, N, Z, V, C) with bits 4..0 of the source operand's low byte and (b) the
program counter advancing past the instruction (and any source extension
words). The upper (system) byte of SR is not affected; no other SR bit is
affected. It is unprivileged on every 68000-family part and raises no
exception for a data-register or ordinary-memory source. This contract models
no timing.

## Project decode, provenance, IR, and emission policy

- **Decode.** Only `M68kDecodeProfile::general_startup` selects `MOVE <ea>,CCR`
  (`M68kInstructionKind::move_to_ccr`). The project-selected source EA set is
  `m68k_ea_move_to_ccr_source`, deliberately narrowed to **data-register-direct
  only** -- the exact shape the authorized generated-native route reaches --
  mirroring how SEG-007-T088 narrowed `m68k_ea_move_to_sr_source` and
  SEG-007-T116 narrowed `m68k_ea_move_from_sr_destination` to `Dn`. The full
  two-byte span is bounds-checked before selection; a one-byte `0x44` prefix is
  `truncated_instruction`. `M68kDecodeProfile::genesis_startup` and
  `M68kDecodeProfile::direct_flow` are unchanged: both still reject
  `0x44C0`-`0x44FF`.
- **Provenance.** The decoded instruction retains its `DecodeSource`, raw image
  bytes, and `length`. `source_ea` carries the decoded `Dn` operand;
  `destination_ea` is `M68kEaMode::unused` (the destination is the implied
  CCR).
- **Lift.** `M68kInstructionKind::move_to_ccr` lifts to
  `M68kIrKind::write_condition_codes`, copying provenance and `source_ea`
  unchanged.
- **Effect** (`m68k_operation_effect`): `operand_size = word`,
  `resolved_source_ea = source_ea`, `affects_condition_codes = true` (like
  `write_status_register`, unlike `read_status_register`), `pc = advance`,
  `pc_delta = length`.
- **C11 emission** (`emit_m68k_operation_c`): emits
  `sr = (uint16_t)((sr & UINT16_C(0xFF00)) | ((<src>) & UINT16_C(0x001F)));`
  via the existing shared `m68k_emit_ea_read` data-register helper (the read is
  the same plain, non-materialized EA read `write_status_register` uses),
  followed by the program-counter advance. Only the five implemented CCR bits
  (X/N/Z/V/C, bits 4..0) of the runtime `sr` field are written -- CCR bits 7..5
  are unimplemented on every 68000-family part and always read 0, so the mask is
  `0x001F`, **not** `0x00FF`; this is what keeps the generated program
  byte-identical to the pinned Musashi oracle, whose `m68ki_set_ccr`
  (`.tools/musashi/m68kcpu.h`) consults only `BIT_4..BIT_0`. The upper (system)
  byte of SR is preserved. No device or memory access. Deterministic across
  repeated emissions.

## Bounded System Control Group *CCR/SR read-write* family inventory

`MOVE <ea>,CCR` is the write-direction mirror of the already-`SUPPORTED`
source-narrowed `MOVE <ea>,SR` (SEG-007-T088) and shares the exact same
existing generic 16-bit `sr`/`status_register` runtime field -- no new
persistent state, no privilege/exception/interrupt model. Every neighboring
System Control Group form remains explicitly out of scope for this capability:

| word(s) | mnemonic | status |
| --- | --- | --- |
| `0x44C0`-`0x44FF` (Dn) | `MOVE Dn,CCR` | **SUPPORTED** by this task. |
| `0x44C0`-`0x44FF` (memory / PC-relative / immediate source) | `MOVE <ea>,CCR` | project-scope exclusion: a non-`Dn` source needs this project's C4 static-memory-fact / runtime-routed memory machinery wired for this kind first (same deferral SEG-007-T088/T116 applied). Falls through to `valid_but_unsupported_instruction`. |
| `0x40C0`-`0x40FF` | `MOVE SR,<ea>` | already `SUPPORTED` (SEG-007-T116, `read_status_register`); unchanged by this task. |
| `0x46C0`-`0x46FF` | `MOVE <ea>,SR` | already `SUPPORTED` (SEG-007-T088, `write_status_register`); unchanged by this task. |
| `0x42C0`-`0x42FF` | `MOVE CCR,<ea>` | 68010+ only; not present on the MC68000 at all. Out of scope. |
| `0x44C0` family, mode 001 (`An`) | -- | An-direct is never a legal `MOVE to CCR` source on any 68000-family part; excluded architecturally, not by project scope. |
| `ANDI/ORI/EORI to CCR` (`0x023C` / `0x003C` / `0x0A3C`) | -- | distinct byte-operation immediate-logical forms; not selected. |

## Project-authored synthetic evidence

- `tests/m68k_pipeline_test.cpp`
  `general_startup_decode_accepts_move_to_ccr_for_dn_sources`: decode / lift /
  effect / deterministic double emission for every `Dn` source, plus
  adversarial negatives for every excluded source mode (`0x44C8` An-direct,
  `0x44D0`/`0x44D8`/`0x44E0`/`0x44E8`/`0x44F9` memory, `0x44FC` immediate), the
  same-primary-family `NEGX.B` (`0x4400`), the reverse `MOVE Dn,SR` (`0x46C0`)
  staying on `move_to_sr`, `MOVE SR,D3` (`0x40C3`) staying on `move_from_sr`, a
  truncated one-byte `0x44`, and the unchanged `genesis_startup` /
  `direct_flow` rejection; and
  `general_startup_retains_move_to_ccr_before_a_later_cpu_frontier`
  (bridge-emission retention before an independent later CPU frontier).
- `tests/m68k_batch_c_static_slice_test.py` MOVE-to-CCR case: `MOVE D2,CCR`
  driven through the shared decode/lift/emission harness with D2 seeded so its
  low byte has bits 7..5 set (`0xDEADBEE0`) and a nonzero SR upper byte,
  asserting only the implemented CCR bits of `sr` change
  (`0xA700` -> `0xA700`: upper byte preserved, `0xE0 & 0x1F == 0x00`; a buggy
  `0x00FF` mask would instead yield `0xA7E0`), D2 unchanged, `pc` advanced two
  bytes, the masked
  `sr = (uint16_t)((sr & UINT16_C(0xFF00)) | ((d[2]) & UINT16_C(0x001F)));`
  form emitted, and byte-identical output across repeated emission; plus
  adversarial harness rejection of every non-`Dn` source and the reverse
  encoding.
- `tests/fixtures/m68k-batch-c-musashi-vectors.json` `move-to-ccr-dn-word`
  (D2 low byte `0xFF`, exercising the `0x001F` mask against a maximally-set
  source) and `move-to-ccr-dn-word-lowbits` (D2 low byte `0x0D`, only
  implemented CCR bits), driven by
  `tests/m68k_batch_c_musashi_differential_test.py`: nonzero-seeded D/A/SR,
  `MOVE D2,CCR`; when the pinned Musashi checkout
  (`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`) is present the differential
  validates d / a / pc / sr / ram identical to the oracle and zero RAM writes.

## Non-inferences

Selecting `MOVE <ea>,CCR` is not a claim that this project models MC68000
instruction timing, prefetch, the trace bit, interrupt sampling,
supervisor/user mode state, privilege-violation exception delivery, or any
neighboring System Control Group form. It does not widen `genesis_startup` or
`direct_flow`. `MOVE CCR,<ea>`, `MOVE from CCR`, memory/PC-relative/immediate
source `MOVE to CCR`, `RESET`, and `STOP` remain unsupported. No interpreter,
JIT, or runtime opcode decoder is introduced; the generated `MOVE to CCR`
lowering is a pure static `0x001F`-masked write of the source's low five bits into the CCR
sub-field of `sr` plus a program-counter advance.
