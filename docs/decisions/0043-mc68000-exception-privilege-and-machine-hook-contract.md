# ADR 0043: MC68000 Exception, Privilege and Machine-Hook Contract

- Status: Accepted
- Date: 2026-09-24
- Task: SEG-021-T017 (contract only; implementation is SEG-021-T018, T019, T020; exception
  timing is SEG-021-T022)
- Extends, without editing their files (the numbered-ADR extend-don't-edit precedent of ADR 0017,
  ADR 0020 and ADR 0037): ADR 0020 §7 (supervisor-mode-only interrupt entry), §8 (frame
  validate-then-commit) and §9 (RTE), and ADR 0037 Decisions A-C (the shared frame primitive and
  vector 5). Their frame shape and memory-safety ordering are kept as-is; this ADR widens who may
  enter an exception, moves ownership of the primitive out of the Genesis platform, and fixes the
  disposition of every MC68000 exception.
- Supersedes, when SEG-021-T018 lands: the unconditional "no privilege check" rules in
  `docs/architecture/genesis-move-an-usp-startup-compatibility-policy.md` (MOVE An,USP, MOVE to SR,
  MOVE from SR, MOVE to CCR). Until then those policies stay in force unchanged.

## Context

The generated-native backend already delivers two exceptions: the Genesis VBlank level-6
autovector (ADR 0020) and the synchronous zero-divide trap, vector 5 (ADR 0037). Both go through one
static frame-construction helper and one RTE routine in `platforms/genesis/runtime/runtime.c`. The
matching C11 lowering for RTE is in `libs/codegen/c11/src/genesis_m68k_runtime.cpp`.

Audit of product `main` at the start of this task:

| aspect | current state |
| --- | --- |
| frame shape | six bytes: saved SR word at the new SP, saved PC long word at SP+2, no format/vector-offset word; the complete extent is validated before the first write (ADR 0020 §7/§8) |
| entry privilege | entry fails closed if S = 0; there is only one stack pointer (`a[7]`); `usp` is an inert field used by MOVE An,USP |
| supervisor/user | the SR S bit is stored but not enforced; no SSP/USP swap; no privilege-violation delivery |
| vectors delivered | level-6 autovector (vector 30) and zero divide (vector 5), both resolved at build time from the cartridge vector table at address 0 |
| RTE | reads SR at SP and PC at SP+2, commits `{sr, pc, a[7] += 6}` atomically; Genesis-only IRQ6 grace bookkeeping keyed by out-of-band frame origin |
| RTR, TRAP, TRAPV, CHK, ILLEGAL, line-A/F, STOP, RESET | rejected by decode (STOP/RESET are length-classified and then rejected); no delivery path |
| Group 0, trace, spurious/uninitialized interrupt | not modeled; misaligned or unmapped accesses end in explicit runtime stops |

In the independent legal-form baseline (`tests/fixtures/m68k-legal-forms.json`, 1526 forms), 852
forms list at least one exception or privilege class. 19 of them are privileged, and 835 list address
error (vector 3). In the coverage snapshot (`tests/fixtures/m68k-capability-coverage.json`),
4 of those 852 pass the `exception_privilege_modeled` row.

## Decision

### 1. Target and scope

The contract describes the original MC68000 only: the 16-bit external bus, 24-bit addresses, and the
Group 0/1/2 exception model of the MC68000 User's Manual (citations U1-U4, P1-P2). It excludes
MC68010-and-later behavior: no format/vector-offset stack word, no VBR, no MOVEC, no MOVE-from-SR
privilege, no instruction continuation, and no RTE format dispatch. An MC68010-or-later encoding is
still architecturally illegal (vector 4) under the legal-form baseline.

### 2. Group 1/2 exception frame (the only frame this project builds)

Every Group 1 and Group 2 exception, and every interrupt, leaves the same final frame on the
supervisor stack. The contract fixes this layout. It does not fix the order of the bus writes: any
order is conforming as long as the final memory image and the fail-closed rule in §5 hold.

```m68k-group12-frame
offset=0 size=2 field=saved_sr
offset=2 size=4 field=saved_pc
total=6
```

- Offsets are relative to the supervisor stack pointer after entry: saved SR at `SSP`, saved PC at
  `SSP+2`. Values are big-endian.
- `saved_sr` is the complete SR as it was before entry changed S, T or the interrupt mask.
- `saved_pc` is the full 32-bit PC value from the table in §3. The MC68000 drives only 24 address
  bits, but the stacked value is the full long word the CPU holds.
- There is no seventh or eighth byte, no format nibble, and no vector-offset word. The test
  `tests/m68k_exception_contract_doc_test.py` checks this block and the runtime implementation that
  must match it.

### 3. Exception table: vector, group, stacked PC and disposition

"Next" is the address of the instruction after the complete current instruction, including its
extension words. "Current" is the address of the first word of the instruction that raised the
exception. The group and priority columns come from U2. The stacked-PC column comes from U3 and P2.

| vector (offset) | exception | group | stacked PC | disposition | owner task |
| --- | --- | --- | --- | --- | --- |
| 0-1 (`$000`/`$004`) | reset: initial SSP and PC | 0 | none (no frame; SSP <- vector 0, PC <- vector 1, SR <- S=1, T=0, I=7) | implemented at build time (existing startup resolution) | existing |
| 2 (`$008`) | bus error | 0 | 14-byte Group 0 frame (§4) | **deferred, fail closed** (§4) | none |
| 3 (`$00C`) | address error | 0 | 14-byte Group 0 frame (§4) | **deferred, fail closed** (§4) | none |
| 4 (`$010`) | illegal instruction (including `ILLEGAL` `0x4AFC` and every word classified architecturally illegal) | 1 | current | implement | T019 |
| 5 (`$014`) | zero divide (DIVS/DIVU) | 2 | next | implemented (ADR 0037); migrates to the shared core | T018 |
| 6 (`$018`) | CHK | 2 | next | implement | T019 |
| 7 (`$01C`) | TRAPV (V set) | 2 | next | implement | T019 |
| 8 (`$020`) | privilege violation | 1 | current | implement | T018 |
| 9 (`$024`) | trace | 1 | next | **deferred, fail closed** (§6) | T018 (guard only) |
| 10 (`$028`) | line 1010 emulator (line-A) | 1 | current | implement | T019 |
| 11 (`$02C`) | line 1111 emulator (line-F) | 1 | current | implement | T019 |
| 15 (`$03C`) | uninitialized interrupt vector | 1 | next (instruction boundary) | machine-selected vector (§7); not produced by Genesis | T020 |
| 24 (`$060`) | spurious interrupt | 1 | next (instruction boundary) | machine-selected vector (§7); not produced by Genesis | T020 |
| 25-31 (`$064`-`$07C`) | level 1-7 interrupt autovectors | 1 | next (instruction boundary); after a STOP wake, the instruction after STOP | implement acceptance; level 6 exists (ADR 0020) | T020 |
| 32-47 (`$080`-`$0BC`) | TRAP #0-#15 | 2 | next | implement | T019 |
| 64-255 (`$100`-`$3FC`) | user (non-autovector) interrupt vectors | 1 | next (instruction boundary) | machine-selected vector (§7) | T020 |
| 12-14, 16-23, 48-63 | reserved | - | - | never delivered; a machine hook that selects one of these fails closed | - |

Entry SR update, for every row that builds a frame: S <- 1 and T <- 0. For an interrupt, the
interrupt mask also becomes the accepted level. No other exception changes the mask; vector 5 keeps
the pre-fault mask, as ADR 0037 already does. A privileged instruction in user mode (S = 0) never
executes any part of its operation. It raises vector 8 with the current PC, as U3 requires.

Privileged set (P1): MOVE to SR, ANDI/ORI/EORI to SR, MOVE An,USP, MOVE USP,An, RTE, STOP and RESET.
The legal-form baseline records 19 privileged forms. MOVE from SR, MOVE to CCR, ANDI/ORI/EORI to
CCR and RTR are **not** privileged on the MC68000.

### 4. Group 0 disposition: deferred and fail closed

Bus error and address error abort the current bus cycle. They build a different 14-byte frame
(U4), with offsets relative to the new SSP:

```m68k-group0-frame-deferred
offset=0 size=2 field=access_status_rw_in_function_code
offset=2 size=4 field=access_address
offset=6 size=2 field=instruction_register
offset=8 size=2 field=saved_sr
offset=10 size=4 field=saved_pc
total=14
```

The stacked PC is not precise: U4 says it is advanced 2 to 10 bytes past the first word of the
faulting instruction, depending on the instruction and the cycle that faulted. No project-authored
rule reproduces that value for every legal form, and no citable table defines it. Matching one
emulator's value would record that emulator's model, not MC68000 behavior. On the Genesis, /BERR is
not an observable software-recovery path. Unmapped access is already a fail-closed stop, and an
emulator would diverge in the same place.

**Decision.** Group 0 delivery is not implemented. Every condition that would raise it keeps ending
in an explicit, sanitized runtime stop, with no partial architectural state. The existing
misaligned-access, invalid-stack-alignment, invalid-stack-range and unmapped-region stop classes
cover these conditions. The ones that matter are:

- a word or long data access at an odd address;
- an odd jump, branch or return target;
- an odd or unwritable stack pointer during exception entry, RTE or RTR. Hardware would take an
  address error there, or halt on a double bus fault.

A handler for vector 2 or 3 in a program's vector table is never used as a target.

**Coverage impact, quantified against the legal-form baseline:**

- 835 of 1526 forms (54.7%) list `address_error_vector_3`. For 797 of them it is the only listed
  class; 38 also list vector 5, 6, 8 or 32-47.
- End-to-end support (`native_exec` and the routes) is unaffected. Those rows run with aligned
  operands, and the misaligned case is a documented fail-closed stop, not a missing form.
- Under its current definition, the `exception_privilege_modeled` row cannot pass for those 835
  forms. The row should report Group 0 as a separate **declared deferred disposition** and not as
  missing modeling. With address error removed from its applicability, the row's denominator is the
  17 forms whose classes are only Group 1/2, plus the 38 mixed forms. This measurement split is
  tracked as a continuation successor of SEG-021-T017, not as T018-T020 scope.
- Bus error: no form lists it, so the impact is 0 forms.

### 5. Entry ordering and memory safety (generalizes ADR 0020 §7/§8)

The shared entry primitive runs these steps in order. All fallible steps come before the first frame
write, so a failure changes nothing: no frame bytes, no SR, no SP, no PC.

1. The caller determines the exception: vector number and stacked PC. For synchronous exceptions both
   are build-time facts of the lowered instruction. For interrupts they come from the acceptance step
   (§7).
2. `saved_sr <- SR`.
3. Compute the new SR (§3). If `saved_sr.S = 0`, the active stack pointer changes from USP to SSP
   (§6). The frame goes on the SSP.
4. Validate the complete frame extent `[SSP-6, SSP)` through the machine's routed stack-write
   validation. This covers alignment, wrap and region.
5. Resolve the handler for the vector through the machine's vector hook (§7). It returns either a
   generated handler entry that was compiled from permitted build-time inputs, or a fail-closed
   result. Nothing is decoded, interpreted or generated at runtime.
6. Write the frame (§2), then commit together: SR <- new SR, active SP <- SSP-6, the USP shadow if
   the mode changed, and PC <- the handler.
7. Report the entry to the machine: timing charge (§8), plus an optional provenance notification
   (§7).

Hardware takes a double bus fault or address error when steps 4 or 5 fail. This project instead
stops with the caller's diagnostic class, as ADR 0020 §8 already does.

**RTE** is privileged; in user mode it raises vector 8 with the current PC. It reads the SR word at
SSP and the PC long word at SSP+2. It then commits together: SR <- the read SR, PC <- the read PC,
SSP <- SSP+6. If the restored S is 0, the active stack pointer becomes the USP after the SSP has been
incremented. If either read fails, the RTE fails closed with nothing changed.

**RTR** is not privileged. It reads a word at the active SP and a long word at SP+2. Only the CCR
(the low byte of SR, where X, N, Z, V and C are the defined bits) is replaced; the upper byte of SR
is unchanged. Then PC <- the read PC and active SP += 6. It uses the same atomic-commit rule as RTE.

### 6. Supervisor/user state and trace

- The CPU instance holds two stack-pointer slots, SSP and USP. A7 names the one selected by SR.S.
  Every SR write that changes S swaps which slot is active. This covers exception entry, RTE, MOVE to
  SR, ANDI/EORI to SR, and STOP. MOVE USP reads or writes the inactive slot while in supervisor mode.
  The existing `usp` runtime field becomes that slot; T018 implements this.
- Reset state: S = 1, T = 0, I = 7, SSP from vector 0. The USP is not defined by reset, so the
  project keeps its deterministic zero initialization and makes no hardware claim.
- **Trace (T bit, vector 9) is deferred and fails closed.** SR.T is stored exactly. However, if an SR
  write or an RTE would leave T = 1 when the next instruction starts, the result is an explicit
  runtime stop, not a trace exception. Vector 9 is never taken. This is a deterministic stop decided
  from runtime state; it involves no decoding. Impact: 0 forms in the legal-form baseline list trace.
  Any SR-writing form whose runtime value sets T stops.

### 7. Ownership and machine hooks

M68K-owned support is a reusable generated-native C11 support unit plus its lowering. T018 extracts
it from `platforms/genesis/runtime`. Its state is one per-instance CPU record: D0-D7, A0-A6, active
SP, the inactive stack-pointer slot, SR, PC and a stopped flag. It uses no file-scope mutable state
and no `genesis_` identifiers. Every machine interaction goes through an explicitly bound hook set
with a per-instance context pointer.

| concern | owner | notes |
| --- | --- | --- |
| SR S/T/I bits, supervisor/user selection, SSP/USP swap | M68K | §6 |
| privilege check for the privileged set; vector 8 | M68K | §3; the check lives in the lowering, not in a runtime opcode decoder |
| exception entry order, frame layout, SR update, RTE, RTR | M68K | §2, §5 |
| vector number of each synchronous exception | M68K (build-time fact of the lowered instruction) | §3 |
| interrupt acceptance rule (levels 1-6 vs mask; level 7 transition-sensitive) | M68K | rules frozen by T020 |
| STOP: load SR (privileged), advance PC, halt the instruction stream | M68K | wake/no-wake policy is a scheduler hook |
| RESET instruction: privileged, CPU state unchanged except PC | M68K | calls `reset_devices` |
| exception entry cycle cost | M68K (value) and machine (clock) | value from U5 (T022); charged through the timing hook |
| memory map, stack and vector-slot routing, alignment/region validation | machine | existing routed access |
| vector table placement and handler resolution | machine | Genesis: cartridge ROM at 0, resolved at build time (ADR 0020 §6, ADR 0037 B) |
| interrupt request lines, levels, sources, acknowledge (autovector, supplied vector, spurious, uninitialized) | machine | Genesis: autovectored levels; only level 6 is wired today |
| device reset consequences | machine | `reset_devices` |
| deterministic scheduler, video/device time, STOP wake sources | machine | ADR 0041 |
| project-only compatibility bookkeeping (IRQ6 resume grace and frame origin, ADR 0020 §9 / ADR 0037) | machine (Genesis) | through the optional entry/return notification; the CPU core has no IRQ6 notion |

Hooks at prose level. Each hook receives the bound machine context, and none is a global:

- **`validate_stack_extent(ctx, base, length, direction) -> ok | stop`** and
  **`stack_read(ctx, address, size) / stack_write(ctx, address, size, value) -> ok | stop`**: the
  machine's routed-access boundary. The validate call comes first so that entry, RTE and RTR can keep
  the validate-then-commit rule.
- **`resolve_vector(ctx, vector_number) -> handler_entry | not_installed | unrepresentable`**:
  selects among generated entries compiled from permitted build-time inputs. `not_installed` and
  `unrepresentable` fail closed at runtime with a sanitized diagnostic. A non-zero slot that cannot
  be represented still fails the build where the vector table is immutable (ADR 0020 §6 / ADR 0037 B).
- **`pending_interrupt_level(ctx) -> 0..7`** and
  **`acknowledge_interrupt(ctx, level) -> autovector | vector(n) | spurious | uninitialized`**: the
  machine states what hardware asserts. The CPU owns the acceptance decision and maps the answer to
  vectors 25-31, n, 24 or 15.
- **`charge_exception_cycles(ctx, cycles)`**: advances the instruction-boundary clock (ADR 0041).
  Until T022 publishes counts, entry charges nothing new, as it does today.
- **`reset_devices(ctx)`**: called by the RESET instruction after the privilege check. CPU registers
  are unchanged, and the stream continues at the next instruction.
- **`wait_while_stopped(ctx) -> woken | no_wake_source`**: the scheduler advances machine time until
  an interrupt passes the acceptance rule. `no_wake_source` ends with an explicit diagnostic instead of
  spinning (T020).
- **`on_exception_entry(ctx, vector_number, frame_base)` / `on_exception_return(ctx, frame_base)`**
  (optional): the machine's provenance notifications. The existing Genesis IRQ6-origin grace moves
  here unchanged.

### 8. Timing

Exception-entry cycle counts are CPU facts from the MC68000 User's Manual exception-processing
timing table (U5). They are charged through `charge_exception_cycles`. Values are published and
validated only by SEG-021-T022; this ADR records no count. The existing ADR 0041 rule stays: timing
is charged at guest instruction boundaries.

### 9. Second 68000 instances (Sega CD sub-CPU, 32X-side 68000)

The design already supports more than one 68000, because:

- CPU state is a per-instance record and every hook is bound per instance, so two instances cannot
  share state through globals;
- vector placement is behind `resolve_vector`. The Sega CD sub-CPU keeps its vectors in writable
  PRG-RAM, so a future binding either proves them immutable at build time or selects among
  already-compiled entries at runtime (ADR 0039). The CPU core does not change in either case, and
  never decodes or generates code at runtime;
- interrupt levels, sources and acknowledge behavior are machine hooks. Sega CD sub-CPU interrupt
  wiring and gate-array behavior therefore stay in a future Sega CD platform;
- RESET device consequences are `reset_devices`, which differs per machine;
- the 32X-side 68000 is the Genesis main CPU with a different memory map, which is also a machine
  concern.

No Sega CD, 32X, Z80 or SH-2 machinery, and no dual-CPU scheduling, is added by this contract.

## Non-goals

- Implementation. T018 covers supervisor/user state, privilege, SR/USP forms and the extraction of
  the shared core. T019 covers TRAP, TRAPV, CHK, ILLEGAL, line-A/F and RTR. T020 covers STOP, RESET
  and interrupt acceptance. T022 covers exception timing.
- Any MC68010+ behavior. Group 0 delivery. Trace delivery.
- Any interpreter, JIT, runtime opcode decoder, runtime code generation, or run-learn-regenerate
  loop. No universal CPU IR or interface.

## Citations

- **U1** Motorola, *M68000 8-/16-/32-Bit Microprocessors User's Manual*, M68000UM/AD (public
  Rev. 8/9 editions), §6 "Exception Processing", subsection "Privilege Modes": supervisor and user
  modes, A7 as SSP/USP, supervisor mode entered only through exception processing, and the reset
  state S=1/T=0/I=7.
- **U2** M68000UM/AD §6, subsection "Exception Processing" and its exception-vector-assignment and
  exception-grouping-and-priority tables. Group 0 = reset, bus error, address error. Group 1 =
  trace, interrupt, illegal, privilege violation. Group 2 = TRAP, TRAPV, CHK, zero divide.
- **U3** M68000UM/AD §6, subsection "Exception Types", with the entries for reset, interrupts
  (level 7 non-maskable, autovectors), uninitialized interrupt, spurious interrupt, instruction
  traps (stacked PC = next instruction), illegal and unimplemented instructions (line-A/F, stacked
  PC = the offending instruction), privilege violations (stacked PC = the offending instruction) and
  tracing. The supervisor-stack-order figure for non-Group-0 exceptions shows the six-byte SR/PC
  frame.
- **U4** M68000UM/AD §6, the "Bus Error" and "Address Error" entries and the Group 0
  supervisor-stack-order figure. They give the 14-byte frame, and state that the saved PC is
  advanced 2 to 10 bytes beyond the first word of the faulting instruction.
- **U5** M68000UM/AD §8, the exception-processing execution-times table (consumed only by T022).
- **P1** Motorola, *M68000 Family Programmer's Reference Manual*, M68000PM/AD Rev. 1 (1992),
  instruction pages for MOVE to SR, ANDI/EORI/ORI to SR, MOVE USP, RTE, STOP and RESET (marked
  privileged). MOVE from SR is marked privileged only for the MC68010 and later. The public 1988
  scan is already cited as `M1` in `docs/references/genesis-rom-startup-contract.md`.
- **P2** M68000PM/AD Table 6-1 "Exception Vector Assignments", already cited by ADR 0020 and
  ADR 0037, and the RTE, RTR, STOP and RESET instruction pages.

Exact printed and PDF page numbers are recorded by the implementing task against the archived
manual, following ADR 0020 and ADR 0037.

## Consequences

- T018-T020 build against one frozen frame, entry order and hook set. Their Musashi differential
  vectors compare the §2 layout and the §3 stacked-PC values.
- The guard test fails if the documented Group 1/2 frame, or the runtime's frame construction and
  RTE addressing, drift from the six-byte SR-at-SP, PC-at-SP+2 layout. T018 must retarget the
  runtime half of the guard when it moves the primitive.
- Group 0 and trace remain explicitly unsupported, fail closed, and are counted in the coverage
  impact; they are not silent gaps.
