/*
 * SEG-032-T009 (ADR 0075): the SAME scripted 68K workload run through the headless sound route and through the REAL viewer core
 * (viewer.c) with a fake clock, presenter and audio sink (no window, no SDL, no real time).
 *   genesis_viewer_audio_harness headless <pcm-out>
 *   genesis_viewer_audio_harness viewer <pcm-out> <slice> <ok|fail|none|mute|full>
 * The workload writes a PSG tone and a YM2612 patch through the real 68K ports and then lets guest time pass. `headless` runs it in one
 * genesis_runtime_run call and flushes like genesis_sound_hook.c; `viewer` runs it in slices through genesis_viewer_run, draining the
 * mixer ring after every slice into the presenter (genesis_audio_present.c) as viewer_main_hook.c does. <pcm-out> receives the canonical
 * stream: the mixer's frame sink (headless) or exactly what the fake sink accepted (viewer). Prints aggregates only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_audio_present.h"
#include "genesis_sound.h"
#include "vdp_render.h"
#include "viewer.h"

typedef struct Step {
  uint32_t address; /* 0 = no access */
  uint8_t value;
} Step;

static Step steps[512];
static uint32_t step_count, step_index;
static FILE *pcm;

static void add(uint32_t address, uint8_t value) {
  steps[step_count].address = address;
  steps[step_count].value = value;
  ++step_count;
}

static void build_script(void) {
  static const uint8_t psg[] = {0x8A, 0x0F, 0x90, 0xA5, 0x08, 0xB2};
  unsigned i, op;
  for (i = 0; i < sizeof psg; ++i) add(0xC00011U, psg[i]);
  {
    static const uint8_t head[][2] = {{0xB0, 0x07}, {0xB4, 0xC0}};
    for (i = 0; i < 2; ++i) { add(0xA04000U, head[i][0]); add(0xA04001U, head[i][1]); }
    for (op = 0; op < 4; ++op) {
      const uint8_t base = (uint8_t)(op * 4U);
      static const uint8_t regs[] = {0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
      static const uint8_t vals[] = {0x01, 0x20, 0x1F, 0x00, 0x00, 0x0F};
      for (i = 0; i < sizeof regs; ++i) { add(0xA04000U, (uint8_t)(regs[i] + base)); add(0xA04001U, vals[i]); }
    }
    add(0xA04000U, 0xA4); add(0xA04001U, 0x22);
    add(0xA04000U, 0xA0); add(0xA04001U, 0x69);
    add(0xA04000U, 0x28); add(0xA04001U, 0xF0);
  }
}

static GenesisControlTransfer dispatch_fn(GenesisRuntime *runtime) {
  if (step_index >= 300U) {
    GenesisControlTransfer done;
    memset(&done, 0, sizeof done);
    done.kind = GENESIS_COMPLETE;
    return done;
  }
  if (step_index < step_count && steps[step_index].address != 0U) {
    GenesisRuntimeStop stop;
    uint32_t value = steps[step_index].value;
    memset(&stop, 0, sizeof stop);
    if (genesis_route_access(runtime, steps[step_index].address, GENESIS_ACCESS_BYTE, GENESIS_ACCESS_WRITE, &value, &stop) != GENESIS_ACCESS_OK) {
      GenesisControlTransfer failed;
      memset(&failed, 0, sizeof failed);
      failed.kind = GENESIS_STOP;
      failed.stop = stop;
      return failed;
    }
  }
  /* writes are spaced past the YM2612 busy period; after the script the clock simply advances */
  {
    const uint32_t cycles = step_index < step_count ? 200U : 4000U;
    ++step_index;
    return genesis_runtime_retire_m68k_instruction(runtime, cycles, step_index * 2U);
  }
}

static void write_frame(FILE *f, int16_t l, int16_t r) {
  uint8_t b[4] = {(uint8_t)((uint16_t)l & 0xFFU), (uint8_t)((uint16_t)l >> 8), (uint8_t)((uint16_t)r & 0xFFU), (uint8_t)((uint16_t)r >> 8)};
  if (f != NULL) fwrite(b, 1U, sizeof b, f);
}

static void mixer_sink(void *context, uint64_t index, int16_t l, int16_t r) {
  (void)context;
  (void)index;
  write_frame(pcm, l, r);
}

/* fake audio sink: `ok` accepts everything, `fail` refuses, `full` reports a permanently full queue */
static const char *sink_mode = "ok";
static uint32_t fake_queued(void *context) { (void)context; return strcmp(sink_mode, "full") == 0 ? GENESIS_AUDIO_QUEUE_LIMIT_FRAMES : 0U; }
static int fake_put(void *context, const int16_t *frames, uint32_t count) {
  uint32_t i;
  (void)context;
  if (strcmp(sink_mode, "fail") == 0) return -1;
  for (i = 0; i < count; ++i) write_frame(pcm, frames[2U * i], frames[2U * i + 1U]);
  return 0;
}
static GenesisAudioSink fake_sink = {NULL, fake_queued, fake_put};
static GenesisAudioPresenter presenter;

static uint64_t fake_now(void *c) { static uint64_t t; (void)c; t += 1000000U; return t; }
static void fake_sleep(void *c, uint64_t ns) { (void)c; (void)ns; }
static int fake_present(void *c, const GenesisFrameArtifact *f) { (void)c; (void)f; return 0; }
static int fake_closed(void *c) { (void)c; return 0; }
static void fake_after_slice(void *c, GenesisRuntime *runtime) {
  (void)c;
  (void)runtime;
  genesis_audio_present(&presenter, genesis_sound_mixer());
}

static GenesisRuntime runtime;
static GenesisFrameArtifact latest;

static void report(const char *what) {
  const GenesisAudio *audio = genesis_sound_audio();
  const GenesisMixer *mixer = &audio->mixer;
  uint8_t digest[32];
  unsigned i;
  genesis_mixer_digest(mixer, digest);
  printf("%s frames=%llu clipped=%llu nonsilent=%d fault=%d ticks=%llu psg_writes=%llu ym_writes=%llu ym_samples=%llu ym_fnv=%016llx ym_state=%016llx sha=", what,
         (unsigned long long)mixer->frames, (unsigned long long)mixer->clipped, genesis_mixer_non_silent(mixer), (int)mixer->fault,
         (unsigned long long)runtime.scheduler.master_ticks, (unsigned long long)audio->psg_writes, (unsigned long long)audio->ym_writes,
         (unsigned long long)audio->ym_samples, (unsigned long long)audio->ym_sample_fnv, (unsigned long long)ym2612_state_digest(audio->ym));
  for (i = 0; i < 32U; ++i) printf("%02x", (unsigned)digest[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  int viewer;
  if (argc < 3) return 2;
  viewer = strcmp(argv[1], "viewer") == 0;
  pcm = fopen(argv[2], "wb");
  if (pcm == NULL) return 2;
  build_script();
  (void)genesis_sound_attach(&runtime);
  if (!viewer) {
    GenesisControlTransfer t;
    genesis_mixer_set_sink(genesis_sound_mixer(), mixer_sink, NULL);
    t = genesis_runtime_run(&runtime, dispatch_fn, 100000U);
    genesis_sound_finish(&runtime, t.kind == GENESIS_STOP);
    printf("outcome=%d ", (int)t.kind);
    report("HEADLESS");
  } else {
    GenesisViewerOptions options;
    GenesisViewerHost host;
    GenesisPacer pacer;
    GenesisLiveFrameObserver observer;
    GenesisViewerResult r;
    if (argc < 5) return 2;
    options.unthrottled = 1;
    options.slice_dispatches = (uint32_t)strtoul(argv[3], NULL, 10);
    sink_mode = argv[4];
    genesis_audio_presenter_init(&presenter, strcmp(sink_mode, "none") == 0 || strcmp(sink_mode, "mute") == 0 ? NULL : &fake_sink,
                                 strcmp(sink_mode, "mute") == 0);
    memset(&latest, 0, sizeof latest);
    memset(&observer, 0, sizeof observer);
    observer.producer = genesis_vdp_produce_frame;
    observer.latest = &latest;
    runtime.live_frame_observer = &observer;
    {
      uint16_t *g = runtime.devices.vdp.registers;
      g[1] = 0x04; g[2] = 0x30; g[4] = 0x04; g[5] = 0x28; g[11] = 0x04; g[12] = 0x81; g[16] = 0x01;
      runtime.devices.vdp.cram[0] = 0x0EU;
    }
    runtime.pc = 0x200U;
    runtime.sr = 0x2000U;
    memset(&host, 0, sizeof host);
    host.now_ns = fake_now;
    host.sleep_ns = fake_sleep;
    host.present = fake_present;
    host.window_closed = fake_closed;
    host.after_slice = fake_after_slice;
    genesis_pacer_init(&pacer, 1);
    r = genesis_viewer_run(&runtime, dispatch_fn, &options, &host, &pacer, 100000U);
    genesis_sound_finish(&runtime, r.outcome == GENESIS_VIEWER_GUEST_STOP);
    fake_after_slice(NULL, &runtime);
    printf("outcome=%d slices=%u ", (int)r.outcome, (unsigned)r.slices);
    report("VIEWER");
    printf("PRESENTER submitted=%llu overrun_dropped=%llu refused=%llu discarded=%llu underruns=%llu ring_dropped=%llu\n",
           (unsigned long long)presenter.submitted, (unsigned long long)presenter.overrun_dropped, (unsigned long long)presenter.refused,
           (unsigned long long)presenter.discarded, (unsigned long long)presenter.underruns, (unsigned long long)genesis_sound_mixer()->ring_dropped);
  }
  fclose(pcm);
  genesis_sound_detach();
  return 0;
}
