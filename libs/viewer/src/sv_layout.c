#include "sv_layout.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

SvRect sv_layout_dest(int win_w, int win_h, int src_w, int src_h, SvAspect aspect, SvScaleMode scale) {
  SvRect r = {0, 0, 0, 0};
  double aw, ah, factor;
  if (win_w <= 0 || win_h <= 0 || src_w <= 0 || src_h <= 0) return r;
  ah = (double)src_h;
  aw = aspect == SV_ASPECT_4_3 ? ah * 4.0 / 3.0 : (double)src_w;
  factor = (double)win_w / aw;
  if ((double)win_h / ah < factor) factor = (double)win_h / ah;
  if (scale == SV_SCALE_INTEGER && factor >= 1.0) factor = floor(factor + 1e-9);
  r.w = (int)(aw * factor + 0.5);
  r.h = (int)(ah * factor + 0.5);
  if (r.w > win_w) r.w = win_w;
  if (r.h > win_h) r.h = win_h;
  if (r.w < 1) r.w = 1;
  if (r.h < 1) r.h = 1;
  r.x = (win_w - r.w) / 2;
  r.y = (win_h - r.h) / 2;
  return r;
}

/* The native display width relative to its pixel width: aw/src_w, kept as the rational (num/den) of native display width over source width. */
static void ext_native_display(int src_w, int src_h, SvAspect aspect, int64_t *num, int64_t *den) {
  /* ORIGINAL: display width = src_w.  4:3: display width = src_h * 4 / 3. Both expressed over src_h so everything stays integral. */
  if (aspect == SV_ASPECT_4_3) { *num = (int64_t)src_h * 4; *den = 3; }
  else { *num = src_w; *den = 1; }
}

int sv_layout_extension_side(int src_w, int src_h, SvAspect aspect) {
  int64_t an, ad, total, extra;
  if (src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096) return 0;
  ext_native_display(src_w, src_h, aspect, &an, &ad);
  /* Required display width for 16:9 at this height is src_h*16/9; in source pixels that is (src_h*16/9) * src_w / (an/ad). */
  total = ((int64_t)src_h * 16 * (int64_t)src_w * ad) / (9 * an);
  extra = total - src_w;
  if (extra <= 1) return 0;
  extra /= 2;
  return extra > SV_EXT_MAX_SIDE ? SV_EXT_MAX_SIDE : (int)extra;
}

int sv_layout_visible_side(int src_w, int src_h, SvAspect aspect, int width_pct, int produced_side) {
  int64_t an, ad, total, extra, num, den;
  int side;
  int n, d;
  if (src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096 || produced_side <= 0) return 0;
  sv_widescreen_width_ratio(width_pct, &n, &d);
  num = n; den = d;
  ext_native_display(src_w, src_h, aspect, &an, &ad);
  total = ((int64_t)src_h * num * (int64_t)src_w * ad) / (den * an);
  extra = total - src_w;
  if (extra <= 1) return 0;
  extra /= 2;
  side = extra > SV_EXT_MAX_SIDE ? SV_EXT_MAX_SIDE : (int)extra;
  return side > produced_side ? produced_side : side;
}

void sv_layout_extension_max(int max_w, int max_h, int *width_out, int *height_out) {
  const int ok = max_w > 0 && max_h > 0 && max_w <= 4096 && max_h <= 4096;
  if (width_out != NULL) *width_out = ok ? max_w + 2 * SV_EXT_MAX_SIDE : 0;
  if (height_out != NULL) *height_out = ok ? max_h : 0;
}

int sv_layout_extended_dest(int win_w, int win_h, int src_w, int src_h, int side, SvAspect aspect, SvScaleMode scale, SvExtendedLayout *out) {
  SvRect none = {0, 0, 0, 0};
  double aw_native, aw_surface, ah, factor;
  SvRect s, n;
  if (out == NULL) return -1;
  out->surface = none;
  out->native = none;
  if (win_w <= 0 || win_h <= 0 || src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096 || side < 0 || side > SV_EXT_MAX_SIDE ||
      win_w > (1 << 20) || win_h > (1 << 20)) return -1;
  ah = (double)src_h;
  aw_native = aspect == SV_ASPECT_4_3 ? ah * 4.0 / 3.0 : (double)src_w;
  aw_surface = aw_native * (double)(src_w + 2 * side) / (double)src_w;
  factor = (double)win_w / aw_surface;
  if ((double)win_h / ah < factor) factor = (double)win_h / ah;
  if (scale == SV_SCALE_INTEGER && factor >= 1.0) factor = floor(factor + 1e-9);
  s.w = (int)(aw_surface * factor + 0.5);
  s.h = (int)(ah * factor + 0.5);
  if (s.w > win_w) s.w = win_w;
  if (s.h > win_h) s.h = win_h;
  if (s.w < 1) s.w = 1;
  if (s.h < 1) s.h = 1;
  s.x = (win_w - s.w) / 2;
  s.y = (win_h - s.h) / 2;
  n.w = (int)(aw_native * factor + 0.5);
  n.h = s.h;
  if (n.w > s.w) n.w = s.w;
  if (n.w < 1) n.w = 1;
  /* Centre exactly: the native rectangle shares the surface's centre (parity of the leftover is split towards the left, like sv_layout_dest). */
  n.x = s.x + (s.w - n.w) / 2;
  n.y = s.y;
  out->surface = s;
  out->native = n;
  return 0;
}

SvExtCheck sv_extended_surface_check(const SvExtendedSurface *e, int native_w, int native_h, uint64_t native_sequence, int max_w) {
  int64_t row_bytes, needed;
  if (e == NULL || e->rgb24 == NULL) return SV_EXT_NO_SURFACE;
  if (e->status != SV_WS_PARTIAL && e->status != SV_WS_AVAILABLE) return SV_EXT_STATUS;
  if (e->mode != SV_WIDESCREEN_EXTENDED_BACKGROUNDS && e->mode != SV_WIDESCREEN_ENHANCED_GAMEPLAY) return SV_EXT_MODE;
  if (e->sequence != native_sequence) return SV_EXT_SEQUENCE;
  if (native_w <= 0 || native_h <= 0 || max_w <= 0 || native_w > max_w || native_w > 4096 || native_h > 4096) return SV_EXT_GEOMETRY;
  if (e->side <= 0 || e->side > SV_EXT_MAX_SIDE || e->height != native_h || e->width != native_w + 2 * e->side) return SV_EXT_GEOMETRY;
  row_bytes = (int64_t)e->width * 3;
  if (e->pitch < row_bytes) return SV_EXT_PITCH;
  needed = (int64_t)e->pitch * (int64_t)(e->height - 1) + row_bytes;
  if ((uint64_t)needed > (uint64_t)e->size) return SV_EXT_SIZE;
  return SV_EXT_OK;
}

void sv_crt_warp(float nx, float ny, int curvature, float *out_x, float *out_y, float *vignette) {
  const float k = (float)curvature / 100.0f * 0.22f; /* strength of the barrel term, <= 0.22 keeps the mapping monotonic */
  const float r2 = nx * nx + ny * ny;
  /* Barrel: the middle of each edge stays on the rectangle, the corners are pulled inward, the middle is magnified. */
  const float f = (1.0f - k * r2) / (1.0f - k);
  if (out_x != NULL) *out_x = nx * f;
  if (out_y != NULL) *out_y = ny * f;
  if (vignette != NULL) {
    float v = 1.0f - (float)curvature / 100.0f * 0.35f * (r2 * 0.5f);
    *vignette = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  }
}

uint8_t sv_crt_scanline_alpha(int row, float period, int strength) {
  /* Box-filtered gap: the lower 45% of each source line is the dark gap; a row's alpha is the share of the row inside a gap. */
  float pos, lo, cover = 0.0f;
  int a, line;
  if (strength <= 0 || period < 1.5f) return 0; /* lines thinner than 1.5 output rows cannot be resolved: no fake moire */
  pos = fmodf((float)row, period);
  lo = period * 0.55f;
  for (line = 0; line < 2; ++line) {
    const float g0 = lo + (float)line * period, g1 = period + (float)line * period;
    const float o0 = pos > g0 ? pos : g0, o1 = pos + 1.0f < g1 ? pos + 1.0f : g1;
    if (o1 > o0) cover += o1 - o0;
  }
  if (cover > 1.0f) cover = 1.0f;
  a = (int)(cover * (float)strength * 2.55f + 0.5f);
  return (uint8_t)(a > 255 ? 255 : a);
}

uint8_t sv_crt_mask_value(int col, int channel, int strength) {
  const int on = (col % 3) == channel;
  const int dim = 255 - (strength * 255 * 6 / 10) / 100; /* strength 100 dims off-stripe channels to 40% */
  if (strength <= 0 || on) return 255;
  return (uint8_t)(dim < 0 ? 0 : dim);
}

int sv_crt_is_identity(const SvVideoSettings *v) {
  return v->scanlines == 0 && v->mask == 0 && v->curvature == 0 && v->bloom == 0 && v->brightness == 100;
}

/* ---- SEG-123: side-area treatment ---- */

SvSideTreatment sv_side_treatment(SvSideStyle style, SvExtSource source) {
  if (style == SV_SIDE_RAW) return SV_TREAT_PASSTHROUGH;
  if (style == SV_SIDE_BORDERS) return SV_TREAT_BORDERS;
  /* COMFORT (and any unknown style, which is the default): only an explicitly AUTHORED surface bypasses; every other source value is generic */
  return source == SV_EXTSRC_AUTHORED ? SV_TREAT_PASSTHROUGH : SV_TREAT_COMFORT;
}

#define SV_SIDE_EDGE_COLS 8   /* native columns averaged along each edge */
#define SV_SIDE_BLUR_RADIUS 12 /* vertical smoothing radius in rows */
#define SV_SIDE_MAX_FADE 257

size_t sv_side_scratch_bytes(int h) {
  if (h <= 0 || h > SV_SIDE_MAX_ROWS) return 0;
  return ((size_t)h * 12u + SV_SIDE_MAX_FADE) * sizeof(int32_t);
}

int sv_side_fill(SvSideTreatment treatment, const uint8_t *native, int w, int h, int pitch, int side, uint8_t *left, uint8_t *right, void *scratch) {
  static const int dark[3] = {6, 6, 8}, panel = 14;
  int32_t *raw, *smooth, *fade;
  int y, k, c, e, d;
  if (treatment != SV_TREAT_COMFORT && treatment != SV_TREAT_BORDERS) return -1;
  if (native == NULL || left == NULL || right == NULL || scratch == NULL || w <= 0 || h <= 0 || h > SV_SIDE_MAX_ROWS || side <= 0 || side > SV_EXT_MAX_SIDE ||
      pitch < w * 3)
    return -1;
  if (treatment == SV_TREAT_BORDERS) {
    memset(left, panel, (size_t)side * (size_t)h * 3u);
    memset(right, panel, (size_t)side * (size_t)h * 3u);
    return 0;
  }
  raw = (int32_t *)scratch;          /* [2][h][3]: left edge average, right edge average */
  smooth = raw + (size_t)h * 6u;     /* [2][h][3] */
  fade = smooth + (size_t)h * 6u;    /* [side]: 0..1024 intensity by distance from the native edge */
  e = w < SV_SIDE_EDGE_COLS ? w : SV_SIDE_EDGE_COLS;
  for (y = 0; y < h; ++y) {
    const uint8_t *row = native + (size_t)y * (size_t)pitch;
    for (c = 0; c < 3; ++c) {
      int32_t a = 0, b = 0;
      for (k = 0; k < e; ++k) { a += row[k * 3 + c]; b += row[(w - 1 - k) * 3 + c]; }
      raw[(size_t)y * 3u + (size_t)c] = a / e;
      raw[(size_t)h * 3u + (size_t)y * 3u + (size_t)c] = b / e;
    }
  }
  for (k = 0; k < 2; ++k) {
    const int32_t *src = raw + (size_t)k * (size_t)h * 3u;
    int32_t *dst = smooth + (size_t)k * (size_t)h * 3u;
    for (y = 0; y < h; ++y)
      for (c = 0; c < 3; ++c) {
        int32_t acc = 0;
        for (d = -SV_SIDE_BLUR_RADIUS; d <= SV_SIDE_BLUR_RADIUS; ++d) {
          int yy = y + d;
          yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
          acc += src[(size_t)yy * 3u + (size_t)c];
        }
        dst[(size_t)y * 3u + (size_t)c] = acc / (2 * SV_SIDE_BLUR_RADIUS + 1);
      }
  }
  /* distance d = 0 at the native edge: quadratic fade from 0.6 of the edge colour down to the dark neutral at the outer boundary */
  for (d = 0; d < side; ++d) {
    const int32_t u = 1024 - (int32_t)(((int64_t)(2 * d + 1) * 1024) / (2 * side));
    fade[d] = (int32_t)(((int64_t)u * u / 1024) * 614 / 1024);
  }
  for (y = 0; y < h; ++y)
    for (k = 0; k < side; ++k) {
      uint8_t *lp = left + ((size_t)y * (size_t)side + (size_t)k) * 3u;   /* k = side-1 is next to the native picture */
      uint8_t *rp = right + ((size_t)y * (size_t)side + (size_t)k) * 3u;  /* k = 0 is next to the native picture */
      const int32_t fl = fade[side - 1 - k], fr = fade[k];
      for (c = 0; c < 3; ++c) {
        const int32_t sl = smooth[(size_t)y * 3u + (size_t)c], sr = smooth[(size_t)h * 3u + (size_t)y * 3u + (size_t)c];
        lp[c] = (uint8_t)(dark[c] + ((sl - dark[c]) * fl) / 1024);
        rp[c] = (uint8_t)(dark[c] + ((sr - dark[c]) * fr) / 1024);
      }
    }
  return 0;
}
