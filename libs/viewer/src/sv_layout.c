#include "sv_layout.h"

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
  return extra > SV_EXT_LEGACY_SIDE ? SV_EXT_LEGACY_SIDE : (int)extra;
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
  side = extra > SV_EXT_LEGACY_SIDE ? SV_EXT_LEGACY_SIDE : (int)extra;
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
#define SV_SIDE_MAX_FADE (SV_EXT_MAX_SIDE + 1) /* one fade weight per side column: sized from the hard bound so scratch can never be overrun */

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

/* ---- SEG-139: checked widescreen surface planner ---- */

SvGeomStatus sv_geom_plan(int native_w, int native_h, SvAspect aspect, SvDisplayRatio target, SvGeomPlan *out) {
  SvGeomPlan plan;
  int64_t total, extra, side;
  plan.status = SV_GEOM_OK;
  plan.native_w = native_w;
  plan.native_h = native_h;
  plan.side = 0;
  plan.surface_w = 0;
  plan.surface_bytes = 0;
  if (native_w < 1 || native_h < 1 || native_w > SV_GEOM_MAX_NATIVE || native_h > SV_GEOM_MAX_NATIVE) {
    plan.status = SV_GEOM_BAD_NATIVE;
  } else if (!sv_ratio_valid(target)) {
    plan.status = SV_GEOM_BAD_ASPECT;
  } else {
    /* The surface must display at num:den at the native height. Width in source pixels = native_h * num / den * native_w / native display width, where the display
     * width of the native picture is native_w (square pixels) or native_h * 4 / 3 (4:3-corrected). Every product is below 2^40. */
    if (aspect == SV_ASPECT_4_3) total = ((int64_t)native_w * 3 * target.num) / (4 * (int64_t)target.den);
    else total = ((int64_t)native_h * target.num) / (int64_t)target.den;
    extra = total - native_w;
    side = extra <= 1 ? 0 : extra / 2;
    if (side > SV_GEOM_MAX_SIDE) {
      plan.status = SV_GEOM_TOO_WIDE;
    } else {
      plan.side = (int)side;
      plan.surface_w = native_w + 2 * plan.side;
      plan.surface_bytes = (size_t)plan.surface_w * (size_t)native_h * 3u;
    }
  }
  if (out != NULL) *out = plan;
  return plan.status;
}

SvGeomStatus sv_geom_resolve_production(const SvVideoSettings *video, int native_w, int native_h, int *side_out) {
  SvGeomStatus status = SV_GEOM_OK;
  int side = 0;
  if (video == NULL) {
    status = SV_GEOM_BAD_ASPECT;
  } else if (!sv_ratio_valid(video->widescreen_ratio)) {
    side = sv_layout_extension_side(native_w, native_h, video->aspect); /* legacy: 16:9, clamped at SV_EXT_LEGACY_SIDE exactly as before */
  } else {
    SvGeomPlan plan;
    status = sv_geom_plan(native_w, native_h, video->aspect, video->widescreen_ratio, &plan);
    side = plan.side;
  }
  if (side_out != NULL) *side_out = status == SV_GEOM_OK ? side : 0;
  return status;
}

int sv_geom_resolve_visible(const SvVideoSettings *video, int native_w, int native_h, int produced_side) {
  SvGeomPlan plan;
  if (video == NULL) return 0;
  if (!sv_ratio_valid(video->widescreen_ratio))
    return sv_layout_visible_side(native_w, native_h, video->aspect, video->widescreen_width, produced_side);
  if (produced_side <= 0 || sv_geom_plan(native_w, native_h, video->aspect, video->widescreen_ratio, &plan) != SV_GEOM_OK) return 0;
  return plan.side > produced_side ? produced_side : plan.side;
}

const char *sv_geom_status_name(SvGeomStatus status) {
  switch (status) {
    case SV_GEOM_OK: return "ok";
    case SV_GEOM_BAD_NATIVE: return "bad-native";
    case SV_GEOM_BAD_ASPECT: return "bad-aspect";
    case SV_GEOM_TOO_WIDE: return "too-wide";
    default: return "unknown";
  }
}

/* ---- SEG-139-T004: geometry declaration and coverage accounting ---- */

#define SV_COV_TEXT_MAX 1024

const char *sv_cov_component_name(SvCovComponent component) {
  switch (component) {
    case SV_COV_SCENERY: return "scenery";
    case SV_COV_HUD: return "hud";
    case SV_COV_ACTORS: return "actors";
    default: return "unknown";
  }
}

const char *sv_cov_status_name(SvCovStatus status) {
  switch (status) {
    case SV_COV_ABSENT: return "absent";
    case SV_COV_COVERED: return "covered";
    case SV_COV_MISSING: return "missing";
    case SV_COV_UNDECLARED: return "undeclared";
    case SV_COV_OUTSIDE_RANGE: return "outside declared range";
    default: return "unknown";
  }
}

static int cov_token_ok(const char *s, size_t n) {
  size_t i;
  if (n == 0 || n > SV_COV_ID_MAX) return 0;
  for (i = 0; i < n; ++i) {
    const char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 1;
}

static int cov_ratio_le(SvDisplayRatio a, SvDisplayRatio b) { return (int64_t)a.num * b.den <= (int64_t)b.num * a.den; }

static int cov_parse_ratio(const char *s, size_t n, SvDisplayRatio *out) {
  char buf[24];
  if (n == 0 || n >= sizeof buf) return -1;
  memcpy(buf, s, n);
  buf[n] = '\0';
  return sv_ratio_parse(buf, out);
}

static int cov_parse_components(const char *s, size_t n, unsigned *mask) {
  size_t i = 0;
  *mask = 0;
  while (i <= n) {
    size_t j = i;
    int c = -1, k;
    while (j < n && s[j] != '+') ++j;
    for (k = 0; k < SV_COV_COMPONENTS; ++k) {
      const char *name = sv_cov_component_name((SvCovComponent)k);
      if (strlen(name) == j - i && memcmp(name, s + i, j - i) == 0) c = k;
    }
    if (c < 0 || (*mask & SV_COV_BIT(c)) != 0) return -1;
    *mask |= SV_COV_BIT(c);
    if (j >= n) return 0;
    i = j + 1;
  }
  return -1;
}

int sv_geomdecl_parse(const char *text, SvGeomDecl *out) {
  SvGeomDecl d;
  size_t len, pos = 0;
  int seen_min = 0, seen_max = 0;
  memset(&d, 0, sizeof d);
  if (out == NULL) return -1;
  if (text == NULL || *text == '\0') { *out = d; return 0; }
  d.present = 1;
  len = strlen(text);
  if (len > SV_COV_TEXT_MAX) goto bad;
  while (pos <= len) {
    const char *part = text + pos;
    size_t n = 0, eq;
    while (pos + n < len && part[n] != ';') ++n;
    for (eq = 0; eq < n && part[eq] != '='; ++eq) {}
    if (eq == n) goto bad;
    if (eq == 3 && memcmp(part, "min", 3) == 0 && !seen_min && !seen_max && d.scene_count == 0) {
      if (cov_parse_ratio(part + 4, n - 4, &d.min) != 0) goto bad;
      seen_min = 1;
    } else if (eq == 3 && memcmp(part, "max", 3) == 0 && !seen_max && d.scene_count == 0) {
      if (cov_parse_ratio(part + 4, n - 4, &d.max) != 0) goto bad;
      seen_max = 1;
    } else if (eq == 5 && memcmp(part, "scene", 5) == 0) {
      const char *v = part + 6;
      size_t vn = n - 6, colon;
      SvGeomScene *sc;
      for (colon = 0; colon < vn && v[colon] != ':'; ++colon) {}
      if (colon == vn || !cov_token_ok(v, colon) || d.scene_count >= SV_COV_MAX_SCENES) goto bad;
      sc = &d.scenes[d.scene_count];
      memcpy(sc->id, v, colon);
      sc->id[colon] = '\0';
      if (d.scene_count > 0 && strcmp(d.scenes[d.scene_count - 1].id, sc->id) >= 0) goto bad;   /* strictly ascending: canonical and duplicate-free */
      if (cov_parse_components(v + colon + 1, vn - colon - 1, &sc->declared) != 0) goto bad;
      d.scene_count++;
    } else {
      goto bad;
    }
    pos += n + 1;
  }
  if (!seen_max) goto bad;   /* a lower bound or a scene without an upper bound would claim more than was declared */
  if (!seen_min) { d.min.num = 4; d.min.den = 3; }
  if (!cov_ratio_le(d.min, d.max)) goto bad;
  d.has_range = 1;
  *out = d;
  return 0;
bad:
  memset(out, 0, sizeof *out);
  out->present = 1;
  out->invalid = 1;
  return -1;
}

int sv_geomdecl_in_range(const SvGeomDecl *decl, SvDisplayRatio requested) {
  if (decl == NULL || !decl->present) return 1;
  if (decl->invalid || !decl->has_range) return !decl->invalid;
  if (requested.num < 1 || requested.den < 1) return 0;
  return cov_ratio_le(decl->min, requested) && cov_ratio_le(requested, decl->max);
}

static int cov_scene_index(const SvGeomDecl *decl, const char *scene) {
  int i;
  if (decl == NULL || scene == NULL) return -1;
  for (i = 0; i < decl->scene_count; ++i)
    if (strcmp(decl->scenes[i].id, scene) == 0) return i;
  return -1;
}

SvCovStatus sv_geomdecl_check(const SvGeomDecl *decl, SvDisplayRatio requested, const char *scene, SvCovComponent component, int delivered) {
  const int si = cov_scene_index(decl, scene);
  const int declared = si >= 0 && (decl->scenes[si].declared & SV_COV_BIT(component)) != 0;
  if (decl != NULL && decl->present && !sv_geomdecl_in_range(decl, requested)) return SV_COV_OUTSIDE_RANGE;
  if (declared) return delivered ? SV_COV_COVERED : SV_COV_MISSING;
  return delivered ? SV_COV_UNDECLARED : SV_COV_ABSENT;
}

void sv_cov_init(SvCovAccount *a, const SvGeomDecl *decl) {
  if (a == NULL) return;
  memset(a, 0, sizeof *a);
  if (decl != NULL) a->decl = *decl;
}

static void cov_bump(uint64_t *v) { if (*v != UINT64_MAX) ++*v; }

int sv_cov_frame(SvCovAccount *a, SvDisplayRatio requested, const char *scene, unsigned delivered) {
  int si, c, in_range;
  if (a == NULL) return 1;
  si = cov_scene_index(&a->decl, scene);
  if (si < 0) si = a->decl.scene_count;   /* the unscoped bucket */
  in_range = sv_geomdecl_in_range(&a->decl, requested);
  cov_bump(&a->frames);
  if (!in_range) cov_bump(&a->outside_frames);
  a->last = requested;
  for (c = 0; c < SV_COV_COMPONENTS; ++c) {
    const SvCovStatus st = sv_geomdecl_check(&a->decl, requested, si < a->decl.scene_count ? a->decl.scenes[si].id : NULL, (SvCovComponent)c, (delivered & SV_COV_BIT(c)) != 0);
    if (st == SV_COV_COVERED) cov_bump(&a->count[si][c][0]);
    else if (st == SV_COV_MISSING) cov_bump(&a->count[si][c][1]);
    else if (st == SV_COV_UNDECLARED) cov_bump(&a->count[si][c][2]);
    else if (st == SV_COV_OUTSIDE_RANGE) cov_bump(&a->count[si][c][3]);
  }
  return in_range;
}

typedef struct CovBuf { char *p; size_t cap, need; } CovBuf;
static void cov_put(CovBuf *b, const char *fmt, ...) {
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(b->p != NULL && b->need < b->cap ? b->p + b->need : NULL, b->p != NULL && b->need < b->cap ? b->cap - b->need : 0, fmt, ap);
  va_end(ap);
  if (n > 0) b->need += (size_t)n;
}

size_t sv_cov_report_json(const SvCovAccount *a, char *buf, size_t cap) {
  CovBuf b;
  char r1[24], r2[24], r3[24];
  int si, c, nscenes;
  b.p = buf; b.cap = cap; b.need = 0;
  if (buf != NULL && cap > 0) buf[0] = '\0';
  if (a == NULL) return 0;
  r1[0] = r2[0] = r3[0] = '\0';
  if (a->decl.has_range) { (void)sv_ratio_format(a->decl.min, r1, sizeof r1); (void)sv_ratio_format(a->decl.max, r2, sizeof r2); }
  (void)sv_ratio_format(a->last, r3, sizeof r3);
  cov_put(&b, "{\"schema\":\"segarecomp.geometry-coverage/1\",\"declared\":%s,\"aspect_min\":\"%s\",\"aspect_max\":\"%s\",\"frames\":%llu,\"outside_range_frames\":%llu,\"last_aspect\":\"%s\",\"last_status\":\"%s\",\"scenes\":[",
          a->decl.present && !a->decl.invalid ? "true" : "false", r1, r2, (unsigned long long)a->frames, (unsigned long long)a->outside_frames, r3,
          a->frames == 0 ? "no frames" : !a->decl.present ? "no declaration" : sv_geomdecl_in_range(&a->decl, a->last) ? "inside declared range" : "outside declared range");
  nscenes = a->decl.scene_count + 1;
  for (si = 0; si < nscenes; ++si) {
    const int unscoped = si == a->decl.scene_count;
    cov_put(&b, "%s{\"id\":\"%s\",\"components\":{", si > 0 ? "," : "", unscoped ? "*" : a->decl.scenes[si].id);
    for (c = 0; c < SV_COV_COMPONENTS; ++c)
      cov_put(&b, "%s\"%s\":{\"declared\":%s,\"covered\":%llu,\"missing\":%llu,\"undeclared\":%llu,\"outside_range\":%llu}", c > 0 ? "," : "", sv_cov_component_name((SvCovComponent)c),
              !unscoped && (a->decl.scenes[si].declared & SV_COV_BIT(c)) != 0 ? "true" : "false",
              (unsigned long long)a->count[si][c][0], (unsigned long long)a->count[si][c][1], (unsigned long long)a->count[si][c][2], (unsigned long long)a->count[si][c][3]);
    cov_put(&b, "}}");
  }
  cov_put(&b, "]}");
  return b.need;
}
