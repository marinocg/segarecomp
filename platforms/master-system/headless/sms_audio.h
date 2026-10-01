#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_AUDIO_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_AUDIO_H

#include <stdio.h>

#include "segarecomp/device/sega/psg/sn76489.h"
#include "sms_sha256.h"

/* Headless audio artifacts (SEG-009-T007; ADR 0064 section 5): `audio.pcm` (s16le mono 44,100 Hz, every sample of the
 * run) and `audio.sha256`, one line per frame that received samples plus the run line:
 *   frame <n> <first sample index> <sample count> <sha256 of that frame's PCM bytes>
 *   run <total samples> <sha256 of the whole audio.pcm>
 * Sample k belongs to frame floor(T_k / frame_cycles), T_k its start T-state. A consumer of `Sn76489PcmSink`. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmsAudioCapture {
  FILE *pcm_file;
  FILE *sums_file;
  Sn76489PcmConfig pcm_config;
  uint64_t frame_cycles;
  SmsSha256 run;
  SmsSha256 frame;
  uint64_t frame_number, frame_first, frame_samples, total_samples;
  int frame_open;
  int failed;
} SmsAudioCapture;

/* Opens `<dir>/audio.pcm` and `<dir>/audio.sha256` (the directory must exist). Returns 0 on failure. */
int sms_audio_capture_open(SmsAudioCapture *capture, const char *dir, const Sn76489PcmConfig *pcm_config, uint64_t frame_cycles);
void sms_audio_capture_sink(void *capture, uint64_t first_index, const int16_t *samples, uint32_t count);
/* Writes the last frame line and the run line and closes both files. Returns 0 if any write failed. */
int sms_audio_capture_close(SmsAudioCapture *capture);

#ifdef __cplusplus
}
#endif
#endif
