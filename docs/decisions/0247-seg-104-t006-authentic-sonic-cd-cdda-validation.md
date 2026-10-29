# ADR 0247: SEG-104-T006 - authentic Sonic CD CD-DA validation on one frozen executable (gate `AUTHENTIC_CDDA_PLAYBACK_GO`)

- Status: Accepted (SEG-104 / M8, combined PR; internal task T006).
- Related: ADR 0244-0246, 0238-0243 (M7 routes and launcher integration), 0229 (frozen multi-route evaluation).
- Privacy: this record holds counts, frame numbers relative to the BIOS hand-off and classes only. No sample, track number, LBA, command schedule or soundtrack data is committed; the full evidence is in the ignored `.cache/seg104` area.

## The frozen executable

One native program, built once from the authorised local Sonic CD image and the Model 1 BIOS with `segarecomp sega-cd build --file-first structural` (no extents, no hints, no runtime compilation, no track configuration), at the product HEAD of the runtime and viewer sources used by every route below; headless `program` (119,626,288 bytes; +360 bytes on disk and +3,491 bytes of text against the M7 program) and `program-viewer` share the generated units (program identity `7caf4565...`). Its hash and directory inventory were verified unchanged after every route (`program_unchanged_after_all_routes = true`), the routes ran with an empty `PATH`.

## Route A - a real game-selected CD-DA interval, against the independent reference

The reference (the private oracle, instrumented to log every CD-DA read, every CDD command and every sample it plays; the same two START presses as the M7 R1 route; entry + 478 and entry + 598) was compared with the product's canonical stream (`--cdda-pcm`, headless, 6,000 onsets, 36 s).

| Measure | Result |
|---|---|
| runs in the reference / in the product | 2 / 2 (title music; first-level music), a single audio track each, selected by the game's own drive commands (19 plays, 14 pauses, 4 seeks, 2 stops, 4 N-track jumps) |
| start frames | both runs start in the **same** video frame in the reference and in the product (entry + 261 and entry + 997); the first run's last frame differs by one (the reference's last playing frame 1863, the product's stop observed at 1864) |
| run 2 (first-level music) | **2,797,116 consecutive sample frames (63.4 s), 0 differing frames**, including the fader scaling (target 784 / 1,024) |
| run 1 (title music) | the first 250,224 frames equal; 14,040 frames (0.32 s) of the fade-out differ (largest sample difference 493 of 32,767) because the product applies the fader write at the next sector boundary (quantisation up to one sector, ADR 0244) while the reference applies it at its next video-frame chunk; afterwards both are silent (the reference stops consuming a muted stream, the product emits exact zeros; 352 of the product's next 30,912 frames are non-zero at the very end of the fade) |
| product digest | `cdda.pcm_sha256` is the digest of the file; sectors 5,259 = frames 3,092,292 / 588; no sink refusal, no I/O error |
| silence | between the two runs (335 frames) and before the first run the product produced nothing |

The fader is not a no-op on this route: the game writes it once, and run 2 equals the reference only because the multiplier, the clamp and the ramp are the same.

## Route B - existing M5 / M6 / M7 routes with CD-DA output **enabled**

`tools/sega_cd_m5_routes.py` replayed R0, R0L, R1a, R1b, R1L, R2a and R2L (exactly the SEG-103 final command) on the frozen program with `--cdda-digest 1`. Compared with the SEG-103 results: **every route record is identical** apart from wall-clock seconds - executed module identities, verified dynamic bindings, CPU retirement outcomes, CDD state, DMA counters, the pre-registered frame / VRAM checkpoints and the typed stop of each route. The M7 qualifications are therefore unchanged and still recorded: R2L's late divergence (`reference frame 44300 / 44500 missing or different`), the sub-CPU timing-skew qualification of M6, and the graphics-ASIC byte-read frontier (`gate_array_register_unsupported` at the same frames, not touched). No earlier baseline was changed.

## Route C - audio controls

Authentic: the 6,000-frame route above contains pause (14), seek (4), stop (2), N-track jump (4) and play (19) commands, and the product reproduces both reference runs. Fixture (`sega_cd_cdda_native_test`): play, pause, resume (contiguous), a play that runs from track 2 through the non-silent INDEX 00 pregap of track 3 (silent) into its INDEX 01 start, seek (nothing heard), play at the target, stop; the stream equals an independent oracle byte for byte.

## The SDL3 viewer on the same executable

Run with SDL's `disk` audio driver (the device file is exactly what the SDL3 device played), the dummy video driver, real-time pacing, 2,400 onsets (title music, the START presses, the first frames of the level):
- the viewer's report is identical to the headless report except the `live` block, and its canonical digest is the headless digest (443,352 frames);
- **pause**: F6 held the machine for 800 polls (about 3.2 s); the report and the digest are again identical to the unpaused run; the device file holds an additional 141,232-frame silence at the pause instant, after at most the queued 100 ms, and the stream resumes;
- **delivery**: with a device draining 25 % faster than real time (`SDL_AUDIO_DISK_TIMESCALE=0.8`) every one of the 443,352 frames was offered and submitted (0 overrun, 0 refused, 0 discarded, 0 pending drops, 88 underruns) and the played stream, without silence, is the canonical stream except the last 456 frames that were still queued when the window closed. With the driver's default timing the disk device consumes more slowly than real time (file I/O per chunk) and the bounded queue drops one or two sectors about every 0.6 s (21,756 of 443,352 frames): the established Genesis policy (drop on overrun, count it) is applied and the machine's digest is unaffected; a real device's clock is a different matter and was not available here.
- A dummy or disk device is **not** a speaker: no claim is made that anyone heard the music in this evidence (ADR 0248).

## Performance and cost

| Measure | Value |
|---|---|
| sectors read / sample frames | 5,259 / 3,092,292 for 6,000 onsets; one `fseek` + 2,352-byte `fread` per sector, 12.4 MB |
| machine buffering | one sector (588 frames, 2,352 bytes on the stack); the viewer holds at most 4,096 frames (never exceeded: `pending_dropped` was 0 in every viewer run) |
| host queue | limit 4,410 frames (100 ms), as for Genesis |
| CPU overhead | none measurable: 36.98 / 36.80 s user without, 36.79 / 36.77 s with output enabled (the route is 2 x 36 s); the M7 routes ran 4-6 % longer in wall time with output on, within the spread of the machine |
| memory | peak RSS 26.4 MB with and without |
| program size | +360 bytes on disk, +3,491 bytes of text |

## What is not established

Longer play (a later level and its music change) was not scripted: the route to the second stage needs input beyond START and runs into the documented graphics-ASIC frontier region. The title-to-first-level music change is established. No manual listening on real speakers. Hardware-exact fader onset and ramp rate, and the sub-sector phase between the drive counter and the DAC, remain open (ADR 0244).
