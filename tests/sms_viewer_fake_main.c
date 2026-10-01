/* SEG-009-T009 test host: drives the SMS viewer core (no window, fake clock, fake audio) over a generated program.
 *   <prog> --frames <N> --poll-script <file> [--record <file>] [--reset-at <k>] [--audio ok|fail|none]
 * The poll-script is a scripted-input file: poll k returns the state in force at frame k. Prints machine-readable lines:
 *   outcome, digest, per-frame framebuffer hashes (the presented frames), the PCM hash, virtual sleep time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_viewer.h"

extern const uint8_t sms_rom_data[];
extern const uint32_t sms_rom_size;
extern const uint32_t sms_rom_mapper_family;

static SmsViewerRig rig;
static SmsInputEvent script[4096];
static uint32_t script_count;
static char text[1 << 20];
static char record_text[SMS_VIEWER_INPUT_CAPACITY * 40u];

typedef struct Fake {
  uint64_t now, slept;
  uint32_t polls;
  long reset_at;
  int audio_mode; /* 0 ok, 1 fail, 2 none */
  SmsSha256 pcm;
  uint64_t pcm_samples, presented_hash_frames;
  SmsSha256 fb;
  uint32_t fb_frames;
  uint8_t fb_hashes[8192][32];
} Fake;
static Fake fake;

static void hex(const uint8_t *b, size_t n, char *out) {
  static const char d[] = "0123456789abcdef";
  size_t i;
  for (i = 0; i < n; ++i) {
    out[2 * i] = d[b[i] >> 4];
    out[2 * i + 1] = d[b[i] & 15];
  }
  out[2 * n] = '\0';
}

static uint64_t f_now(void *c) { return ((Fake *)c)->now; }
static void f_sleep(void *c, uint64_t ns) {
  ((Fake *)c)->now += ns;
  ((Fake *)c)->slept += ns;
}
static int f_poll(void *c, SmsViewerInput *in) {
  Fake *f = (Fake *)c;
  const uint32_t k = f->polls++;
  uint32_t i;
  SmsInputState s = {0, 0, 0};
  for (i = 0; i < script_count && script[i].frame <= k; ++i) {
    s.p1 = script[i].p1;
    s.p2 = script[i].p2;
    s.pause = script[i].pause;
  }
  in->p1 = s.p1;
  in->p2 = s.p2;
  in->pause = s.pause;
  if ((long)k == f->reset_at) {
    in->reset = 1;
    sms_sha256_init(&f->pcm); /* the PCM and framebuffer records restart with the machine */
    f->pcm_samples = 0;
    f->fb_frames = 0;
  }
  return 0;
}
static int f_present(void *c, const uint8_t *fbuf, uint32_t height) {
  Fake *f = (Fake *)c;
  SmsSha256 s;
  sms_sha256_init(&s);
  sms_sha256_update(&s, fbuf, (size_t)SMS_FB_WIDTH * height);
  if (f->fb_frames < 8192u) sms_sha256_final(&s, f->fb_hashes[f->fb_frames]);
  f->fb_frames++;
  return 0;
}
static int f_audio(void *c, const int16_t *samples, uint32_t count) {
  Fake *f = (Fake *)c;
  uint32_t i;
  for (i = 0; i < count; ++i) {
    sms_sha256_update_u16(&f->pcm, (uint16_t)samples[i]); /* s16le */
  }
  f->pcm_samples += count;
  return f->audio_mode == 1 ? -1 : 0;
}

int main(int argc, char **argv) {
  uint64_t frames = 0;
  const char *poll_path = NULL, *record_path = NULL;
  SmsViewerHost host;
  SmsViewerResult r;
  SmsPacer pacer;
  SmsError error;
  uint8_t digest[32], pcm[32];
  char h[65];
  int i;
  memset(&fake, 0, sizeof fake);
  fake.reset_at = -1;
  for (i = 1; i + 1 < argc; i += 2) {
    if (strcmp(argv[i], "--frames") == 0) frames = strtoull(argv[i + 1], NULL, 10);
    else if (strcmp(argv[i], "--poll-script") == 0) poll_path = argv[i + 1];
    else if (strcmp(argv[i], "--record") == 0) record_path = argv[i + 1];
    else if (strcmp(argv[i], "--reset-at") == 0) fake.reset_at = strtol(argv[i + 1], NULL, 10);
    else if (strcmp(argv[i], "--audio") == 0) fake.audio_mode = strcmp(argv[i + 1], "fail") == 0 ? 1 : strcmp(argv[i + 1], "none") == 0 ? 2 : 0;
    else return 64;
  }
  if (poll_path != NULL) {
    FILE *f = fopen(poll_path, "rb");
    size_t n;
    if (f == NULL) return 64;
    n = fread(text, 1, sizeof text, f);
    fclose(f);
    if (sms_input_parse(text, n, script, 4096, &script_count) != 0) return 64;
  }
  if (!sms_viewer_rig_init(&rig, sms_rom_data, sms_rom_size, (SmsMapperFamily)sms_rom_mapper_family, &error)) return 4;
  sms_sha256_init(&fake.pcm);
  host.ctx = &fake;
  host.now_ns = f_now;
  host.sleep_ns = f_sleep;
  host.poll = f_poll;
  host.present = f_present;
  host.audio = fake.audio_mode == 2 ? NULL : f_audio;
  sms_pacer_init(&pacer, 0);
  r = sms_viewer_run(&rig, &host, &pacer, frames);
  if (fake.audio_mode == 2) { /* the ring was drained without a sink: hash the same stream by reading what is left (none) */
    fake.pcm_samples = r.audio_samples;
  }
  sms_machine_digest(&rig.machine, digest);
  sms_sha256_final(&fake.pcm, pcm);
  hex(digest, 32, h);
  printf("outcome %d frames %llu presented %llu resets %u\n", (int)r.outcome, (unsigned long long)r.frames,
         (unsigned long long)r.frames_presented, r.resets);
  printf("cycles %llu digest %s\n", (unsigned long long)rig.machine.rt.state.cycles, h);
  for (i = 0; i < (int)rig.renderer.record_count; ++i) {
    hex(rig.records[i].sha256, 32, h);
    printf("record %llu %s\n", (unsigned long long)rig.records[i].frame, h);
  }
  for (i = 0; i < (int)fake.fb_frames && i < 8192; ++i) {
    hex(fake.fb_hashes[i], 32, h);
    printf("present %d %s\n", i, h);
  }
  hex(pcm, 32, h);
  printf("pcm %llu %s audio_failures %llu slept %llu sleeps_expected %llu\n", (unsigned long long)fake.pcm_samples, h,
         (unsigned long long)r.audio_failures, (unsigned long long)fake.slept, (unsigned long long)pacer.sleep_calls);
  if (record_path != NULL) {
    const size_t n = sms_viewer_format_script(&rig, record_text, sizeof record_text);
    FILE *f = fopen(record_path, "wb");
    if (f == NULL || (n != 0u && fwrite(record_text, 1, n, f) != n) || fclose(f) != 0) return 64;
  }
  return r.outcome == SMS_VIEWER_FRAME_LIMIT ? 0 : 3;
}
