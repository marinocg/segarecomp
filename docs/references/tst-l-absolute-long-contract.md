# MC68000 `TST.L (xxx).L` contract (SEG-007-T004)

**Status:** research contract; it supplies no implementation, fixtures, or tests. It does record
independently reproduced Musashi oracle execution results for its three accepted synthetic vectors
as supporting evidence for the contract's own claimed CCR/PC results (see "Independent execution
oracle" below); it does not implement decoding, IR, C emission, or a runtime.
**CPU covered:** original MC68000 only. **Accessed:** 2026-08-09.

## Selected capability and its private-derivation boundary

This contract selects exactly one MC68000 form: `TST.L (xxx).L` (long-size operand size, absolute-
long addressing mode). It is reached by consuming, without re-deriving or recording, the symbolic
classification SEG-006-T001 already established off-tree by classifying the private primary word at
source_address `0x00000206` (image_offset `518`) in the hash-pinned local Sonic ROM (SHA-256
`46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6`) against public Motorola
predicates (`docs/references/sonic-first-unsupported-inspection-contract.md`, "Public encoding
predicates"): `observed_instruction_family: TST`, `observed_size: long`,
`observed_addressing_mode: absolute_long`.

**The only Sonic-derived instruction fact committed anywhere in this repository is the symbolic
classification `TST` / `long` / `absolute_long`.** The private opcode word, its extension word, the
byte pair, disassembly, or any other Sonic-derived content is never recorded, printed, logged, or
committed by this contract or any of its successors. Every fixture, vector, and hash below is
project-authored and independent of Sonic ROM bytes, and every public MC68000 opcode encoding named
below (e.g. `0x4AB9`, `0x4A80`, `0x4AC0`, `0x4AFC`) is an ordinary, freely publishable, independently
derivable Motorola encoding for a named public form — not a value read from, or asserted to equal,
the Sonic image's private byte at `0x00000206`. Public encodings and project-authored synthetic
fixture bytes are permitted and are clearly labelled as public/synthetic throughout this document;
they are a different thing from a Sonic-derived fact and must not be conflated with one.

## Sources and the boundary of this contract

### Source facts

1. Motorola, *M68000 Family Reference Manual* (1988), section 6, instruction entry
   **TST -- Test an Operand**, headings "Instruction Format", "Operation", "Description", and
   "Condition Codes"; and the same manual's section 2, **Addressing Modes**, "Absolute Data
   Addressing". Verified scan:
   <https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf> (accessed
   2026-08-09). This is the source for `TST`'s opcode format (size field `10` for long, effective
   address `111 001` for absolute-long), its one-operand read-only semantics, its instruction
   length, and its condition-code effects (`N`/`Z` from the tested operand, `V`/`C` always cleared,
   `X` unaffected) below.
2. Motorola, *M68000 Family Reference Manual* (1988), section 6, entry **TAS -- Test and Set an
   Operand Byte**, heading "Instruction Format", at the verified scan in source 1 (accessed
   2026-08-09). This is the source that `TAS` reserves the size field value `11` within the same
   opcode-byte prefix `TST` uses (`0100 1010`), and is therefore a distinct instruction, not a
   `TST` size, despite the shared prefix.
3. Motorola, *M68000 Family Reference Manual* (1988), section 6, **ILLEGAL** entry, and section 3,
   **Program Counter**, at the verified scan in source 1 (accessed 2026-08-09). These establish the
   source-defined illegal word `0x4AFC` and that instruction words are word-aligned, matching the
   inherited `odd_instruction_address`/`illegal_instruction` policy this contract reuses rather than
   re-derives.
4. Karl Stenerud, **Musashi**, commit `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` (2026-03-08),
   `readme.txt`, "Basic configuration", "Using different CPU types", and "Load and save CPU
   contexts from disk"; and `m68k.h`, `m68k_register_t`, `m68k_set_cpu_type`, `m68k_execute`,
   `m68k_get_reg`, and `m68k_set_reg`.
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd> (accessed
   2026-08-09). The MIT license text is at the headers of
   [`m68k.h`](https://raw.githubusercontent.com/kstenerud/Musashi/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd/m68k.h)
   and [`m68kcpu.h`](https://raw.githubusercontent.com/kstenerud/Musashi/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd/m68kcpu.h).
   This is the same pin [SEG-002-T001](moveq-contract.md) § "Independent execution oracle" already
    selected; it is reused verbatim rather than re-pinned (see "Independent execution oracle" below).
5. Sega Enterprises, *Genesis Technical Overview*, v1.00 (1991), p. 9 and pp. 72--75,
   [public PDF archive entry](https://segaretro.org/Genesis_Technical_Overview) (accessed
   2026-08-11). This documents the active controller-I/O interval `$A10000-$A1001F` and
   CTRL1 `$A10009` / CTRL2 `$A1000B` as lower-byte lanes. It supplies region recognition
   only; it does not establish a 32-bit read-back value or a four-byte register semantic.

The source facts are deliberately separate from the **project policy** sections below. This
contract does not infer console mapping, timing, exception-frame contents, prefetch effects,
self-modifying code, or peripheral behavior from `TST.L`.

## Hardware contract

For an instruction word, in big-endian byte order,

```text
15                                  6   5 4 3       0
0 1 0 0 1 0 1 0 | 1 0 | 1 1 1 | 0 0 1
   opcode byte   size   mode    reg
```

`TST.L (xxx).L` has primary word `0100 1010 10 111 001` (size field `10` = long; effective-address
mode field `111`, register field `001` = absolute long), followed by one 32-bit big-endian
absolute address extension. Its encoded length is one word plus one longword extension: 6 bytes
total, so sequential PC is `PC + 6`. It performs exactly one 32-bit memory read at the resolved
absolute address and no write. For the read operand `r`, `N = bit31(r)`, `Z = (r == 0)`, `V = 0`,
`C = 0`; `X` is unaffected. No other SR bits are affected.

`TAS` (source 2) reserves the sibling size-field value `11` within the same opcode-byte prefix
(`0100 1010`) for a distinct read-modify-write instruction; it is not a `TST` size or form under
this classification. The source-defined `ILLEGAL` word `0x4AFC` (source 3) also falls within this
opcode-byte prefix (size field `11`, mode/register `111 100`) but is handled by the earlier,
dedicated `illegal_instruction` rule inherited below and is never reached by `TST` primary
classification.

## Project decode, provenance, and IR policy

This contract is an **extension of the existing shared MC68000 model**
(`include/segarecomp/m68k_pipeline.hpp`), not a parallel representation. It adds exactly one new
enumerator to each of the two existing shared kind enumerations and reuses every existing typed
field verbatim:

```text
M68kInstructionKind += tst_l_absolute_long
M68kIrKind          += test_absolute_long
```

`M68kDecodedInstruction` and `M68kIrOperation` are not extended with new fields and gain no
TST-specific sibling type (there is no `TstLInstruction`, `IrTstL32`, or bespoke six-byte provenance
struct):

```text
M68kDecodedInstruction {
  provenance: InstructionProvenance   // bytes[2] stays the fixed, unchanged verified primary word;
                                       // length is updated from its initial 2 to this form's
                                       // complete length, 6 -- provenance.bytes never grows to 6
                                       // bytes; only length changes, exactly as the existing
                                       // move_l_d0_absolute_long/move_l_absolute_long_d1 kinds
                                       // already do (see "Provenance for a successful six-byte
                                       // form" below)
  kind:       M68kInstructionKind     // = tst_l_absolute_long for this form
  destination: DataRegister           // unused for this form, exactly as for the existing
                                       // move_l_d0_absolute_long/move_l_absolute_long_d1 kinds
  operand:    int8                    // unused for this form, same reason
  raw_bytes:  byte[6]                 // the single verified owner of the complete six-byte
                                       // primary+extension span; distinct storage from
                                       // provenance.bytes, which never duplicates it
  extension:  uint32                  // the big-endian value of raw_bytes[2..5]; here the resolved
                                       // absolute-long operand address, unvalidated
}
M68kIrOperation {
  provenance: InstructionProvenance   // copied verbatim from the decoded record (bytes[2] primary,
                                       // length 6)
  kind:       M68kIrKind              // = test_absolute_long
  destination: DataRegister           // unused, as above
  operand:    int8                    // unused, as above
  raw_bytes:  byte[6]                 // copied verbatim from the decoded record
  extension:  uint32                  // copied verbatim from the decoded record
}
```

### Provenance for a successful six-byte form, versus a rejected one

`InstructionProvenance.bytes` is a fixed two-element array that **always** holds only the verified
primary word, for every selected form, successful or not; it is never resized and never holds an
extension byte. `InstructionProvenance.length` is not a permanently fixed `2` — the existing decoder
sets it to `2` before classification, then, only for a selected six-byte form's successful
`M68kDecodedInstruction`, updates it to `6` (matching exactly how `move_l_d0_absolute_long`/
`move_l_absolute_long_d1` already set `provenance.length = ByteLength{6}` while leaving
`provenance.bytes` unchanged). The complete six-byte span itself — primary and extension together —
lives only in `M68kDecodedInstruction::raw_bytes`, a separate field that a rejection never
populates.

For extension truncation specifically (available bytes `2 <= n < 6`), no `M68kDecodedInstruction` is
produced at all: the result is a `RejectedM68kDecode` whose `provenance` retains only the verified
two-byte primary with `length: 2` (**not** `6`, because the form was never confirmed complete) and
whose `requested_length` is `6`. `RejectedM68kDecode` has no `raw_bytes` field, so there is no
"truncated primary retained as `raw_bytes`" — that field belongs only to a successful
`M68kDecodedInstruction`, and extension truncation never produces one.

This mirrors exactly how `move_l_d0_absolute_long`/`write_d0_absolute_long` and
`move_l_absolute_long_d1`/`read_absolute_long_d1` already populate these same shared struct fields
today (`destination`/`operand` unused because the register is fixed by `kind`, not by a variable
field; `raw_bytes`/`extension` owning the complete six-byte span and resolved address). Only
`decode_m68k_instruction` and `lift_m68k_instruction` gain one additional `case` each; no second
decoder, lifter, or IR representation is introduced.

### Diagnostic-stage ownership

This contract introduces no new diagnostic enum, and it reuses — rather than flattens into one
universe — the two distinct diagnostic layers the shared model already has:

- **`DecodeOutcome`** (`include/segarecomp/moveq.hpp`) is the sole decode-stage failure vocabulary
  and owns exactly: `unsupported_cpu_variant`, `unsupported_address_space`,
  `odd_instruction_address`, `truncated_instruction`, `illegal_instruction`, and
  `valid_but_unsupported_instruction`. A successful decode is represented by which `M68kDecodeResult`
  variant arm is present (`M68kDecodedInstruction`, carrying `kind: M68kInstructionKind`) — there is
  no `DecodeOutcome::decoded_tst_l_absolute_long` value and no `decode_outcome` field on
  `M68kDecodedInstruction`; success is the variant arm itself plus `kind`, not a `DecodeOutcome`
  member.
- **`RejectedM68kDecode`** is the decode-stage rejection record. It carries exactly one
  `DecodeOutcome outcome`, one boolean `unsupported_instruction_form`, and the source/provenance/
  length fields (`source`, `available_bytes`, `requested_length`, `instruction_length`,
  `has_instruction_length`, `provenance`, `has_provenance`). **`unsupported_instruction_form` is not
  a separate `DecodeOutcome` enumerator**: both "same selected opcode family, wrong form" (e.g. any
  `TST` size/addressing-mode other than long/absolute-long) and "unrelated complete primary" (e.g.
  `TAS`, or any other unrelated instruction) share the single outcome
  `DecodeOutcome::valid_but_unsupported_instruction`; the `unsupported_instruction_form` boolean is
  the qualifier that distinguishes the two for reporting, exactly as the existing code already sets
  it (`result.unsupported_instruction_form = …`) alongside `valid_but_unsupported_instruction` for
  every other already-selected profile.
- **Frontend/static validation** (`DirectFlowDiagnostic`, `include/segarecomp/m68k_pipeline.hpp`)
  owns the later, mapping- and address-resolution-stage categories this contract also uses:
  `unmapped_instruction_address`, `effective_address_not_24bit`, `odd_effective_address`, and
  `unmapped_data_access`. These are never decode-stage `DecodeOutcome` values; they belong to the
  existing frontend mapping/EA-validation machinery that runs after a successful decode, using the
  existing frontend diagnostic type, not a new one.

Every category `TST.L` uses is therefore reused verbatim from one of these two existing typed
vocabularies, at the existing layer that already owns it; the overall eight-step fail-closed
precedence below is unchanged by this split (see "Strict result categories and precedence").

`InstructionProvenance` (`source: DecodeSource`, a fixed `bytes[2]` primary-word array, and
`length` — `2` by default, updated to `6` only on this form's successful `M68kDecodedInstruction`,
per "Provenance for a successful six-byte form, versus a rejected one" above), `DecodeSource`
(`cpu_variant, address: M68kProgramAddress, image_offset`), `M68kProgramAddress`
(`space: m68k_program, value: uint32`), and `DataRegister` are the existing shared types; this
contract defines no independent copies of them. `DecodeSource` is created before reading bytes and
is retained by a rejection even when no complete instruction provenance can exist. The decoder
accepts only `{ space: m68k_program, ... }` with `cpu_variant: mc68000`; another address space or
CPU variant is `unsupported_address_space`/`unsupported_cpu_variant`, rejected before byte
classification. `address` and `image_offset` are independently carried: neither is a host pointer
and neither is derived from the other after ingestion.

The minimal IR operation means, atomically: read one 32-bit big-endian value `r` at the address held
in `extension`; `N := bit31(r)`; `Z := (r == 0)`; `V := 0`; `C := 0`; `X` unaffected; `PC += 6`; no
register write and no memory write occurs (see "Shared `M68kOperationEffect` extension" below for
exactly how this reuses the existing shared semantic-effect and CCR layers rather than a
TST-specific executor/emitter branch). A rejection produces no IR or C block; emission reports the
unchanged rejection diagnostic and its typed source instead. The scope state is
`D[0..7]: uint32` (unaffected by this form; read-only reference for other selected forms), `PC:
M68kProgramAddress`, and `SR: uint16`. `A[0..7]`, memory other than the one validated read, devices,
cycles, and exception state are outside this instruction slice and must not be silently changed.

## Data-read region resolution

### Controller-I/O fail-closed extension (SEG-007-T015)

After the existing 24-bit/even, ROM, and synthetic-work-RAM checks, a 32-bit TST.L operand is
classified as `unsupported_device_region_controller_io` exactly when its overflow-safe half-open
access interval `[address, address + 4)` intersects the documented active controller-I/O interval
`[0x00A10000, 0x00A10020)`. This is device-region recognition, not a successful read: it retains the
same instruction/target provenance as `unmapped_data_access`, reads no device value, and does not
advance PC or CCR. In particular, CTRL1 `$A10009` and CTRL2 `$A1000B` remain documented lower-byte
lanes only; this rule never claims either is a contiguous four-byte register. Non-intersecting
neighbors remain `unmapped_data_access`.

`TST.L`'s validated operand address may resolve into either selected region
(`raw_cartridge_rom` or `synthetic_work_ram`; see "Selected memory regions" below) — this is
genuinely new region-resolution behavior this contract adds, not a claim that the existing `MOVE.L`
absolute-long forms already validate against both regions today. The existing shared code
(`m68k_startup_absolute_operand_alignment`, used by both `move_l_d0_absolute_long`'s store and
`move_l_absolute_long_d1`'s load) validates only 24-bit-clean/even alignment; the existing data
operand range check after that (`m68k_startup_ram_operand_in_range`) validates only
`synthetic_work_ram` — the current shared implementation has **no ROM data-read support at all** for
those two forms. `TST.L` extends the range-resolution step (not the alignment step) to also permit a
ROM-resident operand, in exactly this order:

```text
1. m68k_startup_absolute_operand_alignment(address)
   -> effective_address_not_24bit, then odd_effective_address (reused verbatim, unchanged)
2. else if the complete 4-byte access [address, address+4) is entirely inside the selected
   raw_cartridge_rom range [0x000000, image_length):
       region = raw_cartridge_rom
       resolve the immutable, already-statically-verified source bytes/value at that range
       (see "Ownership of the verified ROM data-read fact" below) -- m68k_startup_ram_operand_in_range
       is not consulted for this branch
3. else if the complete 4-byte access [address, address+4) is entirely inside synthetic_work_ram
   (m68k_startup_ram_operand_in_range(address)):
       region = synthetic_work_ram
       use the existing m68k_startup_ram_offset(address) projection, exactly as
       read_absolute_long_d1's existing RAM load already does
4. else: unmapped_data_access
```

`m68k_startup_ram_operand_in_range`/`m68k_startup_ram_offset` are reused **only** for the RAM branch
(step 3); they are never applied to a ROM-resident operand, and a valid ROM operand never passes
through the RAM-window predicate. This makes the "Project-authored synthetic recipes" section's
accepted `$00000200` vectors (a ROM-resident operand address) internally consistent with this
resolution order: those vectors succeed at step 2, not step 3. There is no `rom_write_prohibited`
step anywhere in this order, because `TST.L` never writes.

## Ownership of the verified ROM data-read fact

A successful ROM-region data read (step 2 above) produces one immutable, typed, translation-time
fact — conceptually:

```text
StaticDataRead {
  address:      M68kProgramAddress   // the validated operand address
  image_offset: ImageOffset          // the corresponding image offset under the identity mapping
  region:       raw_cartridge_rom
  bytes:        byte[4]              // the four verified source bytes, read exactly once
  value:        uint32               // the big-endian decoded value of those four bytes
}
```

The exact C++ representation of this fact is not mandated by this contract (an implementation may
store it as a field on the executed/validated record, a small local return value threaded from
validation to the caller, or another shape); what is mandated is the **ownership rule**: mapping/
data-read validation (step 2 above) reads and verifies the four source bytes exactly once and
records this immutable fact; every later stage — execution's CCR application and C lowering alike —
consumes only that already-verified fact. Concretely, the emitter must not reopen or re-read the
original ROM/source-image file, must not re-run region/mapping resolution independently of the
already-validated fact, and must not decode target bytes at runtime. This is the same "read once,
retain, never re-derive" discipline `raw_bytes`/`extension` already apply to instruction bytes,
applied here to the one additional ROM data read `TST.L` newly introduces.

## Shared `M68kOperationEffect` extension

`TST.L` must be expressed as one more case of the existing shared semantic owner
`m68k_operation_effect(const M68kIrOperation &)`
(`include/segarecomp/m68k_pipeline.hpp` § `M68kOperationEffect`), exactly as
`write_moveq`/`write_d0_absolute_long`/`read_absolute_long_d1`/`call_absolute_long`/
`return_from_subroutine` already are — not as logic inlined into a TST-specific executor or emitter
branch outside that function.

The existing `M68kMemoryEffectKind` enumeration (`none`, `absolute_store`, `absolute_load`)
distinguishes a register-affecting load (`absolute_load`, used by `read_absolute_long_d1`, which
writes `D1` and updates CCR from the loaded value) from a store (`absolute_store`). Neither existing
value fits `TST.L`, which reads one 32-bit operand but **never writes any register** — only its CCR
result is observable. This contract therefore requires one new enumerator,
`M68kMemoryEffectKind::absolute_test`: a memory read whose value feeds only condition-code
computation, with no destination register. For `test_absolute_long`, `m68k_operation_effect` must
produce:

```text
register_write:          std::nullopt              // no register is written, unlike absolute_load
memory:                   absolute_test
memory_register:          unused (default)          // there is no destination register
memory_address:           operation.extension        // unvalidated; the caller validates it
affects_condition_codes: true
stack:                    none
pc:                       advance
pc_delta:                 6
```

**Any execution/lowering route that consumes `M68kIrKind::test_absolute_long` must derive its
semantics from `M68kOperationEffect::absolute_test`** — a caller performs the validated read, then
applies **the existing shared CCR helper `m68k_move_result_ccr(status_register, result)` directly to
the read value** — not a new N/Z/V/C/X implementation and not logic inlined outside
`m68k_operation_effect`. This is the same function `write_moveq` and `read_absolute_long_d1` already
call on their own affected value (a register's new contents); `TST.L` is simply the case where the
CCR-affecting value is the memory read result itself rather than a value about to be written to a
register. `m68k_move_result_ccr`'s documented contract — `N`/`Z` from the given 32-bit value, `V`/`C`
cleared, `X` preserved — is exactly `TST.L`'s required CCR effect, so no other CCR computation is
needed anywhere in the implementation.

**This contract does not require every existing route to accept `TST.L`.** T005's obligation is
narrower: its generic, direct synthetic decode/lift/execute/emit path (the same kind of narrow
vertical slice SEG-002 used to first prove `MOVEQ`) must consume `absolute_test` and
`m68k_move_result_ccr` as specified above. T005 must **not** make `execute_m68k_frontend_startup`,
`analyze_startup_profile`, or the `genesis_rom_startup` route accept `TST.L`; those remain unchanged
by this capability. Consequently the current Sonic route's behavior after `TST.L` recognition is
unchanged by this contract: `genesis_rom_startup` still requires the first decoded instruction to be
`MOVEQ` and still rejects a leading `TST.L` as `startup_graph_mismatch` (see "Notes: known next
architecture frontier" below) — advancing past that boundary is bounded general startup static
discovery, which is explicitly out of scope for both T004 and T005.

## Static ROM-versus-RAM operand lowering policy

`TST.L`'s operand may resolve into either selected region under "Data-read region resolution" above.
Both must reach static generated C with the same abstract observable effect — one `data_read`, no
write, the same operand value, and the same CCR result — but by different lowering mechanisms,
because only one of the two regions remains a runtime-visible buffer in generated output:

- **`synthetic_work_ram` operand:** lowers exactly as `read_absolute_long_d1`'s existing RAM load
  already does — read the big-endian 32-bit value from the existing generated RAM representation
  (`M68kMemoryEmissionContext::ram_array`, at the existing shared offset projection
  `m68k_startup_ram_offset`), and apply `m68k_move_result_ccr` to that read value. No new RAM
  representation is introduced.
- **`raw_cartridge_rom` operand:** lowering consumes only the already-verified `StaticDataRead` fact
  from "Ownership of the verified ROM data-read fact" above; it never re-reads the source image or
  re-runs region resolution. The generated program must perform **no runtime ROM/source-image fetch
  and no runtime target-byte decode**. The C emitter therefore either (a) folds the fact's `value`
  into a compile-time constant — a `UINT32_C(0x...)` literal embedded directly in the generated
  read/CCR effect, tagged with the fact's `address`/`image_offset` in a comment for provenance
  auditability, exactly as other emitted operations already retain provenance in their generated
  output — or (b) emits a second, immutable, `const`-qualified static array parallel to `ram_array`
  (an analogous `rom_array`/`rom_base` addition to `M68kMemoryEmissionContext`) populated from that
  same already-verified fact, if a later, less bounded slice needs indexed rather than folded ROM
  data. Either representation satisfies this contract, and neither may require the original ROM
  image file to remain available or be re-read at generated program runtime. What this contract
  forbids is a third option: a "compatibility" memory path that fetches from a host-resident
  source-image buffer at runtime, decodes a target byte at runtime, or otherwise makes generated
  output depend on the presence of the original image file after translation.

T005 must pick and implement exactly one of the two permitted ROM representations above (a folded
literal or a second immutable static array) rather than inventing a third; this contract does not
mandate which, because both are observably equivalent to `TST.L`'s CCR/PC contract and to every
fail-closed category above.

## Selected memory regions

This contract selects no new memory region. It reuses exactly the same typed memory map
[SEG-005](genesis-rom-startup-contract.md) § "Selected map and instruction subset" and
[SEG-012](../architecture/m68k-pipeline-migration-contract.md) already select:

| Region | selected range | access | mapping/provenance |
| --- | --- | --- | --- |
| `raw_cartridge_rom` | `[0x000000,image_length)`, `image_length <= 0x400000` | instruction/data read; no write | inherited SEG-001 identity mapping, `image_offset = address` |
| `synthetic_work_ram` | canonical `[0x00ff0000,0x01000000)` | data read/write only | `ram_offset = address - 0x00ff0000` |

`TST.L`'s one read may resolve into either region, in the exact precedence "Data-read region
resolution" above defines (alignment, then ROM range, then RAM range, else `unmapped_data_access`);
no other region is added or implied.

## Strict result categories and precedence

These diagnostics are project policy for untrusted static input, not CPU exceptions. Every failure
is fail-closed. This contract extends the existing inherited order verbatim
([direct-flow contract](direct-flow-contract.md) § "Fail-closed diagnostics" and
[the Genesis ROM startup contract](genesis-rom-startup-contract.md) § "Records, deterministic
validation, and stops") rather than inventing a new one. Evaluate in this exact order and stop at
the first failure:

Steps 1, 2, 4, 5, and 6 are `DecodeOutcome`-owned (decode stage); steps 3 and 8 are
`DirectFlowDiagnostic`-owned (frontend/static stage); see "Diagnostic-stage ownership" above. The
precedence order below is unchanged by that layering:

1. `unsupported_cpu_variant` / `unsupported_address_space` (`DecodeOutcome`): wrong typed CPU
   variant or address space, checked before any read.
2. `odd_instruction_address` (`DecodeOutcome`): `source_address & 1 != 0`; do not read.
3. `unmapped_instruction_address` (`DirectFlowDiagnostic`, frontend mapping resolution): the
   instruction address falls outside every selected mapping claim; do not read.
4. `truncated_instruction` (`DecodeOutcome`): fewer than 2 bytes available at the mapped offset for
   the primary word; `requested_length:2`, `instruction_length:null`; do not manufacture a word.
5. `illegal_instruction` (`DecodeOutcome`): the fetched primary word is the source-defined `ILLEGAL`
   word `0x4AFC`.
6. Primary classification (`DecodeOutcome`; see below): a two-byte-only decision between the
   successful decode (`M68kDecodedInstruction` with `kind = tst_l_absolute_long`) and a rejection
   carrying `outcome = valid_but_unsupported_instruction`, further qualified by the
   `unsupported_instruction_form` boolean (see "Diagnostic-stage ownership" above — this is one
   `DecodeOutcome` value with a boolean qualifier, not two separate outcome values); only the exact
   selected form proceeds to extension validation.
7. Extension truncation for the selected form only (`DecodeOutcome::truncated_instruction`):
   `2 <= available < 6` produces a `RejectedM68kDecode` with `requested_length:6`,
   `instruction_length:null`, and `provenance` retaining only the verified two-byte primary
   (`length: 2` — see "Provenance for a successful six-byte form, versus a rejected one" above; there
   is no `raw_bytes` span on this rejection). Only `available >= 6` produces a complete
   `M68kDecodedInstruction` with `provenance.length: 6`, a six-byte `raw_bytes` span, and the
   resolved absolute-long operand address in `extension`.
8. Data-read region resolution on the resolved operand address (`DirectFlowDiagnostic`), in this
   exact order (see "Data-read region resolution" above): `effective_address_not_24bit`, then
   `odd_effective_address`, then (ROM range, then RAM range, else) `unmapped_data_access`. A
   rejected operand emits no data read; a valid operand emits its `data_read` only after this
   sequence succeeds.

### Primary classification (step 6), scoped narrowly to this one form

Primary classification is scoped to exactly this one selected form, not to the whole Motorola
opcode-byte prefix `0100 1010` and not to `TAS` generally. Every rejection below carries
`outcome = DecodeOutcome::valid_but_unsupported_instruction`; the labels `unsupported_instruction_form`
and "unrelated complete primary" below distinguish the `unsupported_instruction_form` boolean
qualifier's value on that one shared outcome (see "Diagnostic-stage ownership" above), not two
different `DecodeOutcome` members:

- The exact `TST.L (xxx).L` primary (`0100 1010 10 111 001`; family `TST`, size `long`, EA
  `absolute_long`) is the selected form and proceeds to extension validation.
- Any other valid `TST` size or addressing mode (`family(W) == TST` but `size(W) != long` or
  `EA(W) != absolute_long`) is reported as `unsupported_instruction_form` (i.e.
  `outcome = valid_but_unsupported_instruction` with `unsupported_instruction_form = true`): same
  selected opcode family, wrong form. Example: `TST.L D0` (`0100 1010 10 000 000`) has the selected
  `TST`/`long` classification but the unselected data-register addressing mode.
- `TAS` (size field `11`, any addressing mode other than the reserved `ILLEGAL` encoding) is **not**
  part of the `TST` family under this classification and is reported as plain
  `valid_but_unsupported_instruction` (`unsupported_instruction_form = false`), not
  `unsupported_instruction_form`.
- Every other unrelated complete primary — including primaries this slice already selects
  elsewhere, such as `JSR`/`RTS`, which retain their existing decode/lift/emit behavior unchanged by
  this contract — is reported as plain `valid_but_unsupported_instruction`
  (`unsupported_instruction_form = false`) when encountered outside their own already selected role.
- `ILLEGAL` (`0x4AFC`) is never reached by this classification step; it is handled earlier, at
  step 5, by the dedicated `ILLEGAL` rule.

Principle: same selected opcode family, wrong form → `outcome = valid_but_unsupported_instruction`,
`unsupported_instruction_form = true`; unrelated complete primary →
`outcome = valid_but_unsupported_instruction`, `unsupported_instruction_form = false`.

## Project-authored synthetic recipes and exact vectors

Every recipe below is project-authored and independent of Sonic ROM bytes. Each accepted fixture
places `TST.L ($000200).L` (primary `4AB9`, absolute-long extension `00000200`) at image offset
`0x100` (entry PC `0x000100`), so the resolved operand address `0x000200` is itself within
`raw_cartridge_rom`'s identity-mapped, project-authored data at the same image offset. A fixture
record **must** name its byte sequence and the SHA-256 of exactly those image bytes; a changed byte
sequence requires a changed hash and vector review. The one-instruction execution budget is project
policy: after the one successful instruction, report `instruction_budget_exhausted`.

All vectors give the complete in-scope post-state. `D[]` is unaffected by `TST.L` and is listed once
as `[11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]` for every accepted
row below; it must retain those values at every vector's boundary. The invariant output PC is
`0x000106` (`PC + 6`) and stop reason is `instruction_budget_exhausted`. Initial `SR` is `271F`
(system byte `27`: `T=0,S=1`, interrupt mask `7`; CCR `X=1,N=1,Z=1,V=1,C=1`) for every accepted row —
**not** `A71F`, which sets the trace bit `T=1` and is deliberately avoided as a seed state (see
"Independent execution oracle" below) — so `X` preservation and every affected/cleared CCR bit are
independently observable without enabling 68000 trace behavior.

| recipe | image length / SHA-256 (of exactly the listed bytes) | tested operand (at `0x000200`) | expected `N,Z,V,C,X` | expected `SR` |
| --- | --- | --- | --- | --- |
| `zero` | `0x204` bytes / `32207f6f7540030671bdd53f5403f49cbaa42f1b906efb5a47be899fb590b0b1` | `00000000` | `N=0,Z=1,V=0,C=0,X=1` (unaffected) | `2714` |
| `positive-max` | `0x204` bytes / `8cd33c30981199ad3b4269969d291650560a5c75fc423b4cf1590f8376d2317c` | `7FFFFFFF` | `N=0,Z=0,V=0,C=0,X=1` | `2710` |
| `negative-lsb-set` | `0x204` bytes / `5ac32f832b0a938696808e9488cfceb7441ab22374239d3d24a931f54604a714` | `80000001` | `N=1,Z=0,V=0,C=0,X=1` | `2718` |

Every accepted image has instruction bytes `4A B9 00 00 02 00` at offset `0x100` and the tested
32-bit big-endian operand value at offset `0x200`, with every other byte zero. The tested memory is
left unchanged (no write) at every boundary; only PC and SR change from their seed.

Required fail-closed decode examples (instruction images; each contains only the listed bytes at
offset `0x100`, zero-padded, no operand data needed because the failure precedes any data read):

| recipe | bytes at `0x100` / image length / SHA-256 | required first result |
| --- | --- | --- |
| `illegal-primary` | `4AFC` / `0x102` bytes / `c1dab4680bc54a50399d76a2a5ec2e80e63c958bd22beafe8086ebc86ef1b991` | `illegal_instruction`, complete two-byte provenance, before primary classification, no bus/data read. |
| `tas-unrelated` | `4AC0` (`TAS D0`) / `0x102` bytes / `41b2fe61d26cb210952743e4008e81f25e76a9c1f5e58c091a751565f54fd007` | `valid_but_unsupported_instruction`, complete two-byte provenance, no extension read. |
| `unsupported-form-tst-l-d0` | `4A80` (`TST.L D0`) / `0x102` bytes / `05ab690cecce954578f710652fbfa89cf49217b71ccf4ff276ec048c5c062b7c` | `unsupported_instruction_form`, complete two-byte provenance, no extension read. |
| `unrelated-primary` | `6002` (`BRA.S`, unrelated family) / `0x102` bytes / `26ad000d0129bab4d8d3ef0291739fb29384f8889655012a29681de75dcfe1d8` | `valid_but_unsupported_instruction`, complete two-byte provenance, no extension read. |
| `truncated-primary` | `4A` only (one byte) / `0x101` bytes / `96d2656c461f38ba4d7649e0cfbb088b03e18dbf47b825f636766d9eeeed6822` | `truncated_instruction`, requested 2, no word or instruction length, no bus read. |
| `truncated-extension` | `4AB9 0000` (primary plus two extension bytes only) / `0x104` bytes / `02db1bbf09ff1d9d4d3baee7d6f0a2526b6e62f7fd5b34a72a0b6d9d021b72f9` | `truncated_instruction`, available 4, requested 6, two-byte primary provenance retained, no complete instruction provenance or data read. |

Required fail-closed effective-address examples (each has the complete selected primary/extension
`4AB9` plus the listed operand address, so the instruction record itself is complete but the
operand's data read is rejected):

| recipe | extension bytes (operand address) / image length / SHA-256 | resolved EA | required result |
| --- | --- | --- | --- |
| `ea-not-24-bit` | `01000200` / `0x106` bytes / `7e162b39360bc4ea98f35dd33ca281eaf3b695ae24093f489fc5b02ab8b995f9` | `01000200` | `effective_address_not_24bit`; complete instruction provenance; no data read. |
| `odd-ea` | `00000201` / `0x106` bytes / `cd1ad29a6f0966424f6e12c641f07cf0b7770d2b2ffe63f2b6e0d4b7c8766bce` | `000201` | `odd_effective_address`; complete instruction provenance; no data read. |
| `unmapped-ea` | `00a00000` / `0x106` bytes / `2cb4dcf678a74d3e40b1c4c84e2337827e9ae0dfe77165670fef946bf3f0cede` | `a00000` | `unmapped_data_access`; complete instruction provenance; no data read. |

`ea-not-24-bit` wins over `odd-ea`, which wins over `unmapped-ea`, matching step 8's stated order;
no vector above satisfies more than one predicate simultaneously, so the matrix does not by itself
exercise a tie, but a future implementation test must still enforce the stated order for an address
that could satisfy more than one (e.g., an EA with a nonzero high byte that is also odd).

## Independent execution oracle

**Selected oracle:** reuse of [SEG-002-T001](moveq-contract.md) § "Independent execution oracle"
verbatim — Musashi at commit `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, configured for
`M68K_CPU_TYPE_68000`. No different pin is required.

**No new adapter is introduced or committed.** This project's ignored `.tools/` tree (gitignored;
see `.gitignore`'s `/.tools/` entry) already contains a pinned local Musashi checkout
(`.tools/musashi`, verified at exactly the required commit below) and an existing, general-purpose,
already-uncommitted local adapter, `.tools/m68k-musashi-direct-flow-oracle` (built from
`.tools/m68k-direct-flow-runner.c`), from SEG-003/SEG-004. That adapter already satisfies every
capability a `TST.L` oracle needs — selecting `M68K_CPU_TYPE_68000`, verifying the supplied
`image_sha256` against `image_hex` before mapping, mapping the project-authored image by exact
claimed target/image-offset spans (its "frontend" input mode, matching this contract's
`raw_cartridge_rom` identity mapping, `image_offset = address`), seeding `D0`--`D7`, `PC` (via
`reset_entry`), and `SR` directly (not via reset-vector injection), executing exactly one selected
instruction (`block_instruction_counts:[1]`), and reporting `D0`--`D7`, `PC`, and `SR` — with **no
TST-specific modification**. This contract reuses that existing tool verbatim rather than
introducing a bespoke `tst-l-musashi-oracle`, consistent with "do not commit an adapter unless
policy calls for it": none of the tools below are new, and none is committed by this contract.

Pin verification (run before every invocation; this exact command was run for the results below):

```sh
git -C .tools/musashi rev-parse HEAD
# 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
```

**Compile configuration.** The adapter's `build()` step compiles the pinned Musashi sources with
`cc -std=c11 -O2 -DM68K_INSTRUCTION_HOOK=M68K_OPT_ON -I .tools/musashi
.tools/m68k-direct-flow-runner.c .tools/musashi/m68kcpu.c .tools/musashi/m68kops.c
.tools/musashi/softfloat/softfloat.c -lm`; no other Musashi compile-time option is overridden.
`M68K_INSTRUCTION_HOOK` is required so the runner can stop after exactly one instruction; it does not
affect instruction semantics. Trace behavior is not compiled into this oracle at all: the pinned
`.tools/musashi/m68kconf.h` defines `M68K_EMULATE_TRACE M68K_OPT_OFF` by default and this contract's
build does not override it. Independently of that, the seed `SR` is `271F` (`T=0`), not `A71F`
(`T=1`), so no vector below relies on trace being compiled out — the seed state itself never
requests trace behavior (see "Project-authored synthetic recipes and exact vectors" above for why
`271F` was selected over `A71F`).

`.tools/` is untracked, local, repository-root tooling (not branch content, and not duplicated per
worktree); the commands below were run from the repository's top-level `.tools/` checkout and
reproduced with identical results on a second independent run.

**Actual results.** Each vector below was run against the pinned adapter with its "frontend" JSON
input schema (`fixture_id`, `vector_id`, `image_hex`, `image_sha256`, `reset_entry:"100"`, `d`,
`sr:"271F"`, `block_instruction_counts:[1]`, and one identity `mapping_claims` entry spanning the
complete `0x204`-byte image), producing this project harness's `--blocks 1` output schema
(`oracle`, `executed_blocks`, `boundaries[{d,pc,sr}]`, `stop_reason`) — no field is invented or
approximated:

| vector | command | reported `boundaries[0]` |
| --- | --- | --- |
| `zero` | `.tools/m68k-musashi-direct-flow-oracle --input tst_zero.json --output tst_zero.out.json --blocks 1` | `{"d":["11111111","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000106","sr":"2714"}` |
| `positive-max` | `.tools/m68k-musashi-direct-flow-oracle --input tst_positive-max.json --output tst_positive-max.out.json --blocks 1` | `{"d":["11111111","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000106","sr":"2710"}` |
| `negative-lsb-set` | `.tools/m68k-musashi-direct-flow-oracle --input tst_negative-lsb-set.json --output tst_negative-lsb-set.out.json --blocks 1` | `{"d":["11111111","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000106","sr":"2718"}` |

Every reported `d`, `pc`, and `sr` is byte-for-byte identical to the values already stated in
"Project-authored synthetic recipes and exact vectors" above (`D[]` unaffected, `PC=00000106`,
`SR=2714`/`2710`/`2718`), and every result reports `"stop_reason":"instruction_budget_exhausted"`
and `"oracle":"musashi@313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd"`. No commercial input, Sonic byte,
or non-synthetic data was used to produce these results; each `image_hex`/`image_sha256` is exactly
one of the three project-authored accepted-vector recipes above. Musashi is independent because it
is an externally maintained instruction emulator that fetches and executes the supplied bytes; it
shares neither this project's planned decoder, typed IR, C emitter, nor generated runtime. It is an
oracle for these three accepted vectors only, not evidence that this project supports exceptions,
timing, mapping, self-modifying code, or peripherals.

## Non-goals

- Implementing decoding, IR, C emission, a runtime, fixtures, or tests: this document is research
  only.
- Any `TST` size or addressing mode other than long/absolute-long; any other instruction;
  VDP/device/rendering behavior; general Sonic compatibility; bounded startup static discovery;
  translating or executing further Sonic ROM content beyond the already-established classification.
- Recording, printing, or committing the private opcode word, extension word, byte pair,
  disassembly, or any other Sonic-derived content anywhere in this repository.

## Notes: known next architecture frontier (recorded, not designed, here)

After SEG-007-T006
independently validates this `TST.L` capability, the current `genesis_rom_startup` route still
requires the first decoded instruction to be `MOVEQ` and rejects anything else — including this
newly recognized `TST.L` form — as `startup_graph_mismatch`, per SEG-006-T001's "Current
architecture boundary" finding. The known next prerequisite is therefore **bounded general startup
static discovery** beginning at the reset entry point, consuming only selected shared MC68000
operations and existing typed static edges, and failing closed at the first unsupported
operation/flow/memory boundary — reusing SEG-003's static direct-flow machinery and
SEG-005-T004/T006's call/return and static-frame discovery. Only after that discovery work reruns
the Sonic route and observes its next actual stop should a future refinement select VDP/memory/
device work from that new evidence. This contract does not create or scope that discovery work.
