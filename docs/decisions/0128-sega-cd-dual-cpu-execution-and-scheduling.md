# ADR 0128: Sega CD dual-CPU execution, runtime seam and deterministic scheduler

- Status: Accepted (SEG-062-T001). Implemented by SEG-062-T003 (T004/T005 add device events).
- Related: ADR 0041/0043 (instruction-boundary retirement, exception contract), ADR 0126, contract sections 2, 10-12.

## Decision

1. **Same generated semantics, two state instances.** Each processor is a `ScdCpu` record (`d[8]`, `a[8]`, `usp`, `sr`,
   `pc`, interrupt state, clock) in one `ScdMachine`. Generated code for processor `k` names only `m->cpu[k]` fields
   and calls the bus with the constant `k`; the CPU's own emitter (`emit_m68k_operation_c`) is used unchanged with one
   `M68kRuntimeCEmitter` instance per processor (the Sega CD emitter, `platforms/sega-cd/machine`). Routed lowering
   keeps call/return state on the guest stack, so per-CPU call-frame arrays are not needed; returns are validated
   against the processor's compiled-entry table.
2. **Runtime seam: a Sega CD-owned ABI that composes Genesis devices, not `GenesisRuntime`.** The main CPU's map
   differs from a Genesis (BIOS/PRG window/Word RAM/Gate Array instead of cartridge and Z80), and `GenesisRuntime`
   embeds one CPU's state, its whole scheduler and Genesis-only stops. Composing it for the main CPU would force
   machine conditionals into Genesis code. Instead `platforms/sega-cd/runtime` (strict C11) owns both CPUs, the bus,
   the Gate Array, the scheduler and the disc, and **composes** the Genesis VDP: a zero-initialised `GenesisRuntime`
   is held only as the VDP device host and main-CPU accesses to `$C00000-$C00007` are routed through the existing
   `genesis_route_access`; frames come from the existing `genesis_vdp_produce_frame` and PPM export. VDP DMA is a
   typed stop (its source would be the wrong machine's memory). No universal CPU interface is introduced: the only
   shared code is the CPU library, the emitter and the VDP device.
3. **Scheduler.** Single-threaded, integer ticks (contract section 2). Each processor has a `next` time (start of its
   next instruction). Loop: pick the runnable processor with the smallest `next` (tie: main); first process every
   device event with time `<=` that `next` in a fixed order (CDD tick, timer, CDC DMA progress); then sample and accept
   interrupts at the boundary (ADR 0043 acceptance); then execute exactly one generated instruction and add its
   CPU-owned cycle count times the processor's period. A processor that is stopped (STOP), held in reset or bus-requested
   is not runnable; when woken by an event at time `t` its `next` becomes `max(next, t)`. If no processor is runnable the
   clock advances to the next device event; if there is none and no enabled source can wake a stopped processor the run
   ends `idle`. A deadline only bounds where the loop pauses: the sequence of decisions never depends on it, so any
   partition of a run into deadline slices yields identical state (tested).
4. **CPU-local exceptions** use the CPU-owned `exception_core.h` with per-processor machine hooks (stack extent via
   the processor's own bus, vectors from the processor's own bus, handler must be a compiled entry).
5. **Bus ordering.** An access happens at its instruction's start time; with the earliest-first rule this gives a
   total order across processors consistent with their clocks. Contention and wait states are not modelled (contract
   section 12).
6. **Host concurrency is rejected**: no measurement shows a need, and determinism under slicing is the acceptance
   criterion.

## Go/no-go criteria (falsifiable)

- GO if the T003 fixture runs both processors as generated native C, produces the hand-derived and the independent
  Musashi-reference results, and is bit-identical across at least eight different deadline partitions and two process
  runs. NO-GO if any part of `libs/cpu/m68k` or the generic emitter must change semantics to host the second processor.
