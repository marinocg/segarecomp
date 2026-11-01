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
/* SEG-120 (ADR 0263): Level 2 extended backgrounds. Attaches the adapter-owned preallocated extension target to the rig's renderer (it survives resets).
 * The viewer sets the target's request from the saved Widescreen Mode once per guest frame (never inside one) and presents the surface only when its
 * stamped sequence/geometry/class are valid; otherwise the original picture is shown. Without this call the viewer stays Original and reports Level 2
 * as Unavailable. Level 2 is at best Partial: the Mode 4 name table wraps, so the extension repeats the scenery already on screen. Returns 0, or -1. */
int sms_sdl3_viewer_attach_extension(SmsSdl3Viewer *viewer, SmsViewerRig *rig);
/* The extension target (tests and automation only; NULL when not attached) and the shared host. */
SmsExtension *sms_sdl3_viewer_extension(SmsSdl3Viewer *viewer);
struct SvHost *sms_sdl3_viewer_sv_host(SmsSdl3Viewer *viewer);
void sms_sdl3_viewer_host(SmsSdl3Viewer *viewer, SmsViewerHost *host);
/* 1 when an audio stream is open; `overruns` (optional) counts chunks dropped because the queue was full. */
int sms_sdl3_viewer_audio_active(const SmsSdl3Viewer *viewer, uint64_t *overruns);
/* Shows why the guest stopped instead of a blank window; blocks until dismissed (no-op on a headless video driver). */
void sms_sdl3_viewer_stop_screen(SmsSdl3Viewer *viewer, const char *title, const char *detail);
void sms_sdl3_viewer_close(SmsSdl3Viewer *viewer);
#ifdef __cplusplus
}
#endif
#endif
