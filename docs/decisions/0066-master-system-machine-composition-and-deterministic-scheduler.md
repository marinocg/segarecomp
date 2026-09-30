# ADR 0066: Master System Machine Composition, Typed Device Seams and the Deterministic Scheduler

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T003
- Related: ADR 0058/0060 (Z80 ABI, deadlines), ADR 0064 (run API, artifacts, headless driver), ADR 0065 (memory map, sticky errors),
  `docs/architecture/master-system-machine-contract.md` sections 2, 6, 7, 8, 12, 13, 14 (U11) and
  `docs/architecture/z80-scheduling-contract.md`.

## Decision

1. **One machine object, one loop.** `SmsMachine` (`platforms/master-system/runtime/sms_machine.{h,c}`, strict C11) owns the
   `Z80Runtime`, the T002 `SmsMemory`, the I/O-control byte, the input schedule, the scheduler state and three device
   seams. `sms_run_until_cycle`, `sms_run_until_frame` and `sms_run_bounded` are the only run entry points; the headless
   driver and the later viewer both call them. The scheduler is a small explicit loop (no event framework): the next
   device event is always the next scanline start, `T = 228 x absolute line`.
2. **Port decode is a pure function** (`sms_ports.h`): only A7, A6 and A0 matter, so every port mirrors across its row, and each
   decoded class has exactly one owner (memory control -> T002 owner; V/H counter, VDP data/control/status -> VDP; PSG write
   -> PSG; `$DC`/`$DD` reads and the I/O-control write -> pad). Reads of `$00-$3F` return `$FF` and writes to `$C0-$FF`
   are ignored (no owner). Reads of `$C0-$FF` return `$FF` while port `$3E` bit 2 disables the I/O chip, before any pad
   device is consulted.
3. **Typed device seams.** `SmsPortDevice {context, reset, read, write, digest}` for PSG and pad and `SmsVdpDevice` (the same
   port device plus `scanline` and `irq_sources`) for the VDP are plain function-pointer structs assigned by
   `sms_install_devices`. A NULL `read`/`write` fails closed with `SMS_ERROR_PORT_UNIMPLEMENTED`; nothing returns a silent
   value. `reset` runs in the fixed order VDP, PSG, pad after the machine-owned reset (`z80_reset`, work RAM with
   `$C000 = $AB`, mapper, memory control `$AB`, I/O control `$FF`); T003 defines no VDP/PSG reset state. `digest` lets a
   device append to the machine-state digest.
4. **Time.** The only clock is `Z80State::cycles`. The CPU runs to `min(next scanline start, requested stop)`; events due at or
   before the current T are applied between `z80_run` calls and, defensively, at the top of every host callback, so device state
   has advanced through every event with `T_event <= T_access` (the instruction-start T-state) before any access (U11).
   Because slices end at instruction boundaries and events are applied before the next instruction, any slicing of a run
   reaches the same state; the digest, traces and device logs are identical (unit and generated-native tests).
   Halted time advances on the Z80's own 4-T halted grid; the platform adds no rounding of its own.
5. **Interrupts.** `int_line` is re-derived from `irq_sources` after every scanline event and every device port access (a status
   read deasserts it); source edges are traced at the event or access T-state, acceptance at the acknowledge callback's
   T-state. `nmi_pending` is raised at the frame start where the scripted pause input goes `-` to `P`. An NMI pending at a
   run entry outside a prefix run is sampled at that entry boundary, so its acceptance is traced exactly; across a
   `prefix_lock` run only the T-state at return is known (an upper bound, documented in the code). The Z80 runtime already
   refuses both while `in_prefix_run`; the platform only latches.
6. **Errors stop permanently.** A callback that latches an error (memory latch, unimplemented port) lowers `state.deadline`,
   which is a host-writable ABI field, so the run ends at the next instruction boundary. The platform then returns a permanent
   typed stop (`SMS_STOP_PLATFORM_ERROR` or `SMS_STOP_Z80_ERROR`) with the stop PC, image identity, offending
   address/value and the instruction-start T-state of the offending access; every later run call returns the same stop.
   `SMS_STOP_HALT_IDLE` (halted, interrupts disabled, no scripted pause left) is a resumable diagnostic enabled only when
   the driver has no bound.
7. **Headless driver placement.** The driver needs file I/O, which the runtime library must not use, so it lives in
   `platforms/master-system/headless/` (`sms_headless.c`, plus `sms_devices_none.c`, the baseline with no device attached). The
   generated program compiles them with the runtime and the emitted image; T004/T006/T007 replace `sms_devices_none.c` by the
   real device wiring. `--slice-cycles` / `--slice-seed` are test hooks that split the run (digest must not change). Test
   stubs (`tests/tools/sms_test_devices.c`) implement a minimal frame/line interrupt VDP, PSG log and pad behind the seams;
   they are not VDP/PSG/pad models.
8. **No Genesis extraction.** The Genesis runtime schedules per retired M68K instruction with M68K cycle costs and a master-clock
   divider, and owns YM/VDP/Z80-bus events; SMS schedules whole deadline slices of a generated Z80 image over a single integer
   T-state clock. The only shared idea is "a monotonic integer timebase", which is not worth a shared module. Nothing in
   `platforms/genesis` or `libs/` changed, and the SMS runtime has no Genesis or M68K dependency
   (`sms_machine_dependency_test`).

## U11 evidence (scheduler part)

Generated-native fixtures with accesses whose bus cycle falls after a line start (single OUT series, six-prefix DD chains,
V-counter reads) confirm the platform orders each access at its instruction-start T-state against every scanline event. The
probe `u11_probe` (same read instruction behind k superseded DD prefixes, swept in 4-T steps across a line boundary) shows,
against the pinned references: the platform flips at the same step for every k (instruction-start ordering); Genesis Plus GX
flips k steps earlier (real bus-cycle ordering, exactly 4 T per prefix); Gearsystem flips 2, 3 and 3 steps earlier for
k = 2, 4, 6 (partial). Every deviation lies between 0 and one step per prefix, which is the contract's bound, and only
index-prefix chains in front of a device access are affected; a plain single form is at most about 11 T inside one
instruction and the U2 in-line event offsets are themselves unresolved, below the probe's resolution. The references also
disagree with each other. This does not establish that instruction-start ordering is insufficient for the baseline, so no
SEG-008 continuation is opened; T004/T006/T007 re-check their device-visible accesses (VDP counters, pad reads, PSG write
phase) and T011/T012 attribute any real-title dependence.

## Consequences

- T004 implements `SmsVdpDevice` (scanline events, `irq_sources`, registers/ports, `reset` including U9, `digest`) and adds
  its frame/VDP trace artifacts to the driver; T006 implements the pad seam and consumes `SmsMachine::input` and `io_control`;
  T007 implements the PSG seam (its `write` catches the device up to the `cycles` argument).
- T009's viewer calls the same run API and samples input only at frame boundaries into the same schedule structure.
- T010 links `sms_headless.c` and the real device wiring into the generated program.
