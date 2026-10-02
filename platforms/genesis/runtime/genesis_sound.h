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

/* SEG-032-T009: the mixer of the attached devices (NULL before the first attach). The only consumer-side mutation allowed is
 * draining its ring (genesis_mixer_ring_read). */
GenesisMixer *genesis_sound_mixer(void);

/* End-of-run flush: unless `guest_stopped`, runs the Z80 to the runtime's guest time (the same step any later synchronization would
 * take), then runs both devices to that time so the mixer holds every frame whose window ended. The frame count is therefore a pure
 * function of the final guest time. Safe without a prior attach. */
void genesis_sound_finish(GenesisRuntime *runtime, int guest_stopped);

/* Releases the devices (after the last run). Safe without a prior attach. */
void genesis_sound_detach(void);

#endif
