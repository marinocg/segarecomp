#ifndef SEGARECOMP_VIEWER_GENESIS_VIEWER_SDL3_H
#define SEGARECOMP_VIEWER_GENESIS_VIEWER_SDL3_H
/* SEG-007-T254: optional SDL3 presenter/clock/sleeper for the viewer core. */
#include "viewer.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GenesisSdl3Viewer GenesisSdl3Viewer;
/* Returns NULL on failure (SDL unavailable etc.). */
GenesisSdl3Viewer *genesis_sdl3_viewer_open(const char *title, int scale);
/* Fills `host` with SDL3-backed callbacks bound to `viewer`. */
void genesis_sdl3_viewer_host(GenesisSdl3Viewer *viewer, GenesisViewerHost *host);
void genesis_sdl3_viewer_close(GenesisSdl3Viewer *viewer);
#ifdef __cplusplus
}
#endif
#endif
