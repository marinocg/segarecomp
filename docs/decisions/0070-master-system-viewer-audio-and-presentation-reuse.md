# ADR 0070: Master System Viewer, Audio Output and Presentation Reuse

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T009
- Related: ADR 0064 (execution architecture, scripted input), ADR 0066 (scheduler), ADR 0068 (renderer), ADR 0069 (end-to-end
  fixture); `docs/architecture/master-system-machine-contract.md` sections 10 (PSG PCM) and 11 (input).

## Decision

1. **Reuse audit: no shared presentation helper; the Master System viewer is separate code.** The Genesis viewer is 337 lines
   of source (`viewer.c` 132, `viewer.h` 100, `viewer_sdl3.c` 88, `viewer_sdl3.h` 17). Its candidate shareable parts are the
   accumulated-deadline pacer (about 30 lines), the key-flags-to-mask translation (about 6 lines) and the SDL window, texture and
   present calls (about 35 lines). Checked against the concrete SMS needs:
   - the pacer is the only non-trivial piece. The Genesis version is bound to `GenesisViewerHost`, `GenesisPacer`
     (whose fields the Genesis tests read) and the M68k master-clock constants. A shared parameterized pacer needs its own host
     vtable, period type and constructor, and both platforms keep thin wrappers with their public names. That adds a header, a
     source file and two wrapper sets to save roughly thirty duplicated lines in one of them, and cannot shrink Genesis;
   - the SDL present path is entirely platform-specific: Genesis decodes CRAM entries through its VDP decoder into a fixed 320 x
     224 frame, SMS expands 6-bit CRAM bytes into a 192- or 224-line active area with aspect-preserving letterbox presentation
     and also owns an audio stream;
   - the key maps differ (three buttons plus Start versus two buttons plus pause and reset).

   The extraction rule of the task (shrink both, Genesis unchanged and not larger) therefore fails, and Genesis is untouched.
   Re-evaluate when a third consumer (for example SEG-032) needs the same pacer; the SMS pacer (`SmsPacer`) is written with the
   same algorithm and tests so a later extraction is mechanical.
2. **Layout.** `platforms/master-system/viewer/`: `sms_viewer.{h,c}` is the host-neutral, SDL-free core (rig composition,
   input recording, pacer, run loop, options, key map, script formatter) and is always built as strict C11;
   `sms_viewer_sdl3.{h,c}` is the optional SDL3 window/keyboard/audio adapter, compiled only under
   `SEGARECOMP_ENABLE_SDL3_VIEWER` (the option the Genesis viewer already defines), and `sms_viewer_main.c` is the viewer's
   generated-program `main`. Headless and runtime targets never reference SDL (asserted by a test).
3. **Guest loop.** One iteration is one guest frame: poll input, `sms_run_until_frame(frame + 1)`, `sms_psg_sync`, drain PCM, pace,
   present the newly completed frame. The guest run is the T003 machine loop; nothing in the loop reads wall time into guest
   state and the pacer only decides when to sleep.
4. **Input and equivalence.** Input is sampled once per iteration, at a deterministic frame boundary. The sample becomes a
   scripted-input event whose frame is the first frame start the machine has not applied yet (`ceil(next_line / 262)`), so the
   sample taken before running frame `k` takes effect at frame `k + 1` (frame 0 for the first sample), and the recorded
   `<frame> <p1> <p2> <pause>` text is exactly what the headless driver replays. The viewer rig uses the headless trace
   capacities (the VDP and machine digests contain trace entry counts). Reset re-runs the deterministic reset and restarts the
   recording and the viewer-side records; a run since the last reset is what a recorded script describes. The equivalence test
   (`sms_viewer_equivalence_test.py`) drives the viewer core with a fake clock, poll, presenter and audio sink on the generated
   `machine_e2e` image, replays the recorded stream headlessly and compares the state digest, every per-frame framebuffer hash
   (renderer records and presented frames) and the PCM hash and count, plus the post-reset case. It is hermetic (no window).
5. **Audio.** The PSG writes PCM (s16 mono 44,100 Hz) into the ring of the device library. After every frame the core drains
   the ring to the host audio callback; with no callback the ring is still drained. The SDL3 adapter feeds an
   `SDL_AudioStream`: at most about 100 ms (4,410 samples) may be queued, a chunk that would exceed it is dropped (overrun,
   counted), and a device that drains faster than the viewer produces plays silence (underrun). A failed open, a missing audio
   subsystem or `--viewer-mute` leaves the viewer silent. A refused chunk only increments a counter; nothing returns to guest
   time or state. Tests cover a failing sink and an absent sink (digest identical to a healthy sink).
6. **Presentation.** The framebuffer is the T005 active area (256 x 192 or 256 x 224 CRAM bytes), expanded by
   `sms_render_rgb888` and shown with nearest scaling through SDL's letterbox logical presentation, so the aspect ratio is
   preserved in a resizable window (square pixels; no 4:3 pixel-aspect correction). Keys: arrows = D-pad, Z = button 1,
   X = button 2, P = pause (a tap shorter than a frame still presses pause for one frame, giving one NMI edge), R = reset,
   Escape or window close = quit. Player 2 and gamepads are not mapped.

## Consequences

- SDL3 builds in the existing CI SDL3 viewer job: the job additionally builds `segarecomp_viewer_master_system_sdl3` and the
  compile-checked viewer `main`. Product acceptance of an interactive session is a manual note, not evidence.
- Build/launcher integration (linking the generated program with the viewer core and adapter) is T010.
- Game Gear presentation, gamepads, pixel-aspect correction and frame-blending are out of scope.
