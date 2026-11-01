/* SEG-114 (ADR 0258): the Sega CD main-side Z80.
 *
 * The Sega CD main processor sees the Genesis sound CPU exactly as a Genesis 68000 does ($A00000 sound RAM, $A04000 YM2612, $A11100/$A11200
 * bus request and reset, the 9-bit bank register). This module attaches the existing Genesis Z80 machine (platforms/genesis/runtime/z80_machine.c:
 * the generated-native Z80, the BUSREQ/RESET and image-epoch rules of ADR 0072/0073, the lazy-follower scheduling) to the Sega CD device host
 * (scd_vdp.c), whose zero-initialised Genesis runtime already owns the Z80 RAM and the bus latches. Nothing is decoded and nothing is
 * interpreted: a Z80 image the build compiled executes generated-native code, anything else is a typed stop.
 *
 * Differences from the Genesis, all of them fail closed:
 *  - the Z80's banked window reads the immutable BIOS image only. Every other target of the 68000 bus (PRG RAM, Word RAM, work RAM, devices) and
 *    every banked write is the typed stop z80_bank_target_unsupported: a 68000 bus the main processor also writes cannot be read by a lazily
 *    synchronised follower without a causality rule this milestone does not define (ADR 0072 decision 1);
 *  - the sound devices are not attached: a YM2612 read answers 0 (absent hardware) and PSG/YM writes are discarded, as the Genesis machine does
 *    without an audio attach;
 *  - a build pass (--materialize-dir) records an unknown image epoch and goes on with the Z80 quiesced (state `unmodelled`); the main processor reads sound RAM
 *    as it stands, so such a pass is never a confirmed candidate (the build registers the image and runs again); the executable program stops at an
 *    unknown epoch instead (z80_unsupported / z80_unknown_image);
 *  - when a Z80-side access isolates the Z80 the faulting instruction has already completed (a read returned $FF, later effects of that instruction land).
 *
 * This file is compiled only into programs built with Z80 support (`segarecomp sega-cd build`); runtime sources in the directory above never
 * reference it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../scd_runtime.h"
#include "z80_machine.h"
#include "z80_registry.h"
#ifdef SCD_Z80_AUDIO
#include "genesis_audio.h" /* SEG-115 (ADR 0259): the Genesis YM2612 + PSG devices and their mixer, unchanged */
#endif

#define SCD_Z80_MAX_UNKNOWN 16U /* unknown image epochs one build pass records (the build's image bound is smaller) */
#define SCD_Z80_LOG 32U        /* image epochs kept in the report (diagnostic: time, extent count, image) */ /* unknown image epochs one build pass records (the build's image bound is smaller) */

typedef struct ScdZ80Unknown {
  uint8_t ram[GENESIS_Z80_RAM_BYTES];
  uint8_t written[GENESIS_Z80_RAM_BYTES / 8U];
} ScdZ80Unknown;

#define SCD_Z80_MASTER_TICK_TICKS UINT64_C(500000) /* machine ticks per Genesis master clock (scd_vdp.c) */
#define SCD_Z80_FM_LAG 2U                           /* output frames the YM2612/PSG part trails: its decimation window must have closed (see scd_z80_audio_frame) */

typedef struct ScdZ80 {
  GenesisZ80Machine machine;
#ifdef SCD_Z80_AUDIO
  GenesisAudio audio;      /* the one shared YM2612 + PSG both CPUs reach through host->audio_hooks */
  uint64_t audio_synced;   /* master ticks the devices were last synchronised to by the machine */
  uint8_t audio_stopped;   /* the Z80 met a typed stop while running ahead for the devices: the main processor reports it at its next access */
  uint64_t fm_missing, fm_overrun;
#endif
  GenesisZ80BankView bank;
  ScdMachine *owner;
  uint8_t pass;       /* build pass: record unknown epochs and quiesce instead of stopping */
  uint8_t unmodelled; /* an epoch of this pass was unknown: from then on the Z80 area cannot be observed by the main processor */
  uint32_t unknown_count, unknown_overflow;
  uint32_t log_count;
  struct { uint64_t tick; uint32_t extents; uint32_t image; } log[SCD_Z80_LOG];
  uint8_t finish_fault; /* the final synchronisation reached a typed Z80 stop that no main access observed */
  uint32_t finish_diagnostic;
  ScdZ80Unknown unknown[SCD_Z80_MAX_UNKNOWN];
} ScdZ80;

static ScdZ80 *scd_z80_of(const ScdMachine *m) { return (ScdZ80 *)m->z80; }

/* The Z80's banked read of the 68000 bus: the immutable BIOS image only (module comment). */
static int scd_z80_bank_read(void *context, uint32_t address24, uint8_t *value) {
  const ScdMachine *m = ((const ScdZ80 *)context)->owner;
  const uint32_t a = address24 & UINT32_C(0x3FFFF);
  if (m->bios == NULL || address24 >= UINT32_C(0x00200000) || a >= UINT32_C(0x20000) || a >= m->bios_size) return 0;
  if (a >= 0x70U && a < 0x74U) return 0; /* the level-4 vector words are the Gate Array's, not the image's */
  *value = m->bios[a];
  return 1;
}

static int scd_z80_bank_write(void *context, uint32_t address24, uint8_t value) {
  (void)context; (void)address24; (void)value;
  return 0;
}

/* The epoch observer: one diagnostic record per image epoch (the extents of the hold window and the registry image its signature selects). */
static void scd_z80_on_epoch(void *context, const GenesisZ80EpochEvent *event) {
  ScdZ80 *z = (ScdZ80 *)context;
  uint8_t signature[32];
  uint32_t extents = 0U;
  genesis_z80_activation_signature(event->ram, event->written_bitmap, &extents, signature);
  if (z->log_count >= SCD_Z80_LOG) return;
  z->log[z->log_count].tick = event->master_ticks;
  z->log[z->log_count].extents = extents;
  z->log[z->log_count].image = genesis_z80_image_for_signature(signature);
  ++z->log_count;
}

static void scd_z80_on_unknown(void *context, uint32_t epoch_ordinal, const uint8_t *ram, const uint8_t *written) {
  ScdZ80 *z = (ScdZ80 *)context;
  (void)epoch_ordinal;
  if (!z->pass) return; /* the executable program: the machine stops with z80_unknown_image */
  z->unmodelled = 1U;
  if (z->unknown_count >= SCD_Z80_MAX_UNKNOWN) {
    z->unknown_overflow = 1U;
    return;
  }
  memcpy(z->unknown[z->unknown_count].ram, ram, GENESIS_Z80_RAM_BYTES);
  memcpy(z->unknown[z->unknown_count].written, written, GENESIS_Z80_RAM_BYTES / 8U);
  ++z->unknown_count;
}

static int scd_z80_attach(ScdMachine *m) {
  ScdZ80 *z;
  GenesisRuntime *host = (GenesisRuntime *)m->vdp_host;
  if (host == NULL || m->bios == NULL) return 1;
  z = (ScdZ80 *)calloc(1U, sizeof *z);
  if (z == NULL) return 1;
  z->owner = m;
  z->pass = m->materialize ? 1U : 0U;
  genesis_z80_machine_attach(&z->machine, host);
  z->bank.context = z;
  z->bank.read = scd_z80_bank_read;
  z->bank.write = scd_z80_bank_write;
  z->machine.bank_view = &z->bank;
  z->machine.quiesce_unknown_image = z->pass;
  z->machine.isolate_view_stops = 1U;
  z->machine.on_unknown_image = scd_z80_on_unknown;
  z->machine.unknown_image_context = z;
  z->machine.rearm_faulted_on_epoch = 1U;       /* SEG-115 (ADR 0259): a fresh image epoch re-arms an isolated Z80 */
  host->z80_epoch.accumulate_until_epoch = 1U;  /* SEG-115: an upload spread over several bus holds belongs to the next epoch */
  host->z80_epoch.on_epoch = scd_z80_on_epoch;
  host->z80_epoch.context = z;
#ifdef SCD_Z80_AUDIO
  genesis_audio_attach(&z->audio, host); /* installs host->audio_hooks: the 68000's $A04000 / $C00011 and the Z80's $4000 / $7F11 reach the same devices */
#endif
  m->z80 = z;
  return 0;
}

static void scd_z80_detach(ScdMachine *m) {
  ScdZ80 *z = scd_z80_of(m);
  GenesisRuntime *host = (GenesisRuntime *)m->vdp_host;
  if (host != NULL) {
    host->z80_hooks = NULL;
#ifdef SCD_Z80_AUDIO
    genesis_audio_detach(&scd_z80_of(m)->audio);
#endif
    host->z80_epoch.accumulate_until_epoch = 0U;
    host->z80_epoch.on_epoch = NULL;
    host->z80_epoch.context = NULL;
  }
  free(z);
  m->z80 = NULL;
}

/* Genesis contract section 18: an isolated (muted) or not yet compiled Z80 does not stop the main processor; the main processor reads the sound RAM as it
 * stands. Anything the Z80 would have written is absent (a recorded deviation, reported as state faulted/unmodelled). */
static int scd_z80_readable(const ScdMachine *m) {
  (void)m;
  return 1;
}

static int scd_z80_classify_stop(const ScdMachine *m, uint32_t stop_class, uint32_t diagnostic, uint32_t *detail) {
  (void)m;
  if (stop_class != (uint32_t)GENESIS_STOP_UNSUPPORTED_Z80_EXECUTION &&
      !(diagnostic >= (uint32_t)GENESIS_DIAG_Z80_UNKNOWN_IMAGE && diagnostic <= (uint32_t)GENESIS_DIAG_Z80_UNSUPPORTED_ACKNOWLEDGE))
    return 0; /* only the Z80's own outcomes (56-61); a main access without the bus or to an unmapped address stays main_device_unsupported */
  *detail = diagnostic;
  return 1;
}

/* The final synchronisation: the Z80 follows the machine to the instant of the report. A typed Z80 stop met here was observed by no main
 * access; it is reported, never turned into a machine stop (the machine's result must not depend on where the run was cut). */
static void scd_z80_finish(ScdMachine *m) {
  ScdZ80 *z = scd_z80_of(m);
  GenesisRuntime *host = (GenesisRuntime *)m->vdp_host;
  GenesisRuntimeStop stop;
  memset(&stop, 0, sizeof stop);
  host->scheduler.master_ticks = m->now / UINT64_C(500000);
  if (genesis_z80_machine_run_to(&z->machine, host->scheduler.master_ticks, &stop)) {
    z->finish_fault = 1U;
    z->finish_diagnostic = (uint32_t)stop.diagnostic_category;
  }
#ifdef SCD_Z80_AUDIO
  else genesis_audio_sync(&z->audio, host->scheduler.master_ticks); /* the devices at the instant of the report */
#endif
}

#ifdef SCD_Z80_AUDIO
/* Runs the Z80 and then the devices to guest time `tick`. The scheduler visits events in time order and stamps every main access with the event time, so no main access can
 * still precede `tick`; the Z80 is a lazy follower whose results do not depend on how often it is synchronised (contract section 10). A typed Z80 stop met here is not turned
 * into a machine stop: the Z80 stays where it stopped, and the main processor's next synchronisation (or the final one) meets the same stop. */
static void scd_z80_audio_sync(ScdMachine *m, uint64_t tick) {
  ScdZ80 *z = scd_z80_of(m);
  GenesisRuntimeStop stop;
  const uint64_t master = tick / SCD_Z80_MASTER_TICK_TICKS;
  if (z == NULL || z->audio_stopped || master <= z->audio_synced) return;
  memset(&stop, 0, sizeof stop);
  if (genesis_z80_machine_run_to(&z->machine, master, &stop)) {
    z->audio_stopped = 1U;
    return;
  }
  genesis_audio_sync(&z->audio, master);
  z->audio_synced = master;
}

/* The FM/PSG part of mixed output frame `frame`: the Genesis mixer's frame `frame - SCD_Z80_FM_LAG` (same 44.1 kHz grid from guest time 0). The mixer produces a frame only
 * once both devices delivered every native sample that starts in its window; the longest native (a YM2612 sample) is shorter than an output frame, so by the time output frame
 * `frame` is due (devices synchronised to it) frame `frame - 2` has closed. Reading the mixer's ring by index consumes nothing, so the Genesis digest is unchanged. */
static int scd_z80_audio_frame(ScdMachine *m, uint64_t frame, int32_t out[2]) {
  ScdZ80 *z = scd_z80_of(m);
  const GenesisMixer *mixer = &z->audio.mixer;
  uint64_t k;
  out[0] = out[1] = 0;
  if (frame < SCD_Z80_FM_LAG) return 0;
  k = frame - SCD_Z80_FM_LAG;
  if (k >= mixer->ring_write) {
    ++z->fm_missing;
    return 0;
  }
  if (mixer->ring_write - k > GENESIS_MIXER_RING_FRAMES) {
    ++z->fm_overrun;
    return 0;
  }
  out[0] = mixer->ring[k % GENESIS_MIXER_RING_FRAMES][0];
  out[1] = mixer->ring[k % GENESIS_MIXER_RING_FRAMES][1];
  return 1;
}
#endif

static int scd_z80_write_file(const char *dir, const char *name, unsigned index, const uint8_t *data, size_t length) {
  char path[1024];
  FILE *f;
  int bad;
  if (snprintf(path, sizeof path, "%s/%s_%u", dir, name, index) <= 0) return 1;
  f = fopen(path, "wb");
  if (f == NULL) return 1;
  bad = fwrite(data, 1U, length, f) != length;
  return (fclose(f) != 0) || bad;
}

/* Build pass: one pair of files per unknown epoch, in activation order (z80_epoch_ram_<k>, z80_epoch_written_<k>, k from 0), and the count. */
static int scd_z80_export_epochs(ScdMachine *m, const char *dir) {
  const ScdZ80 *z = scd_z80_of(m);
  char path[1024];
  FILE *f;
  uint32_t k;
  for (k = 0U; k < z->unknown_count; ++k)
    if (scd_z80_write_file(dir, "z80_epoch_ram", k, z->unknown[k].ram, GENESIS_Z80_RAM_BYTES) != 0 ||
        scd_z80_write_file(dir, "z80_epoch_written", k, z->unknown[k].written, GENESIS_Z80_RAM_BYTES / 8U) != 0)
      return 1;
  if (snprintf(path, sizeof path, "%s/z80_epochs.txt", dir) <= 0) return 1;
  f = fopen(path, "w");
  if (f == NULL) return 1;
  fprintf(f, "unknown %u\noverflow %u\n", z->unknown_count, z->unknown_overflow);
  return fclose(f) != 0;
}

static void scd_z80_report(const ScdMachine *m, FILE *out) {
  const ScdZ80 *z = scd_z80_of(m);
  const GenesisRuntime *host = (const GenesisRuntime *)m->vdp_host;
  const GenesisZ80Machine *machine = &z->machine;
  uint8_t digest[32];
  char hex[65];
  uint32_t i;
  genesis_z80_machine_state_digest(machine, digest);
  for (i = 0U; i < 32U; ++i) snprintf(hex + 2U * i, 3U, "%02x", digest[i]);
  fprintf(out, "\"state\":\"%s\",\"images\":%u,\"epochs\":%u,\"unknown_epochs\":%u,\"bound\":%u,\"recoveries\":%u,\"fault\":%u,\"fault_epoch\":%u,\"fault_tick\":%llu,\"finish_fault\":%u,\"cycles\":%llu,"
               "\"reset_assert\":%llu,\"reset_release\":%llu,\"busreq_assert\":%llu,\"busreq_release\":%llu,\"bank_writes\":%llu,\"banked_reads\":%llu,"
               "\"main_ignored_writes\":%llu,\"digest\":\"%s\"",
          z->unmodelled ? "unmodelled" : genesis_z80_machine_sound_faulted(machine) ? "faulted" : "modelled", (unsigned)genesis_z80_image_count,
          (unsigned)host->z80_epoch.epoch_count, z->unknown_count, machine->bound_ordinal, machine->sound_recoveries, (unsigned)machine->sound_fault,
          machine->sound_fault_epoch, (unsigned long long)machine->sound_fault_master_ticks,
          z->finish_fault ? z->finish_diagnostic : 0U, (unsigned long long)machine->cpu.state.cycles,
          (unsigned long long)machine->count_reset_assert, (unsigned long long)machine->count_reset_release,
          (unsigned long long)machine->count_busreq_assert, (unsigned long long)machine->count_busreq_release,
          (unsigned long long)machine->count_bank_writes, (unsigned long long)machine->count_banked_reads,
          (unsigned long long)m->z80_area_ignored_writes, hex);
#ifdef SCD_Z80_AUDIO
  {
    const GenesisMixer *mixer = &z->audio.mixer;
    uint8_t mixer_digest[32];
    char mixer_hex[65];
    genesis_mixer_digest(mixer, mixer_digest);
    for (i = 0U; i < 32U; ++i) snprintf(mixer_hex + 2U * i, 3U, "%02x", mixer_digest[i]);
    fprintf(out, ",\"fm_psg\":{\"z80_ym_writes\":%llu,\"z80_ym_reads\":%llu,\"z80_psg_writes\":%llu,\"ym_writes\":%llu,\"psg_writes\":%llu,\"ym_samples\":%llu,"
                 "\"ym_sample_fnv\":\"%016llx\",\"mixer_frames\":%llu,\"mixer_sha256\":\"%s\",\"mixer_fault\":%u,\"clipped\":%llu,\"non_silent\":%s,"
                 "\"fm_missing\":%llu,\"fm_overrun\":%llu}",
            (unsigned long long)machine->count_ym_writes, (unsigned long long)machine->count_ym_reads, (unsigned long long)machine->count_psg_writes,
            (unsigned long long)z->audio.ym_writes, (unsigned long long)z->audio.psg_writes, (unsigned long long)z->audio.ym_samples,
            (unsigned long long)z->audio.ym_sample_fnv, (unsigned long long)mixer->frames, mixer_hex, (unsigned)mixer->fault, (unsigned long long)mixer->clipped,
            genesis_mixer_non_silent(mixer) ? "true" : "false", (unsigned long long)z->fm_missing, (unsigned long long)z->fm_overrun);
  }
#endif
  fprintf(out, ",\"epoch_log\":[");
  for (i = 0U; i < z->log_count; ++i)
    fprintf(out, "%s{\"tick\":%llu,\"extents\":%u,\"image\":%u}", i ? "," : "", (unsigned long long)z->log[i].tick,
            z->log[i].extents, z->log[i].image);
  fprintf(out, "]");
}

#ifdef SCD_Z80_AUDIO
const ScdZ80Ops scd_z80_ops = {scd_z80_attach, scd_z80_detach, scd_z80_readable, scd_z80_classify_stop,
                               scd_z80_finish, scd_z80_export_epochs, scd_z80_report, scd_z80_audio_sync, scd_z80_audio_frame};
#else
const ScdZ80Ops scd_z80_ops = {scd_z80_attach, scd_z80_detach, scd_z80_readable, scd_z80_classify_stop,
                               scd_z80_finish, scd_z80_export_epochs, scd_z80_report, NULL, NULL};
#endif
