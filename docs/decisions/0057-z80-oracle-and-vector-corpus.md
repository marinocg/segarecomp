# ADR 0057: Z80 Differential Oracle (redcode/Z80) and External Vector-Corpus Roles

- Status: Accepted
- Date: 2026-09-29
- Task: SEG-008-T001
- Related: ADR 0056 (Z80 architectural contract), `docs/architecture/z80-cpu-contract.md`,
  `docs/testing/m68k-conformance-harness.md` (Musashi oracle policy, followed here).

## Question

Which public Z80 core serves as the pinned, test-only, independent oracle for SEG-008? How is it obtained and run?
What role does each external vector corpus play?

## Candidates (evaluated at exact commits)

| candidate | commit | license | undocumented / internal state | interrupts / HALT | state and callbacks | integration |
| --- | --- | --- | --- | --- | --- | --- |
| **redcode/Z80** + redcode/Zeta headers | `6bb4166317108b8d1a4b5934df15761089bdea9e` + `93ba5ab967eef00f074d21bb760fe9dc48afd2d3` | LGPL-3.0-or-later | X/Y, MEMPTR, **Q** (Zilog/NEC/ST models), repeating-block flags and MEMPTR [Banks], DDCB copy-back, `OUT (C),0` (0 or 0xFF option), SLL, IXH/IXL, NMOS `LD A,I/R` quirk option | full IM0, IM1, IM2 (odd vector honoured), NMI 11 T, EI delay, RETI/RETN IFF, HALT per Brewer | all state public incl. `memptr`, `q`, `iff1/2`, `im`, `r`, `halt_line`, EI-pending; fetch/read/write/in/out (16-bit port)/inta/nmia/halted-NOP callbacks; T-states per `z80_run` | ANSI C, 7 build defines, warning-free |
| kosarev/z80 | `4c56dc514b37087751c5d9de29126a0d5c6ec731` | MIT | X/Y, most MEMPTR, DDCB, `OUT (C),0`, SLL; **no Q**, no repeating-block flags | IM0 fixed to RST 38h; IM1/IM2; `initiate_nmi` ungated | getters/setters (not Q); per-tick callbacks | header-only C++11 |
| floooh/chips `z80.h` | `9e88298ce56319953ac7a43213a1120359f7a3a6` | zlib | X/Y, MEMPTR, DDCB; **no Q**, no repeating-block flags | cycle-exact IM0/1/2/NMI; HALT keeps PC on HALT (Young model) | pin-mask per tick; EI-pending not settable | C only |
| superzazu/z80 | `d64fe10a2274e5e40019b1086bf7d8990cbc5f23` | MIT | MEMPTR, X/Y | open interrupt bugs | **8-bit port callbacks (A8-A15 lost)** | unmaintained; not a finalist |
| FUSE core | `4187f415330b66ed47d2b756dde057c19c6f6621` (z80/) | GPL-2.0-or-later | MEMPTR, Q | present | global state bound to FUSE and libspectrum; Spectrum contention | not a finalist |

None of the candidates derives from segarecomp.

### Known-issues audit (upstream trackers, READMEs, source)

- **redcode**: 5 issues, all closed. #1 was a zexall failure, since fixed. #4, #8, #10 and #16 are integration
  questions (WAIT and BUSREQ are not modelled). Documented limitations: the core is instruction-granular, and
  `z80_execute` ignores interrupts, so the adapter steps with `z80_run`.
- **kosarev**:
  - Open issues: #3 (IM0 is RST 38h only), #25/#42 (no SCF/CCF Q), #43 (fails Rak's z80test), #35 (NMOS/CMOS
    differences incomplete), #51 (SCF/CCF race in its own netlist simulation), #12 (callback timing), #71/#72
    (disassembler prefix display). #40 (HALT) is open, but master fetches HALT+1.
  - Closed issues: #9 (NMI), #28 (RETI), #23 (ED under DD/FD), #22 (DDCB), #8 (OUTx B timing).
  - **New finding (T001)**: INI/INIR MEMPTR uses the decremented BC, but [MEMPTR] specifies BC before the decrement
    plus 1.
- **floooh**: open #98 (fails Rak's suite and the block-flags test) and #76 (WAIT sampled before writes). Closed #121,
  #64, #63, #61, #10 and #30.
- **superzazu**: open #8 (redundant DD/FD prefixes, ZEXALL failure), #10 (IM0 double-counts R and cycles), #11 (NMI
  after EI wrongly delayed) and #9 (word-read order). Closed #4 (OTIR Z).
- **FUSE**: evaluated from its test README and source only (MEMPTR, SCF bits 3/5, BIT n,(HL), DD/FD timing cases).

### Independent adversarial smoke checks (authored from UM0080, Young, [MEMPTR], [Banks], Brewer; not from any core's suite)

64 of 69 check-by-core cells passed on the three finalists. Two runs produced byte-identical output.

| check | kosarev | redcode | floooh |
| --- | --- | --- | --- |
| HALT + INT (IM1): 4 T per halted cycle, R +1 per cycle, 13 T exit, pushes HALT+1, IFF cleared, WZ = 0x38 | pass | pass | pass (PC stays on HALT while halted) |
| HALT + NMI: 11 T, PC 0x66, pushes HALT+1, IFF2 kept | pass | pass | pass |
| EI delay, EI chains, EI;HALT | pass | pass | pass |
| NMI directly after EI accepted | pass | pass | pass |
| IM2: even vector, odd vector, 0xFFFF wrap; ordinary reads; 19 T | pass | pass | pass |
| IM0 with RST 28h on the bus: 13 T | **fail** (#3) | pass | pass |
| NMI then RETN restores IFF1 from IFF2 | pass | pass | pass |
| DD/FD chains: last prefix wins, 4 T and R +1 per prefix | pass | pass | pass |
| INT/NMI raised mid-chain taken only after the prefixed instruction | pass (NMI gating in adapter) | pass | pass |
| stray `DD 00` = 8 T, R +2 | pass | pass | pass |
| X/Y on ADD/CP (CP from operand) | pass | pass | pass |
| `BIT 0,(HL)` X/Y from MEMPTR high byte | pass | pass | pass |
| SCF/CCF X/Y with Q (Zilog NMOS) | **fail** (no Q) | pass | **fail** (no Q) |
| INI/OUTI flags, port B-before/after-decrement, MEMPTR; INIR/LDIR repeating-step X/Y, P/V, MEMPTR | **fail** (INI WZ, repeat flags) | pass | **fail** (repeat flags) |
| DDCB `RLC (IX+5),B` copy-back: 23 T, WZ = IX+d, R +2 | pass | pass | pass |
| `OUT (C),0` writes 0x00; WZ = BC+1 | pass | pass | pass |
| `LD A,I` / `LD A,R` P/V = IFF2; R read after its own increments | pass | pass | pass |
| SLL; `LD A,IXH`; R 7-bit wrap keeps bit 7; MEMPTR of LD A,(nn)/LD (nn),A/JP/ADD HL | pass | pass | pass |

How the disagreements resolve:

- **kosarev IM0 and INI MEMPTR**: core deviations.
- **SCF/CCF (kosarev, floooh)**: these cores do not implement the Zilog NMOS Q rule measured by Rak and Banks.
- **Repeating-block flags (kosarev, floooh)**: these cores disagree with [Banks] (hardware-corroborated).
- **HALT PC (floooh)**: the references conflict. This ADR follows Brewer (2014), Woody's HALT2INT (2021) and
  netlist simulation over Young §5.4. The pushed address (HALT+1) is identical in both models.
- **Young §4.4 vs UM0080 on the INI/OUTI port timing of the B decrement**: resolved for UM0080, which [MEMPTR]
  corroborates.

## Decision

1. **Primary oracle: redcode/Z80 `6bb4166317108b8d1a4b5934df15761089bdea9e`** with redcode/Zeta
   `93ba5ab967eef00f074d21bb760fe9dc48afd2d3`, LGPL-3.0-or-later.
   - Build defines: `Z80_STATIC Z80_WITH_EXECUTE Z80_WITH_Q Z80_WITH_FULL_IM0 Z80_WITH_SPECIAL_RESET
     Z80_WITH_UNOFFICIAL_RETI Z80_WITH_ZILOG_NMOS_LD_A_IR_BUG`; runtime model `Z80_MODEL_ZILOG_NMOS`.
   - The adapter steps with `z80_run(cpu, 1)` and repeats while `resume == Z80_RESUME_XY`, so each step covers one
     architectural instruction including its whole prefix chain.
   - It is the only finalist that passed every reference-derived check. That includes the NMOS Q and
     repeating-block flags, which the ADR 0056 contract puts in scope.
2. **Secondary independent check: kosarev/z80 `4c56dc514b37087751c5d9de29126a0d5c6ec731`, MIT.**
   - SingleStepTests/z80 is validated against redcode, so it is not independent evidence for redcode. kosarev
     supplies structurally independent agreement for the documented and common undocumented surface.
   - It runs with a committed deviation mask: IM0 other than RST 38h, SCF/CCF X/Y, INI/INIR/IND/INDR MEMPTR, and
     repeating-step LDxR/CPxR/INxR/OTxR flags and MEMPTR. The adapter gates NMI between prefix and opcode.
   - T003 wires it; T001 only records the decision.
3. **Acquisition.** Plain Git checkouts at the pinned hashes live in the ignored product `.tools/z80-oracles/`
   (`redcode_Z80/`, `redcode_Zeta/`, `kosarev_z80/`), exactly like `.tools/musashi`. Nothing is vendored or
   committed. The product holds only its own adapter sources and the pin hashes. The LGPL core is compiled locally
   into a test binary that is never distributed and never linked into production.
   - `SEGARECOMP_Z80_ORACLE_CHECKOUT` names the directory that holds the clones.
   - If it is unset or missing, the test prints `skipped:` and exits 0.
   - A wrong HEAD or a dirty checkout is a hard failure.
4. **CI.** The M68K oracle policy applies. CI runs hermetic tests only and never fetches or runs an oracle.
   Oracle-validated coverage rows come from a committed validation manifest. It is updated locally by
   `--update-manifest`, which refuses to run without the pinned oracle and only adds credit. A hermetic test
   checks the manifest's attribution consistency. T003 builds the manifest and harness.
5. **T001 evidence in the test tree** (both skip without the checkout):
   - `tests/z80_oracle_adapter_smoke_test.py` and `tests/z80_oracle/adapter_smoke.c` exercise the adapter shape.
     They set the full state, run one instruction or interrupt response, observe memory/I/O/INTA callbacks, and read
     the full state back (MEMPTR, Q, IFF, IM, R, HALT, EI-pending, T-states). Cases: INI, OUTI, IM2 (odd vector),
     HALT, NMI.
   - `tests/z80_legal_forms_oracle_crosscheck_test.py` and `tests/z80_oracle/legal_forms_crosscheck.c` falsify
     the independent dataset (ADR 0056) against the oracle, with exact expectations. For every one of the 1,446
     form encodings they check the exact T-states, the exact R increment (M1 fetches) and the exact resulting PC:
     the length for straight-line forms, and the computed target or fall-through for control forms.
     - Conditional forms (JR cc, JP cc, CALL cc, RET cc, DJNZ) run with the condition forced true and false, and
       each asserts its taken or not-taken T-states and PC.
     - Repeat forms run a repeating iteration (PC unchanged, repeating T-states) and a final iteration.
     - The DD/FD `prefix_ignored` bytes are checked as 4 + base form.
     - Parametric chains (k = 1..8, repeated and alternating) are checked against `4(k-1) + T`, `(k-1) + m1` and
       `k + len - 1`, plus a prefix before ED and a chain before DDCB.
     - Result: **1,909 cases, 0 mismatches**.
     - Built-in mutation controls on in-memory corrupted copies must all be detected, and were: taken/not-taken
       swapped (126 mismatches), repeating T-states changed (8), control-form lengths changed (105), and one M1
       count changed (1).

## Vector corpora: falsification inputs only, never the specification

The specification remains UM0080, Young, [MEMPTR], [Rak] and [Banks], with conflicts resolved as recorded above.

| corpus | license / pin | content | role |
| --- | --- | --- | --- |
| SingleStepTests/z80 | MIT, `ebe1875d48f374bcfd4b505d8eb8ee751568b5f7` | 1,604 files x 1,000 single-instruction cases, about 1.37 GB. Per case: all registers, `wz`, `q`, `p` (LD A,I/R marker), `ei`, IFF, IM, RAM, per-cycle bus samples, ports. No HALT flag, no INT/NMI cases. NMOS. Generated by JSMoo (an Ares translation); final states CI-validated against redcode; bus `cycles` unvalidated (its #3) | sampled local falsification of generated-native execution (T003+) and of kosarev. Never committed or bulk-fetched; pinned per-file fetch into ignored `.tools/`. Not evidence for redcode; per-cycle bus data unused. T001 sanity run: 18 files, 18,000 cases, redcode 0 mismatches |
| zexdoc / zexall | GPL-2.0-or-later | CP/M CRC suites over documented / all flags; no MEMPTR, Q, interrupts, HALT or 16-bit I/O | optional local end-to-end smoke behind a CP/M shim; never committed, never in CI |
| FUSE tests.in / tests.expected | GPL-2.0-or-later | about 1,356 cases with MEMPTR, halted state, timed memory/port events (Spectrum contention to filter) | optional local check of HALT, multi-instruction timing and MEMPTR; not vendored |
| Rak z80test | MIT | Spectrum test programs | cited measurement basis for Q/MEMPTR only |

## Unresolved (bounded; no claim until resolved)

1. **Z80 variant on specific consoles (Genesis board revisions, Game Gear ASIC).** This decides the SCF/CCF X/Y
   flavour, the `OUT (C),0` value and the NMOS `LD A,I/R` quirk. SEG-008 models Zilog NMOS. Platform milestones must
   classify their part before claiming these three behaviours.
2. **SCF/CCF X/Y after an instruction that leaves the flags unchanged.** [Rak]/[Banks] measure the Zilog Q rule,
   while kosarev's netlist reports a race. The contract follows Rak/Banks and the oracle, but T004 must not claim the
   ambiguous sub-case beyond oracle agreement. The kosarev comparison is masked there.
3. **The data-bus byte during interrupt acknowledge on specific consoles** (platform fact; matters for IM0/IM2 only).
4. **MEMPTR on non-final INxR/OTxR iterations (PC+1).** The evidence is netlist-based, not a hardware measurement.
   T007 validates against the oracle and labels it netlist-evidenced.
5. **NMI held off across DD/FD prefixes, and an NMI edge during an NMI response discarded (not deferred).** The
   evidence is netlist and forum-based (Young §5.5 says the NMI-prefix case was not tested). The three finalists agree
   on the prefix case. The second-NMI rule was not in the T001 smoke matrix; the pinned oracle implements
   discard. ADR 0056 adopts both; T007 targets them with explicit vectors.
6. **Maskable INT deferred after `RETI`/`RETN` that change IFF1** (Weissflog 2021; Sainz de Baranda 2022). The
   evidence is netlist and emulator research, implemented by the pinned oracle. Not yet cross-checked against
   kosarev. ADR 0056 adopts it; T007 targets it.
