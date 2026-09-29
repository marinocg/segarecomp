# ADR 0056: Z80 Architectural Contract, Encoding Scope, NMOS Variant and IM0 Static Contract

- Status: Accepted
- Date: 2026-09-29
- Task: SEG-008-T001
- Normative detail: `docs/architecture/z80-cpu-contract.md`. Dataset: `tests/fixtures/z80-legal-forms.json`
  (`docs/testing/z80-legal-forms.md`).
- Related: ADR 0002 (static translation, fail closed), ADR 0057 (oracle), ADR 0058 (static-code strategy),
  ADR 0059 (placement).

## Context

SEG-008 has to fix the complete Z80 surface before any implementation, so that T002-T010 are mechanical and
Master System (SEG-009) never discovers missing CPU forms one frontier at a time. The milestone Notes pose
questions 1-4 and 6-12. This ADR answers 1 (surface) and 2 (matrix). ADR 0057 answers 3-4, ADR 0059 answers 6-7,
and ADR 0058 answers 8-12.

## Decisions

### 1. Surface and variant

- **Variant: NMOS Zilog Z80.** The Master System and Genesis sound CPUs are NMOS Z80-compatible parts. The
  CMOS differences are observable in exactly two places: `OUT (C),0` drives 0xFF, and there is no `LD A,I/R`
  P/V quirk. They become variant parameters only when a consumer needs them; a Game Gear milestone must classify
  its ASIC core first. Board-level variant identification is ADR 0057 unresolved item 1.
- **Undocumented surface: fully in scope, zero exclusions.** Every item below is in scope, because commercial
  Master System software uses several of them and exclusions would recreate frontier-driven discovery.
  - IXH/IXL/IYH/IYL forms.
  - SLL.
  - DDCB/FDCB register copy-back and `BIT` register-field aliases.
  - The 178 ED holes as two-byte 8-T no-operations.
  - `OUT (C),0` and `IN (C)`/`IN F,(C)`.
  - Duplicated ED forms: NEG, RETN and IM aliases, and ED 63/6B.
  - DD/FD before non-HL opcodes, before ED, and in chains.

  The dataset records 261 forms (199 documented, 62 undocumented) and 1,446 encodings. The scope column is
  `in_scope` for every form.
- **Internal state in scope:** MEMPTR/WZ (for the X/Y flags of `BIT n,(HL)`), Q (for the X/Y flags of SCF/CCF),
  the EI-deferral bit, the `LD A,I/R` marker and HALT. Nothing is excluded.

### 2. Matrix

The legal-form dataset (`tools/z80_legal_forms.py`) is the SEG-008 denominator:

- **Spaces.** It covers seven finite 256-byte spaces. Every byte belongs to exactly one form or one explicit
  prefix-behaviour class.
- **Prefix chains.** DD/FD chains are specified parametrically in `prefix_rules`:
  - the last prefix is effective;
  - cost is `4(k-1) + T` T-states and `(k-1) + m1` R increments;
  - length is `k + len - 1`;
  - there is no assumed maximum length.
- **Families.** Each form has one owning family: `data_alu` T004, `control_stack` T005, `cb_bit_prefix` T006
  (which also owns the prefix rules), `ed_io_interrupt` T007.
- **Coverage stages.** The stage columns are `decodes`, `lowers`, `emits`, `compiles`, `executes`,
  `aot_admitted`, `oracle_state`, `oracle_memory`, `oracle_io`, `timing_modeled` and `timing_validated`.
- **Evidence.** The dataset is byte-reproducible, and a two-way independence test enforces its separation from
  production. The pinned oracle check covers all 1,446 encodings, the prefix-ignored bytes and the chain rules:
  1,915 exact cases with 0 mismatches, including instructions and chains that wrap across 0xFFFF.
  - It checks exact T-states, R and resulting PC.
  - Conditional forms run both outcomes; repeat forms run repeating and final iterations; control forms check the
    exact target or fall-through.
  - Built-in mutation controls must all be detected (ADR 0057).

### 3. Interrupt acceptance inside prefix chains (decided)

- Neither INT nor NMI is accepted between a DD/FD prefix and the byte that follows it, including between the
  prefixes of a chain. Acceptance happens only at the end of the effective instruction.
- Therefore a chain of any length together with its effective instruction is one indivisible instruction
  boundary.
- The evidence is UM0080's interrupt-enable section, Young §5.5 and netlist simulation. All three oracle
  finalists agree, and the redcode oracle implements it (ADR 0057 unresolved item 5 records the evidence class).
- Consequence for static code: a chain is decoded and emitted as a single owner instruction. A deadline or
  interrupt can never resume in the middle of a chain.
- An NMI edge arriving during an NMI response is discarded, not deferred (redcode behaviour, netlist-evidenced).
  T007 targets this rule with explicit vectors.

### 4. EI, NMI, HALT, IM1/IM2, RETI/RETN, LD A,I/R

These are decided as written in the contract document (§4):

- **EI and NMI.** EI defers only maskable INT, and NMI is accepted directly after EI.
- **RETI/RETN deferral.** `RETI`/`RETN` that change IFF1 (IFF1 != IFF2 before the instruction, i.e. returning
  from an NMI) defer a maskable INT by one boundary, like `EI`. Sources: Weissflog 2021 and Sainz de Baranda 2022;
  the pinned redcode oracle implements it. It is netlist/emulator-evidenced (ADR 0057, unresolved item 6).
- **HALT.** The architectural PC while halted is HALT+1. Halted cycles cost 4 T-states each and increment R by 1.
- **IM2.** The vector byte is used unmasked, the vector address wraps at 16 bits, and the table is read through
  ordinary memory reads before exact entry dispatch.
- **RETI and RETN** both restore IFF1 from IFF2.
- **`LD A,I/R`.** The NMOS P/V quirk is modeled: P/V reads 0 if INT is accepted at the next boundary.

### 5. IM0 static contract (decided: RST-only)

- **Admissible set.** The set is exactly the eight single-byte `RST p` opcodes. The acknowledge callback returns
  the data-bus byte, and a generation-time table of eight pre-generated acknowledge actions maps each admissible
  byte to its action: push PC, PC := p, 13 T-states (11 + 2 acknowledge wait states), R += 1, IFF1 := IFF2 := 0.
- **Fail closed.** Every other byte fails closed with the typed error `im0_unsupported_acknowledge_byte`. Nothing
  decodes it, and no interpreter exists.
- **Cited facts relied on (UM0080 "Interrupt Response, Mode 0" and "Interrupt Request/Acknowledge Cycle"; Young
  §5.2):**
  - The acknowledge is a special M1 cycle that asserts IORQ, with two automatic wait states.
  - In IM0 the device's byte is executed as an instruction, normally `RST p`, which takes 11 + 2 = 13 T-states.
  - Reset selects IM0.
  - An undriven bus reads 0xFF, which is `RST 38h`.
- **Rationale.** No planned consumer has an IM0 device, and the SMS/Genesis Z80 bus yields 0xFF. A `CALL nn`
  acknowledge would need a device-driven multi-byte bus protocol that has no consumer. Extending the admissible
  set needs a new ADR and a consumer.

### 6. Flags

Flag semantics per class, including DAA, CP X/Y from the operand, block-transfer, block-compare and block-I/O
flags, and repeating-iteration flags [Banks], are specified in the contract document §5. The document cites
UM0080, Young, [MEMPTR], [Rak] and [Banks].

### 7. Timing contract

- Timing is accounted per instruction in T-states from the dataset timing classes:
  - conditional forms by outcome;
  - repeat forms per iteration;
  - +4 per superseded or ignored prefix;
  - 11/13/19 for NMI, IM1/IM0-RST and IM2 responses;
  - 4 per halted cycle.
- Deadlines are checked only at instruction boundaries.
- Intra-instruction access offsets and wait states are outside the contract (SEG-008 Non-goals). A consumer
  platform may add them later as an additive ABI extension through a new ADR.
- The oracle reports T-states per instruction, which matches this granularity.

## Consequences

- T002 implements a decoder that matches the dataset partition byte for byte, and a test-side comparison over all
  7 x 256 bytes plus the chain rules proves it.
- T004-T007 claim coverage per form only through the T003 harness and the ADR 0057 oracle.
- Unresolved facts are bounded and listed in ADR 0057. None blocks T002-T003.
