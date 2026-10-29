# ADR 0248: SEG-104 M8 decision - deterministic CD-DA playback and SDL3 audio

- Status: Accepted. Decision: **`M8_PARTIAL`**.
- Related: ADR 0244-0247. M7 (`M7_GO`, PR #168) was verified merged (`f90d9fe`) and is preserved.

## Why PARTIAL, not GO

Everything technical required for `M8_GO` is established: exact source format and channel/byte order; drive-timed, deadline-partition-independent production; bounded buffering; headless artifact and digest; viewer delivery through the existing SDL3 sink with the viewer's volume/mute; a project-authored end-to-end fixture; authentic Sonic CD title and first-level music reproduced against the independent reference (same start frames; 2,797,116 exactly equal frames); the M5/M6/M7 routes byte-identical with output enabled; no interpreter/JIT; independent review `DELIVERY: PASS` (nine minor findings, none a defect of the claims); fast preset 192/192 and 10 focused suites. Not met:

1. **Exact-head GitHub CI has not run.** The PR is still a draft, for which the workflow skips its test jobs, and marking it ready was not permitted in this session. `M8_GO` requires it.
2. **No listening on real speakers.** Only SDL's dummy/disk devices were used; no one heard the music.
3. Fader onset is quantised to the next sector (<= 13.3 ms) and the ramp/multiplier are the reference's conventions; the sub-sector DAC phase is unverified (ADR 0244). The first-run fade interval therefore differs in 6,956 frames.
4. A later level's music change was not exercised (title-to-level was).

## Review findings, disposition

Dangling ADR 0248 and the run-1 count: fixed here. Open and recorded, not changed (the program is frozen; changing the runtime would void the authentic evidence): `sink_refused` counts viewer drops so a viewer report can differ in that counter; sectors produced before the window opens are not submitted; `--cdda-log` alone does not enable output; `SCD_RUNTIME_ABI` unchanged, so a prebuilt pre-SEG-104 program is silent; the viewer test compares non-zero samples only (the exact claim rests on the equal digest); under a slow host device the bounded queue drops whole chunks (4.9 % with SDL's disk driver at default timing).

## Supported / unsupported / remaining

Supported: 2,352-byte BIN audio tracks, BIOS-image profile. Unsupported: WAVE/MP3/OGG/FLAC (typed `cue_unsupported`), CD+G, synthetic-profile CD-DA (typed stop). Remaining toward complete Sega CD sound: RF5C164 PCM output, YM2612/PSG/Z80, hardware-exact fader, real-device drift handling.

The PR stays open, unmerged, auto-merge disabled. No successor milestone.
