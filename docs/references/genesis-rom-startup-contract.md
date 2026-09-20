# Bounded Genesis ROM startup contract (SEG-005)

**Status:** research contract for SEG-005-T001; it supplies no implementation or fixture.
**Platform/CPU:** Genesis/Mega Drive, original MC68000 only. **Accessed:** 2026-08-06.

This defines one project-authored, synthetic, recognized-Genesis experiment.  Under the inherited
SEG-000 classification ingress and SEG-001 reset-image ingress, reset selects ROM code which writes
and reads one work-RAM longword, makes one direct absolute-long call, and performs that call's one
statically associated return.  It does not claim that a commercial image boots.

## Sources, facts, and policy boundary

| ID | Public source, URL, access date, and locator | Fact used here |
| --- | --- | --- |
| M1 | Motorola, *M68000 Family Programmer's Reference Manual* (1988), [public scan](https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf), accessed 2026-08-06; §1 **Data Organization**; §2 **Addressing Modes**, “Absolute Data Addressing”; §3 **Address Registers** and **Program Counter**; §6 **MOVEQ**, **MOVE**, **JSR**, **RTS**, **ILLEGAL**, and **Address Error** entries, under “Instruction Format”, “Operation”, “Description”, and “Condition Codes”. | Original MC68000 addresses are 24 bits; multi-byte values are big-endian; instruction/word/longword accesses require even addresses; absolute-long uses a 32-bit extension; `MOVE.L` has its stated data/CCR effects; `JSR` saves a return PC and transfers to its effective address; `RTS` obtains PC from the stack; `0x4AFC` is `ILLEGAL`. |
| S1 | Sega Enterprises, *Genesis Technical Overview*, [Sega Retro archive entry](https://segaretro.org/Genesis_Technical_Overview), accessed 2026-08-06; PDF p. 3, **68000 Memory Map**, rows `$000000–$3FFFFF Cartridge ROM` and `$E00000–$FFFFFF Work RAM`. | Primary console source for the ROM and work-RAM areas.  The archive currently has a browser challenge; the title/page/table/row locator remains auditable. |
| S2 | PlutieDev, [*Memory map*](https://plutiedev.com/memory-map#68000-address-space), accessed 2026-08-06; **68000 address space** rows `$000000–$3FFFFF Cartridge slot` and `$E00000–$FFFFFF Work RAM`. | Independently corroborates S1's two areas. |
| O1 | Karl Stenerud, [*Musashi*](https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd), commit `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, accessed 2026-08-06; `readme.txt`, **Basic configuration** and **Using different CPU types**; [`m68k.h`](https://raw.githubusercontent.com/kstenerud/Musashi/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd/m68k.h), `m68k_set_cpu_type`, `m68k_pulse_reset`, `m68k_execute`, `m68k_get_reg`, and `m68k_set_reg`. | A local MIT-licensed adapter can select `M68K_CPU_TYPE_68000`, provide memory callbacks, reset, execute, and observe state.  It is an oracle, not a Genesis-hardware or ISA authority. |

M1/S1 are primary sources.  The header-recognition facts and ingress are inherited from
[SEG-000](header-classification-contract.md); finite identity mapping and vector validation are
inherited unchanged from [SEG-001](genesis-reset-image-contract.md).  Typed provenance,
fail-closed static emission, and decode/discovery discipline are inherited from
[SEG-002](moveq-contract.md), [SEG-003](direct-flow-contract.md), SEG-004, and the
[SEG-012 migration contract](../architecture/m68k-pipeline-migration-contract.md).

Everything below explicitly labelled **project policy** is not a hardware assertion.  In
particular, diagnostics, abstract bus records, static call contexts, and instruction budgets are
not exceptions, physical bus cycles/prefetch, hardware return prediction, or timing evidence.

## Selected map and instruction subset (project policy)

All intervals are half-open. `M68kProgramAddress` is `{space:m68k_program,value:uint32}`;
addresses, image offsets, raw bytes, and verified lengths are independent and never host pointers.
An address used by this slice has a zero high byte and is never masked or wrapped.

| Region | selected range | access | mapping/provenance |
| --- | --- | --- | --- |
| `raw_cartridge_rom` | `[0x000000,image_length)`, `image_length <= 0x400000` | instruction/data read; no write | inherited SEG-001 identity mapping, `image_offset = address` |
| `synthetic_work_ram` | canonical `[0x00ff0000,0x01000000)` | data read/write only | `ram_offset = address - 0x00ff0000` |

S1/S2 name a larger work-RAM area.  Selecting only its final canonical 64 KiB and rejecting aliases
is conservative policy, not a claim that hardware lacks mirrors.  There is no VDP, Z80, I/O, TMSS,
SRAM, mapper, expansion, RAM instruction fetch, or other region map.

| Form | bytes / length | selected effect |
| --- | --- | --- |
| inherited `MOVEQ #s8,D0` | `70 ii`, 2 | SEG-002 semantics and provenance. |
| `MOVE.L D0,(xxx).L` | `23 C0 aa aa aa aa`, 6 | Resolve absolute-long `aaaa`; write big-endian D0; PC `+6`; M1 `MOVE.L` CCR effects. |
| `MOVE.L (xxx).L,D1` | `22 39 aa aa aa aa`, 6 | Resolve `aaaa`; read a big-endian longword to D1; PC `+6`; M1 `MOVE.L` CCR effects. |
| direct `JSR (xxx).L` | `4E B9 aa aa aa aa`, 6 | Resolve an even selected ROM target; predecrement A7 by 4, write sequential PC (`PC+6`) big-endian, then set PC to target; CCR unchanged. |
| selected `RTS` | `4E 75`, 2 | Read PC at A7 and return only to the statically associated direct-call continuation described below; CCR unchanged. |

The JSR/RTS ordering is a representation policy for M1's stack semantics, not a physical bus-order
claim.  No indirect-target discovery or runtime target-byte decoding is permitted.  `RTS` is
selected **only** as the matched return from a selected direct absolute-long JSR in this experiment;
an arbitrary stack-pop transfer, a return with no selected caller, and every other return form fail
closed.

## Typed static call/return contract (project policy)

Before emission, the single shared static route retains the following records in addition to the
inherited decoded instruction and `InstructionProvenance` records:

```text
CallId = { caller: InstructionProvenance, continuation: M68kProgramAddress,
           callee: M68kProgramAddress }
IrDirectCallAbsoluteLong = { provenance, target, continuation, call_id }
IrStaticReturn = { provenance, call_id, expected_continuation }
DirectCallEdge = { source_instruction: InstructionProvenance, kind: direct_call,
                   target, continuation, call_id }
ReturnEdge = { source_instruction: InstructionProvenance, kind: return_to_continuation,
               target: expected_continuation, call_id }
StaticCallFrame = { call_id, expected_continuation }
```

`continuation` is the caller address plus its verified six-byte length.  Discovery associates the
selected callee `RTS` with that one `CallId`; it must retain the caller, callee, continuation, raw
bytes, lengths, target, and image offsets on the IR and both edges.  It may not infer an association
from a numeric stack value.  Emission pushes `StaticCallFrame` when it emits the selected call and
requires the matching frame to be the LIFO top at the selected RTS.  The stack longword remains
observable RAM state, but does not authorize a new return target.

At RTS execution, validate `[A7,A7+4)` for canonical-RAM alignment and range before its stack read.
After that valid selected stack read and before PC/A7 mutation or dispatch, require a nonempty LIFO
frame with this RTS's `call_id`, then require the popped big-endian word to equal that frame's
`expected_continuation`.  `return_context_missing` precedes the comparison; a differing word yields
`return_target_mismatch`.  Both are stable fail-closed diagnostics, retain the RTS and associated
call provenance, and produce no dispatch, target decode, target validation, or implicit block.  On
equality, increment A7, pop the frame, set PC to the continuation, and emit `ReturnEdge`.  This is a
bounded static return policy, not general call/return recovery.

## Records, deterministic validation, and stops (project policy)

```text
BusRecord = { ordinal:uint64, kind:instruction_read|data_read|data_write|stack_read|stack_write,
              address:M68kProgramAddress, length:1|2|4, bytes:byte[length],
              region:raw_cartridge_rom|synthetic_work_ram,
              instruction:InstructionProvenance }
StartupBoundary = { ordinal:uint64, after:reset_seed|InstructionProvenance,
                    d:uint32[8], a7:uint32, sr:uint16, pc:M68kProgramAddress,
                    ram_bytes:ordered {address,bytes}[], bus_through_ordinal:uint64|null,
                    stop_reason:continue|instruction_budget_exhausted }
AttemptedSource = { cpu_variant:CpuVariant, source_address:M68kProgramAddress,
                    image_offset:MoveqImageOffset|null }
StartupFailure = { ordinal:uint64, attempted_source:AttemptedSource,
                   complete_instruction:InstructionProvenance|null,
                   primary_provenance:InstructionProvenance|null,
                   retained_boundary_ordinal:uint64, bus_through_ordinal:uint64|null,
                   category, effective_address, attempted_range, region, a7, available_bytes,
                   requested_length, call_id, expected_continuation, observed_return_target }
```

`attempted_source` records the typed source submitted for the failing attempt; its image offset is
null when no mapped image offset is available.  It is present even when no instruction bytes can be
verified.  `complete_instruction` is present only when the entire attempted instruction has verified
bytes and length.  `primary_provenance` is present only when a verified two-byte primary must be
retained without a complete instruction (the selected-six-byte extension truncation case).  Thus a
primary truncation has neither instruction provenance, an extension truncation retains only its
two-byte primary provenance, and a full primary or full six-byte rejection retains complete
provenance.  `raw_bytes` and `instruction_length` are properties of those provenance records, not
duplicated failure fields.  No null field or absent provenance permits invented bytes, length, EA,
or mapping facts.  Every complete selected instruction rejected during operand validation retains
that complete instruction provenance.

`instruction_read` is one complete verified decoded span, not a fetch/prefetch trace. `ram_bytes`
contains changed ranges only, sorted by address.  Null fields are unavailable, not guessed.  The
complete scoped CPU state is `D0..D7`, A7, all 16 SR bits, and typed PC; `A0..A6`, USP, exception
state, cycles, and devices are unavailable.  A reset boundary is always ordinal 0, uses
`after:reset_seed`, has the complete vector-derived CPU state and zero changed RAM, and has
`bus_through_ordinal:null`: reset-vector reads are inherited ingress validation, not this execution
bus trace.  Each success preserves every preceding boundary and appends exactly one next boundary.
A failure preserves those successes and appends exactly one `StartupFailure` after the last retained
boundary; it never rewrites that boundary or invents a successful post-failure boundary.

For a mapped source, evaluate in this exact order: (1) wrong typed space/CPU (before a read), (2)
odd instruction address, (3) unmapped instruction address, (4) primary truncation (`available < 2`,
`requested_length:2`, no word or instruction length), (5) source-defined `ILLEGAL` primary
`4AFC`, (6) primary classification, and (7) selected-instruction extension validation.  Primary
classification uses only the complete two-byte primary, in this order: a primary in a selected
opcode family but not this slice's selected form is `unsupported_instruction_form`; an exact
selected form proceeds to extension validation (if any); every unrelated complete primary is
`valid_but_unsupported_instruction`.  In particular, `4E90` is a JSR-family primary with an
unselected effective-address form and is `unsupported_instruction_form`, while `4E71` is unrelated
to every selected family and is `valid_but_unsupported_instruction`.  This classification neither
selects an additional instruction nor reads an extension for either rejection; it preserves the
inherited fail-closed distinction between a recognized opcode family/form rejection and an
unrelated primary rejection.

An exact selected six-byte primary then evaluates extension availability before reading an
extension: `2 <= available < 6` is `truncated_instruction` with `requested_length:6`,
`instruction_length:null`, and retained two-byte primary provenance; only `available >= 6` creates
six-byte provenance and resolves its EA.  Thus `ILLEGAL` and rejected primaries never request
extension bytes; an odd address wins over all truncation; and a complete selected primary with a
short extension is never relabelled unsupported.  The primary word is `raw_bytes` for an extension
truncation, while the safely available source bytes and count remain in `available_bytes`;
implementations must not manufacture the missing bytes.

After a complete selected instruction, operand roles have separate, non-interchangeable validation
sequences.  For either absolute-long `MOVE` data operand, validate in this order:
`effective_address_not_24bit` (EA-not-24-bit), `odd_effective_address` (odd EA), then, for the
store only, `rom_write_prohibited`, then `unmapped_data_access`.  A rejected data operand emits no
data bus record; a valid load emits its `data_read` and a valid store emits its `data_write` only
after this sequence succeeds.

For direct absolute-long `JSR`, validate the resolved direct target in this exact semantic order:
`effective_address_not_24bit` (EA-not-24-bit), `odd_direct_target`,
`unmapped_direct_target`.  Success requires an even, selected, statically mapped ROM instruction;
it never decodes target bytes at runtime.  Only after target validation, derive `[A7-4,A7)` and
validate `invalid_stack_alignment`, then `invalid_stack_range`, before every mutation.
`invalid_stack_alignment` means that the current A7 or the derived four-byte stack access violates
this slice's selected even-alignment requirement.  A JSR stack failure retains the current A7, the
derived attempted range/address (`[A7-4,A7)` and its start), and the expected continuation where
available.  Either JSR stack failure emits no `stack_write` (and no other stack bus record),
`DirectCallEdge`, static frame, or dispatch, and changes neither PC, A7, nor RAM.  A rejected direct
target likewise emits no stack bus record, call edge, or frame and makes none of those changes.

For selected `RTS`, derive `[A7,A7+4)` and validate `invalid_stack_alignment`, then
`invalid_stack_range`, before its `stack_read`.  Only a valid interval emits that `stack_read`; then
evaluate `return_context_missing`, followed by `return_target_mismatch`, and succeed only with the
matching static frame and stack word.  Every RTS stack-alignment/range failure has no stack bus
record and no PC/A7/RAM/frame/dispatch change.  A valid read followed by either return failure has
that one `stack_read` but no PC/A7/frame/dispatch change and emits no `ReturnEdge`.  There is no
dynamic return-target validation, mapping, decoding, or dispatch.  ROM-write protection therefore
applies only to a MOVE write; unsupported forms have complete provenance but no operand, stack, or
bus access.  A failure produces no generated successful execution or runtime opcode decoder.

## Synthetic recognized-Genesis fixture and expected record

No binary is created.  A future fixture constructs a zero-filled array of length `0x17A`, writes
only the following values, and leaves every other byte zero.  The inherited classifier therefore
recognizes `SEGA MEGA DRIVE ` at `[0x100,0x110)` and its complete domestic title at `[0x120,0x150)`;
execution is deliberately reserved until `0x160`, beyond the required header/title range.

```text
fixture_id: synthetic/SEG-005/recognized-reset-jsr-rts-v2
W32(0x000, 0x00ff0100)                 # initial SSP / A7
W32(0x004, 0x00000160)                 # reset PC
ASCII(0x100, "SEGA MEGA DRIVE ")        # exactly 16 bytes
ASCII(0x120, "SYNTHETIC SEG-005 STARTUP") then ASCII space through 0x14F
W(0x160, 702a)                         # MOVEQ #42,D0
W(0x162, 23c000ff0000)                 # MOVE.L D0,($00ff0000).L
W(0x168, 4eb900000178)                 # JSR ($00000178).L
W(0x16e, 223900ff0000)                 # MOVE.L ($00ff0000).L,D1
W(0x178, 4e75)                         # RTS
sha256: 825e60ffd0a3bebf73766c0a3cb4477312488d7efd8eeb815caf2ac846443fea
```

`W` writes displayed big-endian hex bytes; `W32` is big-endian.  The hash covers exactly `0x17A`
bytes.  Initial `D` is `[11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`,
`SR=2700`, and all canonical RAM is zero.  The boundary sequence is reset/`0160`; MOVEQ/`0162`;
store/`0168`; JSR/`0178` with `A7=00FF00FC` and `FF00FC:0000016E`; RTS/`016E` with `A7=00FF0100`;
and load/`0174`, budget stop.  At every boundary D2--D7 retain their initial values.  Final D is
`[0000002A,0000002A,33333333,44444444,55555555,66666666,77777777,88888888]`, SR is `2700`, and
changed RAM is `FF0000:0000002A, FF00FC:0000016E`.

The ordered bus records are: `0 instruction_read 0160/702A`; `1 instruction_read
0162/23C000FF0000`; `2 data_write FF0000/0000002A`; `3 instruction_read 0168/4EB900000178`; `4
stack_write FF00FC/0000016E`; `5 instruction_read 0178/4E75`; `6 stack_read FF00FC/0000016E`; `7
instruction_read 016E/223900FF0000`; `8 data_read FF0000/0000002A`.  Each record's instruction
provenance has its listed source and identity image offset.  The final boundary has
`bus_through_ordinal:8` and `instruction_budget_exhausted`; this is a project budget, not STOP.

## Required negative recipes (project policy)

Each starts from the accepted recipe, changes only the listed bytes/state, retains every earlier
successful boundary and bus record, and then appends the one failure record at the attempting
instruction.  A complete selected instruction that reaches operand or stack validation appends its
one `instruction_read` before that validation; a decode rejection and an incomplete instruction do
not.  The bus outcomes below state every record attributable to the failing attempt.  No row is an
oracle-success vector.

| ID / variation | required deterministic result |
| --- | --- |
| `primary-truncated` | shorten the image to `0x161`; the complete inherited recognized header and reset vector remain valid, while decode at `0160` has one byte and yields `truncated_instruction`, requested 2, with `attempted_source` only (no complete or primary provenance) and no bus record. |
| `extension-truncated-store` | shorten to `0x166`; `23C0` at `0162` is retained as two-byte `primary_provenance` and yields `truncated_instruction`, available 4, requested 6, with no complete instruction provenance or bus record. |
| `extension-truncated-jsr` | shorten to `0x16C`; `4EB9` at `0168` retains two-byte `primary_provenance` and yields `truncated_instruction`, available 4, requested 6, with no complete instruction provenance, bus record, stack write, or target guess. |
| `illegal-primary` | replace `702A` with `4AFC`; `illegal_instruction` at `0160`, with complete two-byte provenance, before selected-form decoding and with no bus record. |
| `unsupported-primary` | replace `702A` with unrelated complete primary `4E71`; `valid_but_unsupported_instruction` at `0160`, with complete two-byte provenance, without extension read or bus record. |
| `rom-write` | replace store extension `0164..0167` with `00000100`; `rom_write_prohibited` at `0162`, EA `000100`, with its `instruction_read` only and no data write. |
| `odd-data` | replace it with `00ff0001`; `odd_effective_address`, EA `FF0001`, with its `instruction_read` only and no data write. |
| `unmapped-data` | replace it with `00a00000`; `unmapped_data_access`, EA `A00000`, with its `instruction_read` only and no device access. |
| `invalid-stack-alignment` | initial SSP `00ff0101`; `invalid_stack_alignment` at JSR `0168` (not an EA category), with its `instruction_read` only: no stack bus record, stack write, frame, PC/A7/RAM, or dispatch change. |
| `invalid-stack-range` | initial SSP `00ff0000`; `invalid_stack_range` at JSR `0168` (not an EA category), with its `instruction_read` only: no stack bus record, stack write, frame, PC/A7/RAM, or dispatch change. |
| `return-target-mismatch` | a return-unit setup retains the selected JSR's `StaticCallFrame` and post-JSR boundary but supplies RAM `FF00FC:00000170`; the failing attempt emits its `instruction_read` and then its valid `stack_read`, then emits `return_target_mismatch`, expected `0000016E`, observed `00000170`, with no PC/A7/frame mutation or dispatch.  This injected malformed stack state is a negative harness input, not selected self-modifying behavior. |
| `return-context-missing` | a return-unit setup supplies the valid stack word but omits the matching `StaticCallFrame`; the failing attempt emits its `instruction_read` and then its valid `stack_read`, then emits `return_context_missing`, with no PC/A7/frame mutation or dispatch. |
| `unsupported-form` | replace `4EB9` at `0168` with JSR-family but unselected-form primary `4E90`; `unsupported_instruction_form`, with complete two-byte provenance and no target read/guess or bus record. |

All other MOVEQ destinations, MOVE sizes/EAs, JSR modes, BSR, indirect calls, arbitrary returns,
STOP, exceptions, code writes, mapper behavior, timing, self-modifying code, and peripherals are
unsupported and fail closed with the earliest retained provenance.

## Independent Musashi oracle (project policy)

Musashi and its adapter are local and ignored.  The harness requires
`SEGARECOMP_STARTUP_MUSASHI_ORACLE` to name an executable adapter and verifies its adjacent checkout
before running it:

```sh
test -x "$SEGARECOMP_STARTUP_MUSASHI_ORACLE"
test "$(git -C "$(dirname "$SEGARECOMP_STARTUP_MUSASHI_ORACLE")/musashi" rev-parse HEAD)" = \
  313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
"$SEGARECOMP_STARTUP_MUSASHI_ORACLE" --input "$input" --output "$output" --instruction-counts 1,1,1,1,1
```

The adapter selects `M68K_CPU_TYPE_68000`, verifies the fixture SHA before mapping, invokes reset
from image vectors (it does not inject PC/A7), maps only the contract ROM/RAM callbacks, rejects
unknown fields, bad widths, map deviations, and malformed hashes, and executes exactly one
instruction for each of the five requested boundaries.  Its input includes exact `image_hex`, the
uppercase SHA-256, initial eight D registers, SR, and a `ram_hex` of exactly `0x10000` zero bytes.
Each of five ordered output boundaries includes complete `d[8]`, `a7`, `pc`, `sr`, and complete
`ram_hex` (exactly `0x10000` bytes), plus `executed_instructions:1`; the final output has total
`executed_instructions:5` and `stop_reason:"instruction_budget_exhausted"`.  Hex is uppercase,
fixed-width, and has no `0x` prefix.  Thus the oracle compares complete scoped CPU/RAM state at
each one-instruction cut, not merely final changed ranges.

The project harness—not Musashi—derives and compares `BusRecord`, decoded/lifted provenance,
`StaticCallFrame`, call/return edges, diagnostics, and budget records.  Musashi therefore supplies
only independent bounded MC68000 CPU/RAM-state evidence and does not validate Genesis mapping,
headers, static discovery, timing, exceptions, self-modifying code, or peripherals.

## Explicit non-goals

- No commercial ROM, checksum/bootability claim, mapper/banking, SRAM, ROM mirroring, TMSS, or
  reset sequencing beyond inherited synthetic ingress.
- No timing, cycles, prefetch, arbitration, exceptions/address-error frames, interrupts, VDP, Z80,
  I/O, peripherals, or device behavior.
- No RAM execution, runtime opcode fetch/decode, indirect flow, general function recovery, or
  calls/returns beyond this one statically paired JSR/RTS.
- No MC68010/68020+ behavior, stack frames, USP switching, or general effective-address support.
