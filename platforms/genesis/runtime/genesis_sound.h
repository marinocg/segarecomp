/*
 * SEG-032-T008 (ADR 0072/0073): the Genesis sound subsystem of a built program. One attach point shared by every generated-program
 * hook (the headless sound hook, the build-time materialization pass hook and the viewer hook): the Z80 machine with its compiled
 * image registry, and the shared PSG/YM2612. Plain C11; linked only into programs that carry sound (the runtime never includes it).
 */
#ifndef SEGARECOMP_GENESIS_SOUND_H
#define SEGARECOMP_GENESIS_SOUND_H

#include "genesis_audio.h"
#include "z80_machine.h"

/* Attaches the Z80 machine and the audio devices to `runtime` once (a second call is a no-op: the generated main re-enters the
 * runner for unbounded runs). Returns the Z80 machine. */
GenesisZ80Machine *genesis_sound_attach(GenesisRuntime *runtime);

/* The attached devices (NULL before the first attach): read-only access for aggregate counters. */
const GenesisAudio *genesis_sound_audio(void);

/* Releases the devices (after the last run). Safe without a prior attach. */
void genesis_sound_detach(void);

#endif
