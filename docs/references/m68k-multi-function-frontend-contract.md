# MC68000 multi-unit frontend contract (SEG-004)

**Status:** research contract; it supplies neither implementation nor fixtures.
**CPU:** original MC68000 only. **Accessed:** 2026-08-06.

## Evidence, inheritance, and terminology

This contract selects no hardware behavior beyond the direct-flow subset in
[MC68000 direct-flow contract (SEG-003)](direct-flow-contract.md), §§ “Selected
hardware subset” and “Support matrix and exclusions”.  The original public
sources and exact printed locators are inherited rather than re-described:

1. Motorola, *M68000 Family Reference Manual* (1988), section 6, entries
   **MOVEQ -- Move Quick**, **SUBQ -- Subtract Quick**, and **Bcc -- Branch
   Conditionally**, headings “Instruction Format”, “Operation”, “Condition
   Codes”, and, for `Bcc`, “Description”; and section 3, **Program Counter**,
   and section 6, **Address Error**.  The verified public scan and access date
   are recorded in SEG-003 § “Sources, facts, and scope boundary”:
   <https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf>.
2. Karl Stenerud, **Musashi**, commit
   `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, `readme.txt` headings “Basic
   configuration” and “Using different CPU types”, and `m68k.h` symbols
   `m68k_set_cpu_type`, `m68k_execute`, `m68k_get_reg`, and `m68k_set_reg`.
   The public source and MIT-license locator are recorded in SEG-003 §
   “Sources, facts, and scope boundary”:
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd>.

The selected facts are consequently exactly `MOVEQ #s8,D0`, `SUBQ.L #1,D0`,
nonzero-displacement `BNE.S`, and nonzero-displacement `BRA.S`, including their
two-byte encodings, PC bases, and CCR effects in SEG-003 § “Selected hardware
subset”.  `MOVEQ` source/provenance and primary-word precedence remain governed
by [MC68000 `MOVEQ` contract (SEG-002)](moveq-contract.md), §§ “Project decode,
provenance, and IR policy” and “Strict result categories and precedence”.

An **emission unit** below is a static lowering unit for a strongly connected
component (SCC) of the discovered direct-edge graph.  It is not a recovered
target-language function, a hardware calling convention, a call graph node, or
a claim that either supplied entry is a console reset vector.  This terminology
is deliberately narrower than “function”: the required fixture has exactly two
SCC lowering units and makes no target-function-recovery claim.

## Input, shared route, and provenance (project policy)

The frontend accepts only a validated typed program:

```text
FrontendImageSourceId = immutable stable string
FrontendImage   = { source_id: FrontendImageSourceId,
                    bytes: immutable byte sequence,
                    byte_length: uint64 }
FrontendProgram = { cpu_variant: mc68000,
                      image: FrontendImage,
                      mapping_claims: MappingClaim[] in ingestion order,
                     analysis_entries: immutable ordered
                                       M68kProgramAddress[] }
FrontendOracleVector = { vector_id: unique stable string,
                         fixture_id: string,
                         image_sha256: sha256,
                         cpu_variant: CpuVariant,
                         execution_entry: M68kProgramAddress,
                         initial_registers: D0--D7,
                         initial_sr: unsigned16,
                         block_instruction_counts: positive unsigned[],
                         expected_boundaries: ordered Boundary[] }
FrontendResult  = Accepted { decoded: ordered DecodedInstruction[],
                             ir: ordered M68kIrOperation[],
                             blocks: ordered BlockProvenance[],
                             edges: ordered DirectEdge[],
                             units: ordered StaticEmissionUnit[] }
                | Rejected { diagnostic: DirectFlowDiagnostic }
StaticEmissionUnit = { ordinal: unsigned32, id: `m68k_unit_%08X`,
                       members: nonempty ordered BlockId[],
                       entry_block: BlockId,
                       provenance: nonempty ordered InstructionProvenance[] }
```

`M68kProgramAddress`, `ImageOffset`, `ByteLength`, `CpuVariant`,
`InstructionProvenance`, `MappingClaim`, `BlockProvenance`, `DirectEdge`, and
the complete diagnostic shape have exactly the meanings in SEG-002 § “Project
decode, provenance, and IR policy” and SEG-003 §§ “Typed discovery and
block-boundary contract” and “Fail-closed diagnostics”.  In particular, the
typed address, image offset, raw bytes, and verified byte length are independent
values copied from decode to selected instruction, IR operation, block, edge,
unit, metadata, and rejection; none is reconstructed from a host pointer or
from another provenance field.

`FrontendImage.bytes` is the only typed byte source from which this frontend
may read. `source_id` identifies that immutable supplied source; it is neither
a host path nor a pointer-derived identity. A valid source ID is a present,
nonempty string; its spelling is retained verbatim and is not normalized.
`byte_length` is `uint64`, and must exactly equal the byte-sequence length
before a mapping or decode read is considered. The fixture hash identifies
these exact bytes, while `source_id` identifies the supplied frontend input;
neither substitutes for the other.

Each `MappingClaim` has nonempty half-open target and image-offset spans. Its
span end and length computations use checked `uint64` arithmetic: no endpoint,
length, or address-plus-length calculation may wrap. The target and image spans
must have exactly equal lengths, the image span must be contained in
`[0, FrontendImage.byte_length)`, and byte `n` of a claim maps only to byte `n`
of its paired span. This is the claim's one-to-one mapping rule. Claims remain
in ingestion order; overlapping target claims are not silently coalesced, so
the inherited `conflicting_address_mapping` result remains observable when a
decoded edge target has more than one applicable claim.

A source-address read is claim-bounded. After source resolution identifies one
claim, a request of length `N` is available only if the complete target interval
`[source, source + N)` is within that same claim and its corresponding image
interval is within the declared image length; both interval calculations are
checked. A read may not cross into an adjacent claim, infer bytes from another
claim, or read from a host buffer outside `FrontendImage.bytes`.

Before every source decode, apply these checks in this exact order and stop at
the first failure: (1) validate `cpu_variant` and the source typed space; (2)
validate the frontend image, in this order: (2a)
`invalid_frontend_image_source_id` when `source_id` is missing, is not a
string, or is empty; (2b) `frontend_image_byte_length_mismatch` when the
declared `byte_length` differs from `bytes.length`; then (2c) validate every
claim in ingestion order for a nonempty, checked, equal-length, in-image span,
reporting `invalid_mapping_claim` for the first invalid claim; (3)
`odd_instruction_address`; (4) resolve applicable source claims in ingestion
order, reporting `conflicting_address_mapping` for more than one and
`unmapped_instruction_address` for none; (5) make the claim-bounded two-byte
primary request; (6) where the primary requires an extension, make its complete
claim-bounded request; (7) apply the inherited primary classification order:
source-defined illegal, selected decode, then valid-but-unsupported. Thus a
malformed image or invalid mapping is rejected before address resolution or
reads; an odd source precedes source mapping; and `Bcc` with `d8=0` requests
four bytes before its unsupported-form result. Steps 5--7 preserve, rather
than replace, the SEG-002/SEG-003 primary-word and extension precedence.

Frontend diagnostics retain the inherited `DirectFlowDiagnostic` fields and
add `image_source_id` when a validated `FrontendImage` exists. They also add
`supplied_image_source_id` for image-validation rejection only: it is the
verbatim supplied `source_id` when it is a string (including an empty, invalid
string), and `null` when it is missing or not a string. This field records input
identity only; it never makes the source ID valid or authorizes a byte read.
`declared_image_byte_length` and `actual_image_byte_length` are diagnostic
fields used only for an image-length mismatch. Fields not listed below are
`null`; `mapping_claims` is an ordered list when listed and is otherwise `null`.

`invalid_frontend_image_source_id` has `supplied_image_source_id` as just
defined and has `image_source_id`, declared/actual image lengths, source
address, image offset, byte count, raw bytes, mapping claims, target, and edge
`null`. `frontend_image_byte_length_mismatch` has the valid-but-not-yet-
validated `supplied_image_source_id` plus declared and actual image lengths;
`image_source_id`, source address, image offset, byte count, raw bytes, mapping
claims, target, and edge are `null`. Neither category has a validated
`FrontendImage`: it performs no claim validation, address resolution, image
read, decode, lift, discovery, lowering, emission, oracle execution, boundary
record, or successful artifact creation. `invalid_mapping_claim` has
`image_source_id`, the offending claim in `mapping_claims`, and no source
address, image offset, byte count, raw bytes, target, or edge.
`odd_instruction_address` has the typed source address and `image_source_id`,
but no image offset, bytes, or mapping claim. Source
`conflicting_address_mapping` has the typed source address, `image_source_id`,
and all applicable claims in ingestion order, but no image offset or raw bytes;
source `unmapped_instruction_address` has the typed source address,
`image_source_id`, and an empty `mapping_claims` list, but no image offset or
raw bytes. Primary or extension `truncated_instruction` retains the source
address, selected claim, source ID, image offset, and available/requested
lengths. A primary truncation has `raw_bytes` and
`instruction_length` `null`; an extension truncation retains the verified
primary bytes and has `instruction_length` `null`; neither has a target or edge.
An accepted primary retains source ID, source address, claim, image offset, raw
bytes, and verified length in its selected record (not a diagnostic). Illegal
and valid-but-unsupported primaries retain those same facts in their inherited
diagnostics, with target and edge `null`.

`analysis_entries` is the immutable, ordered analysis input for one common
translation artifact. It alone supplies discovery seeds and therefore fixes the
artifact's discovered blocks, edges, units, metadata order, and C output.
`execution_entry` is instead a per-vector typed execution input: it is applied
only after that common artifact has been accepted. A vector is valid only when
its `fixture_id` and `image_sha256` exactly identify the fixture and its
`execution_entry` exactly equals the start address of an already-discovered
`BlockProvenance`. Validation retains the typed execution address together with
the matched block's mapping-claim and instruction provenance; it neither derives
an address from an image offset or host pointer nor creates provenance by
decoding from the execution entry.

Consequently an execution entry must not be a mid-block address. It has no
effect on discovery worklist contents, unit count, unit/member order, edges, or
the common C/metadata artifact, and it cannot add a block or split one. It is
not an analysis seed, a Genesis startup/reset claim, or evidence for reset-vector
mapping, mapper, boot, or direct-flow entry support.

There is exactly one route: typed program validation, decode, lift, direct-edge
discovery, SCC lowering partition, then structured-C emission.  Every accepted
analysis entry uses that route; an opcode-specific CLI, compatibility bypass, or second
decoder/lifter/emitter route is forbidden.  The existing public commands retain
their own reports as required by
[the SEG-012 migration contract](../architecture/m68k-pipeline-migration-contract.md),
§§ “Current types versus planned unified route” and “Observable deterministic
equivalence”; their report schemas do not establish parallel semantics.

The ingestion-supplied entries are not Genesis startup support.  The limitation
in SEG-012 §§ “Evidence inventory and compatibility boundary” and “Explicit
non-claims” applies: no reset-vector mapping, mapper, boot, or direct-flow entry
is inferred from a startup validator result.

## Static discovery, units, and deterministic order (project policy)

Decode/discovery follows SEG-003 § “Typed discovery and block-boundary
contract”: validate typed space/CPU, alignment, mapping, and bounds before each
read; process the FIFO worklist; form maximal blocks ending at selected direct
transfers; enqueue conditional fallthrough before taken; and retain mapping
claims in ingestion order. `analysis_entries` are initially deduplicated by
typed `(space,value)` while preserving first occurrence. They are analysis
seeds, not function evidence; `execution_entry` is not part of that
deduplication or worklist.

After discovery, form SCCs using only accepted `DirectEdge.target` values.  No
unresolved, guessed, dynamic, or target-byte-decoded edge participates.  A unit
contains one SCC; within it, blocks sort by `(space,value)` and its provenance is
the concatenation of those ordered blocks' instruction provenance.  Units sort
by their least member `(space,value)`, receive zero-based ordinals, and use
`m68k_unit_%08X` names.  Thus the fixture below deterministically produces only
`m68k_unit_00000000` and `m68k_unit_00000001`.

Each C translation artifact has exactly these two static lowering units for this
fixture.  A unit may lower only its statically discovered member blocks and
direct edges.  Generated C is C11 and contains lifted register/CCR/PC behavior,
not the input image, target-byte loads, an opcode decoder, an interpreter, or a
dispatch target outside the emitted blocks.  It must compile with
`-std=c11 -Wall -Wextra -Werror -pedantic`.  Repeating translation with identical
typed input and options must produce byte-identical C and ordered metadata.

## Synthetic test image and independently testable oracle matrix

This is a recipe for a future committed, project-authored fixture; this research
task creates no fixture.  The byte hash covers exactly `image_hex`, not padding
or host files.  The two mapping claims are supplied in this order and each maps
one image interval one-for-one:

```text
fixture_id: synthetic/SEG-004/two-scc-units-v1
image: {source_id:"synthetic/SEG-004/two-scc-units-v1/image", byte_length:12,
        bytes:"7002600c7001538066fa60f8"}
image_hex: 7002600c7001538066fa60f8
sha256: d722d08d765dc5416a8eb8f156347240e00d1875ecb6bc5650b38ad8c3b464dc
claim[0]: {name:"unit_zero", target_span:[0x100,0x104), image_offset_span:[0,4)}
claim[1]: {name:"unit_one",  target_span:[0x110,0x118), image_offset_span:[4,12)}
analysis_entries: [0x100]
blocks: 0x100=[7002,600c], 0x110=[7001,5380,66fa], 0x116=[60f8]
edges: 0x102=BRA.S(unconditional, target:0x110),
       0x114=BNE.S(nonzero, fallthrough:0x116, taken:0x110),
       0x116=BRA.S(unconditional, target:0x110)
units: m68k_unit_00000000=[0x100],
       m68k_unit_00000001=[0x110,0x116]
```

The `BRA.S +12` at `0x102` uses the PC after its two-byte instruction (`0x104`)
and therefore has target `0x110`.  It is the sole direct inter-unit edge:
`m68k_unit_00000000` to `m68k_unit_00000001`.  Its edge provenance is the
ordered decoded instruction provenance `{source_address:0x102,
image_offset:2,raw_bytes:600c,length:2}`, together with the source block's
ordered provenance `[ {0x100,0,7002,2}, {0x102,2,600c,2} ]`; its target block
and unit retain their independently decoded provenance.  The second unit's
ordered block provenance is `0x110=[ {0x110,4,7001,2}, {0x112,6,5380,2},
{0x114,8,66fa,2} ]`, then `0x116=[ {0x116,10,60f8,2} ]`; its unit provenance is
their ordered concatenation.  Thus `0x100` is a singleton SCC and `0x110` plus
`0x116` is one SCC; sorting by least member gives the displayed unit order.
Discovery begins only at `0x100`, reaches the second unit through that static
edge, and emits exactly these two lowering units.  This establishes a static
direct-flow transfer, not a call, return, target-function recovery result, or
hardware function boundary.

Future T002 boundary-negative fixture-schema rows (not fixtures created by this
research task) are:

| schema row | required frontend input variation | first result and decisive facts |
| --- | --- | --- |
| `invalid-image-source-id` | A missing, non-string, or empty image `source_id`; other image and claim fields may also be malformed. | `invalid_frontend_image_source_id` at step 2a, before byte-length or claim validation, source resolution, or read; preserve only the unvalidated supplied string identity when one was supplied. |
| `image-byte-length-mismatch` | A nonempty string `source_id` and a declared `byte_length` unequal to `bytes.length`; claims may also be malformed. | `frontend_image_byte_length_mismatch` at step 2b, before claim validation, source resolution, or read; preserve the unvalidated supplied source identity and declared/actual lengths only. |
| `invalid-mapping-span` | A claim with an empty, wrapping, unequal-length, or image-out-of-range span. | `invalid_mapping_claim` before source resolution/read; source ID and offending claim only. |
| `odd-source` | An odd typed `m68k_program` analysis entry with an otherwise valid image and claim. | `odd_instruction_address` before claim resolution/read; typed source and source ID only. |
| `conflicting-source` | Two otherwise valid, ingestion-ordered claims applying to the same analysis-entry address. | `conflicting_address_mapping` before primary read; typed source, source ID, and both claims in order. |
| `unmapped-source` | A valid analysis entry outside every claim. | `unmapped_instruction_address` before primary read; typed source, source ID, and empty claim list. |
| `truncated-primary` | A selected source with fewer than two claim-bounded bytes. | `truncated_instruction`; source/claim/source ID and available/requested primary lengths, with no raw bytes. |
| `truncated-extension` | A `Bcc d8=0` primary with its two bytes available but fewer than four bytes in that claim. | `truncated_instruction`; source/claim/source ID, primary bytes, available 2, requested 4. |

Every vector below is a unique `FrontendOracleVector` for this exact fixture
ID/hash and this one common artifact. It supplies all D0--D7 values, SR, a
typed `execution_entry`, and positive block instruction counts. Every listed
vector has `cpu_variant: mc68000`. Initial SR is `2700`; D1--D7 are
`[22222222,33333333,44444444,55555555,66666666,77777777,88888888]` in every
row and must retain those values at every boundary. Consequently each displayed
row is a complete full-register state: the displayed D0 is followed by that
unchanged ordered D1--D7 sequence, SR, and PC. The execution entry must match
the listed existing block start and unit; `0x102` is deliberately not a vector
entry because it is an instruction start inside block `0x100`, not a discovered
block start. Each vector requires its ordered boundaries and
`instruction_budget_exhausted` result.

| vector | execution entry / matched block and unit | initial D0 / block instruction counts | expected ordered post-block boundaries `(D0, D1--D7, SR, PC)` | purpose |
| --- | --- | --- | --- | --- |
| `inter-unit-transfer` | `0x100` / `0x100`, `m68k_unit_00000000` | `11111111` / `[2,3]` | `(00000002,2700,00000110)`, then `(00000000,2704,00000116)` | verifies the provenance-carrying static `0x102 -> 0x110` transfer and direct entry/execution of unit 1. |
| `unit-one-direct` | `0x110` / `0x110`, `m68k_unit_00000001` | `aaaaaaaa` / `[3]` | `(00000000,2704,00000116)` | verifies a direct vector entry to the already discovered second unit without changing the common artifact. |
| `unit-one-bra` | `0x116` / `0x116`, `m68k_unit_00000001` | `deadbeef` / `[1]` | `(deadbeef,2700,00000110)` | verifies the second unit's discovered `BRA.S` block without adding a route or changing the artifact. |

For each boundary, D1--D7 retain their seeds.  The harness records edges and
provenance from the frontend, then independently compares only CPU-state
boundaries to the pinned Musashi oracle.  It must verify the checkout identity
before use and invoke an ignored local adapter as specified by SEG-003 §
“Independent boundary oracle”:

```sh
"$SEGARECOMP_DIRECT_FLOW_MUSASHI_ORACLE" \
  --input "$input" --output "$output" --blocks "$blocks"
```

The adapter input must use the exact image/hash, vector ID, typed execution
entry, D0--D7, SR, and the vector's positive instruction-count array. The
frontend must first validate that entry against the already-discovered common
artifact and record the matched block provenance. Its required output identity is
`musashi@313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`; boundary arrays compare in
order.  The adapter supplies no project provenance, unit, or edge result and
does not define block cuts: those remain the static frontend contract.  No
commercial image, Musashi checkout, adapter, generated opcode file, or adapter
artifact may be committed.

## Execution-vector validation (project policy)

This validation is deliberately after successful shared-route discovery and
lowering, and is separate from discovery validation.  It validates one vector
against the immutable accepted artifact identified by its fixture recipe; it
does not decode from `execution_entry`, enqueue it, or re-run discovery.  Apply
the following checks in this exact order and stop at the first failure:

1. `vector_fixture_id_mismatch`: `fixture_id` differs from the artifact recipe.
2. `vector_image_sha256_mismatch`: the supplied image SHA-256 differs from the
   recipe SHA-256.
3. `vector_cpu_variant_mismatch`: the vector CPU is not original MC68000; then
   `vector_execution_entry_space_mismatch`: its entry is not in
   `m68k_program` space.
4. `odd_instruction_address`: the typed execution-entry value is odd.  This
   reuses the inherited alignment vocabulary; it is an entry-validation failure,
   not a target-byte read.
5. `unmapped_instruction_address`: no accepted artifact mapping claim applies
   to the aligned execution entry.  This reuses the inherited mapping vocabulary.
6. `execution_entry_inside_discovered_instruction`: the entry lies strictly
   inside a completed instruction interval.  Otherwise,
   `execution_entry_inside_discovered_block`: it is an instruction boundary
   strictly after a discovered block's entry and before that block's end; for
   this fixture `0x102` has this category and the containing `0x100` block is
   decisive.
7. `execution_entry_not_discovered_block_start`: the aligned, mapped entry is
   neither a discovered block start nor within an already-discovered instruction
   or block.

Only after all seven steps may a vector match its `BlockProvenance`, mapping
claim, unit, and ordered instruction provenance and execute its listed block
counts.  A successful match records those facts before comparing its boundaries
to Musashi.  Every rejection records `vector_id`, category, supplied
`fixture_id`/SHA-256, supplied CPU and typed entry, plus expected fixture ID,
SHA-256, and CPU where the failed check has one.  Its inherited diagnostic
fields are populated as follows. Fixture/hash/CPU/space failures have
`block_entry`, `source_address`, `image_offset`, `available_bytes`,
`requested_length`, `instruction_length`, `raw_bytes`, `target`, `edge`, and
`mapping_claims` all `null`. Odd-entry has `block_entry` equal to the typed entry
and all of those other fields `null`. Unmapped-entry has that `block_entry`, an
empty ordered `mapping_claims` list, and every source/byte/length/edge/target
field `null`. The three entry-location failures have `block_entry` equal to the
entry, `mapping_claims` equal to the ordered applicable claim, and `edge` and
`target` `null`; an interior-instruction failure has that complete instruction
provenance, an interior-block failure has its containing block and the selected
instruction provenance at the entry (thus `0x102`, offset 2, `600c`, length 2),
and no-block-start has all instruction provenance fields `null`.  These
vector-validation categories are needed because no decode or direct edge is
being rejected; inherited categories retain their inherited meaning where
reused.

Vector rejection produces no generated execution, no executed-block count, no
boundary record, no successful oracle result, and no replacement artifact.  It
cannot mutate, rebuild, split, reorder, or otherwise alter the accepted shared
decode/lift/discovery/SCC-lowering/emission artifact.  Thus analysis remains one
shared route even when multiple vectors are tested.

## Fail-closed matrix and explicit exclusions (project policy)

All existing SEG-002 and SEG-003 negatives remain required, with their original
precedence and source/edge/mapping provenance.  The following matrix adds only
frontend-partition regressions; it selects no instruction form.

| case | required result |
| --- | --- |
| duplicate or unordered-looking entry seeds | Deduplicate preserving first supplied occurrence; unit/block/edge/C output remains deterministic. |
| missing/invalid image source ID or image byte-length mismatch | Preserve the step-2a/2b category and supplied-identity/length fields; perform no image or mapping read and create no IR, block, unit, C, execution, boundary, oracle, or successful artifact. |
| odd, unmapped, conflicting, mid-instruction, or unresolved selected target | Preserve SEG-003 § “Fail-closed diagnostics” target order and decisive edge/mapping provenance; produce no unit or C artifact. |
| `4AFC`, truncated primary/extension, unsupported primary word, zero-displacement branch, or any excluded direct-flow form | Preserve SEG-002/SEG-003 decode precedence and source provenance; no IR, block, unit, or C artifact for the failed route. |
| partition would require an unselected or unresolved edge | Reject before unit emission; do not invent an SCC member, target, or dispatch arm. |
| generated output contains image bytes, target-byte fetch, runtime decode, or a non-emitted target | Architecture failure; static-only emission requirement is not met. |
| an execution vector fails any ordered validation above | Return that first stable category with the required vector/provenance fields; no generated execution or boundary success, and leave the common accepted artifact unchanged. |

The only accepted forms are `MOVEQ #s8,D0`, `SUBQ.L #1,D0`, nonzero `BNE.S`, and
nonzero `BRA.S`.  Explicitly unsupported are other `MOVEQ` destinations; all
other `SUBQ` counts, sizes, destinations, and effective-address forms; `TST`;
all other arithmetic/test operations; every other `Bcc` condition; `Bcc`/`BRA`
word displacement; calls, returns, indirect control flow, memory operands,
exceptions, timing, devices, peripherals, mappers, code writes, CPU variants,
and function recovery.  This list inherits and does not enlarge SEG-003 §
“Support matrix and exclusions”.

## Verification boundary

A future implementation validates the fixture matrix by checking hash, typed
mapping/entry provenance, ordered decode/IR/blocks/edges/units, byte-identical
repeat output, strict-C11 compilation, generated execution boundaries, and the
pinned oracle comparison where available.  It also runs the inherited focused
MOVEQ and direct-flow harnesses identified in SEG-012 § “Reproducible
verification plan”.  Passing the matrix is evidence only for this synthetic,
static subset; it is not evidence for target-function recovery or any excluded
hardware behavior.
