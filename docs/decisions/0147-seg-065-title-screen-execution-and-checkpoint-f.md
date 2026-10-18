# ADR 0147: SEG-065 - from the third application module to the Sonic CD title screen, and the Checkpoint F decision

- Status: Accepted (SEG-065-T002, SEG-065-T003). Builds on ADR 0142-0145 (verified broad AOT, authentic BIOS boot, the handoff,
  Checkpoint E) and is judged against the pre-registration of ADR 0146, which it does not change.
- Scope: the authentic BIOS and the Sonic CD (USA) application, generated-native, from the SEG-064-T005 frontier to the title screen,
  and the comparison of the pre-registered title frames with the independent reference. It does **not** claim gameplay, audio output,
  the Z80 sound driver's execution, the intro movie, or any frame other than the three pre-registered ones.
- Privacy: no BIOS or disc byte, hash, address, extent coordinate, file name, frame image or frame digest appears here or in any
  test. The comparisons were made locally; only equality results, counts and capability classes are reported. The regression and
  capability tests use project-authored fixtures and hand-derived expectations only.

## Context

ADR 0145 accepted Checkpoint E for a 60-frame interval after the application entry and recorded the next runtime-selected frontier:
3.3 frames later the main processor enters a third disc-loaded module with no compiled authority (`code_bytes_mismatch`). ADR 0146
fixed the Checkpoint F schedule before any work past that point: no controller input, empty cartridge slot, a formatted empty internal
backup RAM, and the frames completed at application entry + 420, + 480 (primary) and + 540 NTSC frames, compared in a canonical form.

## Decisions

1. **Executable regions: the established extent mechanism, extended by content-aware coverage.** The frontier reproduced exactly with
   the official driver (same class, processor and instant). Four more disc extents resolve every executable region the application
   reaches through the primary frame: the backup-RAM initialisation module's main and sub parts and the title's main and sub parts,
   each a range of the verified disc mapped at its load base (main modules overlay the same base as the earlier ones; the live bytes
   select the image per instruction, ADR 0145 decision 3). Ten extents in all (limit 16). They were found, and their completeness
   established, by a *content-aware* check: every instruction-stream word the reference fetched from RAM after the entry, at its
   address and with its value, must equal the bytes of some declared extent at that address (only the BIOS work-RAM vector thunks,
   already admitted by operand templates, and the verified sub BIOS epoch are outside the extents). An address-only coverage check
   is insufficient once modules overlay one base; it missed the backup-RAM module, which the executed frontier then exposed.
2. **Backup RAM is a machine input.** `--backup-ram FILE` (exactly 8 KiB; anything else is refused before any guest instruction,
   exit 22) installs the internal backup RAM; sub accesses to `$E0000-$EFFFF` (mod 1 MiB) then follow the odd-byte-lane shape
   (contract section 4). Without the input every access stays the typed stop `backup_ram_unsupported`, so every earlier program and
   proof is unchanged. `tools/sega_cd_backup_ram.py` authors the formatted, empty image of ADR 0146 decision 2. The application reads
   and writes it (its save-data initialisation) before the title.
3. **CD-DA play is drive state (BIOS-image profile).** The title starts its audio track. A 75 Hz tick on an audio track no longer
   stops: from the track start the CDD control register reports "audio playing", the decoder runs without a sector and the position
   advances (GPGX `cdd_update`). Audio output is not modelled; nothing the processors read depends on it. The synthetic profile keeps
   `cd_read_audio_track` (SEG-062 behaviour unchanged).
4. **68K->VDP DMA duration (BIOS-image profile).** ADR 0145 recorded "VDP DMA has no duration" as a deviation to revisit if it became
   observable. It did: without it the title screen's first frames were identical to the reference and time-aligned, but once its
   animation loop ran the generated program advanced the animation about 30 % faster (the reference holds frames while the main
   processor waits for its per-frame transfers). The transfer now freezes the main processor for GPGX's slot-model duration
   (contract section 12) and the length/source registers update after it. With this single change every compared frame is identical
   and time-aligned. A fill still has no duration; the data of a transfer is still written at once. The capture's `vram_display_changes` is an
   endpoint comparison, not a VRAM-write trace: it counts the bytes that differ between VRAM at the start of active display and VRAM
   at the vertical-blank onset, and it was 0 at every compared frame. It does not establish that no VRAM write occurred during the
   display (a write restoring the original byte, or a change not surviving to the onset, is invisible to it); the frame equality
   below rests on the canonical comparison with the reference, not on this diagnostic.
5. **Frame capture, reusing the renderer.** `--capture-onset N` (up to 8) with `--capture-dir` samples the VDP register file, CRAM
   and VSRAM at the start of every active line of the frame whose display ends at vertical-blank onset N, renders it with the
   existing `genesis_vdp_produce_frame_raster`, and writes the ADR 0146 canonical frame plus VDP, CPU and RAM state (local
   diagnostics). It is an observer only: a line instant at which no device event is due brings nothing up to date (the graphics ASIC
   and CDC transfers are synced lazily, so an extra sync point would otherwise make the run depend on the capture set), no guest state
   changes, and the result is identical under deadline partitions and capture sets. Shadow/highlight and the palette-select mode are refused (`vdp_state_unsupported`) because
   the renderer does not model them; neither occurs in the compared frames. No second video pipeline exists.
6. **Frame alignment.** (Resolution: the title holds some frames for two or three video frames, so an exact match fixes the offset
   within [0, +1] frames; the pre-registered rule - equality at the primary frame, time-aligned - is met exactly.) GPGX's frame begins at the vertical-blank onset; the N-th onset after the application entry closes the
   display of the reference frame "entry + N" (the entry lies in the active display on both sides: line 112 and line 110). The
   compared onsets are therefore the generated program's (onsets at the entry) + 420 / 480 / 540.
7. **The Z80 is still not executed.** Nothing in the run reads the Z80 area after the Z80 became runnable (that read is a typed stop
   and none occurred); the frames are identical without it. Audio and the sound driver remain outside every claim.

## Result

**Checkpoint F: `TIME_ALIGNED` (ADR 0146 decision 7).**

**Authentic build** (authorized US Model 1 class BIOS, Sonic CD (USA) disc, formatted empty backup RAM, no input; official driver,
cold, gcc -O1 strict C11, 12 jobs): the epoch + 13 operand templates, 14 rounds + the confirming run, 1,046 s; 178,005 instruction
functions, 176 MB of C in 39 translation units; peak compiler resident set 922 MB, emission 546 MB. The program runs from reset to
application entry + 541 frames with **no typed stop** (result `deadline`): 1,846 68K->VDP transfers (the main processor waits for
them), 1,312 graphics-ASIC operations, 1,768 backup-RAM reads and 1,584 writes, the title's audio track playing as drive state.

| check at the pre-registered frames (entry + 420 / 480 / 540; private GPGX reference, same schedule) | result |
| --- | --- |
| canonical frame (256 x 224, digest and dimensions) | **identical at all three**, time-aligned (offset 0) |
| VDP registers 0-23, CRAM, VSRAM, VRAM (64 KiB) | identical at all three |
| Word RAM (256 KiB) | identical at all three |
| sub registers | only the pc differs, inside its polling loop |
| main registers | the stack pointer and SR equal; the pc is inside the same short loop on both sides (iteration phase), so its counters and pointers differ |
| work RAM / PRG RAM | 81-160 bytes in that loop's destination buffer and the stack residue below the stack pointer / 128-138 bytes in the BIOS status area and the sub stack: the classes ADR 0145 already recorded; none is read by the frame |
| VRAM at the start of active display against VRAM at the vertical-blank onset (endpoint comparison, `vram_display_changes`; not a write trace) | byte-identical (0 differing bytes) at all three frames; the canonical frames independently match the reference |
| the frames before the title loop (entry + 200 ... 370, 8 samples) | identical and time-aligned, with and without the DMA duration |
| without the DMA duration (decision 4) | frames from entry + 380 on run ahead (about +18 frames at + 420, +36 at + 480): the cause, not tuned around |
| determinism | one outcome (report, three canonical frames and every state dump) over 2 repeats, 12 seeded deadline partitions and 2 fixed slices (16 runs); the end-of-run report is identical with no capture and with eight other captures (two of them still sampling at the end) |
| reference reproducibility | two reference captures identical at every selected frame |

**Earlier proofs, re-verified under the new runtime.** T004 handoff regression (the stop-enabled program, its three original extents,
rebuilt: 14 rounds): `application_handoff`; main and sub registers all equal (the sub pc now equal too), Word RAM and PCM identical,
27 work-RAM and 44 PRG-RAM bytes - the ADR 0144 classes. T005 interval (60 frames, no backup RAM): no typed stop; executed
instruction starts identical to the reference (main 1,724 = 1,724; sub 3,519 = 3,519); PCM identical; work RAM 27 and PRG RAM 103
bytes, Word RAM the same one-sector lead as ADR 0145 - Checkpoint E unchanged. Synthetic profile and every SEG-062/063/064 fixture:
unchanged (focused Sega CD suites pass).

**New project-authored tests.** `sega_cd_backup_ram_test` (bus table: every access shape, mirrors, fail-closed default; the authored
image; the program option and its refusals), `sega_cd_vdp_dma_timing_test` (hand-derived durations for both windows, both widths,
VRAM/CRAM/VSRAM, display off, mid-line start, frame crossing, length 0; the retire rule; the register update including its 16-bit
wrap; fill without wait), CD-DA rows in `sega_cd_subside_test` (pregap, track start, position report, track end, END, synthetic stop
unchanged).

**Claims kept distinct.** *Fixture*: the tests above. *Authentic BIOS boot*: Checkpoint E (ADR 0145), re-verified. *Bounded commercial
execution*: the application from the entry to entry + 541 frames under the ADR 0146 schedule. *Checkpoint F*: the three title frames
and their VDP state, identical to the independent reference. Not claimed: audio, the Z80, input handling, anything after entry + 541.

## Consequences

- Checkpoint E and Checkpoint F are distinct claims; this ADR changes neither the T004 handoff result nor the T005 interval claim (re-verified
  under the new DMA timing, see Result).
- Further progress (pressing START, gameplay, the intro movie, audio) is new work; the extent, backup-RAM and capture mechanisms apply.
