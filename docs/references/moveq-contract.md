# MC68000 `MOVEQ` contract (SEG-002)

**Status:** research contract for the first 68000 slice; no implementation is supplied here.
**CPU covered:** original MC68000 only.  **Accessed:** 2026-08-05.

## Sources and the boundary of this contract

### Source facts

1. Motorola, *M68000 Family Reference Manual* (1988), section 6, instruction
   entry **MOVEQ -- Move Quick**, specifically its instruction-format diagram,
   operation line, and condition-code table.  Verified scan:
   <https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf>
   (accessed 2026-08-05).  This primary Motorola manual is the source for the
   encoding, sign extension, destination, instruction length, and CCR effects
   below.  The section and entry title are the exact printed locator; they are
   preferable to a PDF-viewer-dependent page index.
2. Motorola, *M68000 Family Reference Manual* (1988), section 3, **Program
   Counter**, and section 6, **Address Error** entry, at the verified scan in
   source 1 (accessed 2026-08-05).  These establish that instruction words are
   word aligned and that an odd word fetch causes an address-error exception.
   This is cited only for the hardware fact; the static decoder's result code
   is project policy.
3. Karl Stenerud, **Musashi**, commit
   `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` (2026-03-08), `readme.txt`,
   “Basic configuration”, “Using different CPU types”, and “Load and save CPU
   contexts from disk”; and `m68k.h`, `m68k_register_t`,
   `m68k_set_cpu_type`, `m68k_execute`, `m68k_get_reg`, and `m68k_set_reg`.
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd>
   (accessed 2026-08-05).  The MIT license text is at the headers of
   [`m68k.h`](https://raw.githubusercontent.com/kstenerud/Musashi/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd/m68k.h)
   and [`m68kcpu.h`](https://raw.githubusercontent.com/kstenerud/Musashi/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd/m68kcpu.h).

The source facts are deliberately separate from the **project policy**
sections below.  In particular, this contract does not infer console mapping,
reset-vector layout, timing, exception-frame contents, prefetch effects,
self-modifying code, or peripheral behavior from `MOVEQ`.

## Hardware contract

For an instruction word, in big-endian byte order,

```text
15                                                     0
 0 1 1 1 | Dn (bits 11..9) | 0 | signed 8-bit immediate
```

`MOVEQ #<data>,Dn` sign-extends its 8-bit immediate to 32 bits and replaces
the complete selected data register.  Its encoded length is one word (two
bytes), so sequential PC is `PC + 2`.  It does not access memory other than
the instruction fetch.  For the 32-bit result `r`, `N = bit31(r)`, `Z =
(r == 0)`, and `V = C = 0`; `X` is unaffected.  No other SR bits are affected.
These are the claims supported by source 1.

`0x7100` is **not** used as an illegal test or labelled illegal by this
contract.  This research does not rely on a classification for its
reserved-looking bit; the source-defined `ILLEGAL` instruction word `0x4AFC`
is used below instead (source 1, section 6, **ILLEGAL** entry).

## Project decode, provenance, and IR policy

This narrow contract uses these explicit types; names are contractual, not a
requirement for a particular C++ representation.

```text
TargetAddressSpace  = enum m68k_program
M68kProgramAddress  = { space: m68k_program, value: unsigned 32-bit }
ImageOffset         = unsigned 64-bit offset into the supplied image
ByteLength          = unsigned 32-bit byte count
DataRegister        = enum D0..D7
SignedImmediate8    = signed 8-bit two's-complement value
UnsignedLong        = unsigned 32-bit value
CpuVariant          = enum mc68000
DecodeOutcome       = enum decoded_moveq, odd_instruction_address,
                      truncated_instruction, illegal_instruction,
                      valid_but_unsupported_instruction
DecodeSource        = { cpu_variant: CpuVariant,
                        address: M68kProgramAddress, image_offset }
InstructionProvenance = { source: DecodeSource, bytes[2], length: 2 }
MoveqInstruction    = { provenance: InstructionProvenance,
                        decode_outcome: decoded_moveq,
                        destination: DataRegister, immediate: SignedImmediate8 }
IrMoveq32           = { provenance: InstructionProvenance,
                        decode_outcome: decoded_moveq,
                        destination: DataRegister, immediate: SignedImmediate8 }
DecodeResult        = Decoded { outcome: decoded_moveq,
                                instruction: MoveqInstruction }
                    | Rejected { outcome: DecodeOutcome - decoded_moveq,
                                 source: DecodeSource, available_bytes,
                                 requested_length, instruction_length }
```

`DecodeSource` is created before reading bytes and is retained in either arm
of `DecodeResult`.  Thus a rejection has the selected `CpuVariant` and typed
address even when no complete instruction provenance can exist.  The decoder
for this contract accepts only `{ space: m68k_program, ... }` with
`cpu_variant: mc68000`; another address space or CPU variant is outside this
decoder's input contract and must be rejected before byte classification.
Address equality includes both `space` and `value`; the fixed `m68k_program`
space is explicit rather than inferred from an untyped integer.  `address` and
`image_offset` are independently carried: neither is a host pointer and
neither is derived from the other after ingestion.  The two raw bytes are the
bytes that produced the word, in image order.

Only the `Decoded` arm may lift.  It produces `IrMoveq32` by copying the
complete `InstructionProvenance`, CPU variant, typed address, and
`decoded_moveq` outcome; a lifter must not synthesize or erase any of them.
The minimal IR operation means, atomically,
`D[destination] := sign_extend_8_to_32(immediate)` and the PC/CCR updates in
the hardware contract.  Emission likewise copies that provenance and outcome
to its emitted-block metadata.  A `Rejected` arm produces no IR or C block;
emission reports the unchanged rejection result and its typed source instead.
Neither stage may reconstruct provenance from a host buffer.  The scope state
is `D[0..7]: UnsignedLong`, `PC: M68kProgramAddress`, and `SR: unsigned
16-bit`.  `A[0..7]`, memory, devices, cycles, and exception state are outside
this instruction slice and must not be silently changed.

### Strict result categories and precedence

These diagnostics are project policy for untrusted static input, not CPU
exceptions.  Every failure is fail-closed and has this fixed machine-readable
shape:

```text
{ category, cpu_variant, source_address: M68kProgramAddress, image_offset, available_bytes,
  requested_length, instruction_length }
```

`requested_length` is always 2 for this primary-word decoder;
`instruction_length` is `2` once a word was read, otherwise `null`.  Evaluate
in this order:

1. `odd_instruction_address`: `source_address & 1 != 0`; do not read.
2. `truncated_instruction`: fewer than two bytes are available at the mapped
   offset; do not manufacture a word.
3. `illegal_instruction`: the fetched word is source-defined `ILLEGAL`
   (`0x4AFC` in this contract).
4. `decoded_moveq`: a word satisfying the verified `MOVEQ` encoding above.
5. `valid_but_unsupported_instruction`: any other available primary word;
   it is not treated as a no-op or guessed as `MOVEQ`.

Required diagnostic examples (all fields are hexadecimal except counts):

| input | required result |
| --- | --- |
| address `{m68k_program, 0x000101}`, offset `0x01`, two bytes available | `{odd_instruction_address, mc68000, {m68k_program, 0x000101}, 0x01, 2, 2, null}` |
| address `{m68k_program, 0x000100}`, offset `0x00`, bytes `70`, one byte available | `{truncated_instruction, mc68000, {m68k_program, 0x000100}, 0x00, 1, 2, null}` |
| address `{m68k_program, 0x000100}`, offset `0x00`, bytes `4A FC` | `{illegal_instruction, mc68000, {m68k_program, 0x000100}, 0x00, 2, 2, 2}` |
| address `{m68k_program, 0x000100}`, offset `0x00`, bytes `60 00` | `{valid_but_unsupported_instruction, mc68000, {m68k_program, 0x000100}, 0x00, 2, 2, 2}` |

The final row is a deliberately out-of-scope instruction word, not a claim
that all bytes required by `BRA` have been validated.  Odd address wins over
truncation when both predicates hold.

## Project-authored synthetic recipes and exact vectors

Each fixture is two project-authored bytes placed at image offset `0x00` and
mapped to entry PC `0x000100`; it contains no commercial content.  A fixture
record **must** name its byte sequence and the SHA-256 of exactly those image
bytes.  A changed byte sequence requires a changed hash and vector review;
an omitted or mismatched hash is invalid.  The one-instruction execution
budget is project policy: after the one successful instruction, report
`instruction_budget_exhausted`, not a hardware STOP instruction or exception.

All vectors below give the complete in-scope post-state.  The listed `D[]` is
`[D0,D1,D2,D3,D4,D5,D6,D7]`; `SR` includes every 16-bit SR bit, making X
preservation and all unaffected SR bits observable.  The invariant output PC
is `0x000102` and stop reason is `instruction_budget_exhausted`.

| recipe | bytes / SHA-256 | seeded `D[]`; `SR` | exact output `D[]`; `SR` |
| --- | --- | --- | --- |
| `zero-d0` | `70 00` / `882a20e6379eb83aecb916f4b9ea79c1e6c08a0d80f6ad1dc9a9421758246001` | `[11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`; `A71F` | `[00000000,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`; `A714` |
| `positive-max-d5` | `7A 7F` / `d722330687fe2c5050f51dc73efac00f3bf2ab86346778bb95bc3aa7c83957d3` | `[11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`; `001B` | `[11111111,22222222,33333333,44444444,55555555,0000007F,77777777,88888888]`; `0010` |
| `negative-one-d7` | `7E FF` / `534c537dcadcee2477f7eac36121885a1772e4f3420d6fe80eff9b3152509ead` | `[11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`; `2704` | `[11111111,22222222,33333333,44444444,55555555,66666666,77777777,FFFFFFFF]`; `2708` |
| `negative-min-d4` | `78 80` / `5b10ebd5bab6452d26998d79831269695d1c139ae68d3af1737f4ab9cf13dc30` | `[11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`; `200F` | `[11111111,22222222,33333333,44444444,FFFFFF80,66666666,77777777,88888888]`; `2008` |

`0x4AFC` has SHA-256
`3bde2f6cecd078ac886c99e03e1803de1926ed3ae687d158e1cc7a071285256d` and
must yield the `illegal_instruction` diagnostic above; `0x6000` has SHA-256
`f3df0a62b10f205b0f29768aa3d69e777154caaa179f64aabb0a4899c666b017` and
must yield `valid_but_unsupported_instruction`.  They are decode negatives,
not execution-oracle inputs.

## Independent execution oracle

**Selected oracle:** Musashi at commit
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, configured for
`M68K_CPU_TYPE_68000`.  Its headers grant the MIT license (source 3), so a
developer may clone it locally and build a local adapter without committing
Musashi, an adapter, or non-synthetic input to this repository.  Pin
verification is mandatory:

```sh
git clone https://github.com/kstenerud/Musashi.git /tmp/musashi
git -C /tmp/musashi checkout --detach 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
git -C /tmp/musashi rev-parse HEAD
```

Musashi supplies a C API, not a JSON command-line program.  The local,
uncommitted adapter is invoked exactly as
`./moveq-musashi-oracle --input vector.json --output actual.json --instructions 1`.
It must set CPU type 68000; map the two image bytes at `pc`; seed all D
registers and SR; execute exactly one instruction; and read `D0`--`D7`, `PC`,
and `SR` through the documented API.  Its input schema is

```json
{"schema":1,"image_hex":"7000","image_sha256":"…","pc":"00000100",
 "d":["11111111","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"sr":"A71F"}
```

and its required output schema is

```json
{"schema":1,"oracle":"musashi@313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd",
 "executed_instructions":1,"d":["00000000","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000102","sr":"A714","stop_reason":"instruction_budget_exhausted"}
```

Hex strings are uppercase, fixed-width, and have no `0x` prefix.  The adapter
checks `image_sha256` before mapping and rejects unknown fields or malformed
widths.  The harness—not Musashi—sets the budget stop reason.  Musashi is
independent because it is an externally maintained instruction emulator that
fetches and executes the supplied bytes; it shares neither this project's
planned decoder, typed IR, C emitter, nor generated runtime.  It is an oracle
for the four successful vectors only, not evidence that this project supports
exceptions, timing, mapping, self-modifying code, or peripherals.
