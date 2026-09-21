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

A row names a legal form id of `tests/fixtures/m68k-legal-forms.json`, which supplies the exact primary words.
It states the EA role (`ea`: `src`/`dst`), how the vector values bind to operands (`bind`: `d@S` = data register
in word bits S..S+2, `ea` = the word's own EA), a sweep `profile` for every word and a `full_profile` for the
listed `full_words` (distinct registers, aliased registers, A7). Profiles are `single`, `cross` or `pair_list`
over named value sets crossed with SR seeds. Boundaries covered by the committed profiles: zero, negative,
carry/borrow, signed overflow, X/C interaction (SR 2700/2710/271F), byte/word/long boundary values, garbage upper
bits, register aliasing and A7 byte auto-update (adjusts by two). No runner change is needed.
Operand binding currently covers Dn, (An), (An)+ and -(An); new EA classes are added once in `bind_operand`.

## Compared state

D0-D7, A0-A7 (A7 = SSP), PC, SR/CCR, USP, byte-granular memory writes against the initial image (this covers
memory RMW results, auto-updated EA memory and exception stacked frames) and the exception-vector hook: when the
final PC is a vector handler address (vectors 2..31 are seeded to distinct handlers) a `k=2` effect records the
vector number, so exception families reuse the same rows and comparison. Timing is not compared.

## Limits (measured, not hidden)

Vectors run in supervisor mode (user-mode/USP-swap semantics need a family-owned binder). A write that does not
change a byte is invisible. Only a fully passing row (every word, every vector) can credit the T002 manifest.
Extension-word forms are outside the current binder, so manifest credit is only meaningful for single-word forms
(the T002 baseline fixes extension words to 0x0004).

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
