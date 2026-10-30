# ADR 0244: SEG-104-T001 - CD-DA source format, drive-timed sector correspondence and the authentic baseline (gate `CDDA_FORMAT_AND_TIMING_GO`)

- Status: Accepted (SEG-104 / M8, combined PR; internal task T001).
- Related: ADR 0129 (disc media), 0147 (Sega CD devices, drive state), 0177 (machine timing, the 75 Hz grid), 0233-0237 (PCM state, unchanged), 0238-0242 (M7).
- Prerequisite verified: SEG-103 product PR #168 merged as `f90d9fe` before any SEG-104 work; the accepted `M7_GO` is preserved.

## Problem

The Sega CD runtime already models the drive: tracks from the build-time disc table, 75 Hz ticks, play / seek / pause / resume / stop, track and end-of-disc transitions, the CDD control byte that says "audio is playing". An audio-track sector advances that state and produces no sound. M8 makes the drive's own sectors audible without changing the drive. Before any scheduling code is written the supported source format and the sector-to-sample correspondence must be fixed and checked against evidence that is independent of the code under test.

## Source format (verified)

| Item | Decision | Evidence |
|---|---|---|
| sector | 2,352 bytes = 588 stereo frames; 75 sectors per second = 44,100 frames per second | Red Book; project-authored fixtures (one sector = 588 frames, 75 sectors = 44,100) |
| sample | signed 16-bit, **left then right**, each sample **least-significant byte first**; decoded byte by byte, so the result is independent of host endianness | Red Book / BIN audio layout. A project-authored disc whose left and right differ at every frame and whose first frames are -32768 / 32767, `0x0001` / `0x0100`, `0` / `-1` is read back exactly (`sega_cd_cdda_test`, `sega_cd_cdda_native_test`: swapping channels or bytes fails). On the authentic disc the product's stream equals the independent reference's over 2,797,116 consecutive frames (ADR 0247), which a channel swap or byte reversal could not do |
| supported container | only what the disc model accepts: a `FILE ... BINARY` audio track with 2,352-byte sectors. `WAVE`, MP3, OGG, FLAC and other file types are the CUE ingestion refusal `cue_unsupported` (existing test); CD+G and subcode are not read | `libs/media/src/disc.cpp`, `tests/sega_cd_disc_ingestion_test.py` |
| never audio | a data track (Mode 1, 2,048 or 2,352), any LBA outside the table | the reader returns the typed errors `disc_cdda_format` / `disc_lba_out_of_range` |

## Track mapping (the disc table is the authority)

`ScdDiscTrack` carries `file_index`, `file_offset` (bytes of INDEX 01), `first_lba` (INDEX 01), `sector_count`, `sector_size`, `type`. The CUE is not parsed again at run time. Consequences, all exercised by fixtures:

- single-file BIN/CUE and separate files (a track in a second file is read through that file's verified handle); several tracks in one file;
- `INDEX 01` is where the track's data begins and where the drive reports "audio playing". An in-file `INDEX 00` pregap and a virtual `PREGAP` both lie before `first_lba`: the drive reports no audio there, so **nothing is produced for them** (the authentic disc has 34 audio tracks, every one with a 2-second `INDEX 00` pregap; a fixture fills that region with non-silent bytes to prove it is never heard);
- track boundaries, audio to audio (contiguous when no pregap separates them), audio to data and end of disc follow the drive state.

## Drive timing

A 75 Hz drive tick with status PLAY on an audio track, the LBA at or after INDEX 01, and **no play/seek command pending** produces the sector at that tick's LBA (then the LBA advances, as before). The correspondences:

| Event | Behaviour |
|---|---|
| play / seek command | the drive reports SEEK at once and the command takes effect on the next tick; production stops at the command and a tick with a command pending is silent (so no sample of the old position is produced after the new state is reported). Seek latency is the existing latency counter: no audio until it is over. Seek pauses at the target (nothing is heard until a play) |
| pause (6), N-track jump (A), stop (1) | production stops at the command |
| resume (7) | continues at the drive's LBA: no replay, no skip |
| data track, `END`, peripheral reset | no production; reset also returns the fader to unity |

Nothing in production alters the drive: the CDD registers, status, LBA, the device log and the CDC are untouched (`sega_cd_cdda_native_test` compares the complete report with output on and off).

## Reference evidence and its limits

An independent reference (the private Genesis Plus GX driver used since M2, consulted for behaviour only, no code copied) was instrumented to log its CD-DA reads, its CDD events and every sample it plays. Separation of what is established:

- **Independently verified against the reference on the authentic Sonic CD**: the frame at which CD-DA starts (both runs), the frame at which it stops, the order and boundaries of the runs, and exact sample equality (ADR 0247).
- **Reference-emulator conventions adopted, not hardware-verified**: audio gated by the drive's "playing" byte; the fader's one-step-per-sample ramp and its multiplier table `v & $7FC` (the reference cites the LC7883KM datasheet; it is not hardware-measured here); targets above unity are clamped (our choice: no amplification exists).
- **Not established by any evidence here**: the sub-sector phase between the drive's LBA counter and the DAC output (the reference consumes audio by video frame, hardware continuously); the audibility of the INDEX 00 region (the drive reports "not playing"; we follow the status); the real fader ramp rate. A fader write takes effect at the next sector boundary, a quantisation of at most one sector (13.3 ms) in the onset of a volume change; the ramp itself is the reference's.

## Authentic baseline (aggregate; no commercial data)

With the local authorised Sonic CD image and the Model 1 BIOS, in the reference (entry-relative video frames; entry = the BIOS hand-off):
- no input (R0): CD-DA starts at entry + 261 (the title);
- two START presses (R1): the title interval runs entry + 261 to entry + 661; the gameplay interval starts at entry + 997 and plays for more than 3,800 further frames (63 s) in the first level; both are single audio tracks selected by the game's own drive commands (play, pause, N-track jump, stop; 19 plays, 14 pauses, 4 seeks, 2 stops, 4 jumps in 6,000 frames); a fader write sets the level target to 784/1024 around the title-to-level transition;
- track numbers, LBAs, command schedules and the sample data stay in the private working area (`.cache`, never committed).

## Gate

`CDDA_FORMAT_AND_TIMING_GO`: the format is exactly established by independent oracles; the sector-to-tick correspondence reproduces the reference's start and stop frames and 2.8 million exact frames. The three open points above are recorded as limitations, not guessed.
