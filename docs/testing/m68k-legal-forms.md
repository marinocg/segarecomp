# Independent legal base-MC68000 form baseline

`tests/fixtures/m68k-legal-forms.json` enumerates every legal base-MC68000 instruction form. It is the
independent denominator for M68K coverage claims (SEG-021); it is test-side expectation data only.

## Derivation

`tools/m68k_legal_forms.py` transcribes the public Motorola M68000 Family Programmer's Reference Manual
(instruction summary, encodings, addressing-mode tables) into data tables and expands them to
mnemonic x form/direction x size x source EA class x destination EA class, with condition variants
(Bcc/DBcc/Scc, one form each), operand-value variants (quick/count/data8/trap vector, recorded not
expanded), privilege class, instruction-defined exception classes and auto-update side-effect class.
Every form also derives its primary opcode words; the tool asserts no word belongs to two forms and
emits the full 65,536-word primary-word partition. Output is byte-for-byte reproducible (sorted keys,
fixed row order, no timestamps): `python3 tools/m68k_legal_forms.py [--check]`.

## Independence rule

* The tool and the dataset never import, read or mention production legality (`libs/cpu/m68k`,
  EA masks, decode predicates) or oracle output; the tool uses only the Python standard library.
* No production source (`libs/`, `platforms/`, `apps/`, product tools, CMake files outside `tests/`)
  imports or reads the tool or the dataset. Only tests and the coverage measurement (T002) consume it.
* `tests/m68k_legal_forms_test.py` enforces both directions (with negative-control scanners).

## Vocabulary

| term | meaning |
| --- | --- |
| legal form | architecturally legal base-MC68000 form listed in `forms` |
| architecturally illegal | primary word with no base-68000 meaning, raises vector 4 (classes `I`, `X`) |
| architecturally reserved exception | line-A (vector 10) / line-F (vector 11) words: defined exceptions, not forms |
| post-68000 encoding | 68010/68020+ encoding, illegal on a 68000 (class `X`); no 68010+ behavior is modeled |
| privileged | legal form raising vector 8 in user mode (class `P`) |
| unsupported by segarecomp | measurement-side status only, assigned by the coverage tool; never stored in the dataset, never equivalent to illegal |

## Musashi primary-word cross-check

`tests/tools/m68k_word_sweep.py` (+ `m68k_word_sweep_musashi_runner.c`) runs all 65,536 primary words on
the pinned Musashi (revision recorded in the sweep fixture) configured as a 68000, once in supervisor and
once in user mode, from a fixed state (zero data registers, A0-A6=0x2000, all extension bytes 0x00,
handler-per-vector table). It is an overlap/hole check of primary words only: it is never used to infer
extension-word or EA legality and is never the coverage denominator. Every disagreement with the manual
partition is recorded with its resolution in `tests/fixtures/m68k-word-sweep-disagreements.json`
(currently one explained Musashi quirk: the unguarded 68040 MOVE16 handler at 0xF620-0xF627; zero
unexplained). The test skips cleanly unless `SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT` names the pinned
checkout. Regenerate the fixture with `python3 tests/tools/m68k_word_sweep.py --compiler cc --write-fixture`.

## Coverage measurement (SEG-021-T002)

`tools/m68k_capability_coverage.py` consumes this dataset and reports, per stage and admission route, how many
legal forms the real pipeline handles; see `docs/testing/m68k-capability-coverage.md` (generated) and the
ratchet `tests/m68k_capability_ratchet_test.py`. Together with `tools/m68k_conformance.py`
(SEG-021-T003, see `docs/testing/m68k-conformance-harness.md`, which takes each form's exact primary words from
`word_ranges`) it is one of the only two product tools permitted to consume the dataset.
