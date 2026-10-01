# ADR 0069: Master System End-to-End Machine Fixture and Reference Comparison

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T008
- Related: ADR 0062 (independent references), ADR 0064 (execution architecture, artifacts), ADR 0066 (scheduler),
  ADR 0067 (VDP), ADR 0068 (renderer), `docs/architecture/master-system-machine-contract.md` sections 14 (U2, U8), 15, 16.

## Decision

1. **One fixture, every baseline area.** `machine_e2e` (tools/sms_fixture_rom.py, 128 KiB, Sega mapper declared by the builder)
   is a project-authored program: reset entry, RAM and its mirror, slot-1/slot-2 bank switches (including a value above the
   bank count), the same logical PC `$4100` under three banks, VRAM/name-table/sprite/CRAM uploads read from banked tables,
   mode 4 scrolling (per frame and from the line interrupt), sprites with per-line overflow and collision, IM1 frame and
   line interrupts, both controller ports under scripted input plus the I/O control readback, the pause NMI and PSG tone and
   noise writes (one channel at a time, so per-channel levels are comparable). Its guest-visible results are ordinary RAM
   (layout in the builder): an init block at `$C100` and one 16-byte log record per frame interrupt at `$C200 + 16 n`. No
   instruction straddles a slot edge (those are fail-closed stubs by ADR 0065).
2. **Independent predictions, hermetic.** `tests/sms_machine_e2e_test.py` predicts every artifact from the contract and the
   published Z80 T-states (`tests/sms_e2e_common.py`): the guest log, the interrupt trace (a flag/counter/enable model driven
   only by the register writes and status reads of the VDP trace), the mapper and register write sequences, the VRAM/CRAM image,
   every framebuffer (the T005 render model) and the PCM (the T007 model fed acknowledge-relative write timestamps from the
   fixture's own handler path). The committed `tests/fixtures/sms-e2e-validation.json` holds the expected artifact digests.
3. **Generated-native-only is tested, not assumed.** The test asserts the link inputs are generated owners plus SMS runtime,
   headless and PSG device sources only, that generated and runtime code contain no interpreter/opcode vocabulary, that the
   executable defines no interpreter or decoder symbol (`nm`; the I/O port decoder is named), and that the same logical PC
   dispatches under three bank identities.
4. **Fault injection.** Six one-line perturbations of a private copy of the runtime (mapper bank mask, VDP control latch reset,
   line counter reload, sprite limit, PSG noise taps, pause edge) are compiled and run; each must change the artifacts its
   subsystem owns.
5. **Reference comparison (local).** `tests/sms_machine_e2e_oracle_test.py` runs the fixture on both pinned libretro cores with
   the same scripted input (`tests/sms_oracle/libretro_frames_host.c`) and records the outcome in the manifest's `reference`
   section. Rules: the init block and every frame log field except the controller/pause columns are exact; the controller
   and pause columns are exact after a constant per-reference input-phase offset (a libretro run boundary is not the platform
   frame boundary; the offsets are recorded, asserted to exist and constant); framebuffers are exact per pixel through the
   colour-class bijection on every frame after both machines' display enable has settled, outside the U8 gap mask; PCM is
   compared per frame by AC RMS under one gain per reference (the references scale and band-limit their output), every steady
   frame within +/-1 dB of it and silent steady frames below 5% of it. A recorded value that stops holding fails as stale.
6. **U8 is now a finding, not an assumption.** Both references disagree with the platform in the left `R8 & 7` pixels of a line,
   and with each other (Gearsystem draws the wrapped background column, Genesis Plus GX a constant colour; the platform keeps
   the backdrop, contract U8). The contract keeps the backdrop (no public source), and the comparison masks exactly that
   region; every other pixel of every compared frame agrees with both references. The mask is checked for staleness.
7. **Startup divergence, recorded.** Genesis Plus GX reaches the display enable about half a frame later than the platform and
   Gearsystem (its first displayed frame is partial), so frame logs are compared from guest frame 1 and framebuffers from
   frame 6.

## Consequences

- A gap in T002-T007 found by the fixture is fixed in its owning family: the status-register composition in `sms_vdp.c` is
  written with explicit unsigned operands (a sign-conversion diagnostic of stricter compilers), behaviour unchanged.
- T009/T010 can run `machine_e2e` through the viewer/build route and compare against the same manifest digests.
- Opposing directions (left+right) are not scripted: Gearsystem suppresses one of them, which a physical pad cannot produce.
- T011 (commercial attribution) still owns any behaviour this fixture cannot reach (real-title timing around U2 and U8).
