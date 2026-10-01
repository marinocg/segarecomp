/* Interactive Master System viewer: the generated program's `main` when built as a viewer (SEG-009-T009; ADR 0070).
 *
 *   <game> [--viewer-unthrottled] [--viewer-mute] [--viewer-frames <n>] [--viewer-scale <n>] [--viewer-record <file>]
 *
 * Keys: arrows = D-pad, Z = button 1, X = button 2, P = pause, R = reset, Esc/close = quit. `--viewer-record` writes the
 * input stream the run consumed as a headless `--input` script (the run since the last reset). Exit: 0 quit or frame
 * limit; 3 guest stop; 4 SMS_ERROR at start; 5 viewer/host failure; 64 usage. The ROM is the build-time embedded array. */
#include <stdio.h>
#include <string.h>

#include "sms_viewer_sdl3.h"

extern const uint8_t sms_rom_data[];
extern const uint32_t sms_rom_size;
extern const uint32_t sms_rom_mapper_family;

static SmsViewerRig rig;
static char script[SMS_VIEWER_INPUT_CAPACITY * 40u];

int main(int argc, char **argv) {
  SmsViewerOptions options;
  SmsViewerResult result;
  SmsViewerHost host;
  SmsPacer pacer;
  SmsSdl3Viewer *viewer;
  SmsError error;
  int status = 0;
  if (sms_viewer_options_parse(argc - 1, (const char *const *)(argv + 1), &options) != 0) {
    fprintf(stderr, "usage: %s [--viewer-unthrottled] [--viewer-mute] [--viewer-frames <n>] [--viewer-scale <n>] [--viewer-record <file>]\n", argv[0]);
    return 64;
  }
  if (!sms_viewer_rig_init(&rig, sms_rom_data, sms_rom_size, (SmsMapperFamily)sms_rom_mapper_family, &error)) {
    printf("sms_error %s\n", sms_error_name(error));
    return 4;
  }
  viewer = sms_sdl3_viewer_open("segarecomp Master System", (int)options.scale, !options.mute);
  if (viewer == NULL) {
    fprintf(stderr, "cannot open the SDL3 window\n");
    return 5;
  }
  sms_sdl3_viewer_host(viewer, &host);
  sms_pacer_init(&pacer, options.unthrottled);
  result = sms_viewer_run(&rig, &host, &pacer, options.frames);
  sms_sdl3_viewer_close(viewer);
  printf("viewer outcome %d frames %llu presented %llu audio_samples %llu audio_failures %llu resets %u\n", (int)result.outcome,
         (unsigned long long)result.frames, (unsigned long long)result.frames_presented,
         (unsigned long long)result.audio_samples, (unsigned long long)result.audio_failures, result.resets);
  if (options.record != NULL) {
    const size_t n = sms_viewer_format_script(&rig, script, sizeof script);
    FILE *f = fopen(options.record, "wb");
    if (f == NULL || (n != 0u && fwrite(script, 1, n, f) != n) || fclose(f) != 0) {
      fprintf(stderr, "cannot write %s\n", options.record);
      status = 64;
    }
  }
  if (status != 0) return status;
  if (result.outcome == SMS_VIEWER_GUEST_ERROR) return 3;
  return result.outcome == SMS_VIEWER_FRAME_LIMIT || result.outcome == SMS_VIEWER_QUIT ? 0 : 5;
}
