/*
 * Local demonstration aid (ADR 0291, docs/testing/genesis-native-gems-validation.md, section 9): save a state WHILE a native-GEMS program is playing, keep running, restore the state and
 * run the same stretch again, and require the two continuations to be identical. It replaces the headless sound hook of a program built with `--keep-work 1`
 * (tools/genesis_state_demo.py builds and links it); it is never part of a product build or of CI.
 *
 *   SEGARECOMP_DEMO_PRE=<dispatches before the save>  SEGARECOMP_DEMO_SPAN=<dispatches compared after it>  SEGARECOMP_DEMO_SOURCE_SHA=<64 hex: identity of the image>
 *
 * It prints counts and equality verdicts only: no byte of the image, no sample and no digest value.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_audio.h"
#include "genesis_mixer.h"
#include "genesis_snapshot.h"
#include "genesis_sound.h"
#include "segarecomp/device/sega/ym2612/ym2612.h"
#include "z80_machine.h"

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance);

typedef struct Observation {
  uint8_t mixer[32], driver[32];
  uint64_t ym, master, frames, dac_samples, commands, sequences, ticks;
  int active, non_silent;
  const char *fault;
} Observation;

static int run_dispatches(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint64_t count) {
  while (count != 0U) {
    const uint32_t slice = count > 4096U ? 4096U : (uint32_t)count;
    const GenesisControlTransfer t = genesis_runtime_run(runtime, dispatch, slice);
    if (t.kind != GENESIS_RUNNER_RESOURCE_LIMIT) return 0;
    count -= slice;
  }
  return 1;
}

static void observe(GenesisRuntime *runtime, Observation *o) {
  GenesisZ80Machine *machine = genesis_sound_attach(runtime);
  GenesisAudio *audio = genesis_sound_audio_mutable();
  GenesisZ80NativeReport report;
  memset(o, 0, sizeof *o);
  genesis_sound_finish(runtime, 0);   /* the same flush the program does at its end: both devices to the guest clock */
  genesis_mixer_digest(&audio->mixer, o->mixer);
  o->ym = ym2612_state_digest(audio->ym);
  o->master = runtime->scheduler.master_ticks;
  o->frames = audio->mixer.frames;
  o->non_silent = genesis_mixer_non_silent(&audio->mixer);
  memset(&report, 0, sizeof report);
  if (machine->native_driver != NULL) {
    machine->native_driver->digest(machine->native_driver->context, o->driver);
    machine->native_driver->report(machine->native_driver->context, &report);
  }
  o->active = machine->native_active;
  o->fault = report.fault != NULL ? report.fault : "-";
  o->dac_samples = report.dac_samples;
  o->commands = report.commands;
  o->sequences = report.sequences;
  o->ticks = report.ticks;
}

static int same(const Observation *a, const Observation *b) {
  return memcmp(a->mixer, b->mixer, 32U) == 0 && memcmp(a->driver, b->driver, 32U) == 0 && a->ym == b->ym && a->master == b->master && a->frames == b->frames &&
         a->dac_samples == b->dac_samples && a->commands == b->commands && a->sequences == b->sequences && a->ticks == b->ticks;
}

static uint64_t env_u64(const char *name, uint64_t fallback) {
  const char *text = getenv(name);
  return text != NULL ? strtoull(text, NULL, 10) : fallback;
}

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance) {
  GenesisControlTransfer result;
  GenesisSnapshotMachine machine;
  GenesisSnapshotIdentity identity;
  GenesisSnapshotBuffer saved;
  Observation at_save, first, second, third;
  const char *sha = getenv("SEGARECOMP_DEMO_SOURCE_SHA");
  const uint64_t pre = env_u64("SEGARECOMP_DEMO_PRE", 6000000U), span = env_u64("SEGARECOMP_DEMO_SPAN", 3000000U);
  unsigned i;
  int status;
  (void)dispatch_allowance;
  memset(&result, 0, sizeof result);
  memset(&saved, 0, sizeof saved);
  memset(&identity, 0, sizeof identity);
  for (i = 0; sha != NULL && i < 32U && strlen(sha) >= 64U; ++i) { unsigned v = 0; sscanf(sha + 2U * i, "%2x", &v); identity.source_sha256[i] = (uint8_t)v; }
  machine.runtime = runtime;
  machine.z80 = genesis_sound_attach(runtime);
  machine.audio = genesis_sound_audio_mutable();
  if (!run_dispatches(runtime, dispatch, pre)) { puts("STATE_DEMO error=guest_ended_before_the_save"); result.kind = GENESIS_COMPLETE; return result; }
  genesis_sound_finish(runtime, 0);
  observe(runtime, &at_save);
  status = (int)genesis_snapshot_capture(&machine, &identity, NULL, &saved);
  printf("STATE_DEMO save status=%d bytes=%u native_active=%d fault=%s commands=%llu sequences=%llu dac_samples=%llu ticks=%llu frames=%llu\n", status, (unsigned)saved.size, at_save.active, at_save.fault,
         (unsigned long long)at_save.commands, (unsigned long long)at_save.sequences, (unsigned long long)at_save.dac_samples, (unsigned long long)at_save.ticks, (unsigned long long)at_save.frames);
  if (status != 0) { result.kind = GENESIS_COMPLETE; return result; }
  (void)run_dispatches(runtime, dispatch, span);
  observe(runtime, &first);
  status = (int)genesis_snapshot_restore(&machine, &identity, saved.data, saved.size);
  observe(runtime, &second);
  printf("STATE_DEMO restore status=%d restored_equals_saved=%d\n", status, same(&at_save, &second));
  (void)run_dispatches(runtime, dispatch, span);
  observe(runtime, &second);
  status = (int)genesis_snapshot_restore(&machine, &identity, saved.data, saved.size);   /* and a third time: a restore is repeatable */
  (void)run_dispatches(runtime, dispatch, span);
  observe(runtime, &third);
  printf("STATE_DEMO continuation native_active=%d fault=%s played_after_save: dac_samples=%llu commands=%llu frames=%llu\n", first.active, first.fault,
         (unsigned long long)(first.dac_samples - at_save.dac_samples), (unsigned long long)(first.commands - at_save.commands), (unsigned long long)(first.frames - at_save.frames));
  printf("STATE_DEMO verdict restore_status=%d continuation_identical=%d third_identical=%d non_silent=%d\n", status, same(&first, &second), same(&first, &third), first.non_silent);
  genesis_snapshot_buffer_free(&saved);
  result.kind = GENESIS_COMPLETE;
  return result;
}
