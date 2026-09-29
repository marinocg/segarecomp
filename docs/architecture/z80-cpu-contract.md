# Z80 CPU Architectural-State and Semantics Contract (SEG-008)

Status: normative for SEG-008 T002-T010. Decided by SEG-008-T001 (ADR 0056 state/scope/IM0; ADR 0057 oracle
and corpus; ADR 0058 static-code strategy, code images and outcomes; ADR 0059 placement). Later tasks
implement this contract. They must not redesign it; changing it requires a new ADR.

## References (public)

- **[UM0080]** Zilog, *Z80 CPU User Manual*, UM0080 (rev. 11): architecture, interrupt response, instruction
  set, M-cycle/T-state tables.
- **[Young]** Sean Young, *The Undocumented Z80 Documented*, v0.91 (2005): undocumented opcodes, flags X/Y,
  DAA table, block-instruction flags, prefix behaviour, interrupt/HALT details, R register, reset state.
- **[MEMPTR]** boo_boo and Vladimir Kladov, *memptr_eng.txt* (2006): the internal WZ/MEMPTR register and its
  effect on `BIT n,(HL)` flags.
- **[Rak]** Patrik Rak, *SCF/CCF flag behaviour* research and "Z80 XCF flavor" test (2012-2018): the internal Q
  register and X/Y of SCF/CCF.
- **[Banks]** David Banks (hoglet), *Z80 block-instruction flag behaviour when interrupted* (2018, published
  via the Visual Z80 / anycpu forums and the stardot thread): X/Y/H/P/V of repeating LDxR/CPxR/INxR/OTxR
  iterations.
- **[Dinu]** Cristian Dinu, *Decoding Z80 Opcodes*: x/y/z/p/q field decomposition.
- **[redcode-doc]** redcode/Z80 README at the pinned oracle commit (ADR 0057): documents its NMOS/CMOS model
  options. It is a secondary citation, used only where the primary references above are silent.

Facts marked **UNRESOLVED** below are bounded and are never guessed. ADR 0057 "Unresolved" lists them. The owning task must settle each one
against the pinned oracle (ADR 0057) and targeted vectors before claiming the dependent coverage stage.

## 1. Variant

One model: **NMOS Zilog Z80** (the Master System and Genesis sound CPU). The CMOS Z80 differs architecturally
from NMOS in only two observable places:

1. `OUT (C),0` (ED 71): NMOS drives 0x00 [Young §3.4, §8.11]; CMOS drives 0xFF [redcode-doc].
2. The `LD A,I`/`LD A,R` P/V interrupt-acceptance quirk (§4.6) is NMOS-only.

These are the only **variant parameters**. They are added only when a consumer requires them (a Game Gear
milestone, whose ASIC-integrated core must be classified first). No CMOS-only instruction behaviour exists.

## 2. Architectural state

| state | width | notes |
| --- | --- | --- |
| A, F, B, C, D, E, H, L | 8 each | main set; pairs AF/BC/DE/HL |
| A', F', B', C', D', E', H', L' | 8 each | alternate set; `EX AF,AF'`, `EXX` |
| IX, IY | 16 each | halves IXH/IXL/IYH/IYL addressable (undocumented, in scope) |
| SP, PC | 16 each | |
| I | 8 | interrupt vector base (IM2) |
| R | 8 | refresh: low 7 bits += 1 per M1 fetch; bit 7 only changed by `LD R,A` [UM0080; Young] |
| IFF1, IFF2 | 1 each | interrupt enable flip-flops |
| IM | 0/1/2 | interrupt mode |
| HALT | 1 | halted flag (§4.4) |
| INT-deferral | 1 | maskable INT not accepted at this boundary: set by `EI`, or by `RETI`/`RETN` that change IFF1 (§4.2) |
| prefix-pending | — | never architecturally visible between instructions: a prefix and its opcode form one indivisible instruction boundary in this contract (§3.2) |
| MEMPTR (WZ) | 16 | internal; in scope because `BIT n,(HL)` X/Y observe it [MEMPTR] |
| Q | 8 | internal; F value written by the last instruction if it changed flags, else 0; in scope because SCF/CCF X/Y observe it [Rak] |
| LD A,I/R marker | 1 | NMOS quirk marker (§4.6) |

Reset state [Young §2.4; UM0080]: PC = 0, I = 0, R = 0, IFF1 = IFF2 = 0, IM = 0, HALT = 0, INT-deferral = 0.
AF = SP = 0xFFFF. All other registers (BC, DE, HL, alternates, IX, IY, MEMPTR, Q) are undefined on hardware.
This contract **defines** them as 0xFFFF (Q = 0, MEMPTR = 0xFFFF) so generated execution is deterministic. The
platform may override any initial value through the runtime ABI.

## 3. Instruction encoding, prefixes and R

### 3.1 Encoding surface

The complete encoding surface is `tests/fixtures/z80-legal-forms.json` (`docs/testing/z80-legal-forms.md`). It
has seven finite 256-byte spaces and parametric DD/FD prefix rules. Every byte is a form or a prefix-behaviour
class, and the scope excludes nothing (ADR 0056).

### 3.2 DD/FD prefix chains

- In a sequence `P1..Pk b` (Pi in {DD, FD}, b not DD/FD), `Pk` is effective. `P1..P(k-1)` are superseded;
  each costs 4 T-states and one M1 fetch (R += 1) [Young §3.7, §6.1].
- `P b` with `b` not using HL/H/L/(HL) (dataset class `prefix_ignored`): the prefix is a 4-T-state, one-M1
  no-operation, and `b` executes as its base form. `P ED`: the prefix is ignored and ED escapes.
- `P CB d op`: `d` and `op` are ordinary memory reads, not M1 fetches (R += 2 for the whole instruction).
- **Instruction boundary for chains.** See §4.7 for interrupt acceptance inside a chain.
- **Decode bound.** A chain has no assumed maximum length. Decode is bounded only by the immutable code image:
  a chain or instruction that runs past the image edge is a typed fail-closed outcome (ADR 0058).

### 3.3 R register

R low 7 bits increment once per M1 fetch: 1 for unprefixed; 2 for CB/ED/DD/FD (and DDCB/FDCB, §3.2); +1 per
superseded or ignored prefix; +2 per repeated block-instruction iteration; +1 per halted NOP cycle (§4.4);
+1 per interrupt acknowledge (INT any mode, NMI). Bit 7 is preserved except by `LD R,A` [Young §6.1; UM0080].
`LD A,R` observes the value after the increments of its own two M1 fetches.

## 4. Interrupts, HALT and EI

### 4.1 Acceptance point

Maskable INT (level) and NMI (edge, latched) are sampled only at instruction boundaries. A boundary here is the
end of a complete instruction, including all its prefixes, or the end of one repeated block-instruction iteration.
NMI has priority over INT.

### 4.2 EI and DI

`EI` sets IFF1 = IFF2 = 1. A maskable interrupt is not accepted at the boundary immediately after `EI`, so the
next instruction always executes first. Consecutive `EI`s keep deferring. `DI` clears IFF1 and IFF2 immediately
(no INT at the boundary after DI) [UM0080; Young §5.3, §5.5]. NMI acceptance is not blocked by the EI deferral (§4.7).

**RETI/RETN deferral.** A maskable INT is also not accepted at the boundary immediately after `RETI` or `RETN`
(any ED RETN/RETI encoding) when IFF1 and IFF2 differed before that instruction, i.e. when the instruction changed
IFF1. This happens only when returning from an NMI handler entered with interrupts enabled. It is relevant because
a Master System pause-button NMI can return while the VDP INT is pending [Weissflog, "A New Cycle-Stepped Z80
Emulator" (2021-12-17); Sainz de Baranda, spectrumcomputing.co.uk t=7086 and stardot t=24662 (2022); implemented by
the pinned redcode oracle]. The evidence is netlist and emulator research, not a Zilog document (ADR 0057,
unresolved item 6). The EI-deferral state therefore generalises to "maskable INT deferred at this boundary": it is
set by `EI`, and by `RETI`/`RETN` when they change IFF1.

### 4.3 Modes

| event | action | T-states | R |
| --- | --- | --- | --- |
| NMI | IFF1 := 0, IFF2 unchanged [Young §5.3 hardware test]; push PC; PC := 0x0066 | 11 | +1 |
| INT, IM1 | IFF1 := IFF2 := 0; push PC; PC := 0x0038 | 13 | +1 |
| INT, IM2 | IFF1 := IFF2 := 0; push PC; vector address V := (I << 8) \| data-bus byte; PC := mem[V] \| mem[(V+1) & 0xFFFF] << 8 | 19 | +1 |
| INT, IM0 | static contract (§4.5) | 13 for RST p | +1 |

- IM2 uses the device byte as supplied: the full byte, including odd values, with no LSB masking. UM0080 asks
  devices to supply an even byte, but NMOS hardware does not force bit 0 [Young §5.2; netlist evidence per ADR 0057]. The two handler bytes are
  read through ordinary CPU memory-read callbacks, and `V+1` wraps at 0xFFFF. The resulting handler PC goes
  through exact generated-entry dispatch (ADR 0058). The table read is a memory read, not an entry lookup.
- `RETN`: IFF1 := IFF2; pop PC. `RETI`: IFF1 := IFF2 as well [Young §5.3]; pop PC. Only the opcode differs,
  which a daisy-chain device observes, and daisy chains are platform policy.
- Interrupt acknowledge pushes the address of the next instruction. For HALT this is HALT+1 (§4.4).

### 4.4 HALT

`HALT` sets HALT = 1 and the CPU then executes internal NOP cycles: 4 T-states and one M1 (R += 1) each.
PC has already advanced past HALT, and the halted M1 cycles read HALT+1 without incrementing PC [UM0080 "HALT Exit";
Tony Brewer, "Z80 Special Reset" (2014); Woody's HALT2INT test (2021). These supersede Young §5.4's
"re-executes HALT", and the software-visible result is identical]. **Architectural PC while halted = the
address after HALT** (the resume address). An accepted INT or NMI clears HALT and pushes that address. Generated
code does not spin: HALT returns the resumable `halted` outcome (ADR 0058) and the runtime accounts halted cycles
up to the deadline. A core that represents PC as pointing at the HALT opcode while halted is normalised to this
contract by the oracle adapter (ADR 0057).

### 4.5 IM0 static contract (decided: RST-only)

In IM0 the interrupting device places an instruction on the data bus during the acknowledge cycle, and the CPU
executes it [UM0080 "Interrupt Response", Mode 0]. The acknowledge cycle is an M1 with two added wait states;
for a single-byte `RST p` the whole response takes 13 T-states (11 for RST + 2 wait) [UM0080 "Interrupt Response"; Young §5.2].
Multi-byte instructions (for example `CALL nn`) read their operand bytes through further bus cycles that the
device must drive.

**Decision:** segarecomp forbids runtime opcode decoding, so the IM0 admissible set is **RST-only**.

- The acknowledge callback returns the data-bus byte. If it is one of the eight `RST p` opcodes (C7, CF, D7,
  DF, E7, EF, F7, FF), the generated runtime performs the pre-generated acknowledge action: push PC; PC := p;
  13 T-states; R += 1; IFF1 := IFF2 := 0. This is a fixed eight-entry table built at generation time, not a
  decoder.
- Any other byte is a typed fail-closed error (`im0_unsupported_acknowledge_byte`). There is no interpreter, and
  the byte is never decoded as an instruction.
- **Rationale.** The only known consumers put `RST 38h` (0xFF, floating bus) on the bus: the Master System and
  Genesis Z80 have no IM0 device. `CALL nn` would need a device-driven multi-byte bus protocol with no consumer.
  It can be added as a platform-declared extension only by a later ADR that has a consumer.

### 4.6 `LD A,I` / `LD A,R` P/V (NMOS quirk, decided: modeled)

P/V := IFF2. On NMOS, if a maskable interrupt is accepted at the boundary immediately after `LD A,I` or
`LD A,R`, the P/V flag that was set is instead read as 0 [redcode-doc, option `Z80_WITH_ZILOG_NMOS_LD_A_IR_BUG`,
which documents this Zilog NMOS behaviour; not covered by Young or UM0080]. Acceptance occurs only
at instruction boundaries in this contract, so the quirk is deterministic. The LD A,I/R marker is set by these two
instructions and cleared by every other instruction. If an INT is accepted while the marker is set, the P/V flag in
the live F register (the value `LD A,I/R` produced) is cleared to 0 before the handler runs. The NMOS default models it; the CMOS variant parameter disables it.

### 4.7 Prefix chains, EI and interrupt acceptance

Neither INT nor NMI is accepted between a DD/FD prefix and the following byte. That includes between the prefixes of a
chain, so a chain of any length plus its effective instruction forms one indivisible instruction boundary. For INT:
[Young §5.5], "interrupts are only accepted between instructions ... also true for prefixed instructions". For NMI:
netlist evidence and the pinned oracle; Young §5.5 states the NMI case was not tested (ADR 0057, unresolved item 5).
All oracle finalists agree (ADR 0056 §3). EI defers only maskable INT, so an NMI is accepted directly after `EI`
[Banks, "NMI during EI"]. An NMI edge that arrives during an NMI response is discarded, not deferred: at least one
instruction executes between two NMI responses. This is netlist-evidenced and implemented by the pinned oracle; it is
not yet covered by a T001 smoke case (ADR 0057, unresolved item 5; T007 targets it).

## 5. Flags

Bits: S(7) Z(6) Y(5) H(4) X(3) P/V(2) N(1) C(0).

| class | S Z | Y X | H | P/V | N | C |
| --- | --- | --- | --- | --- | --- | --- |
| 8-bit ADD/ADC | result | result bits 5/3 | carry from bit 3 | overflow | 0 | carry |
| 8-bit SUB/SBC/NEG | result | result bits 5/3 | borrow from bit 4 | overflow | 1 | borrow |
| CP | result | **operand** bits 5/3 | borrow | overflow | 1 | borrow |
| AND / OR / XOR | result | result | 1 / 0 / 0 | parity | 0 | 0 |
| INC / DEC r | result | result | carry/borrow from bit 3/4 | overflow (0x7F->0x80 / 0x80->0x7F) | 0 / 1 | unchanged |
| 16-bit ADD HL/IX/IY | unchanged | high byte of result | carry from bit 11 | unchanged | 0 | carry |
| ADC/SBC HL | 16-bit result | high byte | from bit 11 | overflow | 0 / 1 | carry |
| RLCA/RRCA/RLA/RRA | unchanged | A bits 5/3 | 0 | unchanged | 0 | shifted-out bit |
| CB rotates/shifts | result | result | 0 | parity | 0 | shifted-out bit |
| BIT n,r | Z = !bit; S = (n==7 && bit) | bits 5/3 of **r** | 1 | = Z | 0 | unchanged |
| BIT n,(HL) | as above | bits 5/3 of **MEMPTR high byte** [MEMPTR] | 1 | = Z | 0 | unchanged |
| BIT n,(IX/IY+d) | as above | bits 5/3 of the high byte of IX/IY+d | 1 | = Z | 0 | unchanged |
| DAA | result | result | per [Young §4.7] table | parity | unchanged | per table |
| CPL | unchanged | A bits 5/3 | 1 | unchanged | 1 | unchanged |
| SCF / CCF | unchanged | ((Q XOR F) OR A) bits 5/3 [Rak] | 0 / old C | unchanged | 0 | 1 / !C |
| RLD / RRD | A | A | 0 | parity(A) | 0 | unchanged |
| LD A,I / LD A,R | A | A | 0 | IFF2 (§4.6) | 0 | unchanged |
| IN r,(C) / IN (C) | input | input | 0 | parity | 0 | unchanged |
| LDI/LDD (and each LDxR step) | unchanged | n = A + value: Y = bit 1 of n, X = bit 3 of n | 0 | BC != 0 | 0 | unchanged |
| CPI/CPD (and each CPxR step) | A - value | n = A - value - H: Y = bit 1, X = bit 3 | borrow | BC != 0 | 1 | unchanged |
| INI/IND/OUTI/OUTD (and repeats) | B after decrement | B after decrement | k > 255 | parity((k & 7) XOR B) | bit 7 of value | k > 255 |

Block I/O: `k = value + ((C + 1) & 0xFF)` (INI), `value + ((C - 1) & 0xFF)` (IND), `value + L` (OUTI/OUTD, L
after the HL update) [Young §4.3]. **Repeating iterations** (a block-repeat instruction interrupted, or observed
between iterations): X/Y come from the high byte of the PC of the repeated instruction, and H and P/V are further
adjusted for INxR/OTxR [Banks]. In scope. T007 validates them where the pinned oracle implements them, and
otherwise with targeted reference-derived vectors (ADR 0057).

MEMPTR update rules follow [MEMPTR] exactly (for example `LD A,(nn)`: MEMPTR = nn + 1; `JP nn`/`CALL nn`:
MEMPTR = nn; `ADD HL,rr`: MEMPTR = HL + 1 before the add; `LD A,(BC)`: BC + 1; `LD (BC),A`: (A << 8) |
((BC + 1) & 0xFF); block and I/O forms per the note). Q: after any flag-writing instruction Q := F; after any
other instruction Q := 0.

## 6. I/O

`IN`/`OUT` use a 16-bit port address. `IN A,(n)` / `OUT (n),A`: port = (A << 8) | n. `(C)` forms: port = BC.
Block I/O: port = BC. INI/IND/INIR/INDR drive the port with B *before* its decrement. OUTI/OUTD/OTIR/OTDR
decrement B first and drive the port with the *decremented* B [UM0080 INI/OUTI pages; [MEMPTR]; Young §4.4 states the opposite, resolved for UM0080 in ADR 0057]. The I/O callback receives the 16-bit port,
the direction and the value. Port decoding is platform policy.

## 7. Timing contract

- **Granularity: instruction-level T-state accounting.** Each instruction adds its T-states from the dataset
  timing class (conditional by outcome, repeat per iteration, +4 per superseded/ignored prefix). Interrupt
  responses add 11/13/19, and halted cycles add 4 each.
- Deadlines are checked at instruction boundaries (repeat iterations included). An instruction never splits
  across a deadline.
- Memory and I/O callbacks receive the cycle count at the start of the current instruction. **Intra-instruction
  access offsets** (which T-state of the instruction performs each access) and **wait states** are outside the
  T-state contract. SEG-008 Non-goals exclude bus-cycle emulation; a platform milestone with a demonstrated
  consumer (for example SEG-032 bank-window waits) must add them through a new ADR as an additive ABI extension.

## 8. Static execution, code images and outcomes

Defined in ADR 0058: broad immutable-image AOT, owners keyed by (code-image identity, 16-bit address), exact
dispatch for runtime-selected targets, and the resumable-versus-fail-closed outcome split.
