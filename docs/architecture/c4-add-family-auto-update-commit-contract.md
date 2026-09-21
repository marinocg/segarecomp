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

### Q3 — `ADDA <auto>(An),An` where the destination `An` equals the source register: declined

`ADDA.L (A0)+,A0` / `ADDA.W -(A0),A0` compose the source operand's own auto-update with the sum
write into the same architectural register. Real MC68000 hardware resolves this by fully applying the
source auto-update before the destination write, but this repository has no differential
(Musashi-oracle or otherwise) evidence for that composed value, and the project charter requires
"Never claim instruction support without differential or fixture-based evidence." Following the MOVE
contract's Q3 reasoning verbatim, this shape is **declined**, not given a guessed combined semantic:
`emit_m68k_operation_c` emits no C for the instruction, and `classify_m68k_c4_gap_shapes` keeps it a
clean `requires_architecture_decision` C4 lowering-gap stop (the `ADDA_AUTO_UPDATE` dimension). No
partial mutation, no guessed value. A future task may revisit this if differential evidence becomes
available.

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
