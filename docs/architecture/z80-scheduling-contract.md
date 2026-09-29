# Z80 machine-neutral scheduling contract (SEG-008-T008)

Scope: how a platform interleaves generated-native Z80 execution with devices deterministically. It owns no platform
timing (wait states, bus contention, bank latency belong to platform milestones) and reuses no M68K timing table.
Sources: ABI `z80_runtime.h`, `docs/architecture/z80-cpu-contract.md`, ADR 0058 and 0060. Validated by
`tests/z80_timing_closure_test.py` (generated-native; also against the pinned oracle when available).

## Time

- Time is T-states in `Z80State::cycles` (monotonic, 64-bit). The platform owns the clock and passes an **absolute
  deadline** to `z80_run(rt, deadline)`; the runner never advances time on its own.
- Every instruction's cost is exact and outcome-dependent only where the published tables say so: JR cc (7/12),
  CALL cc (10/17), RET cc (5/11), DJNZ (8/13); block operations cost 21 per repeating iteration and 16 for the final
  one (each iteration is a boundary); JP cc,nn costs 10 either way. Each extra DD/FD prefix costs 4 T and R+1; a
  DDCB/FDCB form costs 20 (BIT) or 23. HALT accounts 4 T and R+1 per halted M1 cycle.
- Interrupt entry: IM0 (RST byte) 13, IM1 13, IM2 19, NMI 11 T-states.

## Boundary and deadline

- The deadline is checked at instruction boundaries only: execution stops at the **first boundary with
  `cycles >= deadline`**, so the last instruction may overshoot; no instruction is ever split. A deadline already
  reached does no work (zero T-states), so a platform can call `z80_run` repeatedly with the same or a smaller value.
- A block-operation iteration, an interrupt response and a halted M1 cycle are each a boundary (a halted CPU is
  accounted in whole 4-T cycles, rounded up to the deadline).
- Interrupts are sampled only at a boundary, after the deadline test: NMI (edge, latched in `nmi_pending`) before INT
  (level `int_line`, needs IFF1 and no EI/RETI deferral). An input raised while a deadline is already reached is
  therefore seen on the next call, never inside the previous one.
- The only deadline stop that is not an instruction boundary is inside a `prefix_lock` run (an endless DD/FD run). The
  state then carries `in_prefix_run`; the run is treated as one indivisible boundary, so INT and NMI are never accepted
  while it is set, and resuming with it set continues the run in 4-T/R+1 steps. Entering a run with the flag clear is an
  ordinary interruptible boundary.

## Outcomes

| class | outcomes | platform action |
| --- | --- | --- |
| resumable | `deadline`, `halted`, `prefix_lock` | advance time, drive INT/NMI (not while `in_prefix_run`), call `z80_run` again |
| fail-closed error | `no_owner`, `mutable_code`, `unresolved_fetch_mapping`, `unknown_image_identity`, `excluded_form`, `im0_unsupported_acknowledge_byte` | stop; never resume and never treat as a scheduling point |

`z80_outcome_is_resumable` / `z80_outcome_is_error` are disjoint and exhaustive over the named outcomes. Resume always
re-enters through the image dispatcher (exact entry-table lookup of the current PC and code-image identity).

## Determinism

Given the same image, initial state, host callbacks and input schedule, every stop, state, write/I-O log and T-state
count is identical across runs and hosts; a run split at any deadline sequence ends in the same state as the
straight-through run (property-tested over seeded random schedules, including wrapped fetches and splits inside a
`prefix_lock` run with INT/NMI raised between splits).
