/* SEG-057-T002 (ADR 0119): a display-less stand-in for the SDL3 viewer adapter (sms_viewer_sdl3.h), so the REAL viewer `main` and core can be
 * built and run on any host without SDL3: no window, no input, no audio, a synthetic monotonic clock. Test-side only, never shipped. */
#include <stdlib.h>
#include <string.h>

#include "sms_viewer_sdl3.h"

struct SmsSdl3Viewer {
  uint64_t clock;
};

static uint64_t stub_now(void *ctx) { return (((struct SmsSdl3Viewer *)ctx)->clock += 1000u); }
static void stub_sleep(void *ctx, uint64_t ns) { ((struct SmsSdl3Viewer *)ctx)->clock += ns; }
static int stub_poll(void *ctx, SmsViewerInput *in) {
  (void)ctx;
  memset(in, 0, sizeof *in);
  return 0;
}
static int stub_present(void *ctx, const uint8_t *framebuffer, uint32_t height) {
  (void)ctx; (void)framebuffer; (void)height;
  return 0;
}

SmsSdl3Viewer *sms_sdl3_viewer_open(const char *title, int scale, int want_audio) {
  (void)title; (void)scale; (void)want_audio;
  return (SmsSdl3Viewer *)calloc(1, sizeof(struct SmsSdl3Viewer));
}
void sms_sdl3_viewer_host(SmsSdl3Viewer *viewer, SmsViewerHost *host) {
  memset(host, 0, sizeof *host);
  host->ctx = viewer;
  host->now_ns = stub_now;
  host->sleep_ns = stub_sleep;
  host->poll = stub_poll;
  host->present = stub_present;
}
void sms_sdl3_viewer_stop_screen(SmsSdl3Viewer *viewer, const char *title, const char *detail) {
  (void)viewer; (void)title; (void)detail; /* no window: nothing to show */
}
int sms_sdl3_viewer_audio_active(const SmsSdl3Viewer *viewer, uint64_t *overruns) {
  (void)viewer;
  if (overruns != NULL) *overruns = 0u;
  return 0;
}
void sms_sdl3_viewer_close(SmsSdl3Viewer *viewer) { free(viewer); }
