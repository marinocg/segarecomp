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
Rows exist for every legal ordinary auto-update shape of MOVE/MOVEA/ADD/SUB/CMP/ADDA/SUBA/CMPA/logical/immediate/quick/
unary families; CMPM, ADDX/SUBX and NEGX rows are absent because production has no decoder for them yet.

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
