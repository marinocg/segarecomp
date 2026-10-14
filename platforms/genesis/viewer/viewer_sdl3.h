#ifndef SEGARECOMP_VIEWER_GENESIS_VIEWER_SDL3_H
#define SEGARECOMP_VIEWER_GENESIS_VIEWER_SDL3_H
/* SEG-007-T254: optional SDL3 presenter/clock/sleeper for the viewer core. */
#include "genesis_audio_sink.h"
#include "viewer.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GenesisSdl3Viewer GenesisSdl3Viewer;
/* Returns NULL on failure (SDL unavailable etc.). */
GenesisSdl3Viewer *genesis_sdl3_viewer_open(const char *title, int scale);
/* SEG-032-T009: opens the audio output (stereo s16, the mixer's frozen rate) when the audio subsystem and a device are available.
 * Returns NULL (a silent viewer) on any failure; never fatal. The sink stays valid until genesis_sdl3_viewer_close. */
const GenesisAudioSink *genesis_sdl3_viewer_audio_open(GenesisSdl3Viewer *viewer);
/* The audio presenter's overrun counter, shown in the System page (optional; the pointer must outlive the viewer). */
void genesis_sdl3_viewer_report_audio_drops(GenesisSdl3Viewer *viewer, const uint64_t *overrun_dropped);
/* Shows why the guest stopped instead of a blank window; blocks until dismissed (no-op on a headless video driver). */
void genesis_sdl3_viewer_stop_screen(GenesisSdl3Viewer *viewer, const char *title, const char *detail);
/* Fills `host` with SDL3-backed callbacks bound to `viewer`. */
void genesis_sdl3_viewer_host(GenesisSdl3Viewer *viewer, GenesisViewerHost *host);
void genesis_sdl3_viewer_close(GenesisSdl3Viewer *viewer);
#ifdef __cplusplus
}
#endif
#endif
