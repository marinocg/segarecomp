# MC68000 generated-native differential conformance harness (SEG-021-T003)

One reusable harness for every legal base-MC68000 form: deterministic synthetic vectors go through
decode -> lift -> emitted strict-C11 -> compile -> generated-native execute, and the identical vector runs one
instruction on the pinned Musashi core. Results are compared with the SEG-020 machinery
(`tools/m68k_first_divergence.py`, ADR-0042: boundary schema, `compare_streams`, `field_differences`).

Files: `tools/m68k_conformance.py` (driver), `tests/tools/m68k_conformance_emitter.cpp` (public-entry-point emitter),
`tests/tools/m68k_conformance_runner.c` / `m68k_conformance_oracle.c` / `m68k_conformance_common.h` (both sides,
shared memory image and output schema), `tests/fixtures/m68k-conformance-vectors.json` (the table),
`tests/m68k_conformance_harness_test.py`.

## Validating a family = adding table rows

Validating a family normally means adding table rows, not changing the harness. A row names a legal form id of
`tests/fixtures/m68k-legal-forms.json` (which supplies the exact primary words) and uses this small vocabulary:

| key | meaning |
| --- | --- |
| `form` | T001 form id; every concrete primary word of the form is exercised |
| `ext` | ordered literal extension suffixes (hex, whole words; default one empty suffix). Each tested encoding is `primary word + suffix`, e.g. `["0010","FFF0"]` displacements, `["7FFF"]` immediate, `["00012000"]` absolute.L, `["1804"]` brief index, `["00FF"]` MOVEM mask, `["2700"]` STOP SR. No assembler: the bytes are literal table data |
| `bind` | optional `x`/`y` operand bindings (omit for no-operand/control rows) |
| `init` | optional register presets, e.g. `{"d1":"00000010"}` (an index register value) |
| `src_ext_bytes` | optional extension length of an immediate source operand when it is not the size default |
| `profile` / `full_profile` / `full_words` | vector profile per word; `full_profile` for the listed words |

Operand specs (`bind_operand`, MC68000 architectural shapes only, no per-mnemonic logic): `d@S` / `a@S` register in
opcode bits S..S+2; `ea.src` / `ea.dst` the EA in opcode bits 0..5 with class from the form (Dn, An, (An), (An)+,
-(An), d16(An), d8(An,Xn), abs.W, abs.L, d16(PC), d8(PC,Xn), #imm; extension words come from the suffix, the source
EA's extension precedes the destination's); `eaM.dst` the MOVE-style destination field; `pi@S` / `pd@S` an (An)+ /
-(An) operand for two-auto-update shapes (CMPM, ADDX/SUBX). Profiles are `single`, `cross`, `pair_list` (values
crossed with SR seeds) or `state` (SR seeds only, for rows without operands). Committed profiles cover zero,
negative, carry/borrow, signed overflow, X/C interaction (SR 2700/2710/271F), boundary values with garbage upper
bits, register aliasing, A7 byte auto-update (adjusts by two) and supervisor/user state (`state_modes`).

Execution mode and stacks: the SR seed selects supervisor (bit 13) or user state. The vector carries USP and SSP
explicitly; A7 is the active pointer (SSP in supervisor state, USP in user state). Both the generated and the
Musashi runner start from exactly that state and report `usp`/`ssp`; in user mode both report the active stack
pointer as USP. A row that current production cannot execute reports `unsupported`; the harness never rejects a row
because of its mode.

Extension-bearing and no-operand canaries are ordinary committed rows validated against Musashi:
`neg.unary.w.none.disp` (d16(An), two suffixes), `addi.imm_ea.w.imm.dn` (immediate suffixes) and
`nop.none.none.none.none` (no operand, supervisor and user). `tests/m68k_conformance_harness_test.py` additionally
expands (without crediting) two-auto-update, displacement, brief-indexed, absolute, PC-relative, immediate, STOP, LINK,
MOVEM, Bcc and TRAP shapes to prove later family tasks only add rows.

Aliasing of an address-register operand value with a memory pointer (ADDA `(An)+,An`, MOVE.L `An,(An)+`): the bound
register value is also the pointer, so such rows take pair values from the table's `values.alias_pointer` (in-window
addresses) instead of the profile pairs; an A7 operand value also becomes the active stack pointer of the vector.
Rows exist for every legal ordinary form of MOVE/MOVEA/ADD/SUB/CMP/ADDA/SUBA/CMPA/AND/OR/EOR (SEG-021-T007: all sizes and
EAs, incl. `(d8,PC,Xn)` sources and indexed destinations)/immediate/quick/unary and (SEG-021-T008) BTST/BCHG/BCLR/BSET families (every legal EA, bit numbers 0/7/8/31/32/33/255 and
Dn aliasing; profiles `bit_sweep`/`bit_full`, static bit numbers as literal suffixes); CMPM, ADDX/SUBX and NEGX rows are absent because production has no decoder for them yet.

## Compared state

D0-D7, A0-A7 (A7 = active stack pointer), PC, SR/CCR, USP, SSP, byte-granular memory writes against the initial
image (memory RMW results, auto-updated EA memory and exception stacked frames) and the exception-vector hook: vectors
2..255 are seeded to distinct handler addresses (`CF_HANDLER(v)`); when the final PC is a handler a `k=2` effect
records the vector number (TRAP #0..#15 = 32..47, user vectors above), so exception families reuse the same rows and
comparison. Timing is not compared. Exception instruction semantics are not implemented by this harness.

## Limits (measured, not hidden)

A credited primary word means the vectors declared by its row (all suffixes, all profile cases) matched Musashi;
it is NOT semantic exhaustiveness (a handful of extension values, fixed baseline registers and memory pattern).
Family tasks own deeper family-specific vector expansion. A write that does not change a byte is invisible. The
generated model has one active A7 plus USP, so SSP is shadowed by the runner. Timing is out of scope. Only a fully
passing row can credit the T002 manifest (`update_manifest` only adds credit and never removes it).

## Oracle policy

Pinned Musashi (revision in the table, checked by `build_oracle`) is found through
`--musashi-checkout` or `SEGARECOMP_M68K_CONFORMANCE_MUSASHI_CHECKOUT` (the older checkout variables are accepted
as fallbacks). Without it the oracle comparison is skipped, never failed; emit/compile/execute/determinism still run.
`--update-manifest` refuses to run without the oracle and adds only fully validated rows to
`tests/fixtures/m68k-validation-manifest.json`; the test asserts the manifest words attributed to the table equal the
table's declared words. After a manifest change regenerate the T002 snapshot with
`python3 tools/m68k_capability_coverage.py --probe <probe> --update-snapshot`.

Fault injection lives only in the test's temporary copies of the emitted C (`mutate=`); production paths cannot
perturb generated code.

## SEG-021-T005: MOVE / MOVEA / CLR / NOT / TST rows and the routed differential

Rows exist for every legal ordinary form of the five mnemonics (all EA classes, sizes and register aliases; the
primary words enumerate every register combination, including A7 byte stepping). Extension data is literal table
data: `disp` `0010`, brief `index` `1004` (D1, word index, preset `d1`), `absw` `4000`, `absl` `00020000`, `pcdisp` `0010`,
`pcindex` `1040`, and two immediates per size (zero and negative). Rows involving an index register use the
`move_even` profile (even values only, so no vector forms an odd word address, which Musashi would answer with an
address-error exception this harness does not model). MOVE to/from SR/CCR/USP forms are out of scope for T005 (owned by SEG-021-T018).

The conformance emitter is the direct linear-memory lowering. `tests/m68k_routed_lowering_test.py` (emitter `--routed`)
runs the Genesis runtime-routed lowering, the route C4 and the immutable-ROM AOT candidates use, against the direct
lowering on identical vectors, so the routed deferred-commit code inherits the Musashi evidence. Absolute and
PC-relative operands are outside that test by construction (they address cartridge space or the sign-extended top
of the address space, which the work-RAM-only routed environment cannot host) and are covered by the direct rows only.

## SEG-021-T006: ADD / ADDA / ADDI / ADDQ, SUB / SUBA / SUBI / SUBQ, CMP / CMPA / CMPI rows

Rows exist for every legal form of the eleven mnemonics (all sizes, directions and EA classes including brief-indexed
and PC-relative-indexed sources, indexed and absolute RMW destinations, immediate and quick forms, ADDQ/SUBQ to An,
and the ADDA/SUBA `(An)+`/`-(An)` same-register alias). Extension conventions are those of T005; immediate forms use
two (byte/word) or three encodings per size. Legality is encoded in `libs/cpu/m68k` from the Motorola manual
(`m68k_ea_add_sub_cmp_source`, `m68k_ea_data_alterable_with_index`) and never reads the T001 dataset. All 787 rows match the
pinned Musashi. `m68k_routed_lowering_test.py` additionally compares the routed lowering to the direct one for every
legal shape (the direct lowering is emitted with the emitter's `--window` option so its linear window sits at the
work-RAM addresses and both sides see identical An values, including An sources and CMPA), and forces a routed-access stop for
every auto-updating shape to prove no partial architectural mutation. The immutable-ROM AOT predicate now admits
the whole family (the emitter still fails closed on a shape it cannot lower).

## Timing coverage of the bit operations (SEG-021-T008)

`m68k_instruction_cycles` has a published static row for every legal BTST/BCHG/BCLR/BSET form except the dynamic
`BTST Dn,#<data>` form (`btst.dn_ea.b.dn.imm`), which is recorded as timing-unsupported: no row is asserted without a
verified Motorola table cell. Timing is not compared by this harness (`timing_validated` stays 0).

## SEG-021-T009: shift and rotate rows

Rows exist for every legal ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR form (104): register forms with an immediate count 1-8
or a Dn count (all sizes) and the memory-word forms over every memory-alterable EA including `(d8,An,Xn)`. The register
destination is bound with `d@0` (bits 3-5 of these words are count-source/family bits, not an EA mode, so `ea.dst` does
not apply). Dn-count rows use the `shift_count` profile (counts 0/1/7/8/9/15/16/31/32/33/63/64/65/255 against values
that exercise ASL overflow and rotate-through-X chains, with SR seeds 2700/2710/271F for X-carry chains); immediate and
memory rows use `shift_imm` (the `shift` value set includes 0x40/0xC0/0x4000/0xC000/0x40000000/0xC0000000 sign-change
boundaries). All 104 rows (147168 vectors) match the pinned Musashi with zero divergences. Legality is encoded in
`libs/cpu/m68k` (`m68k_ea_shift_memory_destination`) from the Motorola manual and never reads the T001 dataset.
`m68k_routed_lowering_test.py` compares the routed lowering (deferred address commit for `(An)+`/`-(An)`) with the direct
lowering for every memory-word shape.

Timing: memory-word forms have a published static row (`8 + EA`, table 8-1) and are admitted to the immutable-ROM AOT
route; the register forms' retirement time depends on the runtime count (`6/8 + 2n`) and is emitted as a dynamic
retirement expression, so `m68k_instruction_cycles` (static) records them as timing-unsupported (owned by SEG-021-T021).
Timing is not compared by this harness.

## SEG-021-T010: MULS.W/MULU.W/DIVS.W/DIVU.W source-EA completion

Legality is `m68k_ea_mul_div_source` (every mode except An-direct, including `(d8,An,Xn)` and, newly, `(d8,PC,Xn)` --
the exact same formula `m68k_ea_and_or_source` already uses), written from the Motorola manual's MULS/MULU/DIVS/DIVU
source-operand tables and independent of the T001 dataset.

MULS.W/MULU.W rows exist for all 11 legal source forms (`dn`, `ind`, `postinc`, `predec`, `disp`, `index`, `absw`,
`absl`, `pcdisp`, `pcindex`, `imm`) using a dedicated `muldiv_sweep` profile (zero, sign edges 0x7FFF/0x8000/0xFFFF,
boundary dividend/multiplicand values 0x7FFFFFFF/0x80000000, and a generic non-power-of-two case); the `index`/
`pcindex` rows use the existing `move_even` profile instead (its bounded low-word values avoid the destination-Dn/
index-Xn register-aliasing address overflow `muldiv_sweep`'s wider values would otherwise trigger against the
conformance harness's fixed 1 MiB window when the opcode's Dn field happens to equal the brief extension word's own
index register -- a genuine test-harness constraint, not an instruction-semantic one). All MULS.W/MULU.W rows match
the pinned Musashi.

DIVS.W/DIVU.W have NO T003 table rows: both kinds only ever emit through the Genesis runtime-routed C4 lowering
(`memory->runtime_routing`, needed for the vector-5 divide-by-zero raise, ADR-0037) -- this predates T010 and its
Scope explicitly keeps that path unchanged. The T003 harness's default emitter mode is always the direct/
non-routed linear-memory lowering, so DIVS.W/DIVU.W structurally cannot be exercised or credited through it (every
declared table word must be Musashi-validated to update the manifest); this is the acceptance criterion's own
"as applicable" carve-out, not a gap. `m68k_pipeline_test.cpp` proves the widened `(d8,PC,Xn)` DIVS.W/DIVU.W forms
decode and lift correctly; the shared `m68k_emit_materialized_ea_read`/`m68k_emit_runtime_ea_address` EA-read
mechanism they reuse verbatim is the identical one MULS.W's own T003 rows already validate against Musashi end to
end. DIVS.W/DIVU.W's own divisor-zero/overflow/quotient-remainder value semantics (independent of which EA mode
supplies the divisor) are covered by `tests/m68k_divs_word_musashi_differential_test.py` and
`tests/m68k_divu_word_musashi_differential_test.py`.

## SEG-021-T012: MOVEM word/long EA completion

Legality is `m68k_ea_movem_register_to_memory`/`m68k_ea_movem_memory_to_register` (`libs/cpu/m68k/include/
segarecomp/cpu/m68k/instruction.hpp`), written from the Motorola manual's "control alterable"/"control"
addressing categories and independent of the T001 dataset: register->memory widened with `(d8,An,Xn)` (no
PC-relative form is ever legal for a MOVEM store); memory->register widened with both `(d8,An,Xn)` and
`(d8,PC,Xn)`, completing MOVEM's EA-mode ceiling to all 28 T001 forms (14 memory->register x 2 sizes, 6+6
register->memory x 2 sizes -- absl/absw/disp/ind/index/predec for the store direction; absl/absw/disp/ind/
index/pcdisp/pcindex/postinc for the load direction).

Rows exist for all 28 legal MOVEM forms (`movem.mem_reglist.*`/`movem.reglist_mem.*`), each with three
mask suffixes (`0000` empty, `00FF` D0-D7 only, `FFFF` every register) so every concrete primary word of a
form (one per base An 0-7 for the `ind`/`disp`/`index`/`predec`/`postinc` classes) is exercised at least once
with the empty mask (architectural An unchanged: no transfer, but predecrement/postincrement auto-update
side effects still apply per Musashi), once with a mask excluding every An (no addressing-register alias),
and once with the full 16-register mask, which -- because the base An varies 0-7 across the form's own
concrete words while the mask stays fixed -- systematically covers the addressing-register alias case (the
EA's own base An is always among the selected registers) and the A7-in-mask case (A7 is always selected) for
every base register in one row. The two word-size `ind`/`postinc` load forms additionally carry a dedicated
`.signext` row (`bind: {"x": "ea.src"}`, mask `0001` selecting D0 only, `unary_sweep` profile) that seeds the
exact loaded word at the transfer's own base address with boundary values including `0x8000` (sign bit set)
and `0x00FF` (clear), directly differentially validating "every loaded WORD sign-extends to 32 bits" against
Musashi rather than relying on incidental baseline register/memory content. Predecrement's reversed mask
ordering is exercised structurally by every `predec` row's full-mask case (a wrong order would misplace which
register's value lands in which memory slot, a byte-granular memory-write mismatch against Musashi). All 30
rows (1032 synthetic vectors) pass the self-consistency stage (emit/compile/native-execute/determinism);
Musashi oracle comparison is skipped without a pinned local checkout (never failed) and, when run with one,
requires a subsequent `--update-manifest` pass before `m68k_conformance_harness_test.py`'s manifest-
attribution consistency check (a hermetic bookkeeping check independent of oracle availability) can pass
again, exactly like every prior family task's own manifest update step.

The immutable-ROM AOT admission predicate (`m68k_operation_is_immutable_rom_aot_safe`,
`platforms/genesis/machine/include/segarecomp/machine/genesis/frontend.hpp`) returns `false`
unconditionally for `movem_transfer`, uniformly across every EA mode both before and after this task
(confirmed unchanged by the regenerated coverage snapshot's unaffected `route_immutable_rom_aot` count for
this family) -- MOVEM has never been immutable-ROM AOT eligible, for the same pre-existing reason DIVS/DIVU
are not (the shared family-independent completeness probe constructs a non-routed emission context that
never reaches MOVEM's routed body); this is a pre-existing, family-uniform fact this task's widening does
not change, not a per-EA-mode carve-out. The C4-routed admission gate (`m68k_c4_represented_ir_kind`'s
`movem_transfer` case, `libs/codegen/c11/src/frontend.cpp`) is widened alongside decode's own legal-EA
masks to admit the two new EA modes through the shared routed emitter.

Timing: `m68k_instruction_cycles` (`libs/cpu/m68k/src/timing.cpp`) has a published static Table 8-10 row for
every legal MOVEM form, including the two newly widened EA classes -- `(d8,An,Xn)` register->memory (base 14,
matching the table's own pre-existing literal cell) and both `(d8,An,Xn)`/`(d8,PC,Xn)` memory->register (base
18, the same An-relative/PC-relative pairing this same switch already applies to `d16(An)`/`d16(PC)` one row
above). No form is timing-unsupported.

## SEG-021-T013: Bcc/BRA/BSR, DBcc, LINK/UNLK, SWAP/EXT rows

Inventory found decode, lift, effects, C11 emission and static-discovery treatment of every form in this family
already complete (SEG-007-T025's shared condition-code owner, `m68k_condition_from_selector`, already covers
BRA/all 14 Bcc conditions/BSR and DBcc's all-16-condition set including DBT/DBF; LINK/UNLK/SWAP/EXT.W/EXT.L/RTS/
NOP already decode, lift and emit). NOP already had its own committed T003 row (`nop.none.none.none.none`,
`state_modes` profile, pre-existing and unaffected by this task -- see "Extension-bearing and no-operand
canaries" above) validated against Musashi before this task started. The actual remaining gap was exclusively
in the rest of the T003 differential table: none of Bcc/BRA/BSR/DBcc/LINK/UNLK/SWAP/EXT had a single committed
row, so `semantic_validated`/`ccr_sr_validated` credited none of them despite their structural support. This
task's only change is closing that evidence gap (53 new rows; no production code in `libs/cpu/m68k` or
`libs/codegen/c11` changed).

Rows: Bcc byte and word displacement, all 14 conditions (`bcc.disp8.b.none.target.<cc>` /
`bcc.disp16.w.none.target.<cc>`); BRA and BSR, both displacement sizes; DBcc, all 16 conditions including DBT
(never loops) and DBF/DBRA (always loops) with word displacement (`dbcc.dn_disp16.w.dn.target.<cc>`, `d@0`-bound
Dn sweep over `boundary` values including 0/1/0xFFFF wraparound); LINK.W and UNLK; SWAP, EXT.W and EXT.L. A new
`cc_full` profile (16 SR seeds, one per NZVC nibble 0x2700-0x270F, X and supervisor fixed) gives every Bcc/DBcc
row genuine full-CCR-combination coverage of the shared condition evaluator per the parent milestone's
acceptance criterion; a new `dbcc_full` profile crosses that same 16-state sweep with the `boundary` value set.
All 53 rows (83840 synthetic vectors) match the pinned Musashi with zero divergences; legal-form enumeration
(word ranges, exception classes, `dn_disp16`/`disp8`/`disp16` shapes) is `tests/fixtures/m68k-legal-forms.json`'s
own independent T001 dataset, never read by production.

Byte-displacement odd-target branches (Bcc/BRA/BSR low byte odd, e.g. 0x01/0xFF) are exercised by every
committed row (the full T001 word range, not a hand-picked subset) and validate cleanly: the pinned Musashi
checkout builds with `M68K_EMULATE_ADDRESS_ERROR` off (its documented default), so neither side raises a
synchronous address-error exception on an odd branch target; this harness therefore proves nothing about
address-error entry (out of this task's scope; base MC68000 address-error synchronous exception entry is not
implemented by production at all, a separate architecture decision the same way ADR-0037 owns divide-by-zero).

`M68kMemoryEmissionContext::continuation` (the value a `call_general`/`bsr_call` push writes) is a caller-
supplied constant, never derived from `operation.provenance` by the lowering -- production's real callers
(the whole-program static-discovery/AOT pipeline) always supply the real call site's own continuation. No row
had ever exercised a call-shaped kind through the T003 driver's own single-instruction direct emitter before
this task, so it had a stale placeholder (`0`) for every instruction; `tests/tools/m68k_conformance_emitter.cpp`
now computes the correct continuation (`kBase + this instruction's own length`) for every emitted instruction, a
test-harness-only fix (the emitted C11 lowering itself is unchanged).

RTS has NO T003 table row, the same acceptance-criterion carve-out already established for DIVS.W/DIVU.W
(SEG-021-T010): production's direct/non-routed RTS lowering (`M68kIrKind::return_from_subroutine`,
`libs/codegen/c11/src/m68k.cpp`) only completes a return whose popped address matches a statically tracked call
frame's own recorded continuation (`memory->frame_ids_array`/`frame_continuations_array`/`frame_depth`) -- by
design (ADR-0011/ADR-0039's whole-program continuation authority), not a bug. An isolated RTS with no
established frame (exactly what a standalone T003 row would be) fails closed (`return 1`, i.e. a runtime stop)
in every legal context this harness can construct, so it is correctly unsupported through the direct route; the
existing T002 capability snapshot's own `RTS | program_control | 1 | 1 | native_exec 1` unsupported-mnemonic row
already recorded this independently, unaffected by this task. RTS's real frame-matched semantics (including the
fail-closed non-member-continuation and forced-routed-read-failure cases) are covered end to end by
`tests/genesis_immutable_rom_aot_return_from_subroutine_and_bit_clear_generated_test.py` against the real
ADR-0011 whole-program continuation authority; RTR remains entirely out of this task's scope (a distinct
mnemonic, never claimed here).
