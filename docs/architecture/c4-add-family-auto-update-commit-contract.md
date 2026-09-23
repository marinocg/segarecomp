# C4 ADD-family auto-updating-operand deferred-commit contract (SEG-007-T145)

## Purpose and boundary

This is a bounded extension of the already-accepted deferred-single-commit mechanism recorded in
[`c4-move-predecrement-postincrement-commit-contract.md`](c4-move-predecrement-postincrement-commit-contract.md)
(Q1-Q5) and ADR-0015 §5/§7. It is **not** a new architecture decision, a new `M68kIrKind`, a new
persistent-state concept, or any hardware-timing claim. It records exactly how the C4 emitter lowers
an ADD-family instruction (`M68kIrKind::add` / `M68kIrKind::add_address`) whose operand effective
address uses an auto-updating mode (predecrement `-(An)` or postincrement `(An)+`), and which
same-register aliasing shape stays an explicit clean decline.

The MOVE contract's Q1 caller table already names the `add`/`adda` family as carrying the identical
unguarded auto-update-before-access hazard through the same shared
`m68k_emit_ea_read` / `m68k_emit_ea_write` / `m68k_emit_runtime_ea_address` primitives. This contract
resolves that one row for the add family only; every other row in that table (`subtract`, the three
logical families, `cmp`, the four `bit_*` kinds, `write_movea`, `pea`, `link`, `unlk`, `tst`, `clr`,
`andi`, `suba`, `cmpa`) remains out of scope and unchanged.

## Decision

**A add-family-local deferred-address-commit technique, confined entirely to the `add` /
`add_address` block-emission case in `emit_m68k_operation_c` (`libs/codegen/c11/src/m68k.cpp`), directly
applying the MOVE contract's Q2/Q4/Q5 discipline to the add family's single-memory-operand shape. The
shared EA primitives are not restructured.**

### Q1 — Shape: single memory operand, at most one touched address register

Unlike MOVE, an ADD-family instruction has at most one memory-referencing operand:

- `M68kIrKind::add` — either the source EA (`ADD <ea>,Dn`) or the destination EA (`ADD Dn,<ea>`, a
  read-modify-write) may be `-(An)`/`(An)+`; the other operand is always `Dn`. The two can never both
  be auto-updating, and the auto-updating operand's register is never also named by the other
  operand (the other operand is a data register).
- `M68kIrKind::add_address` (`ADDA <ea>,An`) — only the source EA may be `-(An)`/`(An)+`; the
  destination is always `An`-direct. The destination `An` **may** equal the source's auto-updating
  register (`ADDA.L (A0)+,A0`).

So the mechanism only ever snapshots one address register into one local and performs one deferred
commit — the MOVE contract's single-local / single-writeback case, never its two-register Q2 case.

### Q2/Q5 — Mechanism and ordering guarantee

Confined to the eligible case, when `memory->runtime_routing` is set and either operand is
`-(An)`/`(An)+`:

1. Snapshot the touched address register into one local, `m68k_add_auto_ea` (never the live array).
2. For predecrement, subtract the operand width from that local immediately, before the access. The
   A7-with-byte-size steps-by-2 stack-pointer-alignment exception is mirrored from
   `m68k_emit_runtime_ea_address` / the MOVE contract.
3. Route every access this instruction performs through the existing
   `m68k_emit_routed_read` / `m68k_emit_routed_write` boundary, using `m68k_add_auto_ea` as the
   address:
   - auto-updating source: one routed read;
   - auto-updating RMW destination (`add` only): one routed read then one routed write, both to the
     same `m68k_add_auto_ea` (the auto-update is applied once, never re-applied for the write).
4. For postincrement, add the operand width to the local strictly after its access(es).
5. Apply the arithmetic (`add_destination + add_source`; `ADDA.W` sign-extends the source),
   write the non-address result (`Dn` via the shared `m68k_emit_ea_write` data-register path, or the
   `An` destination for `ADDA`), and update CCR (`add` only; `ADDA` never affects flags).
6. **Only then** emit the single live-register-file commit,
   `memory->address_registers[reg] = m68k_add_auto_ea;`, placed strictly after every routed access
   in the generated C.

`m68k_emit_routed_read` / `m68k_emit_routed_write` each either fall through on success or execute
`return transfer;` on failure from inside their own generated statement. Because the single commit is
textually after every routed access and no other path reaches it, any `GENESIS_STOP` during this
instruction's lowering returns before the commit — the touched address register always retains its
exact pre-instruction value; no partial decrement/increment is ever observable. This is the same
reasoning that already makes `movem_transfer`'s and `write_move`'s single-register cases correct.

### Q3 — `ADDA <auto>(An),An` where the destination `An` equals the source register: supported (SEG-021-T006)

`ADDA.L (A0)+,A0` / `ADDA.W -(A0),A0` compose the source operand's own auto-update with the sum
write into the same architectural register. The composed value is now backed by differential evidence
(pinned Musashi, `adda.ea_an.{w,l}.{postinc,predec}.an` rows with `alias_pointer` values, all words including
A7): the source auto-update is applied first, the destination operand is then the already-updated register, and
the sum write is the last write. The lowering models this with the deferred-commit local: the destination operand
is `m68k_add_auto_ea` after its adjustment and the final live-register commit is skipped (the sum write wins). The
routed lowering is compared against the direct one by `tests/m68k_routed_lowering_test.py`. The earlier decline
(and its `ADDA_AUTO_UPDATE` gap dimension) remains only as the generic fallback for a shape the emitter cannot lower.

`M68kIrKind::add` can never reach this shape (its non-auto operand is always a data register).

### Q4 — `retain_fact` / `require_fact`

`retain_fact` is unchanged: it already declines to record a `M68kStaticMemoryFact` for
register-relative modes (`m68k_is_statically_foldable_control_ea` accepts only
`absolute_word`/`absolute_long`/`pc_disp16`).

`require_fact` (`src/m68k_pipeline_frontend.cpp` equivalent, `libs/codegen/c11/src/frontend.cpp`) narrows
its unconditional predecrement/postincrement rejection so it no longer applies to the `add` and
`adda` instruction kinds — exactly as SEG-007-T070 did for `move` — deferring all
representability/aliasing validation to the block-emission case above. The rejection remains
unconditional for `tst`/`clr`/`andi`/`btst`/`suba`/`cmpa`.

The per-operation C4 gap classifier `classify_m68k_c4_gap_shapes` no longer emits a
`requires_architecture_decision` auto-update row for `M68kIrKind::add`, nor for `M68kIrKind::add_address`
except the Q3 aliasing shape. `subtract_address` / `compare_address` keep their existing auto-update
gap rows unchanged.

## Explicit non-goals / non-claims

- `ADDX` is out of scope of the SEG-007 record above; it is implemented by SEG-021-T014 (see the
  "SEG-021-T014: extended arithmetic" section at the end of this document).
- `add_immediate` (`ADDI`) is not a C4-represented `M68kIrKind` and is unaffected (it remains a
  `missing_dispatcher` gap).
- **SEG-007-T153 update:** `add_quick` (`ADDQ`) is now C4-represented and reuses this exact
  deferred-single-address-commit mechanism for an auto-updating (`(An)+` / `-(An)`) destination. It is
  a strict subset of the shape above — ADDQ's source is always the instruction-embedded quick
  immediate, so only the destination can be auto-updating and the ADDA same-register aliasing
  sub-case (Q3) can never arise. `add_auto_kind` in `emit_m68k_operation_c` gains `add_quick`; the
  destination-branch source expression becomes the materialized immediate instead of a data register;
  everything else (single local, predecrement-before / postincrement-after, one commit strictly after
  every routed access, A7 byte step-by-2, `M68kAdditionResultSpecification` CCR) is unchanged. The
  shared EA primitives are still not restructured.
- No change to `subtract` / logical / `cmp` / `bit_*` / `write_movea` / `pea` / `link` / `unlk` /
  `tst` / `clr` families' structurally identical latent hazard.
- No change to the shared `m68k_emit_ea_read` / `m68k_emit_ea_write` /
  `m68k_emit_runtime_ea_address` / `m68k_emit_routed_read` / `m68k_emit_routed_write` primitives.
- No new `M68kIrKind`, persistent state, interpreter/JIT, or runtime opcode decode.

## SEG-021-T004 operand/update mechanism inventory (evidence, no emitter change)

The legal-form matrix was exercised through the T003 harness (direct linear route, generated-native vs pinned
Musashi) for every legal ordinary auto-update shape of the emitted families: MOVE `(An)+`/`-(An)` as source,
destination and both (same or different An), MOVEA/ADDA/SUBA/CMPA with `(An)+`/`-(An)` source including the
destination An equal to the pointer An, ADD/SUB/CMP `<ea>,Dn` auto sources, ADD/SUB/AND/OR/EOR `Dn,<ea>`,
ADDI/SUBI/ANDI/ORI/EORI/CMPI, ADDQ/SUBQ, CLR/TST/NOT/NEG memory RMW with auto destinations, byte-A7 stepping
(step 2) and word/long, both stack modes. Every one already matches Musashi (D/A/PC/SR/USP/SSP, byte memory
writes), so no legal ordinary shape is declined and no emitted output changed. Decision per shape:

| shape | mechanism | reason |
| --- | --- | --- |
| single auto operand, read-only (TST, CMP, MUL/DIV, MOVEA/ADDA source) | shared `m68k_emit_ea_read` / `m68k_emit_materialized_ea_read` | one address, one mutation ordered by the shared primitive |
| single auto destination RMW (AND/OR/EOR/NOT/CLR/NEG/bit/shift) | shared read-then-write with mode demoted to `(An)` | the read owns the mutation; the write reuses the computed address |
| ADD-family / SUBI routed auto-commit, MOVE/MOVEA routed commit | family-local (unchanged) | runtime-routed access may stop; the routed contracts require one local address and one commit after all routed accesses; these paths are not exercised by the linear harness and stay under their Genesis/contract tests |
| same An in both operands (MOVE, ADDA with An dest) | already handled by the existing local snapshot | value read at the pre-mutation snapshot; harness rows with in-window pointer values match Musashi |

Not merged into one universal helper: the families differ in routed-stop commit order, so a merge would change
byte-comparable routed output without evidence of a defect. Remaining declines are missing families rather than
shape declines. CMPM, ADDX/SUBX and NEGX were completed by SEG-021-T014, ABCD/SBCD/NBCD by SEG-021-T015 and EXG/MOVEP/Scc/TAS by SEG-021-T016 (below).

### MOVEM (`M68kIrKind::movem_transfer`): decision — no change, retain the family-specific mechanism

MOVEM is inventoried explicitly as a row of the T004 shape table. Its existing mechanism is retained:

- Register-mask ordering is architectural MOVEM behavior owned by `m68k_movem_transfer_order`
  (`libs/cpu/m68k/src/effects.cpp`); it is consumed by effects, static discovery and the emitter, and legitimately
  stays family-specific.
- Ordinary `(An)` / `d16(An)` sequences snapshot the effective base once, so a selected load into the base An cannot
  alter later transfer addresses.
- Register-to-memory `-(An)`: the original architectural An is snapshotted into one working EA; the working EA is
  decremented before each transfer, every transfer address derives from that local, architectural An is not mutated
  incrementally, and it receives one final writeback. When the base An is in the mask, its stored value is the
  original architectural value.
- Memory-to-register `(An)+`: one snapshot of the original An; each transfer reads at the working EA and increments
  it afterwards, so a transient load into the base An cannot affect later addresses; one final unconditional
  architectural An writeback follows all transfers.
- MOVEM is word/long only, so the byte-A7 step-by-2 exception does not apply.

Why it is not folded into a shared single-EA/two-EA update helper: MOVEM has a register mask ordering, several
accesses over one working EA, base-register-in-mask semantics and a final-writeback rule that none of the
one-address-one-mutation shapes share; a universal helper would be less clear than this owner. Existing evidence: the
pinned-Musashi Batch C differential (`tests/m68k_batch_c_musashi_differential_test.py`, C5a ordinary forms plus
base-alias correction, C5b predecrement, C5c1 postincrement), the routed-startup generated test
`tests/genesis_startup_runtime_c4_movem_adjacent_lea_test.py` and
`c4-movem-adjacent-lea-constant-propagation-contract.md`. Completing/auditing every legal MOVEM form (and adding its
T003 rows) belongs to SEG-021-T012; T004 adds no MOVEM mode or matrix.

Final T004 conclusion: ordinary single-EA forms use the existing shared EA helpers; MOVE source/destination
alias/update uses its existing deferred commit; MOVEM keeps its working-EA/mask/order/final-writeback mechanism;
CMPM and ADDX/SUBX memory forms are the family-local paired postincrement/predecrement work delivered by SEG-021-T014, and
ABCD/SBCD are delivered by SEG-021-T015 on the same mechanism. No new shared production mechanism is required.

## SEG-021-T006: SUB / SUBA / SUBQ / SUBI and CMP / CMPA / CMPI

The same operation-local technique now covers the subtract and compare families through one arithmetic-family-local
helper (`m68k_emit_routed_arith_auto_update`, `libs/codegen/c11/src/m68k.cpp`), used only by the routed lowering
(`memory->runtime_routing`; the direct linear lowering is unchanged and Musashi-validated). Legal forms carry at most one
auto-updating operand: `SUB <auto>,Dn`, `SUB Dn,<auto>`, `SUBQ/SUBI #n,<auto>`, `SUBA <auto>,An`, `CMP <auto>,Dn`,
`CMPA <auto>,An`, `CMPI #n,<auto>`. Steps: snapshot the touched An into a local; predecrement the local; routed read (and
for a memory RMW destination the routed write) from the local; postincrement the local strictly after the accesses; update
the result/CCR; commit the live An in one statement after every routed access; advance PC last. A `GENESIS_STOP` returns from
inside the failing access, before any architectural write, so D/A/SR/PC/memory keep their pre-instruction values
(`tests/m68k_routed_lowering_test.py` forces the stop for every auto-updating shape). BYTE on A7 steps by 2.

Same-register aliases (the pinned-Musashi SUBA/CMPA rows include them, e.g. `93D9`, `B3D9`): the source auto-update is applied first and
the aliased An destination operand is the already-updated register. `SUBA <auto>(An),An` then writes the difference to that An
(the difference write wins: no trailing commit, exactly like ADDA). `CMPA <auto>(An),An` writes no result, so the auto-updated An is
committed (the source auto-update stays architectural).

The C4 classifier (`classify_m68k_c4_gap_shapes`) and the decoded-instruction pre-gate in `frontend.cpp` no longer
produce `requires_architecture_decision` rows for these shapes; `c4_arithmetic_auto_update_admission` in
`tests/m68k_pipeline_test.cpp` proves zero preflight rows and a routed body for ADDA/SUB/SUBA/SUBQ/CMP/CMPA/CMPI
auto-updating forms. Logical, MUL/DIV, bit-test and ANDI classifier rows are unchanged.

## SEG-021-T007: AND / OR / EOR and ANDI / ORI / EORI

The logical family joins the same technique through `m68k_emit_routed_logical_auto_update`
(`libs/codegen/c11/src/m68k.cpp`), used only by the routed lowering. Legal forms carry at most one auto-updating operand and
the other operand is always Dn or an instruction-embedded immediate: `AND/OR <auto>,Dn`, `AND/OR Dn,<auto>`, `EOR Dn,<auto>`
and `ANDI/ORI/EORI #n,<auto>`. Steps: snapshot the touched An into `m68k_logical_auto_ea`; predecrement the local; routed read
from the local; (for a source operand) postincrement the local right after the read; compute the result; write it (routed
write of the local for a memory destination, ordinary Dn write otherwise); postincrement the local after a destination write;
update N/Z (V/C cleared, X preserved); commit the live An in one statement after every routed access; advance PC last. A
`GENESIS_STOP` returns from inside the failing access, before any architectural write. BYTE on A7 steps by 2. There is no
same-register aliasing hazard (the other operand is never An). The C4 classifier and the decoded-instruction pre-gate no
longer emit `requires_architecture_decision` rows for these shapes; immutable-ROM AOT admission
(`m68k_operation_is_immutable_rom_aot_safe`) is family-level for all six mnemonics. Remaining declined auto-update shapes:
MULS/MULU/DIVS/DIVU sources (SEG-021-T010); the bit-test destinations declined at T007 are lowered by SEG-021-T008 (below).

## SEG-021-T008: BTST / BCHG / BCLR / BSET

The bit-operation family joins the same technique through `m68k_emit_routed_bit_auto_update` (routed lowering only). The bit
number is Dn or an instruction-embedded immediate (never memory); only the destination can auto-update. Steps: materialize the
bit number; snapshot the touched An into `m68k_bit_auto_ea`; predecrement the local; routed read from the local; compute the
result and Z from the ORIGINAL tested bit (modulo 32 for Dn, 8 for memory); for BCHG/BCLR/BSET a routed write of the local
(BTST never writes back); postincrement the local; commit the live An in one statement after every routed access; advance PC
last. A routed stop returns before any architectural write; byte on A7 steps by 2. The C4 classifier no longer emits
`requires_architecture_decision` rows for these shapes and immutable-ROM AOT admission is family-level for all four
mnemonics; legality (incl. `(d8,An,Xn)`, and for BTST `d16(PC)`, `(d8,PC,Xn)` and dynamic `#imm`) is owned by decode.
`tests/m68k_routed_lowering_test.py` proves routed-vs-direct equality and stop atomicity for every auto-updating shape.
Remaining declined auto-update shapes: MULS/MULU/DIVS/DIVU sources (SEG-021-T010).

AOT admission additionally requires the shared retirement-timing seam to account for the operation
(`m68k_instruction_cycles`): the dynamic `BTST Dn,#<data>` form has no published static timing row and is declined at
analysis time rather than admitted and rejected by codegen (which invalidated the whole immutable-ROM AOT program).

## SEG-021-T009 extension: memory-word shift/rotate

ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR memory-word forms (count fixed at 1) are a one-address read-modify-write like NOT/NEG. In
the routed (C4/AOT) context an auto-updating `(An)+`/`-(An)` destination uses the same operation-local deferred commit:
one snapshot local (`m68k_shift_auto_ea`), one routed read and one routed write at it, and the single live-register
commit strictly after both accesses, so a routed stop leaves no partial architectural mutation. Non-auto-updating
destinations, including `(d8,An,Xn)`, use the shared routed read/write primitives; foldable absolute destinations retain
their destination_read/destination_write facts exactly as NOT does.

## SEG-021-T010: MULS.W / MULU.W / DIVS.W / DIVU.W

The mul/div family joins the same technique through `m68k_emit_routed_muldiv_auto_update`
(`libs/codegen/c11/src/m68k.cpp`), the source-side counterpart of `m68k_emit_routed_bit_auto_update`: the destination is
architecturally always Dn (decode.cpp fixes it), so only the SOURCE can auto-update, never the destination. Steps:
snapshot the touched An into `m68k_muldiv_auto_ea`; predecrement the local; routed read from the local; postincrement the
local right after the read; commit the live An in one statement immediately (before the multiply/divide itself -- the
source operand, including its own auto-update, is fully consumed at fetch time on real hardware, unconditionally, even
when DIVS/DIVU's own divisor==0 check afterward raises the synchronous vector-5 exception); compute the product (MULS/
MULU, full 32-bit Dn write, N/Z/V/C exactly as the non-auto-update body) or the packed quotient/remainder (DIVS/DIVU,
Dn write conditional on no overflow, divisor==0 still raises vector-5 through the existing `emit_runtime(...).
divide_by_zero` helper unchanged); advance PC last. A routed stop returns from inside the failing access, before any
architectural write. The C4 classifier (`classify_m68k_c4_gap_shapes`) no longer emits `requires_architecture_decision`
rows for an auto-updating MULS/MULU/DIVS/DIVU source. Immutable-ROM AOT admission (`m68k_operation_is_immutable_rom_aot_
safe`) is now family-level for MULS.W/MULU.W (widened from the prior storage-free-only source restriction -- every legal
source EA, including memory and auto-updating forms, lowers through the same fact-free routed primitives the MOVE family
already established); DIVS.W/DIVU.W stay categorically excluded from immutable-ROM AOT regardless of source EA, but NOT
because an isolated AOT candidate lacks a live runtime object -- `emit_immutable_rom_aot_body` configures the exact same
live, routed `GenesisRuntime` context as an ordinary block, so the ADR-0037 vector-5 helper itself is not the blocker.
The actual invariant: `validated_immutable_rom_aot_entries` additionally requires
`m68k_operation_has_complete_c_emission(operation)`, and that shared, family-independent completeness probe
intentionally constructs a NON-routed `M68kMemoryEmissionContext` for every IR kind. DIVS.W/DIVU.W's C emission is
intentionally gated on `memory->runtime_routing` (synchronous divide-by-zero needs the live runtime exception service),
so it produces no body under that non-routed probe and DIVS/DIVU remain unsupported on the immutable-ROM AOT route --
an implementation/admission limitation of the shared completeness probe, not an MC68000 hardware limitation and not a
property of ADR-0037's own mechanism. Widening that shared probe to a routed context would touch every other
AOT-eligible kind's own validation path and is broader architecture work outside this task's scope (confirmed by a
bounded experiment: temporarily admitting DIVS/DIVU into the safety predicate still leaves them excluded by the
completeness probe; MULS.W/MULU.W are unaffected because their own emission needs no `runtime_routing` gate at all).
`tests/m68k_pipeline_test.cpp`'s `c4_arithmetic_auto_update_admission` proves zero preflight rows and the expected routed
commit statement for one representative `(An)+`/`-(An)` shape of each of the four mnemonics;
`immutable_rom_aot_safe_family_boundary_is_shared_and_fact_free` pins the completeness-probe invariant directly.

Source-EA legality (`m68k_ea_mul_div_source`, decode.cpp) is widened to the full Motorola-manual set -- every mode except
An-direct, now including `(d8,PC,Xn)` (previously excluded as an unevidenced non-goal; matches `m68k_ea_and_or_source`'s
formula exactly). DIVS.W/DIVU.W have no T003 conformance-table rows at all: both kinds only ever emit through this
runtime-routed C4 path (needed for the vector-5 raise), so the T003 harness's direct/non-routed emitter mode structurally
cannot exercise or credit them (see `docs/testing/m68k-conformance-harness.md`'s own SEG-021-T010 section for the full
routing rationale); MULS.W/MULU.W's full 11-form source-EA matrix is validated against pinned Musashi through T003 rows
instead.

## SEG-021-T014: extended arithmetic (ADDX, SUBX, NEGX, NEG, CMPM)

- New decoded/lifted kinds `add_extended`, `subtract_extended`, `negate_extended` (NEGX) and `compare_memory` (CMPM);
  NEG keeps `negate_word` (historical name, every size). Legality is encoded in `libs/cpu/m68k` from the Motorola
  encodings (ADDX/SUBX `1101/1001 Rx 1 ss 00 R Ry`, CMPM `1011 Ax 1 ss 001 Ay`, NEG/NEGX `0100 0100/0000 ss ea`
  with every data-alterable EA), never from the T001 dataset.
- One shared semantic owner, `M68kExtendedArithmeticSpecification` (`effects.hpp`), gives ADDX/SUBX/NEGX their result,
  X=C, N, V and the sticky Z rule (Z cleared by a non-zero result, otherwise unchanged; pre-operation X is an input).
  NEGX is SUBX with destination 0. CMPM reuses `M68kSubtractionResultSpecification` with X preserved.
- Memory pairs use the operation-local deferred address commit of this contract (`m68k_emit_extended_pair`,
  `libs/codegen/c11/src/m68k.cpp`): each An is snapshotted into one local, the destination local starts from the source
  local when both name the same register (source update first, as on the MC68000), the A7 byte step is two per operand,
  and both live registers are committed after every access and the CCR computation, PC last. A routed stop or window
  guard returns before any architectural write. NEG/NEGX share one one-address RMW lowering.
- C4/AOT: all five are represented C4 kinds with no gap rows (NEG/NEGX absolute operands retain the NOT-shaped
  destination read/write facts; auto-updating, indexed and register operands need none) and are admitted family-level
  to immutable-ROM AOT. Static discovery resolves NEG/NEGX like NOT.
- Timing: Table 8-4 literal rows (ADDX/SUBX `Dy,Dx` 4/4/8, `-(Ay),-(Ax)` 18/18/30; CMPM 12/12/20) and Table 8-6 NEGX rows
  via the existing single-operand row.

## SEG-021-T015: packed BCD (ABCD, SBCD, NBCD)

- New decoded/lifted kinds `add_decimal`, `subtract_decimal`, `negate_decimal` (byte only). Legality is encoded in `libs/cpu/m68k`
  from the Motorola encodings (ABCD/SBCD `1100/1000 Rx 1 0000 R Ry`, NBCD `0100 1000 00 ea` data-alterable), never from the T001 dataset.
- ABCD/SBCD reuse `m68k_emit_extended_pair` unchanged (same deferred address commit, A7 byte step of two, aliased-pair rule); NBCD
  reuses the NEG/NEGX one-address RMW lowering. Only the compute/update emitters differ (`M68kDecimalArithmeticSpecification`).
- N and V are undefined on the base MC68000; production matches the pinned Musashi core (documented as such, see
  `docs/testing/m68k-conformance-harness.md`). X/C/Z are documented semantics (sticky Z).
- C4/AOT: all three are represented C4 kinds with no gap rows (absolute NBCD operands retain NOT-shaped destination facts) and are
  admitted family-level to immutable-ROM AOT. Static discovery resolves NBCD like NOT/NEG.
- Timing: Table 8-4 rows (ABCD/SBCD 6 / 18) and the Table 8-6 NBCD row (Dn 6, memory 8 + EA).

## SEG-021-T016: EXG, MOVEP, Scc, TAS

- New decoded/lifted kinds `exchange_registers`, `movep` (`movep_transfer` in the IR), `set_conditional`, `test_and_set`. Legality is
  encoded in `libs/cpu/m68k` from the Motorola encodings (EXG `1100 Rx 1 01000/01001/10001 Ry`, MOVEP `0000 Dn 1 oo 001 An` + d16, Scc
  `0101 cccc 11 ea`, TAS `0100 1010 11 ea`, Scc/TAS data-alterable), never from the T001 dataset. The decode collisions are disjoint by
  construction: MOVEP's `001` operand field is never a legal bit-operation destination, DBcc owns Scc's mode-001 slot, and EXG's opmodes
  are illegal AND encodings. Scc reuses `M68kCondition`/`m68k_condition_c_expr` (all 16 conditions, T and F included).
- Lowering (`libs/codegen/c11/src/m68k.cpp`): EXG swaps through one local; MOVEP performs each byte as its own routed/guarded byte access
  (all reads before the Dn write, PC last, An never updated); Scc is CLR-shaped (write-only byte; the auto-updating operand uses the
  operation-local deferred commit); TAS is NOT-shaped (one-address byte RMW, N/Z from the operand byte, V/C cleared, X kept, then bit 7 set;
  the indivisible bus cycle stays platform-owned and is not modelled).
- C4/AOT: all four are represented C4 kinds; Scc has CLR's and TAS has NOT's retained-fact shape (auto-updating, indexed and register
  operands need none); all are admitted family-level to immutable-ROM AOT. Static discovery resolves Scc like CLR and TAS like NOT.
- Timing: Table 8-4 EXG 6, Table 8-3 MOVEP 16 (word) / 24 (long), Table 8-6 TAS (Dn 4, memory 10 + EA) and Scc memory rows (8 + EA).
  Scc `Dn` is 4 (false) / 6 (true): condition-dependent, so `m68k_instruction_cycles` records it timing-unsupported and the retirement
  seam supplies the dynamic expression `m68k_scc_true ? 6 : 4` (assigned by the lowerer through `timing_scc_true`).
