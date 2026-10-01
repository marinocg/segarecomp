# ADR 0072: Genesis Z80 Platform Integration (map, bus, clocks, scheduling, interrupts)

- Status: Accepted (SEG-032-T001)
- Date: 2026-10-01
- Related: ADR 0058/0071 (Z80 broad AOT, owners), ADR 0066 (Master System scheduler, the second-consumer precedent),
  ADR 0073, ADR 0074, `docs/architecture/genesis-z80-audio-contract.md` (the frozen facts).

## Context

The Genesis runtime has no Z80: the SEG-007 compat policies latch BUSREQ/RESET, grant the bus immediately and expose the Z80
RAM as flat bytes. SEG-008 delivered a generated-native Z80 with a host ABI (`Z80Host`) and SEG-033 grouped owners; SEG-009
made the Sega PSG a platform-neutral device. SEG-032 must run a real Z80 image inside the Genesis machine.

## Decision

1. **The existing Genesis scheduler owns both CPUs.** `GenesisInterruptScheduler.master_ticks` stays the one clock. A new
   Genesis-local function `genesis_z80_sync(runtime, target_master_ticks)` runs the generated Z80 (`z80_run` with
   `deadline = target / 15` Z80 cycles) while the Z80 is runnable. It is called after every M68K retirement advances the
   clock (inside `genesis_irq6_scheduler_and_admit`'s caller) and before any M68K access to Z80-domain state (Z80 RAM,
   `$A11100`, `$A11200`, YM2612, PSG, bank register). No generic multi-CPU scheduler, no event queue: the M68K is always
   the leader and the Z80 a lazily synchronized follower (the Z80 instruction stream is independent of when it is
   synchronized because it only observes shared state through callbacks that are themselves timestamped).
2. **Ordering.** A 68K access at tick `t` happens before the Z80 instruction that starts at `t`. The Z80 stops at the first
   instruction boundary at or after the target (overshoot < one instruction); its time never moves backwards. Device clocks
   are monotonic: a write timestamped earlier than the device clock is applied at the device clock.
3. **Z80 state ownership.** `Z80Runtime`, the bound image, the BUSREQ/RESET/`pristine` latches and the 8 KiB RAM live in the
   Genesis device state next to the existing `GenesisZ80BusState` (which keeps `z80_ram` as the single RAM that both the
   68K window and the Z80 view alias). They enter the checkpoint evidence (schema bump coordinated with T005-T007).
4. **Host callbacks.** One Genesis-local Z80 view module implements `Z80Host` (`read`, `write`, `io_in`, `io_out`,
   `interrupt_acknowledge`, `code_image`, and T003's `code_matches`). The bank window routes to the already embedded owned
   cartridge regions (ROM) and to work RAM writes; everything unsupported is a typed stop that surfaces as a Genesis stop
   diagnostic. No manifest of samples or driver ranges exists; the cartridge image is already embedded as one owned region by the
   M68K path (measured cost for larger images is recorded by T004).
5. **BUSREQ/RESET** semantics are exactly sections 4-5 of the contract: the grant requires `/RESET` released, the Z80 stops at
   an instruction boundary, resumes at a multiple of 15 master ticks, resets (architecturally, YM2612) on `/RESET` release, and
   the *runnable transition* while pristine begins an image epoch (ADR 0073).
6. **Interrupts.** A level INT line is true during the VBlank onset scanline; not latched; acknowledge byte `$FF`; no NMI.
7. **Typed fail-closed outcomes** extend the Genesis stop reasons: `z80_unknown_image`, `z80_code_mismatch`, `z80_no_owner`,
   `z80_mutable_code`, `z80_bank_target_unsupported`, `z80_view_unmapped_access`, `genesis_68k_z80_area_without_bus`. No outcome
   decodes bytes.
8. **Transitional seam (allowed, time-boxed).** Between T004 and T006/T007 the Z80 view reaches the compat YM2612/PSG
   functions; the owning task deletes each when it lands. No other fake path remains after T005.

## Rejected alternatives

- A generic bus-arbitration or multi-CPU framework (no second consumer; Genesis only).
- Running the Z80 as an independent thread of virtual time with a priority queue (determinism cost, no benefit at one Z80).
- Per-instruction lockstep of both CPUs (cost; the lazy follower yields identical results because every interaction point
  synchronizes first).
- Immediate BUSREQ grant (the retired compat policy): wrong whenever the Z80 runs.

## Consequences

- The M68K-only Genesis tests that do not touch the Z80 area are unchanged in outcome.
- The sync-cadence invariance test (extra synchronization points change nothing) is the guard for decision 1.
- 68K stalls caused by Z80 bus use and DMA/Z80 contention are not modelled (references disagree, no supported workload).

## T004 implementation record (2026-10-01)

- `platforms/genesis/runtime/z80_machine.[ch]` (one translation unit, plain C11) is the Z80's view: `Z80Host` callbacks over the runtime's
  own state (`devices.z80_bus.z80_ram` is the one RAM both CPUs alias; `devices.z80_bus.bank` is the 9-bit bank register that both a
  Z80 write at `$6000` and a 68K write at `$A06000` shift in). It is compiled only into programs that carry a Z80 image registry:
  `runtime.c` never includes a Z80 header, so every 68K-only program and test links exactly as before.
- Banked reads are served from the cartridge image the generated program already embeds as owned regions (no sample range, no manifest);
  work-RAM writes are live; everything else is a typed stop. `+3` Z80 cycles are added per access to the 68K bus (the two references agree).
- The 68K side of the Z80 area (`genesis_z80_area_access`) replaces the SEG-007-T103 flat window; its generation-time classifier
  (`address_space_contract.h`, `address_space.cpp`) is widened to the same shapes. New typed diagnostics: `z80_view_unmapped_access`,
  `z80_bank_target_unsupported`, `genesis_68k_z80_area_without_bus` (wire names registered in the bridge validator).
- The YM2612/PSG seam `genesis_ym2612_port_{read,write}` / `genesis_psg_port_write` (runtime.h) is the one entry both CPUs use; its bodies
  are the SEG-007 compat models until T006/T007 replace them. This is the only transitional path.
- The bank register is added to the evidence-bearing `GenesisZ80BusState` (`checkpoint_evidence.h`); its digest/oracle coverage lands with the
  other schema changes in T005.

## T005 implementation record (2026-10-01)

- **Bus latches follow the hardware rules** (`genesis_z80_bus_write`, runtime.c): power-on `/RESET` asserted (`GenesisZ80BusState.reset_released`
  replaces `reset_asserted`; a zeroed runtime is "held in reset"), `bus_granted = bus_requested AND reset_released`, edges (not level
  writes) reach the machine. The SEG-007-T102 immediate-grant policy, the reset-only latch and the "no Z80 exists" statements are gone; the
  policy document is marked superseded.
- **One seam, `GenesisZ80Hooks`** (`runtime.h`, plain C): `run_to(master_ticks)` and `bus_event(...)`. `runtime.c` calls `run_to` after
  every M68K retirement at a 512-tick quantum (configurable, results independent of it) and before every 68K access to Z80-domain state
  (Z80 area, BUSREQ/RESET, YM2612, PSG); it never includes a Z80 header. A program without an attached machine keeps the bus latches
  and tracker but executes nothing.
- **`z80_machine.c` is the secondary CPU**: `genesis_z80_machine_attach`, `run_to` (instruction-boundary stop at or after the target, the INT
  line a pure function of guest time with segment splitting at its two edges per frame), the edge actions (`/RESET` release = architectural
  reset + YM reset + `cycle_base`; BUSREQ release = resume at the next multiple of 15 ticks, held time is not made up) and the activation
  of an image at an epoch by signature (`genesis_z80_image_for_signature`; an empty hold window re-binds the previous image; unknown =
  typed `z80_unknown_image`, with a materialization callback for T008). Typed outcomes: new stop class `unsupported_z80_execution` with
  `z80_unknown_image`, `z80_code_mismatch`, `z80_no_owner`, `z80_mutable_code`, `z80_unresolved_fetch_mapping`, `z80_unsupported_acknowledge`.
- **Tracker.** The T002 epoch observer is now the runtime's always-on hold-window tracker (`runtime->z80_epoch`); `on_epoch` is the optional
  probe seam. Z80 CPU state is machine-private (like the VBlank scheduler state, ADR 0020 section 10) and is not checkpoint evidence; the
  evidence-bearing `z80_bus` gained the bank register and the inverted reset field, and the independent checkpoint oracle was updated.
- Evidence: `tests/genesis_z80_machine_test.py` (power-on, hold/resume with the 68K clock running, reset while executing, restart epoch,
  same Z80 PC under two images with dirty carry-over data, unknown image, data vs code mutation, VBlank INT with and without IFF1,
  determinism and sync-cadence invariance at quanta 1/512/4096, no decoder symbol in the linked executable).

## T006 implementation record (2026-10-01)

- **One shared PSG, attached.** `genesis_audio.[ch]` owns the `Sn76489` of `libs/device/sega/psg` (the default Sega variant, power-on state)
  and installs `GenesisAudioHooks` into the runtime; the 68000's `$C00011/13/15/17` (the odd mirrors now routed on both sides of the static
  seam) and the Z80's `$7F11/13/15/17` window both call `psg_write(value, master_ticks)` on the same instance. A program without an attached sound
  device accepts the byte and discards it (absent hardware). Time is the Z80/PSG clock (master / 15); the device clock is monotonic
  (`sn76489_advance` ignores a time in the past), so a Z80 write whose instruction started before a 68000 write is delivered first even
  when its stamp is later.
- **Evidence.** The SEG-007-T109 command-latch model is removed. The evidence-bearing PSG record is the log of the 68000's port traffic
  (`write_count`, FNV-1a digest); the independent checkpoint oracle mirrors it. The device's own state is determinism evidence of the
  audio artifact (T009). A data byte before any latch is accepted and ignored by the device (counted), as on the Master System, not a stop.
- Tests: `genesis_startup_runtime_psg_test` (port semantics, stand-in device), `genesis_audio_psg_test` (real runtime + Z80 machine + shared
  device: 68000-only, Z80-only, interleaved with the overshoot case, state equal to the library driven directly at cycle = ticks / 15,
  cadence independence, wrong-ratio and dropped-writer controls, library builds with no Genesis code).
