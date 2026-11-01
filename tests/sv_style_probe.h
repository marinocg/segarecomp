#ifndef SEGARECOMP_TESTS_SV_STYLE_PROBE_H
#define SEGARECOMP_TESTS_SV_STYLE_PROBE_H
/* SEG-123: shared probe of the side styles on a REAL platform adapter's frame (header-only, test code). With the extension on screen it cycles
 * Comfort -> Raw -> Borders -> Comfort through the host's settings (no new frame is presented, the guest and the producer are not involved) and checks on the
 * rendered window output: the treatment each style applies, the native centre bit-identical in all of them, Comfort different from and quieter than Raw at the
 * sides, Borders flat, Raw reproducible, and the requested mode untouched. Returns the number of failed checks and prints each one. */
#include <SDL3/SDL.h>
#include <stdio.h>

#include "sv_host.h"
#include "sv_layout.h"

static int sv_probe_px(SDL_Surface *s, int x, int y, int *r, int *g, int *b) {
  Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
  if (!SDL_ReadSurfacePixel(s, x, y, &rr, &gg, &bb, &aa)) return 0;
  *r = rr; *g = gg; *b = bb;
  return 1;
}
static unsigned long long sv_probe_hash(SDL_Surface *s, int x0, int y0, int w, int h) {
  unsigned long long v = 1469598103934665603ull;
  int x, y, r, g, b;
  for (y = y0; y < y0 + h; ++y)
    for (x = x0; x < x0 + w; ++x) { sv_probe_px(s, x, y, &r, &g, &b); v = (v ^ (unsigned long long)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16))) * 1099511628211ull; }
  return v;
}
static double sv_probe_activity(SDL_Surface *s, int x0, int y0, int w, int h) { /* mean absolute horizontal step of the luminance */
  double acc = 0.0;
  int x, y, r, g, b, l0, l1;
  for (y = y0; y < y0 + h; ++y)
    for (x = x0; x + 1 < x0 + w; ++x) {
      sv_probe_px(s, x, y, &r, &g, &b); l0 = r + g + b;
      sv_probe_px(s, x + 1, y, &r, &g, &b); l1 = r + g + b;
      acc += l1 > l0 ? l1 - l0 : l0 - l1;
    }
  return w > 1 && h > 0 ? acc / ((double)(w - 1) * (double)h) : 0.0;
}
static double sv_probe_mean(SDL_Surface *s, int x0, int y0, int w, int h) {
  double acc = 0.0;
  int x, y, r, g, b;
  for (y = y0; y < y0 + h; ++y) for (x = x0; x < x0 + w; ++x) { sv_probe_px(s, x, y, &r, &g, &b); acc += r + g + b; }
  return w > 0 && h > 0 ? acc / ((double)w * (double)h) : 0.0;
}

#define SV_PROBE(cond, what) do { if (!(cond)) { ++bad; printf("FAIL: %s: %s (probe line %d)\n", name, what, __LINE__); } } while (0)

static int sv_style_probe(SvHost *host, const char *name, int native_w, int native_h, SvAspect aspect, SvScaleMode scale) {
  static const SvSideStyle order[] = {SV_SIDE_COMFORT, SV_SIDE_RAW, SV_SIDE_BORDERS, SV_SIDE_COMFORT};
  static const SvSideTreatment want[] = {SV_TREAT_COMFORT, SV_TREAT_PASSTHROUGH, SV_TREAT_BORDERS, SV_TREAT_COMFORT};
  const SvSideStyle original_style = sv_host_settings(host)->video.widescreen_side_style;
  const SvWidescreenMode requested = sv_host_settings(host)->video.widescreen;
  const int side = sv_host_extension_side(host);
  unsigned long long nat[4] = {0, 0, 0, 0}, full[4] = {0, 0, 0, 0}, lhash[4] = {0, 0, 0, 0};
  double act[4] = {0, 0, 0, 0}, mean[4] = {0, 0, 0, 0};
  int bad = 0, k;
  SvExtendedLayout lay;
  int lx = 0, lw = 0, ly = 0, lh = 0, nx = 0, ny = 0, nw = 0, nh = 0;
  uint32_t acc0, rej0, tex0, acc1, rej1, tex1;
  SV_PROBE(side > 0, "an extension is on screen");
  if (side <= 0) return bad;
  sv_host_extension_stats(host, &acc0, &rej0, &tex0);
  for (k = 0; k < 4; ++k) {
    SvSettings s = *sv_host_settings(host);
    SDL_Surface *a;
    s.video.widescreen_side_style = order[k];
    sv_host_apply_settings(host, &s);
    SV_PROBE(sv_host_side_treatment(host) == want[k], "the style is applied at once with the expected treatment");
    SV_PROBE(sv_host_settings(host)->video.widescreen == requested, "the requested widescreen mode is untouched");
    sv_host_toast(host, "");
    a = sv_host_capture_output(host);
    SV_PROBE(a != NULL, "capture");
    if (a == NULL) continue;
    if (sv_layout_extended_dest(a->w, a->h, native_w, native_h, side, aspect, scale, &lay) != 0) { SV_PROBE(0, "layout"); SDL_DestroySurface(a); continue; }
    /* margins of 2 output pixels keep the seams of a fractional scale out of the comparison */
    lx = lay.surface.x + 1; lw = lay.native.x - lay.surface.x - 3; ly = lay.native.y + 2; lh = lay.native.h - 4;
    nx = lay.native.x + 2; ny = lay.native.y + 2; nw = lay.native.w - 4; nh = lay.native.h - 4;
    nat[k] = sv_probe_hash(a, nx, ny, nw, nh);
    full[k] = sv_probe_hash(a, 0, 0, a->w, a->h);
    lhash[k] = sv_probe_hash(a, lx, ly, lw, lh);
    act[k] = sv_probe_activity(a, lx, ly, lw, lh);
    mean[k] = sv_probe_mean(a, lx, ly, lw, lh);
    SDL_DestroySurface(a);
  }
  SV_PROBE(nat[0] == nat[1] && nat[1] == nat[2] && nat[2] == nat[3], "the native centre is bit-identical in Comfort, Raw and Borders");
  SV_PROBE(full[0] == full[3], "Comfort -> Raw -> Borders -> Comfort returns to the identical output");
  SV_PROBE(lhash[0] != lhash[1] && lhash[2] != lhash[1], "Comfort and Borders differ from the raw extension at the sides");
  SV_PROBE(act[0] <= act[1] + 1e-9 && act[2] <= 1e-9, "Comfort is no busier than Raw and Borders is flat");
  SV_PROBE(mean[0] <= mean[1] + 1e-9, "Comfort is not brighter than Raw");
  sv_host_extension_stats(host, &acc1, &rej1, &tex1);
  SV_PROBE(acc1 == acc0 && rej1 == rej0 && tex1 == tex0, "no producer-facing counter and no texture changed");
  { SvSettings s = *sv_host_settings(host); s.video.widescreen_side_style = original_style; sv_host_apply_settings(host, &s); }
  return bad;
}
#undef SV_PROBE
#endif
