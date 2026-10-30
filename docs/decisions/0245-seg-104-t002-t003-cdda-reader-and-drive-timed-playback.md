# ADR 0245: SEG-104-T002/T003 - the CD-DA sector reader and the drive-timed playback state (gates `CDDA_SECTOR_READER_GO`, `CDDA_DETERMINISTIC_PLAYBACK_GO`)

- Status: Accepted (SEG-104 / M8, combined PR; internal tasks T002 and T003).
- Related: ADR 0244 (format and timing), 0129 (disc media), 0147 / 0177 (drive state and the 75 Hz grid).

## Decision

**T002 - `scd_disc_read_cdda(disc, lba, int16_t out[1176])`** (`scd_disc.c`). One function, no media subsystem. It resolves the LBA through the existing `scd_disc_track_of`, requires an audio track with 2,352-byte sectors, checks `offset + 2352 <= verified file size` and the existing `long` seek bound before touching the file, reads exactly one sector with the file handle `scd_disc_open` verified (no second identity check), and decodes the 1,176 samples byte by byte (little-endian, left first). Refusals are typed: `disc_lba_out_of_range`, `disc_cdda_format` (a data track, or a non-2,352 audio track), `disc_io` (range beyond the file, seek failure, short read). Storage is one 2,352-byte stack buffer: no track is ever cached whole.

**T003 - `scd_cdda.c` and four calls from the drive.** The drive tick already owns the timeline, so there is no second scheduler:
- in the audio branch of `scd_cdd_tick`, after the existing state updates, `scd_cdda_play(m, lba, track)` runs when output is enabled, the LBA is at or after INDEX 01 and no command is pending;
- silent ticks (latency, pause, seek target, stopped, data track, past the end, INDEX 00 region, pending command) call `scd_cdda_interrupt`; the CDD commands stop (1), play (3), seek (4), pause (6) and N-track jump (A) interrupt at the command; the peripheral reset ends playback and restores the fader.

`ScdCdda` (in `ScdMachine`) holds the optional sink, an `enabled` flag, the continuity state and plain counters (sectors, frames, runs started and ended, track changes, pregap ticks, silenced pending ticks, refused sectors, commands by code, the first and last vertical-blank onset). Production has no buffer: a sector is converted, scaled and handed to the sink in the tick that produces it, so memory is bounded by one sector regardless of the deadline slice (a 400-command stress, and runs under deadline slices and seeded partitions, produce identical output). Output disabled (`enabled = 0`) reads nothing from the disc and changes nothing the machine does: the device log, CPUs, memory digests and CDD state are bit-identical with output on, off, or with a refusing sink.

A disc read failing while output is enabled is the typed stop `cd_read_failed` (existing class). A disabled output cannot fail. An absent, failing or muted audio device is not an emulation event: the viewer only counts it.

**Fader** (`$FF8034`, previously acknowledged and ignored). The authentic route writes it (the game sets the level for the first stage), so it is implemented, minimally and as documented in ADR 0244: 12-bit target in bits 15-4 clamped to unity, one step per produced sample frame, multiplier `v & $7FC` (`v & 3` below 4), truncating division by 1,024; unity at power-on and peripheral reset; byte writes compose the register; volume 0 is true silence while the position still advances (the reference stops consuming a muted stream; the hardware keeps rotating). The viewer's volume/mute is host playback gain and is never mixed into this.

## Not done (by design)

No new mixer, no sample-rate conversion, no compressed or WAV tracks, no CD+G, no RF5C164 output, no change to the drive's commands, latencies or registers, no Sonic-specific data.

## Tests (`sega_cd_cdda_test`, native C against the real runtime; ASan/UBSan clean)

Reader exactness against a closed-form oracle on a two-file disc (extremes, byte-order-sensitive values, channel-distinct frames), data-track and pregap refusals, over-claiming tables; power-on silence; one sector = 588 frames and 75 sectors = 44,100 exactly; play, pause (no audio), resume (contiguous), seek (nothing until play, then exactly the target), stop, data-track PLAY, peripheral reset with fader reset, track boundary across the INDEX 00 pregap into the next file, end of disc; repeated start/stop determinism; the fader ramp sample by sample against the closed form; clamp; byte-composed register; true silence at volume 0; refusing-sink and disabled-output equality of the machine state; 400 play commands over a long jump.
