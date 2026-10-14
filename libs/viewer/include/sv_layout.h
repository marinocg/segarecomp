#ifndef SEGARECOMP_VIEWER_SV_LAYOUT_H
#define SEGARECOMP_VIEWER_SV_LAYOUT_H

/* Pure presentation math (SDL-free): destination rectangle of the game picture, and the CRT pixel profiles. */
#include <stdint.h>

#include "sv_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SvRect { int x, y, w, h; } SvRect;

/* Largest rectangle for a `src_w` x `src_h` picture inside a `win_w` x `win_h` output, centred.
 *  SV_ASPECT_ORIGINAL: square pixels (the picture keeps src_w:src_h).
 *  SV_ASPECT_4_3: the picture is stretched horizontally to a 4:3 display aspect (the TV picture of both consoles).
 *  SV_SCALE_FIT: any size that fits.  SV_SCALE_INTEGER: the vertical scale is a whole multiple (horizontal follows the aspect);
 *  when the output is smaller than 1x it falls back to fit. A zero or negative size returns an empty rectangle. */
SvRect sv_layout_dest(int win_w, int win_h, int src_w, int src_h, SvAspect aspect, SvScaleMode scale);

/* Barrel distortion of a normalized coordinate pair in [-1, 1] (curvature 0..100). Maps the undistorted position of a mesh vertex to its
 * position on the curved tube; 0 is the identity. `vignette` receives a 0..1 brightness factor for the corner falloff. */
void sv_crt_warp(float nx, float ny, int curvature, float *out_x, float *out_y, float *vignette);

/* One scanline-gap alpha (0..255) for output row `row` when each source line spans `period` output rows: 0 on the bright part of the
 * line, rising to `strength` percent of 255 at the dark gap between lines. Smooth, periodic, pure. */
uint8_t sv_crt_scanline_alpha(int row, float period, int strength);

/* Aperture-mask multiplier (0..255) for output column `col` and channel 0..2 (R,G,B): 255 on the channel's stripe, reduced elsewhere. */
uint8_t sv_crt_mask_value(int col, int channel, int strength);

/* True when the settings describe the untouched nearest-neighbour picture (no overlay, no warp, no brightness change). */
int sv_crt_is_identity(const SvVideoSettings *video);

#ifdef __cplusplus
}
#endif
#endif
