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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

GenesisControlTransfer genesis_sound_hook_run(GenesisRuntime *runtime, GenesisDispatchFunction dispatch, uint32_t dispatch_allowance) {
  GenesisControlTransfer transfer;
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
  transfer = genesis_runtime_run(runtime, dispatch, dispatch_allowance);
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
