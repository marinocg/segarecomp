# Z80 generated-native differential conformance harness (SEG-008-T003)

One reusable harness for every legal NMOS Z80 form. Deterministic synthetic vectors go through
decode -> lower -> emitted strict-C11 -> compile -> generated-native execution, and the identical vector text runs on
the pinned independent oracle (redcode/Z80, ADR 0057). The two result streams are compared per step with a
Z80-local first-divergence comparator. This is the Z80 sibling of `docs/testing/m68k-conformance-harness.md`: validating
a family means adding table rows, not changing the harness.

| file | role |
| --- | --- |
| `libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h` | machine-neutral generated-code runtime ABI (plain C11) |
| `libs/codegen/c11/{include/segarecomp/codegen/c11,src}/z80.*` | image-level emission (owners, entry table, dispatcher) |
| `libs/codegen/c11/{include/.../z80_lowering.hpp,src/z80_lower*.cpp}` | lowering registry and one row file per family |
| `tests/tools/z80_image_emitter.cpp` | public-entry-point CLI over the emitter (image spec -> sharded C) |
| `tests/tools/z80_conformance_runner.c` | generated-native side (linked with the emitted image) |
| `tests/z80_oracle/z80_conformance_oracle.c` | pinned-oracle side (test-only, never linked into production) |
| `tests/tools/z80_conformance_common.h` | shared vector reader and result writer (one schema for both sides) |
| `tools/z80_conformance.py` | driver: row expansion, batching, build, run, compare, manifests |
| `tests/fixtures/z80-conformance-vectors/<family>.json`, `scenarios.json` | the tables |
| `tests/fixtures/z80-validation-manifest/<family>.json` | committed oracle credit |
| `tests/z80_generated_pipeline_test.py`, `z80_conformance_harness_test.py`, `z80_conformance_oracle_test.py`, `codegen_c11_z80_dependency_test.py` | tests |

## Runtime ABI (`z80_runtime.h`)

- `Z80State`: A/F/B/C/D/E/H/L and the alternate set, IX, IY, SP, PC, WZ (MEMPTR), I, R, IM, IFF1/IFF2, Q, and the
  internal bits `halted`, `int_deferral` (one-boundary INT deferral: EI, IFF1-changing RETI/RETN), `ld_a_ir` (NMOS
  marker), **`in_prefix_run`** (ADR 0058 section 5), `nmi_reject`; the inputs `int_line` (level) and `nmi_pending`
  (edge) driven by the host; `cycles` and `deadline`.
- `Z80Host`: `read`, `write`, `io_in`, `io_out` (16-bit port), `interrupt_acknowledge` (the data-bus byte: IM2 vector or
  IM0 RST opcode; IM1 performs the transaction and disregards the byte) and `code_image(address, &{identity,
  window_base})`, which is the current-code-image-identity query (zero return: no immutable code mapped).
  Callbacks receive the cycle count at the start of the current instruction (contract section 7).
- `Z80Outcome` in two classes with predicates `z80_outcome_is_resumable` / `z80_outcome_is_error`:
  resumable `deadline`, `halted`, `prefix_lock`; errors `no_owner`, `mutable_code`, `unresolved_fetch_mapping`,
  `unknown_image_identity`, `excluded_form` (reserved: ADR 0056 excludes nothing, so it is class-checked at the ABI level
  only) and `im0_unsupported_acknowledge_byte`. `Z80_OUTCOME_NONE` is an internal "no stop requested" value in neither class.
- `z80_run(rt, deadline)` is generated per image. Boundary logic (deadline, NMI, INT, halted-cycle accounting) is
  in the header (`z80_boundary`); the generated dispatcher asks the host for the identity, does an exact entry-table lookup
  of `(identity << 16 | PC)` (absolute owners) or `(identity << 16 | PC - window_base)` (window-relative owners), and
  runs the owner chain. A HALTed or prefix-locked CPU is resumed by calling `z80_run` again with a later deadline.

## Image emission (`emit_image_set`)

Input: images with a unique identity <= 0xFFFF, kind `invariant` (exactly one window, absolute owners, directly bindable)
or `banked` (one or more windows of identical exposed range). Owners follow ADR 0058 B1 granularity, classified by the
T002 logical-fetch decoder: a full owner per decoded start whose form has a lowering row, a `prefix_lock` owner per
endless-run start, a typed stub per `mutable_code` / `unresolved_fetch_mapping` start. A decoded start whose form has
no lowering row yet gets no owner: dispatch to it fails closed with `no_owner`, and direct binding never targets it.
A banked image in several windows gets one **window-relative owner per offset**; a crossing start whose class differs
per window carries per-base variants (`switch (window_base)`), so a shared owner can be a full owner in one window and
the correct typed stub in another. Direct owner-to-owner binding (fall-through only in T003) exists solely from an
invariant-window owner to an emitted invariant-window successor; base-relative targets are never bound.

Output is sharded through `translation_units.hpp` with the packed dense key (`image ordinal x length + offset`, page
shift 8); the shared header carries only the ABI include (no owner declarations: bound successors are declared in block
scope, and the entry-table/dispatcher main TU declares each owner once). Output is byte-identical across runs.

## Vector schema (`z80_conformance_common.h`)

Text, one vector per `V ... E` block: `R` (16 byte registers), `P` (IX IY SP PC WZ), `X` (I R IM IFF1 IFF2 Q halted
deferral ldair prefix_run nmireject), `M`/`F` (memory patch/fill), `IN` (I/O input script), `ACK` (interrupt-acknowledge
bytes), `K` (code-image map entries for the generated side) and `S` steps: `i` = one instruction, interrupt response or
halted cycle (DD/FD chains completed on the oracle), `r` = a raw run of N T-states (used for the endless prefix lock),
each with the INT level and an NMI edge applied before the step. `a` (SEG-008-T008, generated side only) passes an absolute cycle deadline, used by the timing/deadline property test (`tests/z80_timing_closure_test.py`, contract in `docs/architecture/z80-scheduling-contract.md`). Each step prints one result line with the full
architectural and internal state, T-states, the ordered memory-write log and the ordered I/O log (IN, OUT and INTA).
Every run is bounded: each step passes a finite deadline and a vector has at most 64 steps.

The comparator reports the first diverging vector, step, domain and fields in the SEG-020 order: `cpu` (state), `memory`
(ordered write log), `io`, `timing`; a stopped or missing generated stream is an `outcome` divergence. It is Z80-local:
`m68k_first_divergence.py` is bound to the M68k boundary schema, so nothing was extracted and M68k behaviour is unchanged.

## Table rows: validating a family

A row of `tests/fixtures/z80-conformance-vectors/<family>.json` names a legal form id of the independent dataset:

| key | meaning |
| --- | --- |
| `form` | legal-form id; every opcode byte of the form is exercised (the dataset supplies the encodings) |
| `bytes` | optional subset of opcode bytes (hex) |
| `operands` | hex strings of the operand bytes (displacement, immediate, `nn`); default `A5..`/`5A..` |
| `prefixes` | hex DD/FD chains placed before a base-space form (only prefix-ignored bytes apply) |
| `profiles` | value profiles: `zero`, `ones`, `edge`, `mixed` (seeded, arbitrary values incl. WZ, Q, IFF, IM, R) |
| `steps` | step list (`mode`, `budget`, `int`, `nmi`, `repeat`); default one instruction step |

Memory around BC/DE/HL/IX/IY/SP is initialised deterministically. `scenarios.json` carries explicit images
(`invariant`/`banked`, windows, fill, patches), code-image `maps`, initial `state`, `memory`, `in`/`ack` scripts, steps,
generated-side `expect` assertions (outcome, T-states, any state field), `same_final_as` (a split run or an unwrapped
placement must end identically) and `oracle: false` for generated-only scenarios (typed errors, remapping).

## Oracle-absent policy (ADR 0057, Musashi convention)

Without `SEGARECOMP_Z80_ORACLE_CHECKOUT` the oracle tests print `skipped: ...` and exit 0; a wrong HEAD or dirty pinned
checkout is a hard failure. CI never fetches or runs an oracle. Oracle-derived coverage (`oracle_state`, `oracle_memory`,
`oracle_io`, `timing_validated`) comes only from the committed manifests, written by
`python3 tools/z80_conformance.py --emitter <z80_image_emitter> [--cc cc] --update-manifest`, which refuses to run
without the pinned oracle and only adds credit. A manifest entry records the digest of the form's placement-independent
expanded vectors and the applicable stages; the hermetic harness test rejects stale or unattributed credit, and the
coverage tool grants none for it, so changing a row or a value profile requires re-validation.

## Fault injection

`z80_conformance_harness_test.py` injects faults into result streams (flag, R, PC, WZ, internal state bit, memory write
value/address/order/count, I/O value/transaction, T-states) and checks the exact vector, step, domain and field.
With the oracle, `z80_conformance_oracle_test.py` builds five mutants (R, T-states, a flag, a memory write, an
interrupt-acknowledge transaction) of the emitted C or ABI header and checks that each is reported as the first divergence.

## Adding a family (T004-T007)

1. **Lowering rows**: add rows to `libs/codegen/c11/src/z80_lower_<family>.cpp` only (`data_alu`, `control_stack`,
   `cb_bit_prefix`, `ed_io_interrupt`; the four accessors are already registered in `z80_lowering.cpp` and in the CMake
   source list, so no shared file is touched). A row is `{space, mnemonic, dst, src, lower}`; `lower(LowerContext)`
   returns the C statements of the effect over `s`/`rt` (use `z80_read`, `z80_write`, `z80_io_in/out`, `z80_push16`,
   `LowerContext::start_pc`/`next_pc` for anything PC-dependent, never literals). The emitter owns the prologue
   (deadline/interrupts), the R increment (before the statements, so `LD A,R` sees it), Q (`writes_flags`), the fixed
   T-states and the fall-through; conditional/repeat forms supply `cycles_expression`; jumps/returns assign `s->pc` and use
   `Flow::dispatch`. A form claimed by two rows is a registry error.
2. **Vector rows**: add rows to `tests/fixtures/z80-conformance-vectors/<family>.json` (the dataset's `family` column
   decides the file); extra scenarios go to `scenarios.json`.
3. **Oracle credit**: run `tools/z80_conformance.py --update-manifest` locally (the pinned oracle is required); commit the
   family's manifest. Then `python3 tools/z80_capability_coverage.py ... --update-snapshot` regenerates the snapshot and
   the coverage report; the ratchet enforces that no form drops a stage.

## External corpus, secondary oracle and static budgets (SEG-008-T009)

All three are local, opt-in and never part of CI. Inputs live only in the ignored product `.tools/`.

| tool | role |
| --- | --- |
| `tools/z80_sst_corpus.py fetch` | takes a bounded HTTP-Range prefix (default 96 KiB, the leading complete cases, about 120 of the 1,000 per file) of every pinned SingleStepTests/z80 per-opcode file (revision in ADR 0057) into `<checkout>/sst-cache` (or `SEGARECOMP_Z80_SST_CACHE`). It never downloads a file in full and commits nothing. |
| `tools/z80_sst_corpus.py run --emitter E [--oracle] [--secondary]` | converts each case to the T003 vector text, places the case's own instruction bytes in a banked, window-relative code image at its real PC (per-vector code-image map), runs the generated-native runner (and redcode / kosarev on the same text) and compares with the corpus final state: registers, WZ, Q, IFF, IM, R, the EI and LD A,I markers, RAM, port transactions and T-states (= corpus cycle count). Output is aggregated per opcode file and field; only non-reconstructable aggregates are printed. |
| `tests/z80_oracle/z80_conformance_kosarev.cpp`, `tools/z80_conformance.py --secondary` | the secondary independent oracle (kosarev/z80 at the ADR 0057 pin, header-only, built with `c++ -std=c++17`). Unmodelled state (Q, LD A,I marker, prefix-run, NMI latch) is not compared; the ADR deviation mask lives in `SECONDARY_MASK` / `SECONDARY_FORM_MASK`. Anything else that differs is unexplained and fails. |
| `tools/z80_static_budget.py` | measures the ADR 0058 static-code budgets with the real lowerings on full-size synthetic images (64 KiB invariant; 512 KiB SMS-shaped banked with window-relative owners): generated C size, -j8 and -j1 compile time, per-process peak RSS, executable size, exact lookup (the bench includes the generated main TU to call its static lookup) and byte-identical re-emission. |

Classified corpus disagreement (the only one): the corpus `ei` field is the EI marker only, and it is 0 after every ED
RETN/RETI encoding, while the contract (section 4.2) and the pinned redcode oracle defer a maskable INT by one boundary when
RETI/RETN changes IFF1. The driver skips the `deferral` field for the eight ED RETN/RETI opcode files and counts the skips;
IFF, IM, PC, WZ, R and T-states of those files still compare. A test with a wrong PC on such a file must (and does) fail.

Hermetic tests: `z80_sst_corpus_test.py` (synthetic corpus, injected register/R/PC/memory/port/timing faults, the
RETN classification; the oracle part skips without the checkout) and `z80_secondary_oracle_test.py` (every form vector
against kosarev under the mask, plus a mutant check; skips without the checkout).
