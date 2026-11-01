#ifndef SEGARECOMP_VIEWER_SV_LAYOUT_H
#define SEGARECOMP_VIEWER_SV_LAYOUT_H

/* Pure presentation math (SDL-free): destination rectangle of the game picture, and the CRT pixel profiles. */
#include <stddef.h>
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

/* ---- SEG-118: presentation-only 16:9 extension geometry (pure; no SDL, no platform knowledge) ----
 * The extended surface is the native picture plus `side` source pixels of extra scenery on EACH side (always symmetric, so the native picture is
 * exactly centred). The display aspect is 16:9; a window of another shape only letterboxes it. Native pixels keep the aspect policy: SV_ASPECT_ORIGINAL
 * shows square pixels, SV_ASPECT_4_3 stretches the native picture to 4:3 and the extension gets the same pixel shape (4:3 content is never stretched to 16:9). */
#define SV_EXT_MAX_SIDE 256 /* hard bound of one side's extension in source pixels (documents the maximum allocation) */

/* Source pixels of extension per side for a `src_w` x `src_h` native picture so the whole surface displays at 16:9, clamped to [0, SV_EXT_MAX_SIDE].
 * 0 for an invalid size or a native picture already at least as wide as 16:9 (extension adds nothing). Depends only on the native geometry and the
 * aspect policy, never on the window, so a window resize does not change what the producer must make. */
int sv_layout_extension_side(int src_w, int src_h, SvAspect aspect);
/* Largest extended surface any picture up to `max_w` x `max_h` can need: width max_w + 2 * SV_EXT_MAX_SIDE, height max_h (0 for invalid input). */
void sv_layout_extension_max(int max_w, int max_h, int *width_out, int *height_out);

typedef struct SvExtendedLayout {
  SvRect surface; /* where the whole extended surface is drawn inside the window (centred, letterboxed) */
  SvRect native;  /* where the native picture lies inside the window (centred on `surface`, same scale) */
} SvExtendedLayout;
/* Placement of an extended surface (`side` pixels each side of a `src_w` x `src_h` native picture) in a `win_w` x `win_h` output. Same scale rules as
 * sv_layout_dest (fit, or whole-number vertical scale with the fit fallback below 1x), applied to the extended surface so the native picture keeps the
 * very scale it would have had with the extension. Returns 0, or -1 (both rectangles empty) for any invalid size or side. */
int sv_layout_extended_dest(int win_w, int win_h, int src_w, int src_h, int side, SvAspect aspect, SvScaleMode scale, SvExtendedLayout *out);

/* The borrowed extended surface a platform adapter hands to the host for ONE frame (never retained, never written by the host). The native picture sits
 * `side` pixels in from the left edge and `side` from the right edge; `sequence` is the frame identity the producer stamped, `mode` the Widescreen
 * mode that was actually produced for that frame, `status` the typed outcome (UNAVAILABLE = ignore the surface). */
typedef struct SvExtendedSurface {
  const uint8_t *rgb24;
  size_t size;      /* readable bytes at rgb24 */
  int width, height, pitch;
  int side;
  uint64_t sequence;
  SvWidescreenMode mode;
  SvWidescreenSupport status;
} SvExtendedSurface;

typedef enum SvExtCheck {
  SV_EXT_OK = 0,
  SV_EXT_NO_SURFACE,  /* absent surface/buffer */
  SV_EXT_STATUS,      /* status UNAVAILABLE (or unknown) */
  SV_EXT_MODE,        /* the frame was produced as Original */
  SV_EXT_SEQUENCE,    /* identity differs from the frame being presented (stale/mismatched) */
  SV_EXT_GEOMETRY,    /* size/side inconsistent with the native picture, not centred, or beyond the bound */
  SV_EXT_PITCH,
  SV_EXT_SIZE         /* the readable bytes do not cover the surface */
} SvExtCheck;
/* Validates `surface` as an extension of the `native_w` x `native_h` picture with identity `native_sequence`, for hosts that accept at most `max_w` native
 * pixels per row. SV_EXT_OK means every pixel the host will read is inside `size`. Pure; no overflow for any input. */
SvExtCheck sv_extended_surface_check(const SvExtendedSurface *surface, int native_w, int native_h, uint64_t native_sequence, int max_w);

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
