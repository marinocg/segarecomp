# ADR 0286: Audio modding architecture

- Status: accepted (SEG-145-T001, combined delivery F01). Contract: [modding-audio-architecture.md](../architecture/modding-audio-architecture.md).

## Decision

1. Two mechanisms: guest-native modification (original hardware behavior, statically valid guest data/code, declared mappings) and optional enhanced host audio (no guest effect, explicit presentation, spike-gated).
2. Existing hardware (YM2612, PSG, DAC, RF5C164, CD-DA, mixer) is never a task. The Master System YM2413 is unsupported hardware outside this milestone.
3. The only shared concepts are the declared mapping, the audio asset class, the audio capability names (`audio.event.read`, `audio.command.send`, `audio.host.play`) and the event phases. Everything else is console-specific; no universal tracker or chip abstraction.
4. The audio asset declaration and mapping shapes are fixed now, inside schema 3, so SEG-145 needs no schema revision.
5. No commercial audio in packages or repositories.

## Consequences

SEG-145-T002 and T008 implement parsers and APIs against these shapes. Rejected alternatives: a universal sound-driver emulator; a host audio path that influences guest timing; modelling YM2413 inside this milestone.
