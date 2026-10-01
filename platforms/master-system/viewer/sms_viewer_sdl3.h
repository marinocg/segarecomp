#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_VIEWER_SDL3_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_VIEWER_SDL3_H
/* SEG-009-T009: optional SDL3 window, keyboard and audio adapter for the SMS viewer core. Never linked by headless
 * targets. Audio is best effort: with no device (or `want_audio` 0) the viewer is silent and the guest is identical. */
#include "sms_viewer.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SmsSdl3Viewer SmsSdl3Viewer;
/* Returns NULL when the window cannot be created. `scale` >= 1 multiplies the 256 x 192 framebuffer. */
SmsSdl3Viewer *sms_sdl3_viewer_open(const char *title, int scale, int want_audio);
void sms_sdl3_viewer_host(SmsSdl3Viewer *viewer, SmsViewerHost *host);
/* 1 when an audio stream is open; `overruns` (optional) counts chunks dropped because the queue was full. */
int sms_sdl3_viewer_audio_active(const SmsSdl3Viewer *viewer, uint64_t *overruns);
void sms_sdl3_viewer_close(SmsSdl3Viewer *viewer);
#ifdef __cplusplus
}
#endif
#endif
