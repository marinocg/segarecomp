# ADR 0067: Master System VDP State, Port Protocol, Status and Frame/Line Interrupts

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T004
- Related: ADR 0062 (independent references), ADR 0064 (artifacts, headless driver), ADR 0066 (scheduler, device seams, U11),
  `docs/architecture/master-system-machine-contract.md` sections 7, 9, 14 (U2, U3, U9, U11).

## Decision

1. **One independent C11 VDP** (`platforms/master-system/runtime/sms_vdp.{h,c}`), no Genesis VDP code, data structure or
   register semantics, no shared helper (the CRAM-to-RGB function is a pure inline function of the 6-bit SMS colour with a different
   input format from `genesis_vdp_decode_cram_entry`'s 9-bit entry, so no extraction). `SmsVdp` owns VRAM (16 KiB), CRAM
   (32 bytes, `--BBGGRR`, low 6 bits stored), registers 0-10 (11-15 accepted and ignored), the 14-bit address and 2-bit code,
   the first/second-byte latch, the read buffer, the frame/overflow/collision/line pending flags and the line counter. It is a
   plain struct so the renderer (T005) reads it directly; there is no accessor layer.
2. **Seam, not a framework.** `sms_vdp_install(machine, vdp)` fills the T003 `SmsVdpDevice` (port `read`/`write`, `reset`,
   `digest`, `scanline`, `irq_sources`) and binds the error sink to the machine's memory latch. The VDP has no clock: it is
   advanced only by the machine's scanline events (line L of frame F at `T = (F x 262 + L) x 228`; every in-line event at
   offset 0, U2) and by port accesses stamped with the instruction-start T-state (U11). Nothing depends on how a run is
   sliced (unit, native and slice-equivalence tests).
3. **Port protocol (contract 9.2).** The first control byte updates the address low byte immediately; the second sets the
   code and address bits 13-8 and performs the command (code 0: pre-fetch the buffer and increment; code 2: register write
   with the first byte as value; code 1/3: nothing). A status read and every data access clear the latch. Data writes go to
   VRAM (codes 0-2) or CRAM (code 3, `address & 31`), load the buffer and increment; reads return the buffer, refill it and
   increment; the address wraps at `$3FFF`.
4. **Status (9.3) and interrupts (9.4, 7).** Status returns flags in bits 7-5 and `%11111` in bits 4-0 and clears the three
   flags, the line pending flag and the latch. The frame flag is set at line 193 (192-line) / 225 (224-line); the line counter is
   decremented on lines 0..active inclusive, an underflow reloads it from R10 and sets the line pending flag, every other
   line reloads it. `irq_sources` returns `SMS_IRQ_FRAME`/`SMS_IRQ_LINE` only when the matching enable bit is set, so a
   pending-but-disabled flag asserts `/INT` at the enabling write and a status read deasserts it; the machine derives the
   `int_line` level and the interrupt trace edges from that one function. Sprite overflow/collision are set by T005 through
   `sms_vdp_set_sprite_flags` and returned/cleared here.
5. **Modes fail closed (9.5), at the check point.** `sms_vdp_mode` classifies M4/M3/M2/M1 and R0 bit 0. An unsupported mode
   latches `SMS_ERROR_VDP_MODE_UNSUPPORTED` at the first observable effect and never approximates: at a scanline event with
   the display enabled or an interrupt enable set, at a status read and at a V counter read. Writing the mode bits with the
   display blanked does not stop. The H counter read latches `SMS_ERROR_HCOUNTER_UNRESOLVED` (U3 stays open: it needs the TH
   latch trigger, T006). The machine latches the VDP's typed errors into the same sticky memory latch as every other platform
   error and stops permanently (`sms_machine.c` checks the latch after a device callback and after scanline events).
6. **Reset (U9).** `sms_vdp_reset` (run by the machine reset) applies the project convention: registers as the contract
   column, address/code 0, latch clear, buffer 0, flags 0, line counter `$FF`, VRAM/CRAM zero. The observable parts (first
   buffer read, status, VRAM contents, interrupts with only the reset registers) agree with both pinned references;
   Genesis Plus GX additionally powers on with pending frame/overflow flags (classified, time origin + T005).
7. **Trace and artifacts.** `SmsVdpTraceEntry {cycles, kind, arg, byte, data}` records register writes (registers 0-15), status
   reads, frame/line flag events (line in `arg`) and, at every frame start and at the end of a run, VRAM/CRAM write
   summaries (write count + the first 8 bytes of SHA-256 of the memory). IRQ edges remain in the machine's `irq.trace`
   (same T-state stamps; every assertion has a VDP flag/register cause and every deassertion a status read or register
   write, checked in the native test). The headless driver writes them through `sms_write_device_artifacts`
   (`platforms/master-system/headless/sms_devices_vdp.c`): `vdp.trace`, `vram.bin`, `cram.bin`, `vdp.json`. The stub-device
   and no-device units provide the same hook as a no-op.
8. **Validation.** Three implementations are compared: the C runtime, the independent Python model
   (`tests/sms_vdp_model.py`, written from the contract, with named mutations) and the two pinned references through
   project-authored fixtures. Deliberately wrong rules (latch not cleared by status, no read pre-fetch, write does not load
   the buffer, address saturates, no CRAM alias, first byte not immediate, line counter off by one) must disagree with the
   generated-native results. Fixture ROMs are regenerated at test time from `tools/sms_fixture_rom.py` (only hashes are
   committed).

## Findings

- **Port/state** generated sequences (two seeds, ~700 operations each, every protocol edge case): identical reads and VRAM
  read-back checksum in the platform, the model and both references once the status flag bits are masked (bits 6-5 are the
  T005 sprite flags; bit 7 depends on the reference's power-on frame phase in a run-from-reset sequence and is verified by
  the interrupt fixtures instead).
- **Interrupts**: handler-observed V counter and status per phase (frame IRQ 192/224 lines, line IRQ with R10 = 15 and 0
  including the reload across the frame boundary, pending-but-disabled then enabled, status read before the enable, R10
  written at the end of the display) are identical in the platform, Gearsystem and Genesis Plus GX.
- **U2** (in-line offsets): an R10 write flips the next line's reload at exactly the same 4-T step in all three; the frame
  flag and V counter step are seen 12 T (3 steps, 9-15 T) earlier than the platform by both references, which agree with
  each other. There is no absolute agreement and the scheduler has no in-line offsets, so offset 0 is kept and the 12 T lead
  is an asserted validation tolerance (0..16 T).
- **U11 (VDP)**: instruction-start ordering gives the same result as the references for ordinary accesses and the R10
  write; the status/V counter deviation above is inside the contract's one-boundary bound. No SEG-008 continuation.
- A probe with the line interrupt enabled while R10 was rewritten in the vertical blanking interval with a stale counter
  showed Genesis Plus GX raising line interrupts on vblank lines, where MacDonald, Gearsystem and the platform do not
  (reload on every line outside the display). The synchronised fixtures kept in the suite agree across all three; the
  unsynchronised probe is not retained as a fixture (recorded here as an open classification for T011/T012 if a real title
  depends on it).

## Consequences

- T005 reads `SmsVdp` (registers, VRAM, CRAM, `line`/`frame`) from the per-line hook (`line_hook`, called on every active line
  before the counter/flag logic, only in a supported mode) and reports sprite flags with `sms_vdp_set_sprite_flags`.
- T006 will add the TH latch trigger and H counter value table (U3); until then the read stops typed.
- In-line event offsets (U2) are a scheduler feature only if a later task finds software that needs them.
