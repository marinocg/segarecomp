#include "sms_audio.h"

static void hex(const uint8_t *bytes, size_t n, char *out) {
  static const char digits[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; ++i) {
    out[2u * i] = digits[bytes[i] >> 4];
    out[2u * i + 1u] = digits[bytes[i] & 15u];
  }
  out[2u * n] = '\0';
}

static void finish_frame(SmsAudioCapture *c) {
  uint8_t digest[32];
  char text[65];
  if (!c->frame_open) return;
  sms_sha256_final(&c->frame, digest);
  hex(digest, sizeof digest, text);
  if (fprintf(c->sums_file, "frame %llu %llu %llu %s\n", (unsigned long long)c->frame_number,
              (unsigned long long)c->frame_first, (unsigned long long)c->frame_samples, text) < 0)
    c->failed = 1;
  c->frame_open = 0;
}

int sms_audio_capture_open(SmsAudioCapture *c, const char *dir, const Sn76489PcmConfig *pcm_config, uint64_t frame_cycles) {
  char path[1024];
  c->failed = 0;
  c->frame_open = 0;
  c->total_samples = 0;
  c->pcm_config = *pcm_config;
  c->frame_cycles = frame_cycles;
  sms_sha256_init(&c->run);
  if (snprintf(path, sizeof path, "%s/audio.pcm", dir) >= (int)sizeof path) return 0;
  c->pcm_file = fopen(path, "wb");
  if (c->pcm_file == NULL) return 0;
  if (snprintf(path, sizeof path, "%s/audio.sha256", dir) >= (int)sizeof path) return 0;
  c->sums_file = fopen(path, "wb");
  if (c->sums_file == NULL) {
    fclose(c->pcm_file);
    c->pcm_file = NULL;
    return 0;
  }
  return 1;
}

void sms_audio_capture_sink(void *context, uint64_t first_index, const int16_t *samples, uint32_t count) {
  SmsAudioCapture *c = (SmsAudioCapture *)context;
  uint32_t i;
  if (c->pcm_file == NULL) return;
  for (i = 0; i < count; ++i) {
    const uint64_t index = first_index + i;
    const uint64_t frame = sn76489_pcm_sample_start(&c->pcm_config, index) / c->frame_cycles;
    const uint16_t bits = (uint16_t)samples[i];
    const uint8_t bytes[2] = {(uint8_t)(bits & 0xFFu), (uint8_t)(bits >> 8)};
    if (c->frame_open && frame != c->frame_number) finish_frame(c);
    if (!c->frame_open) {
      sms_sha256_init(&c->frame);
      c->frame_open = 1;
      c->frame_number = frame;
      c->frame_first = index;
      c->frame_samples = 0;
    }
    sms_sha256_update(&c->frame, bytes, 2);
    sms_sha256_update(&c->run, bytes, 2);
    c->frame_samples += 1u;
    c->total_samples += 1u;
    if (fwrite(bytes, 1, 2, c->pcm_file) != 2u) c->failed = 1;
  }
}

int sms_audio_capture_close(SmsAudioCapture *c) {
  uint8_t digest[32];
  char text[65];
  if (c->pcm_file == NULL) return 0;
  finish_frame(c);
  sms_sha256_final(&c->run, digest);
  hex(digest, sizeof digest, text);
  if (fprintf(c->sums_file, "run %llu %s\n", (unsigned long long)c->total_samples, text) < 0) c->failed = 1;
  if (fclose(c->pcm_file) != 0) c->failed = 1;
  if (fclose(c->sums_file) != 0) c->failed = 1;
  c->pcm_file = NULL;
  c->sums_file = NULL;
  return !c->failed;
}
