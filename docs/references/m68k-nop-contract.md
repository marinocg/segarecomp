# MC68000 `NOP` contract (SEG-007-T114)

**Status:** implementation contract for the `general_startup` decode profile's
one selected no-operand System Control Group form.
**CPU covered:** original MC68000 only. **Accessed:** 2026-08-29.

## Sources and the boundary of this contract

### Source facts

1. Motorola, *M68000 Family Programmer's Reference Manual* (1988) -- the
   project-wide `M1` citation defined in
   [`genesis-rom-startup-contract.md`](genesis-rom-startup-contract.md) and
   already used by SEG-007-T059/T085/T088;
   [public scan](https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf),
   accessed 2026-08-29. §6 instruction entry **NOP -- No Operation**, System
   Control Group, under "Operation", "Description", and "Condition Codes": "No
   operation occurs. The processor state, other than the program counter, is
   unaffected. Execution continues with the instruction following the NOP
   instruction." Its condition-code table records X, N, Z, V, C all as "not
   affected". The entry has no privilege-violation line (NOP is
   **unprivileged**), a fixed 16-bit opcode word, and no operands or extension
   words.
2. Motorola, *M68000 Family Programmer's Reference Manual* (1988), §3 **Program
   Counter** and the §2 word-alignment material (same scan as source 1). These
   establish that instruction words are word aligned.
3. Karl Stenerud, **Musashi**, commit
   `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, `m68kdasm.c` `g_opcode_info`
   table row for `nop` (mask `0xffff`, match `0x4e71`).
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd>
   (accessed 2026-08-29). Consulted **only** as a structural encoding-table
   cross-check that the opcode word is a single fully-fixed 16-bit pattern with
   no mode/register/size sub-fields -- the same encoding-structure citation
   style SEG-007-T025/T088 already use. Musashi is never an authority for NOP's
   hardware semantics, privilege class, or timing here.

The source facts are deliberately separate from the **project policy** sections
below. This contract infers no console mapping, reset-vector layout, timing,
prefetch effect, exception behavior, or interrupt effect from `NOP`.

## Hardware contract

For an instruction word, in big-endian byte order:

```text
15                                              0
 0 1 0 0  1 1 1 0  0 1 1 1  0 0 0 1     = 0x4E71
```

`NOP` has no operands, no extension words, and no addressing mode (implied). Its
encoded length is one word (two bytes), so the only architectural effect is that
the program counter advances to `PC + 2`. It performs no memory or device
access other than its own instruction fetch. Condition codes X, N, Z, V, C are
not affected; no other SR bit is affected. It is unprivileged: it executes
identically in user and supervisor mode and raises no exception. This contract
models no timing.

## Bounded no-operand System Control Group family inventory

`NOP` is the **only** no-operand System Control Group form implementable through
the existing shared `decode -> lift -> IR -> C11` owner
(`M68kInstructionKind` / `M68kIrKind`) with no new architecture. Every other
no-operand form in that opcode neighborhood is explicitly out of scope for this
capability because each needs a distinct architecture decision that the project charter
and this task's Non-goals forbid:

| word | mnemonic | why out of scope |
| --- | --- | --- |
| `0x4E70` | `RESET` | privileged; asserts the external RESET line -- external-device-reset architecture. Remains a recognized `M68kCpuFrontierKind::reset` CPU frontier. |
| `0x4E72` | `STOP #imm` | privileged; loads SR and halts -- privilege + halt/interrupt-wait architecture; also not no-operand (immediate extension word). Remains `M68kCpuFrontierKind::stop_immediate_word`. |
| `0x4E73` | `RTE` | privileged; exception-frame architecture. |
| `0x4E76` | `TRAPV` | conditional exception-delivery architecture. |
| `0x4E77` | `RTR` | stack-frame return architecture. |
| `0x4AFC` | `ILLEGAL` | illegal-instruction exception delivery. |
| `0x4E75` | `RTS` | already `SUPPORTED` through the control-flow owner (`M68kInstructionKind::rts`); unchanged by this task. |

Because only `NOP` fits the existing owner, SEG-007-T114 is a single-instruction
capability batch, and that is the correct shape: batching any neighbor would
require a separate new architecture decision. A later, non-identical no-operand
form is reported as its own frontier, not folded in here.

## Project decode, provenance, and IR policy

- **Decode.** Only `M68kDecodeProfile::general_startup` selects `NOP`
  (`M68kInstructionKind::nop`, length 2). The full two-byte span is
  bounds-checked before selection; a one-byte `0x4E` prefix is
  `truncated_instruction`, never a partial NOP. `M68kDecodeProfile::genesis_startup`
  is unchanged: it still rejects `0x4E71` and does not select `NOP`.
  `M68kDecodeProfile::direct_flow` is unchanged: `0x4E71` there remains
  `valid_but_unsupported_instruction`.
- **Provenance.** The decoded instruction retains its `DecodeSource`, the two
  raw image bytes, and `length = 2`. No `source_ea` or `destination_ea` is set
  (both `M68kEaMode::unused`).
- **Lift.** `M68kInstructionKind::nop` lifts to `M68kIrKind::no_operation`,
  copying provenance unchanged.
- **Effect** (`m68k_operation_effect`): `pc = advance`, `pc_delta = 2`,
  `affects_condition_codes = false`, no `register_write`, no
  `address_register_write`, no `resolved_source_ea` / `resolved_destination_ea`,
  `memory = none`, `stack = none`.
- **C11 emission** (`emit_m68k_operation_c` and the `general_startup` runtime C
  emitter): emits only a `/* NOP */` comment followed by the program-counter
  advance (`<pc> += UINT32_C(2);`, using the same program-counter projection as
  `load_effective_address`). No register, SR, memory, or device write.
  Deterministic across repeated emissions.

## Project-authored synthetic evidence

- `tests/m68k_pipeline_test.cpp`
  `general_startup_decode_accepts_nop_as_a_pure_pc_advance`: decode / lift /
  effect / emission, deterministic double emission, plus adversarial negatives
  for `0x4E70`, `0x4E72`, `0x4E73`, `0x4E74`, `0x4E76`, `0x4E77`, `0x4E78`, the
  `0x4E60`-`0x4E67` `MOVE An,USP` boundary, `0x4E75` `RTS`, a truncated one-byte
  `0x4E`, and the unchanged `genesis_startup` rejection.
- `tests/move_an_usp_bridge_test.py` NOP case: `MOVEQ #0,D0; NOP; RESET` driven
  through the production generate/compile/run bridge, proving strict-C11
  compilation and execution with pc advancing exactly four bytes past the
  straight-line block and no register/SR/memory write attributable to the NOP.
- `tests/fixtures/m68k-batch-c-musashi-vectors.json` `nop-pure-pc-advance` and
  `tests/m68k_batch_c_static_slice_test.py` NOP case: nonzero-seeded D/A/SR,
  asserting only pc changes (`0x00000100 -> 0x00000102`). The Musashi
  differential (source 3, `m68kcpu.c` execution) validates d / a / pc / sr / ram
  identical to the oracle whenever the pinned checkout is present.

## Non-inferences

Selecting `NOP` is not a claim that this project models MC68000 instruction
timing, prefetch, the trace bit, interrupt sampling between instructions,
supervisor/user mode state, or any neighboring System Control Group form. It
does not widen `genesis_startup` or `direct_flow`. `RESET` and `STOP` remain
recognized-but-unsupported CPU frontiers. No interpreter, JIT, or runtime opcode
decoder is introduced; the generated NOP lowering is a pure static
program-counter advance.
