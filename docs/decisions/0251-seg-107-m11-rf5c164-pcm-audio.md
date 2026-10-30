# ADR 0251: SEG-107 M11 - Sega CD RF5C164 PCM audio and sound effects

- Status: Accepted contract (T001); implementation and decision follow in the same PR (SEG-107 / M11, one combined PR; internal tasks T001-T005). Decision: see "Decision" (filled at T005).
- Related: ADR 0233 and 0237 (SEG-102: PCM sample clock and wave-address readback, unchanged), ADR 0244-0248 (SEG-104: CD-DA), ADR 0250 (M10; its `M10_HINT_PARTIAL` and the accepted R2 three-frame lag are not touched). Product PR #171 and harness PR #117 were verified merged before this work began.
- Privacy: counts, relative frame numbers and classes only; no ROM bytes, samples, soundtrack data or traces are committed. Full evidence is in the ignored `.cache/seg107`.

## T001 - the reference synthesis contract (gate `PCM_AUDIO_FEASIBLE`)

Sources read (observations, no code copied): Genesis Plus GX `core/cd_hw/pcm.c` (`pcm_run`, `pcm_write`) and MAME `src/devices/sound/rf5c68.cpp` (`sound_stream_update`, RF5C164 variant with 16 output bits).

| Question | Verified answer |
|---|---|
| wave sample encoding | one byte per sample, sign-magnitude, **bit 7 set = positive**, magnitude = bits 6-0 (GPGX `pcm_run`; MAME `sample & 0x80` adds). Value $80 and $00 are both zero |
| loop marker | byte $FF. On reading it the channel address becomes `LS << 11`; the byte at LS is read instead. If that byte is also $FF the channel outputs nothing (an infinite loop on itself). GPGX applies **no** address increment on the loop event; MAME adds the step after the jump (a pointer difference of one step per loop). The product keeps the SEG-102 pointer model (GPGX), which is the already-validated CPU-visible behavior |
| channel gain | per channel and side: `(sample7 x ENV x pan_nibble) >> 5`; left uses PAN bits 3-0, right PAN bits 7-4; ENV is 8 bits. Peak per channel and side: 127 x 255 x 15 >> 5 = 15,181 |
| rounding | GPGX shifts the signed product (floor); MAME shifts the magnitude then negates (toward zero). They differ by at most 1 LSB per channel and side for negative samples. Product: GPGX (floor); the independent test reference uses MAME's rule and allows <= 1 LSB per active channel |
| sum and saturation | all running channels are summed per side; the sum is clamped to [-32768, 32767] (GPGX limiter; MAME `put_int_clamp`). No per-channel clamp and no master attenuation (GPGX `pcm_volume` is a user setting, default 100 %) |
| chip enable | CTRL bit 7 (written with every CTRL write, whichever of channel/bank it selects). Disabled: output is silence (GPGX mutes; MAME returns before mixing). Addresses do not move while disabled (SEG-102, unchanged) |
| channel ON/OFF | register $08 is active-low: a clear bit = channel ON. Turning OFF reloads the address from ST; ST while OFF reloads too |
| sample clock | 1 sample per 384 sub-CPU cycles = 12,500,000 / 384 = 32,552.08 Hz native (GPGX `PCM_SCYCLES_RATIO`; MAME `clock/384`); output is the same rate in both |
| ordering | per channel: read the byte at the current address, handle the loop, **then** advance (GPGX and MAME agree; the sample is taken before the pointer moves) |
| relation to 44.1 kHz | GPGX resamples the native stream with a band-limited blip buffer; MAME with its stream resampler. Neither is hardware and both are host policy. Product: zero-order hold of the latest native sample at the guest-time instant of each 44.1 kHz frame (below) |
| mixing with CD-DA | both are summed per side and saturated in the host mixer (GPGX/MAME). CD-DA keeps its fader; PCM has no further global gain |

### Product synthesis and mixing rule (decided)

- One sample clock: the existing event grid `origin + k x 384 sub cycles` of `scd_pcm_sync`. Each event produces one stereo sample (the byte is read before the address moves). No second clock or address progression exists.
- One output timeline: 44.1 kHz frame `n` is at guest time `T_n = floor(n x ticks_per_second / 44100)` from machine tick 0, computed incrementally with an exact integer remainder. Frame `n` takes the latest native sample whose event time is `<= T_n` (zero-order hold; nothing before the first event, and silence while the chip is disabled or no channel runs).
- CD-DA enters the same timeline: a sector arriving at tick `t` is queued after the output has been advanced to `t`; its 588 frames are consumed one per output frame (a bounded 2,048-frame queue; contiguous 75 Hz sectors are seamless because 588 x ticks_per_second / 44100 = ticks_per_second / 75 exactly).
- Output = saturating sum of the (already fader-scaled) CD-DA frame and the PCM sample. With one of the two silent the other passes unchanged.
- Advancement points are guest events only (CD-DA sector, video-frame event, PCM register access, chip reset), so the stream cannot depend on scheduler slicing; audio output is a one-way sink that never reaches the machine.
- Generation is optional (`pcm_audio`); disabled, the machine and every report are exactly as before.

### Authentic observation (aggregate, frozen SEG-106 program, R1 route, 8,000 frames after the hand-off)

The game loads wave RAM (about 124 KB written by the sub CPU), programs channels with ENV $FF and PAN $FF, start-address and loop registers, and a nonzero frequency step (about 0.49 sample per tick, i.e. about 16 kHz playback), and turns a channel ON from frame 473 to 751 after the hand-off (one run, one channel), alongside the CD-DA music (2 runs). The game therefore does configure and use the PCM channels for audible output. Further effects need gameplay input beyond this route; none is claimed here.

### Gate

`PCM_AUDIO_FEASIBLE`: the encoding, loop, gain, saturation, enable and ordering rules agree between two independent implementations; the only differences (negative rounding, post-loop increment) are bounded and documented. The 44.1 kHz conversion is a documented host policy, not a hardware claim. Hardware-exact analog output (DAC, filtering, RF5C164 output level relative to CD-DA) is not established.
