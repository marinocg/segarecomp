#include "sv_layout.h"

#include <math.h>

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
