# MC68000 `MOVE from SR` contract (SEG-007-T116)

**Status:** implementation contract for the `general_startup` decode profile's
one selected System Control Group *status-register read* form.
**CPU covered:** original MC68000 only. **Accessed:** 2026-08-30.

## Sources and the boundary of this contract

### Source facts

1. Motorola, *M68000 Family Programmer's Reference Manual* (1988) -- the
   project-wide `M1` citation defined in
   [`genesis-rom-startup-contract.md`](genesis-rom-startup-contract.md) and
   already used by SEG-007-T059/T085/T088/T114;
   [public scan](https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf),
   accessed 2026-08-30. §6 instruction entry **MOVE from SR -- Move from the
   Status Register**, System Control Group, under "Operation", "Description",
   "Instruction Format", and "Condition Codes":
   - **Operation:** `SR -> Destination`.
   - **Size:** word only. The source operand, the status register, is a word.
   - **Condition Codes:** X, N, Z, V, C all "not affected".
   - **Privilege:** the entry has no privilege-violation line for the
     MC68000. `MOVE from SR` is **unprivileged on the MC68000**; it became a
     privileged instruction only on the MC68010 and later parts (on those
     parts a user-mode `MOVE SR,<ea>` takes a privilege-violation trap).
   - **Destination `<ea>`:** the data-alterable addressing modes -- data
     register direct plus the six memory-alterable modes
     (`(An)`, `(An)+`, `-(An)`, `d16(An)`, `(xxx).W`, `(xxx).L`); address
     register direct, the PC-relative modes, and immediate are not legal
     destinations.
   - **Instruction Format:** the opcode word is `0100 0000 11 mmm rrr`
     (`0x40C0`-`0x40FF`), where `mmm`/`rrr` are the standard effective-address
     mode/register subfields. A register destination consumes no extension
     word.
2. Motorola, *M68000 Family Programmer's Reference Manual* (1988), §3 **Program
   Counter** and the §2 word-alignment material (same scan as source 1):
   instruction words are word aligned.
3. Karl Stenerud, **Musashi**, commit
   `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, `m68kdasm.c` `g_opcode_info`
   table row `{d68000_move_fr_sr, 0xffc0, 0x40c0, 0xbf8}`.
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd>
   (accessed 2026-08-30). Consulted **only** as a structural encoding-table
   cross-check -- the same style SEG-007-T025/T088 already use -- that the
   opcode word is `mask 0xffc0 / base 0x40c0` and that its legal-EA legend
   `0xbf8` is byte-for-byte identical to `CLR`'s own row legend, i.e. the
   data-alterable set (Dn plus the six memory-alterable modes; An-direct,
   PC-relative, and immediate excluded). Musashi is never an authority for
   this instruction's hardware semantics, privilege class, or timing.
4. Alan Clements / Motorola-derived secondary corroboration: prb28,
   *m68k-instructions-documentation*, `instructions/move.md` "MOVE from SR"
   section, <https://github.com/prb28/m68k-instructions-documentation>
   (accessed 2026-08-30). Independently states the same operation
   (`[destination] <- [SR]`), word size, "not privileged in the 68000, but
   ... privileged in the 68010, 68020, and 68030", condition codes X/N/Z/V/C
   all "not affected", and the data-alterable destination-mode row. Used only
   as a second independent statement of the already-primary `M1` facts, per
   this project's console-research triangulation discipline.

The source facts are deliberately separate from the **project policy** sections
below. This contract infers no console mapping, reset-vector layout, timing,
prefetch effect, exception behavior, interrupt effect, or supervisor/user
mode-state model from `MOVE from SR`.

## Hardware contract

For an instruction word, in big-endian byte order:

```text
15                                              0
 0 1 0 0  0 0 0 0  1 1 m m  m r r r     = 0x40C0 | (mode << 3) | reg
```

`MOVE from SR` is a word operation whose source is the fixed 16-bit status
register and whose destination is any data-alterable effective address. Its
only architectural effects are (a) the word write of the current SR value to
the destination and (b) the program counter advancing past the instruction
(and any destination extension words). Condition codes X, N, Z, V, C are not
affected; no other SR bit is affected. On the original MC68000 it is
unprivileged: it executes identically in user and supervisor mode and raises
no privilege-violation exception. A data-register or ordinary-memory
destination raises no exception. This contract models no timing.

## Project decode, provenance, IR, and emission policy

- **Decode.** Only `M68kDecodeProfile::general_startup` selects `MOVE from SR`
  (`M68kInstructionKind::move_from_sr`). The project-selected destination EA
  set is `m68k_ea_move_from_sr_destination`, deliberately narrowed to
  **data-register-direct only** -- the exact shape the authorized
  generated-native route reaches -- mirroring how SEG-007-T088 narrowed
  `m68k_ea_move_to_sr_source` to `Dn`/`#imm`. The full two-byte span is
  bounds-checked before selection; a one-byte `0x40` prefix is
  `truncated_instruction`. `M68kDecodeProfile::genesis_startup` and
  `M68kDecodeProfile::direct_flow` are unchanged: both still reject
  `0x40C0`-`0x40FF`.
- **Provenance.** The decoded instruction retains its `DecodeSource`, raw
  image bytes, and `length`. `destination_ea` carries the decoded `Dn`
  operand (matching the `clr` convention for a write-shaped sole operand);
  `source_ea` is `M68kEaMode::unused` (the source is the implied SR).
- **Lift.** `M68kInstructionKind::move_from_sr` lifts to
  `M68kIrKind::read_status_register`, copying provenance and `destination_ea`
  unchanged.
- **Effect** (`m68k_operation_effect`): `operand_size = word`,
  `resolved_destination_ea = destination_ea`, `affects_condition_codes =
  false` (contrast `write_status_register`, which sets it), `pc = advance`,
  `pc_delta = length`. No `register_write` shortcut, no memory/stack effect at
  this layer.
- **C11 emission** (`emit_m68k_operation_c`): emits
  `<dest> = (<dest> & UINT32_C(0xFFFF0000)) | (((uint32_t)(uint16_t)<sr>) &
  UINT32_C(0xFFFF));` via the existing shared `m68k_emit_ea_write` data-
  register helper (a word write preserves the register's upper 16 bits,
  exactly the hardware word operation), followed by the program-counter
  advance. No SR/CCR write, no device or memory access. Deterministic across
  repeated emissions.

## Bounded System Control Group *status-register read* family inventory

`MOVE from SR` is the reverse direction of the already-`SUPPORTED`
`MOVE <ea>,SR` (SEG-007-T088). It reuses the exact same existing generic
16-bit `sr`/`status_register` runtime field -- no new persistent state, no
privilege/exception/interrupt model. Every neighboring System Control Group
form remains explicitly out of scope for this capability:

| word(s) | mnemonic | status |
| --- | --- | --- |
| `0x40C0`-`0x40FF` (Dn) | `MOVE SR,Dn` | **SUPPORTED** by this task. |
| `0x40C0`-`0x40FF` (memory dest) | `MOVE SR,<mem>` | project-scope exclusion: a memory destination needs this project's C4 static-memory-fact / runtime-routed memory machinery wired for this kind first (same deferral SEG-007-T088 applied to `MOVE to SR`'s memory sources). Falls through to `valid_but_unsupported_instruction`. |
| `0x44C0`-`0x44FF` | `MOVE <ea>,CCR` | **not in scope.** A distinct System Control Group form (source -> CCR; word read but only the low byte updates the CCR; **all** condition codes affected; unprivileged on every 68000-family part). It is the CPU-instruction-semantics frontier the authorized route reaches *behind* the `m68k_discovery_max_instructions` budget once `MOVE from SR` is supported; it is handed off as a same-owner successor, not folded in here. |
| `0x46C0`-`0x46FF` | `MOVE <ea>,SR` | already `SUPPORTED` (SEG-007-T088, `write_status_register`); unchanged by this task. |
| `0x40C0` family, mode 001 (`An`) | -- | An-direct is never a legal `MOVE from SR` destination on any 68000-family part; excluded architecturally, not by project scope. |
| `MOVE from CCR` (`0x42C0`-`0x42FF`) | `MOVE CCR,<ea>` | 68010+ only; not present on the MC68000 at all. Out of scope. |

`MOVE to CCR` is a genuinely distinct form (it *does* affect condition codes
and updates only the CCR sub-field), so it is reported as its own frontier
rather than folded into this `MOVE from SR` batch, matching SEG-007-T114's own
"a later, non-identical form is its own frontier" rule.

## Project-authored synthetic evidence

- `tests/m68k_pipeline_test.cpp`
  `general_startup_decode_accepts_move_from_sr_for_dn_destinations`: decode /
  lift / effect / deterministic double emission for every `Dn` destination,
  plus adversarial negatives for every excluded destination mode
  (`0x40C8` An-direct, `0x40D0`/`0x40D8`/`0x40E0`/`0x40E8`/`0x40F9` memory
  forms), the same-primary-family `NEGX.B` (`0x4000`), the not-in-scope
  `MOVE Dn,CCR` (`0x44C0`), the reverse `MOVE Dn,SR` (`0x46C0`) staying on its
  own `move_to_sr` kind, a truncated one-byte `0x40`, and the unchanged
  `genesis_startup` / `direct_flow` rejection.
- `tests/m68k_batch_c_static_slice_test.py` MOVE-from-SR case: `MOVE SR,D3`
  driven through the shared decode/lift/emission harness with a nonzero-seeded
  D3 upper word and SR, asserting only D3's low word changes
  (`0xDEADBEEF` -> `0xDEAD271F`), `sr` unchanged, `pc` advanced two bytes, no
  emitted `sr = ` assignment, and byte-identical output across repeated
  emission; plus adversarial harness rejection of every non-`Dn` destination
  and the reverse encoding.
- `tests/fixtures/m68k-batch-c-musashi-vectors.json` `move-from-sr-dn-word`
  and `tests/m68k_batch_c_musashi_differential_test.py`: nonzero-seeded
  D/A/SR, `MOVE SR,D3`; when the pinned Musashi checkout
  (`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`) is present the differential
  validates d / a / pc / sr / ram identical to the oracle and zero RAM
  writes.

## Non-inferences

Selecting `MOVE from SR` is not a claim that this project models MC68000
instruction timing, prefetch, the trace bit, interrupt sampling,
supervisor/user mode state, privilege-violation exception delivery, or any
neighboring System Control Group form. It does not widen `genesis_startup` or
`direct_flow`. `MOVE to CCR`, `MOVE from CCR`, memory-destination `MOVE from
SR`, `RESET`, and `STOP` remain unsupported. No interpreter, JIT, or runtime
opcode decoder is introduced; the generated `MOVE from SR` lowering is a pure
static SR-word read into a data register plus a program-counter advance.
