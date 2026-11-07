/*
 * SEG-131 Checkpoint E: drives the REAL Genesis runtime (generated-native Z80, YM2612, PSG, mixer) through the Genesis state service's rewind timeline.
 *   genesis_rewind_harness <script> <budget-bytes> <interval-ms> <target-ms> [state-dir]
 * Script ops (decimal / hex as noted):
 *   retire <m68k cycles>      one retirement through the real hook, then the service's once-per-slice capture (the barrier)
 *   w8|w16 <hex addr> <hex>   68K write through genesis_route_access
 *   hist                      "HIST count bytes captures evictions failures" then "ENTRY i age_ms frame ticks thumb_w thumb_h" per entry
 *   avail                     "AVAIL <0|1> <reason>" and "NOTE <text>" (history limit)
 *   open | cancel             the rewind surface opened / closed without a restore (prints ANCHOR ok|moved)
 *   restore <i>               restore history entry i (prints "RESTORE i <status>" and the history count after)
 *   save <slot> | load <slot> slot files (needs state-dir)
 *   fullhash | mixer          independent whole-state hash / mixer aggregates
 *   resets                    number of sink resets the after-restore callback performed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "genesis_audio.h"
#include "genesis_viewer_state.h"
#include "vdp_render.h"
#include "z80_machine.h"

static GenesisRuntime runtime;
static GenesisZ80Machine machine;
static GenesisAudio audio;
static GenesisFrameArtifact latest;
static unsigned sink_resets;

static void after_restore(void *ctx) { (void)ctx; ++sink_resets; }

static void print_hex(const uint8_t *p, size_t n) { size_t i; for (i = 0; i < n; ++i) printf("%02x", (unsigned)p[i]); }

static void fullhash(uint8_t out[32]) {
  GenesisSha256 sha;
  GenesisRuntime *copy = (GenesisRuntime *)malloc(sizeof *copy);
  GenesisZ80Machine z = machine;
  GenesisAudio *a = (GenesisAudio *)malloc(sizeof *a);
  uint64_t ym = ym2612_state_digest(audio.ym);
  uint32_t i, k;
  genesis_sha256_init(&sha);
  memcpy(copy, &runtime, sizeof *copy);
  memset(&copy->execution_history, 0, sizeof copy->execution_history);
  memset(&copy->m68k_checkpoint, 0, sizeof copy->m68k_checkpoint);
  memset(&copy->device_checkpoint, 0, sizeof copy->device_checkpoint);
  copy->pad1 = copy->pad2 = 0U;
  copy->owned_regions = NULL; copy->cartridge_sram = NULL; copy->cartridge_sram_storage = NULL; copy->live_frame_observer = NULL; copy->live_raster = NULL; copy->execution_coverage = NULL;
  copy->z80_hooks = NULL; copy->audio_hooks = NULL; copy->z80_epoch.on_epoch = NULL; copy->z80_epoch.context = NULL;   /* host pointers differ per process (ASLR) */
  genesis_sha256_update(&sha, (const uint8_t *)copy, (uint32_t)sizeof *copy);
  free(copy);
  memset(&z.count_reset_assert, 0, (size_t)((char *)(&z + 1) - (char *)&z.count_reset_assert));
  z.cpu.state.deadline = 0U;
  z.runtime = NULL; memset(&z.hooks, 0, sizeof z.hooks); memset(&z.cpu.host, 0, sizeof z.cpu.host); z.cpu.observe_pc = NULL; z.cpu.observer = NULL; z.on_unknown_image = NULL; z.unknown_image_context = NULL; z.bank_view = NULL;
  z.cpu.outcome = (Z80Outcome)0;
  memset(z.cpu.live_code, 0, sizeof z.cpu.live_code);
  genesis_sha256_update(&sha, (const uint8_t *)&z, (uint32_t)sizeof z);
  memcpy(a, &audio, sizeof *a);
  memset(a->trace_ticks, 0, sizeof a->trace_ticks);
  memset(a->trace_bytes, 0, sizeof a->trace_bytes);
  memset(a->ym_trace_ticks, 0, sizeof a->ym_trace_ticks);
  memset(a->ym_trace_port, 0, sizeof a->ym_trace_port);
  memset(a->ym_trace_value, 0, sizeof a->ym_trace_value);
  a->trace_count = a->ym_trace_count = 0U;
  memset(a->mixer.ring, 0, sizeof a->mixer.ring);
  a->mixer.ring_read = a->mixer.ring_write = a->mixer.ring_dropped = 0U;
  a->mixer.sink = NULL; a->mixer.sink_context = NULL; a->ym = NULL; a->runtime = NULL; memset(&a->hooks, 0, sizeof a->hooks); a->psg.config.levels = NULL;
  for (k = 0U; k < 2U; ++k) {
    GenesisMixerSource *s = k == 0U ? &a->mixer.ym : &a->mixer.psg;
    const GenesisMixerSource *o = k == 0U ? &audio.mixer.ym : &audio.mixer.psg;
    uint8_t word[4];
    memset(s->fifo, 0, sizeof s->fifo);
    s->fifo_head = 0U;
    genesis_sha256_update(&sha, (const uint8_t *)s, (uint32_t)sizeof *s);
    for (i = 0U; i < o->fifo_count; ++i) {
      const uint32_t at = (o->fifo_head + i) % GENESIS_MIXER_FIFO_WINDOWS;
      memcpy(word, &o->fifo[at][0], 4U);
      genesis_sha256_update(&sha, word, 4U);
      memcpy(word, &o->fifo[at][1], 4U);
      genesis_sha256_update(&sha, word, 4U);
    }
    memset(s, 0, sizeof *s);
  }
  genesis_sha256_update(&sha, (const uint8_t *)a, (uint32_t)sizeof *a);
  genesis_sha256_update(&sha, (const uint8_t *)&ym, 8U);
  free(a);
  genesis_sha256_final(&sha, out);
}

int main(int argc, char **argv) {
  char line[512];
  FILE *script;
  GenesisViewerStateConfig config;
  GenesisViewerState *state;
  SvStateProvider provider;
  if (argc < 5) return 2;
  script = fopen(argv[1], "r");
  if (script == NULL) return 2;
  runtime.a[7] = 0x00FFFE00U;
  runtime.sr = 0x2700U;
  runtime.pc = 0x00000200U;
  genesis_z80_machine_attach(&machine, &runtime);
  genesis_audio_attach(&audio, &runtime);
  memset(&config, 0, sizeof config);
  config.machine.runtime = &runtime;
  config.machine.z80 = &machine;
  config.machine.audio = &audio;
  config.latest_frame = &latest;
  memset(config.identity.source_sha256, 0xA5, 32U);
  config.state_dir = argc > 5 ? argv[5] : NULL;
  config.after_restore = after_restore;
  config.rewind_budget_bytes = (uint32_t)strtoul(argv[2], NULL, 10);
  config.rewind_interval_ms = (uint32_t)strtoul(argv[3], NULL, 10);
  config.rewind_target_ms = (uint32_t)strtoul(argv[4], NULL, 10);
  state = genesis_viewer_state_create(&config);
  if (state == NULL) return 2;
  genesis_viewer_state_provider(state, &provider);
  while (fgets(line, sizeof line, script) != NULL) {
    char op[16];
    unsigned long a = 0, b = 0;
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%15s %lx %lx", op, &a, &b) < 1) return 3;
    if (strcmp(op, "retire") == 0) {
      GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(&runtime, (uint32_t)strtoul(line + 7, NULL, 10), 0U);
      if (t.kind == GENESIS_STOP) { printf("STOP %d %d\n", (int)t.stop.stop_class, (int)t.stop.diagnostic_category); return 0; }
      (void)genesis_vdp_produce_frame(runtime.devices.vdp.vram, runtime.devices.vdp.vsram, runtime.devices.vdp.cram, runtime.devices.vdp.registers, &latest);
      genesis_viewer_state_after_slice(state);
    } else if (strcmp(op, "hist") == 0) {
      GenesisViewerStateStats st;
      int i;
      genesis_viewer_state_stats(state, &st);
      printf("HIST %u %llu %llu %llu %llu\n", (unsigned)st.history_entries, (unsigned long long)st.history_bytes, (unsigned long long)st.captures, (unsigned long long)st.evictions,
             (unsigned long long)st.capture_failures);
      for (i = 0; i < provider.history_count(provider.ctx); ++i) {
        SvRewindEntry e;
        GenesisSnapshotInfo info;
        provider.history_info(provider.ctx, i, &e);
        (void)info;
        printf("ENTRY %d %lld %dx%d\n", i, (long long)e.age_ms, e.thumb.w, e.thumb.h);
      }
    } else if (strcmp(op, "ticks") == 0) {
      printf("TICKS %llu frame %llu\n", (unsigned long long)runtime.scheduler.master_ticks, (unsigned long long)runtime.virtual_frame_count);
    } else if (strcmp(op, "avail") == 0) {
      char why[160] = "", note[160] = "";
      const int ok = provider.rewind_available(provider.ctx, why, sizeof why);
      const int limited = provider.history_note(provider.ctx, note, sizeof note);
      printf("AVAIL %d %s\n", ok, why);
      printf("NOTE %d %s\n", limited, note);
    } else if (strcmp(op, "open") == 0) {
      provider.rewind_opened(provider.ctx);
    } else if (strcmp(op, "cancel") == 0) {
      provider.rewind_cancelled(provider.ctx);
    } else if (strcmp(op, "restore") == 0) {
      char error[96] = "";
      const int rc = provider.restore_entry(provider.ctx, (int)a, error, sizeof error);
      printf("RESTORE %lu %d %s count=%d\n", a, rc, error, provider.history_count(provider.ctx));
    } else if (strcmp(op, "save") == 0) {
      char error[96] = "";
      const int rc = provider.save_slot(provider.ctx, (int)a, error, sizeof error);
      printf("SAVE %lu %d %s\n", a, rc, error);
    } else if (strcmp(op, "load") == 0) {
      char error[96] = "";
      const int rc = provider.load_slot(provider.ctx, (int)a, error, sizeof error);   /* (a separate statement: printf argument evaluation order is unspecified) */
      printf("LOAD %lu %d %s count=%d\n", a, rc, error, provider.history_count(provider.ctx));
    } else if (strcmp(op, "fullhash") == 0) {
      uint8_t d[32];
      fullhash(d);
      printf("FULL ");
      print_hex(d, 32U);
      printf("\n");
    } else if (strcmp(op, "mixer") == 0) {
      uint8_t digest[32];
      GenesisRuntimeStop stop;
      memset(&stop, 0, sizeof stop);
      (void)genesis_z80_machine_run_to(&machine, runtime.scheduler.master_ticks, &stop);
      genesis_audio_sync(&audio, runtime.scheduler.master_ticks);
      genesis_mixer_digest(&audio.mixer, digest);
      printf("MIXER frames=%llu sha=", (unsigned long long)audio.mixer.frames);
      print_hex(digest, 32U);
      printf("\n");
    } else if (strcmp(op, "bench") == 0) {
      /* measurement only: N captures and N restores of the current machine, timed (C11 timespec_get wall time, portable to the Windows UCRT; nothing here feeds back into the guest) */
      enum { MAX_RUNS = 512 };
      static uint64_t cap_ns[MAX_RUNS], res_ns[MAX_RUNS];
      const unsigned runs = a > MAX_RUNS ? MAX_RUNS : (unsigned)a;
      unsigned i, j;
      GenesisSnapshotMachine machine_view;
      GenesisSnapshotIdentity ident;
      GenesisSnapshotBuffer buf;
      uint32_t size = 0U;
      memset(&machine_view, 0, sizeof machine_view);
      machine_view.runtime = &runtime; machine_view.z80 = &machine; machine_view.audio = &audio;
      memset(&ident, 0, sizeof ident);
      memset(ident.source_sha256, 0xA5, 32U);
      for (i = 0U; i < runs; ++i) {
        struct timespec t0, t1;
        timespec_get(&t0, TIME_UTC);
        if (genesis_snapshot_capture(&machine_view, &ident, NULL, &buf) != GENESIS_SNAPSHOT_OK) return 4;
        timespec_get(&t1, TIME_UTC);
        cap_ns[i] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        size = buf.size;
        timespec_get(&t0, TIME_UTC);
        if (genesis_snapshot_restore(&machine_view, &ident, buf.data, buf.size) != GENESIS_SNAPSHOT_OK) return 5;
        timespec_get(&t1, TIME_UTC);
        res_ns[i] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        genesis_snapshot_buffer_free(&buf);
      }
      for (i = 0U; i + 1U < runs; ++i)
        for (j = 0U; j + 1U < runs - i; ++j) {
          uint64_t t;
          if (cap_ns[j] > cap_ns[j + 1U]) { t = cap_ns[j]; cap_ns[j] = cap_ns[j + 1U]; cap_ns[j + 1U] = t; }
          if (res_ns[j] > res_ns[j + 1U]) { t = res_ns[j]; res_ns[j] = res_ns[j + 1U]; res_ns[j + 1U] = t; }
        }
      printf("BENCH runs=%u size=%u capture_us min=%.1f median=%.1f max=%.1f restore_us min=%.1f median=%.1f max=%.1f\n", runs, (unsigned)size, (double)cap_ns[0] / 1000.0,
             (double)cap_ns[runs / 2U] / 1000.0, (double)cap_ns[runs - 1U] / 1000.0, (double)res_ns[0] / 1000.0, (double)res_ns[runs / 2U] / 1000.0, (double)res_ns[runs - 1U] / 1000.0);
    } else if (strcmp(op, "resets") == 0) {
      printf("RESETS %u\n", sink_resets);
    } else {
      GenesisRuntimeStop stop;
      uint32_t value = (uint32_t)b;
      GenesisAccessWidth width = (op[1] == '8') ? GENESIS_ACCESS_BYTE : GENESIS_ACCESS_WORD;
      GenesisAccessDirection direction = (op[0] == 'w') ? GENESIS_ACCESS_WRITE : GENESIS_ACCESS_READ;
      memset(&stop, 0, sizeof stop);
      if (genesis_route_access(&runtime, (uint32_t)a, width, direction, &value, &stop) != GENESIS_ACCESS_OK) { printf("STOP %d %d\n", (int)stop.stop_class, (int)stop.diagnostic_category); return 0; }
    }
  }
  genesis_viewer_state_destroy(state);
  printf("END\n");
  fclose(script);
  return 0;
}
