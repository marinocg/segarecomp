# MC68000 direct-flow contract (SEG-003)

**Status:** research contract, version 1, for the first register-only direct-flow
slice; it supplies no implementation.  **CPU:** original MC68000 only.
**Accessed:** 2026-08-05.

## Sources, facts, and scope boundary

### Source facts

1. Motorola, *M68000 Family Reference Manual* (1988), section 6,
   instruction entries **MOVEQ -- Move Quick**, **SUBQ -- Subtract Quick**,
   and **Bcc -- Branch Conditionally**.  In each entry, the printed
   “Instruction Format”, “Operation”, “Condition Codes”, and (for `Bcc`)
   “Description” headings are the locators.  Verified scan:
   <https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf>
   (accessed 2026-08-05).  This primary manual establishes the selected
   encodings, operand sizes, instruction lengths, CCR effects, condition
   selection, signed branch displacements, and the PC-relative branch base.
2. Motorola, *M68000 Family Reference Manual* (1988), section 3,
   **Program Counter**, and section 6, **Address Error**, at source 1
   (accessed 2026-08-05).  These establish word-aligned instruction fetch and
   an address-error exception for an odd word access.  They do **not** make a
   static-analysis diagnostic a CPU exception.
3. Karl Stenerud, **Musashi**, commit
   `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, `readme.txt` (“Basic
   configuration” and “Using different CPU types”) and `m68k.h`
   (`m68k_set_cpu_type`, `m68k_execute`, `m68k_get_reg`, `m68k_set_reg`).
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd>
   (accessed 2026-08-05).  Its MIT license is in
   [`m68k.h`](https://raw.githubusercontent.com/kstenerud/Musashi/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd/m68k.h).

The rest of this document explicitly labels **project policy**.  It neither
selects a Genesis/SMS/Game Gear ROM format or mapper nor specifies reset-vector
mapping, timing, prefetch, exceptions, self-modifying code, memory operands,
or peripherals.  `MOVEQ` provenance and primary-word rejection precedence are
consumed from [the completed SEG-002 contract](moveq-contract.md), rather than
redefined here.

## Selected hardware subset

The source-1 format diagrams and operation/CCR tables support these exact
facts (words and displacements are big-endian):

| instruction/form | encoding and length | hardware operation / CCR fact |
| --- | --- | --- |
| `MOVEQ #<s8>,Dn` | `0111 ddd0 iiii iiii`; 2 bytes | Sign-extend `s8` to 32 bits and write `Dn`; sequential PC is `PC+2`. `N`/`Z` describe the long result, `V=C=0`, and `X` is unchanged. |
| `SUBQ.L #1,D0` | `0101 0011 1000 0000` = `53 80`; 2 bytes | `D0 := D0 - 1` as a long.  `X,N,Z,V,C` are set as the `SUBQ` long-result table specifies; other SR bits are preserved; sequential PC is `PC+2`. |
| `BNE.S <d8>` | `0110 0110 dddd dddd`, where `d8 != 0`; 2 bytes | Branch when `Z=0`; otherwise continue at `PC+2`.  The signed 8-bit displacement is added to the PC after the instruction word.  No CCR bit changes. |
| `BRA.S <d8>` | `0110 0000 dddd dddd`, where `d8 != 0`; 2 bytes | Always take the signed-8-bit branch from the PC after the instruction word.  No CCR bit changes. |

Thus the selected `Bcc` condition is only **NE** (`Z=0`); selected `BRA` is
its unconditional true condition.  A zero low displacement byte is deliberately
not selected: source 1 says it supplies a signed 16-bit extension displacement
on MC68000, making that form four bytes.  Long branch displacements are not an
MC68000 form.  Branch targets may be odd in the encoded ISA; source 2 describes
the resulting hardware fetch fault, while this contract rejects such a target
before execution as policy.

### Support matrix and exclusions (project policy)

| accepted exactly | excluded, fail closed |
| --- | --- |
| inherited `MOVEQ #s8,D0` used to seed the loop; `SUBQ.L #1,D0`; `BNE.S` with nonzero `d8`; `BRA.S` with nonzero `d8` | Other `MOVEQ` destinations in this slice; every `SUBQ` size, count, destination, and effective-address form other than the one shown; `TST` and all other arithmetic/test operations; all other `Bcc` conditions; `Bcc`/`BRA` word displacement; every call, return, indirect transfer, memory operand, exception, timing, peripheral, mapper, and code-write behavior. |

The only loop recipe is `MOVEQ #2,D0; SUBQ.L #1,D0; BNE.S -4; BRA.S -6`.
`MOVEQ` is its one register-only initializer.  No form is selected because an
emulator happens to accept it.

## Typed discovery and block-boundary contract (project policy)

All address and instruction records retain SEG-002's `M68kProgramAddress`,
`ImageOffset`, `CpuVariant=mc68000`, and complete `InstructionProvenance`.
The following additional names are contractual data, not required C++ names:

```text
BlockId = { space: m68k_program, entry: unsigned32 }
BlockProvenance = { id: BlockId, entry_instruction: InstructionProvenance,
                    instructions: nonempty ordered InstructionProvenance[] }
DirectEdge = { source_block: BlockId, source_instruction: InstructionProvenance,
               kind: fallthrough | bne_taken | bne_fallthrough | bra_taken,
               condition: always | z_clear | z_set,
               target: M68kProgramAddress }
BlockBoundary = { ordinal: unsigned64, block: BlockProvenance,
                  incoming_edge: DirectEdge | reset_seed,
                  state: { d: unsigned32[8], pc: M68kProgramAddress,
                           sr: unsigned16 },
                  outgoing_edge: DirectEdge | null,
                  executed_blocks: unsigned64, budget: unsigned64,
                  stop_reason: continue | instruction_budget_exhausted |
                  fail_closed_diagnostic }
MappingClaim = { name: string, target_span: [M68kProgramAddress, M68kProgramAddress),
                 image_offset_span: [ImageOffset, ImageOffset) }
```

`BlockProvenance.instructions` is the ordered, complete decoded interval; it
never contains bytes guessed from a target.  An edge copies the *terminating*
instruction provenance, including its source address, image offset, raw bytes,
and length.  A fallthrough edge target is the source instruction address plus
its verified length.  A taken edge uses the signed displacement and source-1
PC base.  State is complete for this scope: all `D0..D7`, 16 SR bits, and typed
PC; `A[]`, memory, devices, cycles, and exception state are explicitly absent.

Discovery has one typed reset seed supplied by ingestion, not an inferred
console reset vector.  It processes a FIFO worklist, initially sorted by
`(space,value)`.  It emits a maximal straight-line block ending in the first
selected direct transfer; it enqueues successors in `fallthrough`, then
`taken` order (only `taken` for `BRA`), deduplicating by `BlockId`.  Symbols are
independent of discovery timing: sort final blocks by `(space,value)` and name
them `m68k_block_%08X` (for example `m68k_block_00000100`).  These rules make
traversal, metadata, and symbols deterministic.

The block budget is a positive maximum number of *completed blocks*, not CPU
cycles or instructions.  Record boundary ordinal 0 at the reset seed before
execution.  After each completed block, increment `executed_blocks`, record
its selected edge and post-block state at its target boundary, and stop without
executing that target when it equals `budget`.  The final record therefore has
the real outgoing edge, target PC, full post-state, and
`instruction_budget_exhausted`; it is not a hardware `STOP` or exception.

## Fail-closed diagnostics (project policy)

Every rejection has the common record
`{category,cpu_variant,block_entry,source_address,image_offset,available_bytes,
requested_length,instruction_length,raw_bytes,target,edge,mapping_claims}`.
Unavailable fields are `null`; `source_address`, `image_offset`, and raw bytes
come from the earliest applicable instruction/edge provenance and are never
reconstructed from a host pointer.  A diagnostic creates no empty block,
no-op, guessed target, or successful termination.

For a source decode: wrong typed space/variant is rejected before reads; then
`odd_instruction_address`, `unmapped_instruction_address`, insufficient bytes
for the primary word, insufficient extension bytes (`truncated_instruction`),
source-defined illegal instruction, selected decode, then
`valid_but_unsupported_instruction` apply in that order.  In particular, a
`Bcc` primary word with `d8=0` requests four bytes before it is rejected as an
unsupported form.

For a decoded selected edge, evaluate target failures in this fixed order:
`odd_direct_target`, `conflicting_address_mapping`, `unmapped_direct_target`,
`mid_instruction_direct_target`, then `reached_unresolved_direct_edge`.
Mapping conflict means two applicable target-address-to-image-offset claims;
the diagnostic retains both claims.  `mapping_claims` serializes the applicable
claims in their ingestion order as
`[{name,target_span,image_offset_span}, ...]`, with hexadecimal target addresses
and decimal image offsets; it is never reordered by a host container.  A
`MappingClaim` span is half-open and both spans have the same byte length.
Mid-instruction means an otherwise mapped target lies strictly inside an already
complete instruction interval.  That interval may be completed structural
`InstructionProvenance` supplied as analysis input; its presence does not imply
that this slice decoded or supports its opcode.
The final category is a persisted edge whose analysis status is unresolved;
execution must trap it rather than dispatch or decode dynamically.

## Project-authored fixture recipes

All bytes below are project-authored synthetic image bytes, mapped one-for-one
at the shown program address, and their SHA-256 covers exactly the shown bytes.
`reset_entry` is a typed discovery seed, not a claim about a console reset
vector.  Initial `D` order is D0 through D7.  The accepted vectors use the
same complete image and provenance `synthetic/SEG-003/direct-loop-v1`:

```text
image: 70 02 53 80 66 FC 60 FA
sha256: a47da47425be5b3ed345cb96295ae2618ad4e5ead18a918d7d64551337f97749
blocks: 00000100 = [7002], 00000102 = [5380,66FC], 00000106 = [60FA]
initial D: [11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]
initial SR: 2700
```

| vector | exact boundary edge sequence (post-state `D0`, `SR`, `PC`) | final result |
| --- | --- | --- |
| `loop-bne-taken-and-fallthrough` | reset entry `100`; `100 --fallthrough--> 102` (`00000002`, `2700`, `102`); `102 --bne_taken--> 102` (`00000001`, `2700`, `102`); `102 --bne_fallthrough--> 106` (`00000000`, `2704`, `106`); `106 --bra_taken--> 102` (`00000000`, `2704`, `102`) | full D is `[00000000,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`; budget 4; `instruction_budget_exhausted`. |
| `bne-fallthrough` | same complete bytes/hash/provenance, but reset entry `102`, initial D `[00000001,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`, initial SR `2700`; `102 --bne_fallthrough--> 106` (`00000000`, `2704`, `106`); `106 --bra_taken--> 102` (`00000000`, `2704`, `102`) | same full final D/SR/PC; budget 2; `instruction_budget_exhausted`. |

The following discovery-only negatives have no oracle execution.  Unless a row
says otherwise, their reset entry is `100`, initial D is
`[11111111,22222222,33333333,44444444,55555555,66666666,77777777,88888888]`,
initial SR is `2700`, and budget is 1.  Their final state is that unchanged
input state and their stop reason is the stated diagnostic.  They are also
synthetic and retain the stated source/edge provenance.

| recipe | bytes / SHA-256 / setup | required category and decisive provenance |
| --- | --- | --- |
| `truncated-bcc-word` | `66 00` / `887a0529fd9d76eb33c0ec808c05daaf32fa9f5e21e4c32224fb4bfaaa0af385`; map `100..101` only | `truncated_instruction`, source `100`, offset 0, available 2, requested 4, primary provenance bytes `6600`. |
| `odd-target` | `60 01` / `9e67b12fd8c58953460459cad7a6d4dd7d6d57594affce8206d1397c9c4db543` at `100` | `odd_direct_target`; edge source `100`, bytes `6001`, target `103`. |
| `unmapped-target` | `60 7E` / `b551069b2b515fc50b66b2b02a38db492e98019263978843200733a6f67720cb`; only `100..101` mapped | `unmapped_direct_target`; edge source `100`, target `180`. |
| `conflicting-map` | `60 02 4E 71` / `eb39bc7a4f01dce785fe4cc232b0be05a398d421ce1cc4d07a065ae39c2c4176`. The source claim is `entry = {name:"entry",target_span:[0x100,0x102),image_offset_span:[0,2)}`. The two named, applicable target claims are, in ingestion order, `low_copy = {name:"low_copy",target_span:[0x104,0x106),image_offset_span:[0,2)}` and `high_copy = {name:"high_copy",target_span:[0x104,0x106),image_offset_span:[2,4)}`. | Decode `6002` at target `100` through `entry`; its target `104` is ambiguous **before target decode**. Require `conflicting_address_mapping`, edge source `100`, bytes `6002`, target `104`, and exact ordered diagnostic provenance `mapping_claims:[{name:"low_copy",target_span:[0x104,0x106),image_offset_span:[0,2)},{name:"high_copy",target_span:[0x104,0x106),image_offset_span:[2,4)}]`. |
| `mid-instruction-target` | `60 00 00 04 60 FC` / `31140d81f405e6f87e45f6ab241e0a17ba41eab1044cc989a110e40072de56d3`; map `100..105` one-for-one, reset entry `104`. Fixture precondition: analysis input already contains completed structural `InstructionProvenance` `{source_address:100,image_offset:0,raw_bytes:60000004,length:4}` and therefore interval `[100,104)`. Discovery starts at `104` and decodes only the selected short `BRA` `60FC`; it does not FIFO-discover or decode the excluded `BRA.W` bytes at `100`. | The decoded edge source `104`, bytes `60FC`, target `102` must be validated against the supplied interval before any target decode: `100 < 102 < 104` is true, so require `mid_instruction_direct_target` with target `102` and that interval as decisive provenance. This is structural overlap metadata for the target validator, not new executable `BRA.W` support. |
| `unsupported-nop` | `4E 71` / `da9c9d32a1f9be264c491f8c1988ec06d1cdd81d302d5a163a8413eda1997394` at `100` | `valid_but_unsupported_instruction`; source `100`, offset 0, bytes `4E71`, length 2. |
| `reached-unresolved-edge` | `60 02` / `1a33f434c3fc58e156600f1814ef65f7c14ef8f9d2647208ff106b232120c871`; inject persisted unresolved status for its otherwise decoded edge | `reached_unresolved_direct_edge`; source `100`, bytes `6002`, target `104`, and the stored unresolved reason. |

## Independent boundary oracle

Musashi source 3 is legally usable locally at the pinned revision and is
independent of this decoder/lifter/emitter.  The ignored local adapter actually
used for this contract is
`/root/dev/segarecomp/.tools/m68k-musashi-direct-flow-oracle`.  Its executable
interface is `--input INPUT --output OUTPUT --blocks BLOCKS`.  Reproduce the
recorded run without committing the checkout, generated opcode files, adapter,
or input/output artifacts:

```sh
test "$(git -C /root/dev/segarecomp/.tools/musashi rev-parse HEAD)" = \
  313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
/root/dev/segarecomp/.tools/m68k-musashi-direct-flow-oracle \
  --input /root/dev/segarecomp/.tools/direct-flow-loop.json \
  --output /root/dev/segarecomp/.tools/direct-flow-loop.actual.json --blocks 4
```

The actually used input JSON object has exactly these fields: `image_hex`
(hex image bytes), `image_sha256` (uppercase 64-hex SHA-256), `reset_entry`
(a `0x`-prefixed address string), `d` (eight `0x`-prefixed 32-bit register
strings in D0--D7 order), `sr` (a `0x`-prefixed 16-bit string), and
`block_instruction_counts` (an array of positive instruction counts).  For the
first accepted loop it is:

```json
{"image_hex":"7002538066fc60fa","image_sha256":"A47DA47425BE5B3ED345CB96295AE2618AD4E5EAD18A918D7D64551337F97749","reset_entry":"0x100","d":["0x11111111","0x22222222","0x33333333","0x44444444","0x55555555","0x66666666","0x77777777","0x88888888"],"sr":"0x2700","block_instruction_counts":[1,2,2,1]}
```

`--blocks` is the requested number of supplied block counts to execute.  The
adapter's output JSON has `oracle`, `executed_blocks`, `boundaries`, and
`stop_reason`.  Each `boundaries` member is a post-block state object with
`d` (eight fixed-width uppercase hexadecimal strings), `pc` (fixed-width
uppercase hexadecimal string), and `sr` (fixed-width uppercase hexadecimal
string).  It does **not** emit project `BlockBoundary` provenance or edges;
those remain the contract's static-analysis records.  This adapter supplies an
independent CPU-state boundary oracle only, and the supplied instruction-count
array—not Musashi—defines the block cuts and budget stop.

On 2026-08-05, the pinned checkout revision was
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` and the command above produced:

```json
{"boundaries":[{"d":["00000002","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000102","sr":"2700"},{"d":["00000001","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000102","sr":"2700"},{"d":["00000000","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000106","sr":"2704"},{"d":["00000000","22222222","33333333","44444444","55555555","66666666","77777777","88888888"],"pc":"00000102","sr":"2704"}],"executed_blocks":4,"oracle":"musashi@313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd","stop_reason":"instruction_budget_exhausted"}
```

Thus the state boundaries are respectively `(D0=2, SR=2700, PC=102)`,
`(D0=1, SR=2700, PC=102)`, `(D0=0, SR=2704, PC=106)`, and
`(D0=0, SR=2704, PC=102)`; D1--D7 retain their supplied values in every
record.  This confirms the first accepted loop's four contract boundaries,
including taken and fallthrough `BNE` behavior and budget exhaustion.  It does
not validate unexecuted vectors, discovery diagnostics, timing, self-modifying
code, mapper behavior, peripherals, exceptions, or instruction semantics
beyond this synthetic execution.
