# Independent NMOS Z80 legal-form dataset

`tests/fixtures/z80-legal-forms.json` enumerates the complete NMOS Z80 instruction encoding space. It is the
independent denominator for every SEG-008 Z80 coverage claim. It is test-side expectation data only.

## Derivation

`tools/z80_legal_forms.py` transcribes public references into data tables: the Zilog Z80 CPU User Manual
(UM0080), Sean Young's *The Undocumented Z80 Documented* (v0.91) and Cristian Dinu's *Decoding Z80 Opcodes*
(x/y/z/p/q field decomposition). It classifies every byte of the seven finite canonical opcode spaces:

| space | bytes | meaning |
| --- | --- | --- |
| `base` | 256 | unprefixed opcode byte |
| `cb` | 256 | byte after `CB` |
| `ed` | 256 | byte after `ED` |
| `dd` / `fd` | 256 each | byte after the effective `DD` / `FD` prefix |
| `ddcb` / `fdcb` | 256 each | fourth byte of `DD CB d op` / `FD CB d op` |

Each byte gets exactly one classification: a **form**, or a **prefix-behaviour class** (`escape_*`,
`prefix_chain`, `prefix_ignored`, `prefix_ignored_before_ed`). Bytes with identical classification form one row
(mnemonic x operand class, e.g. `ld.r_ix_d.dd`). Each row records:

- the space and the exact opcode bytes (`byte_ranges`);
- documented or undocumented status, and `alias_of` for encodings with the same architectural effect as a
  documented form;
- length, operand-byte layout and M1-fetch count (the R-register increment);
- the NMOS timing class: `fixed`, `conditional` (taken / not taken, only where timing genuinely differs),
  `repeat` (repeating / final iteration) or `halt`;
- the coarse effect classes and the oracle comparison columns that apply (`state`, `memory`, `io`);
- the owning family and its task: `data_alu` (T004), `control_stack` (T005), `cb_bit_prefix` (T006) or
  `ed_io_interrupt` (T007);
- the scope: `in_scope` or `excluded:<reason>`. The T001 scope decision (ADR 0056) excludes nothing.

`DD`/`FD` prefix chains have unbounded length, so they are not enumerated. `prefix_rules` specifies them
parametrically (`index_prefix_chain`, `index_prefix_ignored`, `index_cb_displacement`, `no_other_chains`): the
last prefix is effective; each superseded or ignored prefix costs 4 T-states and one M1 fetch; the length and T-state
cost are functions of the prefix count; decode is bounded only by the immutable image and fails closed at its edge.

Output is byte-for-byte reproducible (sorted keys, fixed row order, no timestamps):
`python3 tools/z80_legal_forms.py [--check] [--summary]`.

## Counts (schema 1)

261 forms (199 documented, 62 undocumented), 1,446 form encodings, 0 exclusions.

| space | forms | documented bytes | undocumented bytes | prefix-behaviour bytes |
| --- | --- | --- | --- | --- |
| base | 75 | 252 | 0 | 4 (CB, DD, ED, FD escapes) |
| cb | 22 | 248 | 8 (SLL) | 0 |
| ed | 44 | 58 | 198 (178 ED holes, aliases, `IN (C)`, `OUT (C),0`) | 0 |
| dd / fd | 38 each | 39 each | 46 each (IXH/IXL/IYH/IYL) | 171 each |
| ddcb / fdcb | 22 each | 31 each | 225 each (copy-back, SLL, BIT aliases) | 0 |

| family | task | forms | encodings |
| --- | --- | --- | --- |
| data_alu | SEG-008-T004 | 125 | 375 |
| control_stack | SEG-008-T005 | 29 | 66 |
| cb_bit_prefix | SEG-008-T006 | 66 | 768 |
| ed_io_interrupt | SEG-008-T007 | 41 | 237 |

`cb_bit_prefix` (T006) also owns the prefix rules.

## Coverage schema

The coverage unit is the form. A form is claimed in a stage only when every byte in `byte_ranges` passes. The
stage columns are `decodes`, `lowers`, `emits`, `compiles`, `executes`, `aot_admitted`, `oracle_state`,
`oracle_memory`, `oracle_io`, `timing_modeled` and `timing_validated`. `oracle_memory` and `oracle_io` are not
applicable to a form whose `observables` omit them. Snapshots are regenerated deterministically and ratcheted
monotonically, as for M68K (`docs/testing/m68k-capability-coverage.md`). The coverage probe is built by T003.
"Unsupported" is a measurement-side status only; it is never stored in the dataset.

## Independence rule

- The tool and the dataset never import, read or mention production Z80 decoding (`libs/cpu/z80`,
  `codegen_c11_z80`) or oracle output. The tool uses only the Python standard library.
- No production source (`libs/`, `platforms/`, `apps/`, product tools, root CMake files) imports or reads the
  tool or the dataset. Only tests and later coverage/conformance measurement tools consume it.
- `tests/z80_legal_forms_test.py` enforces both directions with negative-control scanners. It also checks
  byte-for-byte reproducibility, exactly-one ownership of all 7 x 256 bytes, the family-to-task mapping, the pinned
  counts and spot encodings/timings.
