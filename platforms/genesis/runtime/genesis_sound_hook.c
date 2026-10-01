/*
 * SEG-032-T008 (ADR 0073): the production seam of the final Genesis program with sound. The build compiles the UNMODIFIED
 * generated main TU with -Dgenesis_runtime_run=genesis_sound_hook_run (the same mechanism as the viewer and probe hooks), so the
 * generated main hands its own runtime to this function, which attaches the Z80 machine and the sound devices and then runs the
 * real runner. The build-time materialization pass links genesis_materialize_hook.c in place of this file: the generated units and
 * the image registry are identical in both programs.
 */
#include <stdio.h>
#include <stdlib.h>

#include "genesis_sound.h"

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance);

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance) {
  GenesisControlTransfer transfer;
  const GenesisZ80Machine *machine = genesis_sound_attach(runtime);
  transfer = genesis_runtime_run(runtime, dispatch, dispatch_allowance);
  /* Opt-in aggregate counters (counts only: never a byte, address or identity). */
  if (getenv("SEGARECOMP_SOUND_SUMMARY") != NULL) {
    const GenesisAudio *audio = genesis_sound_audio();
    fprintf(stderr,
            "SOUND_SUMMARY {\"epochs\":%u,\"bound_image\":%u,\"psg_writes\":%llu,\"ym_writes\":%llu,\"virtual_frames\":%llu,\"result_kind\":%d,\"stop_class\":%d,\"diagnostic\":%d}\n",
            (unsigned)runtime->z80_epoch.epoch_count, (unsigned)machine->bound_ordinal,
            (unsigned long long)(audio != NULL ? audio->psg_writes : 0U), (unsigned long long)(audio != NULL ? audio->ym_writes : 0U),
            (unsigned long long)(runtime->scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_FRAME), (int)transfer.kind,
            transfer.kind == GENESIS_STOP ? (int)transfer.stop.stop_class : 0,
            transfer.kind == GENESIS_STOP ? (int)transfer.stop.diagnostic_category : 0);
  }
  return transfer;
}
