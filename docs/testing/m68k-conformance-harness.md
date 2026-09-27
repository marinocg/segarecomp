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
Dn aliasing; profiles `bit_sweep`/`bit_full`, static bit numbers as literal suffixes).
SEG-021-T014 adds 50 rows: ADDX/SUBX (`Dy,Dx` and `-(Ay),-(Ax)`, all sizes, 12 rows), CMPM (3 rows), NEGX (24 rows, every
legal EA) and the 11 NEG rows previously missing (absolute, `(d8,An,Xn)` and the byte/long `d16(An)` forms). They use the
`ext_sweep`/`ext_full` (pair profiles) and `xunary_sweep`/`xunary_full` (unary) profiles, whose SR seeds
(2700/2704/2710/2714, plus 271F in the sweeps) cover every X/Z combination, so carry/borrow chains, X propagation, sticky Z,
signed overflow, register aliasing (`(A0),(A0)` pairs collapse to consistent states) and the A7 byte step of two are
compared against the pinned Musashi core. The full-profile words per row are the aliased (Rx = Ry), A7 and mixed pairs.

## Compared state

D0-D7, A0-A7 (A7 = active stack pointer), PC, SR/CCR, USP, SSP, byte-granular memory writes against the initial
image (memory RMW results, auto-updated EA memory and exception stacked frames) and the exception-vector hook: vectors
2..255 are seeded to distinct handler addresses (`CF_HANDLER(v)`); when the final PC is a handler a `k=2` effect
records the vector number (TRAP #0..#15 = 32..47, user vectors above), so exception families reuse the same rows and
comparison. Timing is compared only for rows that set `"timing": true` (see the SEG-021-T021 section). Exception
instruction semantics are not implemented by this harness.

## Limits (measured, not hidden)

A credited primary word means the vectors declared by its row (all suffixes, all profile cases) matched Musashi;
it is NOT semantic exhaustiveness (a handful of extension values, fixed baseline registers and memory pattern).
Family tasks own deeper family-specific vector expansion. A write that does not change a byte is invisible. The
generated model has one active A7 plus USP, so SSP is shadowed by the runner. Timing is compared only for
`"timing": true` rows, at the fidelity of published instruction cycle totals (not bus cycles). Only a fully
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

`m68k_instruction_cycles` has a published static row for every legal BTST/BCHG/BCLR/BSET form. The dynamic
`BTST Dn,#<data>` form (`btst.dn_ea.b.dn.imm`) was timing-unsupported until SEG-021-T022 resolved its cell: Table 8-8's
dynamic BTST memory row (4) plus the Table 8-1 `#<data>` byte/word cell (4) = 8, which is also the pinned Musashi
total. The bit-operation rows are not `"timing": true` rows (timing is compared only from
SEG-021-T021 on, for the rows listed there).

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
Superseded by SEG-021-T021 (below): the register forms now have a CPU-owned `register_count` rule and are timing-validated.

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

## SEG-021-T015: ABCD / SBCD / NBCD rows and the undefined-flag policy

12 rows cover every legal form (`Dy,Dx` and `-(Ay),-(Ax)` of ABCD and SBCD, and NBCD over every data-alterable EA including
`(d8,An,Xn)`), all byte-sized. The `bcd_sweep`/`bcd_full` (pairs) and `bcd_unary_sweep`/`bcd_unary_full` profiles use a
`bcd` value set (nibble/decade boundaries 09/0A/0F/19/1A/99/9A/A0/F0/FF with garbage upper register bits) and SR seeds
2700/2704/2710/2714 (every X/Z combination). All 12 rows match the pinned Musashi core.

Semantics: the byte result is the decimal-adjusted sum/difference/ten's complement; X and C are the decimal carry/borrow; Z is
cleared by a non-zero result and otherwise unchanged (sticky, as ADDX/SUBX). **N and V are undefined on the base MC68000**
(Motorola's manual lists them undefined). Production reproduces the pinned Musashi core exactly for them
(`M68kDecimalArithmeticSpecification` in `libs/cpu/m68k/include/segarecomp/cpu/m68k/effects.hpp`: N = bit 7 of the adjusted result;
V = bit 7 of `~(pre-adjust intermediate) & adjusted result`; NBCD's "nothing to negate" outcome, a zero byte with X clear, leaves the
byte unchanged, clears X/C/V, keeps Z and sets N). This is a matched-to-oracle policy, not documented hardware behavior; it is labeled
in `undefined_flags` of the vector table and in the validation manifest description, so the `ccr_sr_validated` credit of these words
means X/C/Z as documented and N/V as matched only.

`tests/m68k_bcd_exhaustive_musashi_test.py` (tier `full`, oracle-available; skipped with exit 0 without the pinned checkout; about
4.5 minutes) additionally sweeps EVERY (source byte, destination byte) pair times the four X/Z seeds for ABCD/SBCD in register and
predecrement forms and every operand byte times the four seeds for NBCD on `Dn`, `(An)`, `(An)+` and `-(An)`: 1,052,672 vectors, all
identical to Musashi including N/V. `m68k_routed_lowering_test.py` compares the routed lowering with the direct one for the register,
aliased and A7 pairs and every NBCD EA class, and forces routed stops on every auto-updating shape.

Timing: published static rows exist (ABCD/SBCD `Dy,Dx` 6, `-(Ay),-(Ax)` 18; NBCD `Dn` 6, memory 8 + byte EA cell), so no form is
timing-unsupported; `timing_validated` stays 0 (timing is not compared).

## SEG-021-T016: EXG / MOVEP / Scc / TAS rows

143 rows cover every legal form: EXG (`Dx,Dy`, `Ax,Ay`, `Dx,Ay`; distinct, aliased and A7 pairs), MOVEP (word/long, both directions,
even, odd and negative displacements, `unary_sweep`/`unary_full` register values over the deterministic memory pattern), Scc (all 16
conditions x every data-alterable class incl. `(d8,An,Xn)`, every SR CCR combination 2700-270F via `scc_sweep`) and TAS (every
data-alterable class, boundary bytes incl. 0x00/0x7F/0x80/0xFF). All 143 rows (about 95,000 vectors) match the pinned Musashi with zero
divergences; CCR unaffected by EXG/MOVEP/Scc is verified because every vector compares the whole SR. `m68k_routed_lowering_test.py`
compares the routed lowering with the direct one for every EXG pair, MOVEP shape, Scc condition and Scc/TAS operand class, and forces
routed stops on every memory form (no partial architectural state).

Semantics: Scc writes 0xFF/0x00 and never touches CCR; a memory destination is read (byte, value discarded) before it is written and
`Dn` Scc performs no memory access (generated-C ordering, read-rejected/write-rejected atomicity and auto-update commit-once are proved by
`genesis_immutable_rom_aot_exg_movep_scc_tas_generated_test`); TAS sets N/Z from the operand byte, clears V/C, keeps X and writes the byte with
bit 7 set (CPU semantics only: the indivisible bus read-modify-write cycle is platform-owned); MOVEP moves the bytes of the register
most-significant first to/from every second byte of `d16(An)` and never updates An; EXG changes no CCR bit.

Timing: published static rows exist for EXG (6), MOVEP (16 / 24), TAS (Dn 4, memory 10 + EA) and Scc memory forms (8 + EA). The 16 `Scc Dn`
forms are recorded as timing-unsupported in `m68k_instruction_cycles` (4 false / 6 true depends on the runtime condition); generated
retirement uses the dynamic expression `m68k_scc_true ? 6 : 4`. Superseded by SEG-021-T021 (below): `Scc Dn` has a CPU-owned
`condition` rule and is timing-validated.

## SEG-021-T018: status register, CCR, USP and privilege rows

39 rows cover every legal form of MOVE <ea>,SR and MOVE <ea>,CCR (all data addressing sources), MOVE SR,<ea>
(all data-alterable destinations), MOVE An,USP, MOVE USP,An, ANDI/ORI/EORI to CCR and to SR, and RTE
(4,617 vectors, all validated against the pinned Musashi core configured as a plain 68000, 179 primary words
credited to the manifest). The vectors' SR seeds include **user-mode** states (`0715`, `0015`, `0000`), so the
same rows compare the privilege-violation entry (vector 8: frame on the SSP, saved SR with S = 0, stacked PC =
the privileged instruction, the k=2 vector entry), the SR-write stack-pointer swap in both directions, and RTE
restoring S = 0 onto the USP (`rte_frame` binds the frame through the new `sr@sp` / `pc@sp` operand specs).
The generated runner now models `usp` as the inactive stack-pointer slot (USP in supervisor mode, SSP in user
mode) and reports the architectural USP/SSP pair.

Project-only, never compared with Musashi: SR values with T = 1 (bit 15). ADR 0043 §6 defers trace; the
generated code stops fail-closed where Musashi (trace emulation off) would continue, so every SR source value
and every stacked RTE SR in the table keeps T = 0, and the T = 1 stop is proved by
`tests/genesis_status_register_privilege_generated_test.py` and `tests/m68k_exception_core_ownership_test.py`.
MOVE SR,<memory> performs the MC68000 read-before-write (as SEG-021-T016 memory Scc does); the pinned Musashi
core omits the dummy read, which is invisible in the conformance memory model.

## SEG-021-T019: TRAP / TRAPV / CHK / ILLEGAL / RTR rows and architecturally reserved words

Rows exist for every legal form of TRAP #n (all 16 vectors), TRAPV, CHK.W (all 11 data-addressing bound classes; the
tested Dn bound with `d@9`, the bound with `ea.src`; `chk_pairs` covers in range, equal to the bound, zero, Dn.W
negative, Dn.W above the bound, a negative bound and garbage upper register bits; the immediate row uses four literal
bounds with `chk_single`), ILLEGAL and RTR (`sr@sp`/`pc@sp` frame binding, `rtr_frame` pairs with garbage in the
upper byte of the popped word). SR seeds include supervisor and user states (`exception_state`, `trapv_state` with V
set and clear), so every row compares the vector number (the k=2 entry), the six-byte frame bytes on the SSP (saved
SR word at SP, stacked PC long at SP+2), the SSP/USP swap and the post-exception SR against the pinned Musashi core.
15 legal rows (18,254 vectors) are validated and credited; CHK takes vector 6 in 11,008 of its vectors and continues
in 7,136, in both modes.

Architecturally reserved words are exercised by three pseudo-form rows that take their primary words from the T001
partition classes named in the row (`partition_classes`; the tool still holds no legality knowledge):
`reserved.partition.illegal` (unassigned and post-MC68000 encodings, vector 4, 11,528 words),
`reserved.partition.line_a` (vector 10, 4,096 words) and `reserved.partition.line_f` (vector 11, 4,088 words),
supervisor and user state (39,424 vectors), all validated. Pseudo-form rows never credit the legal-form manifest.
Deviation: `0xF620-0xF627` (`exclude_words`) is the pinned Musashi's CPU-type-unguarded 68040 MOVE16 handler, which
executes instead of raising vector 11 on the 68000 core (the documented `m68k-word-sweep-disagreements.json` quirk);
production raises vector 11 for these words as the manual requires.

CHK.W condition codes: Z/V/C are undefined on the MC68000 and N is undefined when no trap is taken. Production matches
the pinned Musashi core (Z <- Dn.W == 0, V <- 0, C <- 0, N changed only on a trap: set for Dn.W < 0, cleared for
Dn.W > bound), which agrees with every case the manual defines; `undefined_flags.chk_policy` in the table records it.
An `(An)+`/`-(An)` bound commits its address-register update before the exception (the saved frame carries the new
flags).

Timing (SEG-021-T022): the retiring paths have static rows (TRAPV V = 0: 4; CHK.W in range: 10 + word EA cell; RTR:
20); a taken exception reports its Table 8-14 entry time (TRAP, TRAPV, ILLEGAL, line A/F: 34; CHK: 40 + the bound's
word EA cell), which the machine charges at the entry commit. All of these rows are timing rows except the
memory-bound CHK rows (Musashi omits the EA term; see the SEG-021-T022 section).

## SEG-021-T021: outcome-dependent timing for Bcc, DBcc, Scc Dn and register shift/rotate

**Owner.** `m68k_instruction_timing` (`libs/cpu/m68k/include/segarecomp/cpu/m68k/timing.hpp`) is the one CPU-owned
retirement-time rule: `fixed` (every static `m68k_instruction_cycles` row, unchanged), `condition` (true/false),
`dbcc` (condition true / counter expired / branch taken) or `register_count` (`base + per_count * n`). The shared M68k
lowering owner renders it as C11 (`m68k_timing_c_expression`, `libs/codegen/c11`) against the outcome locals the
lowering already materializes, and every generated route (ordinary blocks, C4 prefixes, immutable-ROM AOT bodies) plus
the conformance emitter's `--timing` mode consume that one rendering; `m68k_timing_cycles` is the host-side evaluation
for tests. The generated C text is byte-identical to the previous hand-written expressions.

**Published rows** (MC68000 User's Manual section 8):

| Form | Rule |
| --- | --- |
| Bcc.B / Bcc.W (Table 8-10) | taken 10; not taken 8 (byte) / 12 (word); BRA stays the static 10 |
| DBcc (Table 8-10) | condition true 12; condition false and counter expired (Dn.W = -1) 14; condition false and branch taken 10 |
| Scc Dn (Table 8-6) | condition true 6, false 4 (memory forms stay static: 8 + byte EA) |
| ASd/LSd/ROd/ROXd register (Table 8-9) | B/W 6 + 2n, L 8 + 2n; n = immediate 1..8, or the count register modulo 64 (n = 0 costs the base); ROXL/ROXR use the same n for timing although the rotation is taken modulo size + 1 |
| memory shift/rotate (Table 8-9) | static 8 + word EA (unchanged) |

**Fidelity.** Published instruction cycle totals consumed by the deterministic scheduler at instruction retirement. Not
bus-cycle accurate: no wait states, prefetch, bus arbitration or intra-instruction access timing.

**Validation.** 110 rows carry `"timing": true` (28 Bcc, 2 BRA, 16 DBcc, 16 Scc Dn, 48 register shift/rotate = the 8
families x {Dn count, immediate count} x {B, W, L}). For every vector the generated side stores the rendered rule's value
for the executed outcome in `cf_cycles`; the oracle side reports what `m68k_execute(1)` consumed for exactly that one
instruction; `compare_timing` reports a mismatch as a `domain: "timing"` first divergence with a `cycles` field, and a
timing row whose form has no rule is `unsupported` (fail closed). The existing vector profiles already exercise every
outcome: `cc_full`/`dbcc_full` sweep all 16 CCR states (Bcc taken and not taken; DBcc true, expired from Dn.W = 0 and
branch taken) and `shift_count` includes count registers 0, 1, 8, 63, 64, 65 and 255 (modulo 64), while the immediate
forms cover 1..8 through the word ranges. Measured with the pinned core: 227,776 timing comparisons, 0 divergences,
7,168 primary words credited to `timing_validated_words` (`timing_validated`: 0 -> 110 of 1526 forms).
`tests/m68k_conformance_harness_test.py` also checks, independently of both the production owner and Musashi, a sample of
rows against a test-owned transcription of the published rows and condition tests (generated side always; Musashi side
when pinned), that each outcome and the counts 0/1/8/63 occur, that an injected timing fault in one function fails exactly
that word, and that a timing row whose function reports no rule is unsupported (since SEG-021-T022 every decodable
form has a rule, so the missing rule is injected into one function).

**Musashi agreement.** For all 110 rows the pinned core's reported cycles equal the published tables (its 68000 constants
`CYC_BCC_NOTAKE_B = -2`, `CYC_BCC_NOTAKE_W = 2`, `CYC_DBCC_F_NOEXP = -2`, `CYC_DBCC_F_EXP = 2`, `CYC_SCC_R_TRUE = 2`,
`CYC_SHIFT = 1` over base cycles 10/10/12/4/6/8, and it charges the modulo-64 count for ROXL/ROXR). No deviation is
recorded.

**Still fail closed at T021 (24 forms; closed by SEG-021-T022, see below).** MULU.W/MULS.W (22 forms), `BTST
Dn,#<data>` and RESET. After T022 only RESET (a decode frontier) and the direct_flow-profile-only `BNE.S`
compatibility kind have no rule.

## SEG-021-T020: STOP row and the interrupt acceptance differential

`stop.imm16.none.imm.none` uses eight literal SR immediates (`2700`, `2000`, `271F`, `2015`, `0715`, `0000`, `5F3F`,
`7FFF`: mask values 0-7 across the set, supervisor-to-user transitions and every implemented CCR bit) with the
`sr_state` seeds (40 vectors). Supervisor seeds compare the loaded SR, the PC after the immediate word and the
stack-pointer swap when S clears; user seeds compare vector 8 with STOP itself stacked on the SSP. All vectors match
the pinned Musashi core and the row credits `0x4E72`. Since SEG-021-T022 the row is a timing row: the supervisor
vectors retire in 4 cycles and the user-mode vectors report the 34-cycle privilege-violation entry, both equal to
Musashi. T = 1 immediates are project-only (the deferred-trace stop; see the SEG-021-T018 section)
and the wait/wake itself is machine scheduler behavior, proved by `tests/genesis_stop_interrupt_generated_test.py`.

Interrupt acceptance is not an instruction and has no table row: `tests/m68k_interrupt_acceptance_test.py` scripts the
M68K-owned contract and the pinned Musashi core (interrupt-acknowledge callback enabled) through the same 85
instruction-boundary steps -- levels 1-6 against every mask, level-7 transitions at every mask, level 7 held at
mask 7 and then recognized once the mask is lowered, a 7 -> 3 -> 7 re-transition, autovector/supplied/spurious/
uninitialized acknowledges, user-mode entry, T cleared on entry, and STOP woken by an accepted level versus staying
stopped on masked ones -- and compares PC, SR, A7, USP and the six bytes at A7 after every step.

## SEG-021-T022: MULU/MULS, DIVU/DIVS and exception-entry timing

**Owner.** The data-dependent rules are CPU-owned C in `libs/cpu/m68k/include/segarecomp/cpu/m68k/timing_core.h`
(strict C11, `<stdint.h>` only), consumed verbatim by the C++ descriptor (`m68k_instruction_timing`: rules
`multiply_unsigned`, `multiply_signed`, `divide_unsigned`, `divide_signed`, `cycles` = the Table 8-1 word EA cell) and
by the generated retirement expressions (`segarecomp_m68k_*_word_cycles(...) + UINT32_C(ea)`, through the Genesis
runtime header). The former per-program `genesis_m68k_mulu/muls_word_cycles` helper text is gone. Every descriptor
also carries `exception_entry_cycles`, the exception-processing time of the form's one synchronous exception path
(ADR 0043 §8 implementation note); the direct route reports it when the exception is taken
(`timing_exception_taken`), the routed raises pass it to the machine, which charges it at the entry commit.

| Rule | Source | Value |
| --- | --- | --- |
| MULU.W (Table 8-4) | published | 38 + 2n + EA, n = 1 bits of the source word |
| MULS.W (Table 8-4) | published | 38 + 2n + EA, n = 01/10 pairs of `<source>:0` |
| DIVU.W, divisor != 0 | Cwik's microcode analysis (Table 8-4 gives only `< 140`) | overflow 10; else 76..136 + EA |
| DIVS.W, divisor != 0 | same (Table 8-4: `< 158`) | absolute overflow 16/18; else 120..156 + EA |
| BTST Dn,#<data> (Table 8-8) | published | 4 + 4 = 8 |
| exception entry (§8 exception table) | published | 34 illegal / line A / line F / privilege / TRAP / TRAPV / trace; 38 + EA zero divide; 40 + EA CHK; 44 interrupt |

**DIV approximation replaced.** DIVU/DIVS formerly retired the Table 8-4 maxima (140 / 158 + EA) for every operand;
they now retire the exact count, which is 2..142 cycles shorter depending on the operands (overflow is detected up
front and is the cheapest case). The pinned Musashi core still charges the fixed maxima (and does so for overflow
too), so it cannot validate DIV timing: the rule is checked instead against an independent test-owned transcription of
the same published analysis (`tests/m68k_muldiv_auto_update_generated_test.py`: 17 dividend/divisor pairs covering
every sign combination, absolute and signed overflow, the most negative dividend and a zero divisor, through
generated DIVU/DIVS Dn and DIVU (An)+ code on the Genesis runtime, plus the published bounds over a sweep) and the
host rule (`tests/m68k_dynamic_timing_test.cpp`). DIV has no direct-route emission, so it has no conformance timing row.

**MUL rows.** 22 `*.timing` rows (every MULU.W/MULS.W source-EA form) use the `mulu_timing` profile (sources
`0000`, `0001`, `0003` ... `FFFF`, `5555`, `AAAA`, `8000`: n = 0..16) and the `muls_timing` profile (zero and
negative sources `0000`, `8000`, `FFFF`, `8001`, `8005`, `8015`, `8055`, `8155`, `8555`, `9555`, `AAAA`, `CCCC`: n = 0
and every odd n); the immediate rows carry the same patterns as literal extension words. All 30,528 vectors match
the pinned core. **Documented deviation:** Musashi counts MULS pairs only while source bits remain, so for every
positive nonzero source it misses the final `1 -> 0` pair and reports Table 8-4 - 2 (a negative source ends with its
sign bit and has only odd n, which Musashi counts exactly). Production follows Table 8-4 (the same n Genesis Plus GX
and the manual use); `tests/m68k_conformance_harness_test.py` pins the generated value for positive sources and the
exact Musashi offset, and `tests/m68k_dynamic_timing_test.cpp` checks all 65,536 source words of both rules against a
test-owned transcription.

**Exception entry.** Exception-taking vectors of the timing rows compare the entry time with Musashi, which reports
`CYC_EXCEPTION[vector] - CYC_INSTRUCTION[IR]` on top of the instruction row, i.e. exactly the table's total. It agrees
for TRAP #n, TRAPV, ILLEGAL / line A / line F / every reserved word, the privilege violation of every privileged form and
CHK with a Dn bound. **Documented deviation:** because Musashi undoes the whole instruction row, EA included, it charges
a taken CHK trap 40 regardless of the bound's EA (and a zero divide 38); the manual's rows are "+ EA", which production
follows, so the memory-bound CHK rows are not timing rows (the offset is pinned by the harness test). The interrupt
entry (44) is compared step by step in `tests/m68k_interrupt_acceptance_test.py` (every acknowledge kind, user mode, STOP
wake), except that Musashi's table carries a placeholder 4 for the user vectors 64-255 (documented; 44 is asserted).

**Timing audit of the SEG-021 families (T005-T016, T018-T020, T034-T036).** Every conformance row was run once with
`"timing": true` against the pinned core (1,663,616 vectors). 1,366 of 1,484 rows matched as they stood; the 118
divergent rows split into production errors, corrected here, and Musashi deviations, documented and kept out of the
timing rows (`TIMING_ORACLE_DEVIATION_ROWS` in `tests/m68k_conformance_harness_test.py` pins the exact list):

| Rows | Production before | Published row | Musashi | Resolution |
| --- | --- | --- | --- | --- |
| TST memory (21) | EA only | Table 8-6: 4 + EA | 4 + EA | corrected |
| MOVE to -(An) (35) | 4 + src + 6/10 | Tables 8-2/8-3: -(An) destination = (An) column | same | corrected |
| CMPA.W (12) | 8 + EA | Table 8-4: 6 + EA | 6 + EA | corrected |
| BCLR Dn,Dn / #,Dn (2) | 8 / 12 | Table 8-8: 10 / 14 (maxima) | 10 / 14 | corrected |
| ADD/SUB/AND/OR.L Dn,Dn; ADDA/SUBA.L Dn/An,An (8) | 8 | Table 8-4 "**": 8 | 6 | deviation |
| ADD/SUB/AND/OR.B/.W #,Dn; ADDA/SUBA.W #,An (10) | 8 / 12 | 4 + 4 / 8 + 4 | +2 | deviation |
| ADDQ.W #,An (1) | 8 | Table 8-5: 8 | 4 | deviation |
| ANDI.L #,Dn (1) | 16 | Table 8-5: 16 | 14 | deviation |
| CHK memory bound, trap taken (10) | 40 + EA | Table 8-14: 40 + EA | 40 | deviation |
| TAS memory (7) | 10 + EA | Table 8-6: 10 + EA | 14 + EA | deviation |
| MULS with positive sources (11) | Table 8-4 n | 38 + 2n | n - 1 | deviation; `.timing` rows validate the rest |

Every deviation keeps the published row (Genesis Plus GX's hardware-oriented cycle table also agrees with production
for all of them). The audit also added 21 rows for forms that had no conformance row (LEA and PEA in all seven control
EAs, JMP/JSR absolute and d16(PC), MOVEQ); they exposed one more production error, corrected here: the indexed LEA /
PEA rows of Table 8-10 are 12 / 20 (production had the table 8-1 cell, 10 / 18; Musashi 12 / 20).

Result: 1,479 of 1,527 rows are timing rows (22 of them the new `.timing` MUL rows) and every one matches the pinned
core: 1,612,772 + 4,612 timing comparisons, 0 divergences. The 48 deviation rows remain semantic/CCR/EA rows only.

**Coverage (capability snapshot).** `timing_model_present` 1,502 -> 1,525 of 1,526 forms (99.93%); `timing_validated`
110 -> 1,457 of 1,526 forms (7.21% -> 95.48%; 7,168 -> 43,850 primary words). The 69 forms without validated timing,
exhaustively:

- RESET (1): no timing descriptor -- it is still a decode frontier (its Table 8-11 row is 132).
- DIVU.W / DIVS.W (22, every source EA): exact rule modeled; no direct-route emission (the vector-5 raise needs the
  routed runtime) and the pinned Musashi charges fixed maxima, so they are validated against the independent
  transcription instead of the oracle.
- JMP / JSR with (An), d16(An), (d8,An,Xn), (d8,PC,Xn) (8) and RTS (1): published static rows modeled; their targets are
  runtime-owned (indirect / return-target authority), which the single-instruction direct harness cannot emit.
- Oracle-deviation forms (37): ADD/SUB/AND/OR .B/.W `#,Dn` and .L `Dn,Dn`, ADDA/SUBA .L `Dn/An,An` and .W `#,An`,
  ADDQ.W `#,An`, ANDI.L `#,Dn`, the 10 memory-bound CHK forms and the 7 memory TAS forms (table above).
