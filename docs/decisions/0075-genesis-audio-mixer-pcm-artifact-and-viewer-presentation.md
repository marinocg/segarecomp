# ADR 0075: Genesis Audio Mixer, Deterministic PCM Artifact and Viewer Presentation

- Status: Accepted (SEG-032-T009)
- Date: 2026-10-02
- Related: ADR 0070 (viewer audio policy), ADR 0072 (platform integration, scheduling), ADR 0074 (YM2612),
  `docs/architecture/genesis-z80-audio-contract.md` sections 11 and 17.

## Decision

1. **One mixer, one core.** `genesis_mixer.[ch]` implements the frozen section 11 format exactly: 44,100 Hz, stereo, s16le, integer box filter
   against master-tick windows, PSG unipolar `0..8192` equal in L and R, `clip16(ym + psg)` with a clip counter, SHA-256 over the canonical
   bytes followed by `u64le frame_count`. It has no allocation, I/O, host clock or environment access. The headless program hook and the viewer
   hook share it through the one `GenesisAudio` owner (the devices deliver native samples into it); the viewer only drains its ring.
2. **Source delivery and synchronization.** Native samples are delivered with their start tick. The PSG is stepped tick by tick by the audio
   owner; both devices are also advanced from the Z80 synchronization point (new optional `GenesisAudioHooks.sync`, invoked by the Z80 machine
   hook after `run_to`) so audio exists even when no register is accessed. This only runs devices forward to a time no access can still
   precede; the cadence-invariance tests (quanta 1 to 65,536) and the slice-size equivalence tests prove it.
3. **Closure rule.** A window closes for a source when the source's watermark reaches its end; a frame needs both. Consequently the stream and
   frame count depend only on the native streams and the final guest time, not on chunking, ordering or slicing. A source running too far ahead,
   a non-monotonic native or a time overflow is a typed mixer fault (fail closed), not silent corruption.
4. **Gains unchanged.** No cited reference for different PSG/FM balance exists, so the T001 constants stand (contract U5 closed as a project
   decision). Mutation controls (wrong rate, swapped channels, wrong PSG gain, rounding instead of truncation, dropped writes) change the digest.
5. **Headless artifact.** The sound hook flushes (`genesis_sound_finish`) and reports aggregates in `SOUND_SUMMARY`; `SEGARECOMP_AUDIO_PCM_OUT`
   writes the canonical PCM and `SEGARECOMP_AUDIO_RANGE` digests a frame range. A malformed range exits 3 before the run; an unusable PCM path
   never perturbs the run. `status.json` stays a build artifact (a build runs no program), so aggregates live in the run summary.
6. **Viewer.** The platform-neutral policy (`genesis_audio_present.c`) is separate from the SDL3 stream (`viewer_sdl3.c`). The viewer core gains
   one optional `after_slice` observer callback; the hook drains the ring there. Wall time decides only when SDL consumes; no value returns to
   the runtime. `SEGARECOMP_VIEWER_MUTE=1`, a failed device open, a refusing or full sink only change presenter counters.
7. **Pacer extraction (deferred by ADR 0070): not done.** SEG-032 adds no third pacer: the Genesis viewer already existed and keeps its own
   `GenesisPacer`; the SMS viewer keeps `SmsPacer`. The audio presentation policy is new (not duplicated code), and extracting the pacer would
   still add a header, a source and two wrappers to save about thirty lines without shrinking Genesis. Re-evaluate only when a platform with a
   different viewer host appears.
8. **SHA-256.** The mixer uses the runtime's own `GenesisSha256` (runtime.h). Reusing the Master System implementation was tried and rejected by
   `sms_dependency_gate_test`: the Genesis tree must not reference an SMS product.

## Consequences

- Evidence: `genesis_audio_mixer_test` (independent reference, boundaries, clipping, ring, faults, mutations), `genesis_audio_pcm_test`
  (whole machine vs the YM2612 and PSG libraries driven directly), `genesis_audio_equivalence_test` (headless vs the real viewer core with fake
  clock and sinks, wall-clock scan) and `genesis_audio_artifact_test` (built program: digest vs file, range digest, repeats, `--jobs`, prefix).
- Not exercised in automation: a real audio device. The SDL3 adapter was run only against SDL's dummy audio and video drivers.
