/*
 * SEG-032-T008 (ADR 0073): the production seam of the final Genesis program with sound. The build compiles the UNMODIFIED
 * generated main TU with -Dgenesis_runtime_run=genesis_sound_hook_run (the same mechanism as the viewer and probe hooks), so the
 * generated main hands its own runtime to this function, which attaches the Z80 machine and the sound devices and then runs the
 * real runner. The build-time materialization pass links genesis_materialize_hook.c in place of this file: the generated units and
 * the image registry are identical in both programs.
 *
 * SEG-032-T009 (ADR 0075): this is also the headless audio artifact route. After the run the mixer is flushed to the final guest time
 * and the run summary carries the PCM aggregates (frame count, SHA-256 digest, clip counter, non-silence). Options, all optional and
 * validated (a malformed value exits 3 before the run): SEGARECOMP_AUDIO_PCM_OUT=<file> writes the canonical s16le stereo stream;
 * SEGARECOMP_AUDIO_RANGE=<first>:<count> adds the digest of that frame range. A failing PCM file never changes the digest or the guest.
 *
 * SEG-068-T007: when SEGARECOMP_CAPTURE_DIR is present the run loop is delegated to the existing headless frame capture
 * (genesis_frame_capture_run_from_environment, platforms/genesis/viewer/frame_capture.c) instead of genesis_runtime_run, so one
 * guest execution yields the captured frames (with the optional frame-keyed pad script SEGARECOMP_PAD_SCRIPT) AND the unchanged sound
 * summary. The capture only observes already-current VDP state and applies a host pad mask between runner slices; the sound devices,
 * the mixer and the SOUND_SUMMARY below are exactly those of the capture-free run.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* clock_gettime for the optional performance clock (SEG-125) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "frame_capture.h"
#include "sv_perf.h"
#include "genesis_sound.h"

#define C(x) (unsigned long long)(x)
GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance);

typedef struct PcmFile {
  FILE *file;
  uint64_t write_errors;
} PcmFile;

static PcmFile g_pcm;

static void pcm_sink(void *context, uint64_t frame_index, int16_t left, int16_t right) {
  PcmFile *pcm = (PcmFile *)context;
  uint8_t bytes[4];
  (void)frame_index;
  bytes[0] = (uint8_t)((uint16_t)left & 0xFFU);
  bytes[1] = (uint8_t)((uint16_t)left >> 8);
  bytes[2] = (uint8_t)((uint16_t)right & 0xFFU);
  bytes[3] = (uint8_t)((uint16_t)right >> 8);
  if (pcm->file != NULL && fwrite(bytes, 1U, sizeof bytes, pcm->file) != sizeof bytes) ++pcm->write_errors;
}

static int parse_u64(const char *text, size_t length, uint64_t *out) {
  uint64_t value = 0U;
  size_t i;
  if (length == 0U) return 0;
  for (i = 0U; i < length; ++i) {
    const unsigned digit = (unsigned)(text[i] - '0');
    if (digit > 9U || value > (UINT64_MAX - digit) / 10U) return 0;
    value = value * 10U + digit;
  }
  *out = value;
  return 1;
}

static void hex(const uint8_t digest[32], char out[65]) {
  static const char digits[] = "0123456789abcdef";
  unsigned i;
  for (i = 0U; i < 32U; ++i) {
    out[2U * i] = digits[digest[i] >> 4];
    out[2U * i + 1U] = digits[digest[i] & 15U];
  }
  out[64] = '\0';
}

/* ---- SEG-125 (ADR 0273): the opt-in headless performance session. SEGARECOMP_PERF_REPORT=<file> runs the SAME runner in bounded slices (the re-entrant
 * contract the viewer uses) and reads the host clock once per slice; SEGARECOMP_PERF_MODE=summary|detailed, SEGARECOMP_PERF_PRIVACY=private|shareable and
 * SEGARECOMP_PERF_SLICE=<dispatches> are optional. The guest sees nothing of it: no host time flows into the machine, and the sound summary, digests and stop
 * class are those of the unprofiled run (verified by tests/perf_equivalence_test.py). Without the variable none of this code runs. ---- */
static uint64_t perf_clock(void) {
#if defined(_WIN32)
  LARGE_INTEGER counter, frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return (uint64_t)((double)counter.QuadPart * 1.0e9 / (double)frequency.QuadPart) + 1U;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec + 1U; /* never 0: 0 means "span not started" */
#endif
}

static void perf_write(const SvPerf *perf, const char *path, const char *status, const char *outcome, const char *privacy, uint64_t slice, const GenesisRuntime *runtime,
                       uint64_t dispatches, uint64_t slices) {
  SvPerfMeta meta;
  SvPerf copy = *perf;
  FILE *f;
  const GenesisMixer *mixer = genesis_sound_mixer();
  memset(&meta, 0, sizeof meta);
  meta.platform = "genesis";
  meta.entry = "headless";
  meta.status = status;
  meta.outcome = outcome;
  meta.privacy = privacy;
  meta.paced = 0;
  meta.slice_dispatches = slice;
  copy.counters[0] = dispatches;
  copy.counters[1] = slices;
  copy.counters[2] = runtime->z80_epoch.epoch_count;
  copy.counters[3] = mixer != NULL ? mixer->frames : 0U;
  copy.guest_master_ticks = runtime->scheduler.master_ticks;
  meta.counter_names[0] = "m68k_dispatches";
  meta.counter_names[1] = "runner_slices";
  meta.counter_names[2] = "z80_epochs";
  meta.counter_names[3] = "audio_frames";
  meta.unavailable[0][0] = "m68k_time"; meta.unavailable[0][1] = "aggregated in emulation: timing it would need a clock read per instruction";
  meta.unavailable[1][0] = "z80_time"; meta.unavailable[1][1] = "aggregated in emulation: the Z80 is synchronized inside M68K retirement";
  meta.unavailable[2][0] = "vdp_compose"; meta.unavailable[2][1] = "headless runs do not compose frames unless capture is requested";
  meta.unavailable[3][0] = "present"; meta.unavailable[3][1] = "headless: there is no presentation";
  meta.unavailable[4][0] = "dma_interrupt_counts"; meta.unavailable[4][1] = "no existing inexpensive counter";
  f = fopen(path, "wb");
  if (f == NULL) { fprintf(stderr, "perf: cannot write the performance report\n"); return; }
  sv_perf_write_json(&copy, &meta, f);
  if (fclose(f) != 0) fprintf(stderr, "perf: the performance report could not be written completely\n");
}

static GenesisControlTransfer run_profiled(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance, const char *path) {
  static SvPerf perf; /* ~100 KiB: static, never on the stack */
  const char *mode = getenv("SEGARECOMP_PERF_MODE"), *privacy = getenv("SEGARECOMP_PERF_PRIVACY"), *slice_text = getenv("SEGARECOMP_PERF_SLICE");
  uint64_t slice_size = 4096U, total, dispatches = 0U, slices = 0U, last_vf;
  GenesisControlTransfer t;
  if ((mode != NULL && strcmp(mode, "summary") != 0 && strcmp(mode, "detailed") != 0) || (privacy != NULL && strcmp(privacy, "private") != 0 && strcmp(privacy, "shareable") != 0) ||
      (slice_text != NULL && (!parse_u64(slice_text, strlen(slice_text), &slice_size) || slice_size == 0U || slice_size > UINT32_MAX))) {
    fprintf(stderr, "perf: malformed SEGARECOMP_PERF_MODE / SEGARECOMP_PERF_PRIVACY / SEGARECOMP_PERF_SLICE\n");
    exit(3);
  }
  if (privacy == NULL) privacy = "private";
  sv_perf_init(&perf, perf_clock, 16683333U /* NTSC frame, a documented constant (3420 * 262 master ticks at 53.693175 MHz); PAL runs are compared per console */,
               mode != NULL && strcmp(mode, "detailed") == 0);
  sv_perf_enable(&perf);
  perf_write(&perf, path, "incomplete", "running", privacy, slice_size, runtime, 0U, 0U); /* a killed session still leaves a marked report */
  total = dispatch_allowance == UINT32_MAX ? UINT64_MAX : dispatch_allowance;
  last_vf = runtime->virtual_frame_count;
  for (;;) {
    const uint64_t remaining = total - dispatches;
    const uint32_t slice = remaining < slice_size ? (uint32_t)remaining : (uint32_t)slice_size;
    const uint64_t t0 = sv_perf_span_begin(&perf);
    uint64_t crossed;
    if (remaining == 0U) { memset(&t, 0, sizeof t); t.kind = GENESIS_RUNNER_RESOURCE_LIMIT; t.next_pc = runtime->pc; break; }
    t = genesis_runtime_run(runtime, dispatch, slice);
    sv_perf_span_end(&perf, SV_SPAN_EMULATION, t0);
    ++slices;
    dispatches += t.kind == GENESIS_RUNNER_RESOURCE_LIMIT ? t.runner_dispatch_count : 1U;
    crossed = runtime->virtual_frame_count - last_vf;
    last_vf = runtime->virtual_frame_count;
    sv_perf_frames(&perf, runtime->virtual_frame_count, crossed, 0);
    if (t.kind != GENESIS_RUNNER_RESOURCE_LIMIT) break;
  }
  sv_perf_finish(&perf);
  {
    const char *outcome = t.kind == GENESIS_STOP ? "guest_stop" : (t.kind == GENESIS_COMPLETE ? "guest_complete" : "runner_resource_limit");
    perf_write(&perf, path, t.kind == GENESIS_STOP ? "incomplete" : "complete", outcome, privacy, slice_size, runtime, dispatches, slices);
  }
  if (t.kind == GENESIS_RUNNER_RESOURCE_LIMIT) t.runner_dispatch_count = (uint32_t)(dispatches >= UINT32_MAX ? UINT32_MAX - 1U : dispatches); /* UINT32_MAX is reserved: "allowance exhausted" */
  return t;
}

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance) {
  GenesisControlTransfer transfer;
  const char *perf_path = getenv("SEGARECOMP_PERF_REPORT");
  const GenesisZ80Machine *machine = genesis_sound_attach(runtime);
  GenesisMixer *mixer = genesis_sound_mixer();
  const char *pcm_path = getenv("SEGARECOMP_AUDIO_PCM_OUT");
  const char *range = getenv("SEGARECOMP_AUDIO_RANGE");
  if (mixer != NULL && mixer->frames == 0U) {  /* configure once, before the first frame (the runner may be re-entered) */
    if (range != NULL) {
      const char *colon = strchr(range, ':');
      uint64_t first = 0U, count = 0U;
      if (colon == NULL || !parse_u64(range, (size_t)(colon - range), &first) || !parse_u64(colon + 1, strlen(colon + 1), &count) ||
          count == 0U) {
        fprintf(stderr, "sound: malformed SEGARECOMP_AUDIO_RANGE\n");
        exit(3);
      }
      genesis_mixer_set_range(mixer, first, count);
    }
    if (pcm_path != NULL && g_pcm.file == NULL) {
      g_pcm.file = fopen(pcm_path, "wb");
      if (g_pcm.file == NULL) ++g_pcm.write_errors;  /* an unusable artifact path never perturbs the run */
      genesis_mixer_set_sink(mixer, pcm_sink, &g_pcm);
    }
  }
  if (perf_path != NULL && *perf_path != '\0' && genesis_frame_capture_requested()) {
    fprintf(stderr, "perf: a frame-capture run is not measured; no performance report is written\n");
    perf_path = NULL;
  }
  transfer = genesis_frame_capture_requested() ? genesis_frame_capture_run_from_environment(runtime, dispatch, dispatch_allowance)
             : (perf_path != NULL && *perf_path != '\0') ? run_profiled(runtime, dispatch, dispatch_allowance, perf_path)
                                                          : genesis_runtime_run(runtime, dispatch, dispatch_allowance);
  genesis_sound_finish(runtime, transfer.kind == GENESIS_STOP);
  if (g_pcm.file != NULL && fflush(g_pcm.file) != 0) ++g_pcm.write_errors;
  /* Opt-in aggregate counters (counts only: never a byte, address or identity). */
  if (getenv("SEGARECOMP_SOUND_SUMMARY") != NULL) {
    const GenesisAudio *audio = genesis_sound_audio();
    char digest_hex[65] = "", range_hex[65] = "";
    uint8_t digest[32];
    if (mixer != NULL) {
      genesis_mixer_digest(mixer, digest);
      hex(digest, digest_hex);
      genesis_mixer_range_digest(mixer, digest);
      hex(digest, range_hex);
    }
    fprintf(stderr,
            "SOUND_SUMMARY {\"epochs\":%u,\"bound_image\":%u,\"sound_fault\":%d,\"sound_fault_epoch\":%u,\"psg_writes\":%llu,\"ym_writes\":%llu,\"virtual_frames\":%llu,\"result_kind\":%d,\"stop_class\":%d,\"diagnostic\":%d,"
            "\"classes\":{\"reset_assert\":%llu,\"reset_release\":%llu,\"busreq_assert\":%llu,\"busreq_release\":%llu,\"bank_writes\":%llu,\"banked_reads\":%llu,\"ym_dac\":%llu,\"ym_dac_enable\":%llu,\"ym_key\":%llu,\"ym_timer\":%llu,\"ym_global\":%llu,\"ym_operator\":%llu,\"ym_address\":%llu},"
            "\"audio\":{\"rate_hz\":%llu,\"channels\":%u,\"frames\":%llu,\"sha256\":\"%s\",\"range_frames\":%llu,\"range_sha256\":\"%s\","
            "\"clipped\":%llu,\"changes\":%llu,\"span\":%d,\"non_silent\":%s,\"fault\":%d,\"ring_dropped\":%llu,\"pcm_file_errors\":%llu}}\n",
            (unsigned)runtime->z80_epoch.epoch_count, (unsigned)machine->bound_ordinal, (int)machine->sound_fault,
            (unsigned)machine->sound_fault_epoch,
            (unsigned long long)(audio != NULL ? audio->psg_writes : 0U), (unsigned long long)(audio != NULL ? audio->ym_writes : 0U),
            (unsigned long long)(runtime->scheduler.master_ticks / GENESIS_NTSC_MASTER_TICKS_PER_FRAME), (int)transfer.kind,
            transfer.kind == GENESIS_STOP ? (int)transfer.stop.stop_class : 0,
            transfer.kind == GENESIS_STOP ? (int)transfer.stop.diagnostic_category : 0,
            C(machine->count_reset_assert), C(machine->count_reset_release), C(machine->count_busreq_assert), C(machine->count_busreq_release),
            C(machine->count_bank_writes), C(machine->count_banked_reads),
            C(audio != NULL ? audio->ym_class_dac : 0U), C(audio != NULL ? audio->ym_class_dac_enable : 0U), C(audio != NULL ? audio->ym_class_key : 0U),
            C(audio != NULL ? audio->ym_class_timer : 0U), C(audio != NULL ? audio->ym_class_global : 0U), C(audio != NULL ? audio->ym_class_operator : 0U),
            C(audio != NULL ? audio->ym_class_address : 0U),
            (unsigned long long)GENESIS_MIXER_RATE_HZ, (unsigned)GENESIS_MIXER_CHANNELS,
            (unsigned long long)(mixer != NULL ? mixer->frames : 0U), digest_hex,
            (unsigned long long)(mixer != NULL ? mixer->range_frames : 0U), range_hex,
            (unsigned long long)(mixer != NULL ? mixer->clipped : 0U), (unsigned long long)(mixer != NULL ? mixer->changes : 0U),
            mixer != NULL ? (int)genesis_mixer_span(mixer) : 0, mixer != NULL && genesis_mixer_non_silent(mixer) ? "true" : "false",
            mixer != NULL ? (int)mixer->fault : 0, (unsigned long long)(mixer != NULL ? mixer->ring_dropped : 0U),
            (unsigned long long)g_pcm.write_errors);
  }
  return transfer;
}
#undef C
