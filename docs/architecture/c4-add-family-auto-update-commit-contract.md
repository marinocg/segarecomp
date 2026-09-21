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

- `ADDX` is not implemented. Its only memory form is `-(Ay),-(Ax)` (two independent predecrement
  address registers plus the X flag) and it is not currently a distinct selected `M68kIrKind`; it
  remains an explicit bounded non-goal here. If a re-executed Sonic terminal ever selects it, it is a
  separately scoped, separately evidenced task.
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
shape declines, including CMPM, ADDX/SUBX, ABCD/SBCD and NEGX (owned by SEG-021-T014/T015; NBCD, Scc and TAS by T015/T016), which have no decoded `M68kIrKind` (harness reports `unsupported`).

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
CMPM and ADDX/SUBX memory forms are future family-local paired postincrement/predecrement work in SEG-021-T014, and
ABCD/SBCD in SEG-021-T015. No new shared production mechanism is required.

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
