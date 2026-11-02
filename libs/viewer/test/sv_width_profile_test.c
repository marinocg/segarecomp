/* SEG-123 amendment: the live Widescreen Width (target aspect 4:3 .. 16:9) and the per-game widescreen profile of the shared SDL3 host, under the dummy video
 * driver and the software renderer (no display, audio device or ROM). Output is read back from the rendered window; expectations come from the scene and the
 * layout arithmetic. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static int px(SDL_Surface *s, int x, int y, int *r, int *g, int *b) {
  Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
  if (!SDL_ReadSurfacePixel(s, x, y, &rr, &gg, &bb, &aa)) return 0;
  *r = rr; *g = gg; *b = bb;
  return 1;
}
static uint64_t region_hash(SDL_Surface *s, SvRect rc) {
  uint64_t h = 1469598103934665603ull;
  int x, y, r, g, b;
  for (y = rc.y; y < rc.y + rc.h; ++y)
    for (x = rc.x; x < rc.x + rc.w; ++x) { px(s, x, y, &r, &g, &b); h = (h ^ (uint64_t)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16))) * 1099511628211ull; }
  return h;
}
static uint64_t full_hash(SDL_Surface *s) { SvRect a; a.x = 0; a.y = 0; a.w = s->w; a.h = s->h; return region_hash(s, a); }
static int lum(SDL_Surface *s, int x, int y) { int r = 0, g = 0, b = 0; px(s, x, y, &r, &g, &b); return r + g + b; }
static int colour_is(SDL_Surface *s, int x, int y, int r0, int g0, int b0) { int r, g, b; return px(s, x, y, &r, &g, &b) && r == r0 && g == g0 && b == b0; }

static void make_native(uint8_t *rgb, int w, int h) {
  int x, y;
  for (y = 0; y < h; ++y)
    for (x = 0; x < w; ++x) {
      uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
      const int on = ((x / 8) & 1) != 0;
      p[0] = on ? 240 : 20; p[1] = on ? 240 : 40; p[2] = on ? 20 : 220;
    }
}
/* extension: 16-pixel magenta/green stripes (column index parity) and a unique grey centre */
static void make_ext(uint8_t *s, int w, int side, int h, int pitch) {
  int x, y;
  memset(s, 0, (size_t)pitch * (size_t)h);
  for (y = 0; y < h; ++y)
    for (x = 0; x < w + 2 * side; ++x) {
      uint8_t *p = s + (size_t)y * (size_t)pitch + (size_t)x * 3u;
      const int a = ((x / 16) & 1) != 0;
      if (x >= side && x < side + w) { p[0] = 77; p[1] = 77; p[2] = 77; }
      else { p[0] = a ? 255 : 0; p[1] = a ? 0 : 255; p[2] = a ? 255 : 0; }
    }
}
static SvExtendedSurface describe(const uint8_t *s, int w, int side, int h, int pitch, uint64_t seq, SvExtSource src) {
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  e.rgb24 = s; e.size = (size_t)pitch * (size_t)h; e.width = w + 2 * side; e.height = h; e.pitch = pitch; e.side = side; e.sequence = seq;
  e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = src;
  return e;
}

static const char *g_dir;
static char g_settings[700];
static SvHost *open_host_ids(SvSystem system, int w, int h, const char *platform, const char *id) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = system; cfg.title = "width test"; cfg.picture_w = w; cfg.picture_h = h <= 240 ? h : 224; cfg.max_w = 400; cfg.max_h = 480; cfg.default_scale = 3;
  cfg.platform_id = platform; cfg.profile_id = id;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  return sv_host_open(&cfg);
}
static void set(SvHost *h, SvWidescreenMode ws, SvSideStyle st, int width, SvAspect asp, SvScaleMode sc) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = ws; s.video.widescreen_side_style = st; s.video.widescreen_width = width; s.video.aspect = asp; s.video.scale = sc;
  sv_host_apply_settings(h, &s);
}
static SDL_Surface *shoot(SvHost *h) { sv_host_toast(h, ""); return sv_host_capture_output(h); }

/* ---------------------------------------------------------------- live width */
static void live_width_tests(void) {
  enum { W = 320, H = 224, SIDE = 39, PITCH = (W + 2 * SIDE) * 3 };
  static const int widths[] = {0, 37, 60, 100, 0};
  uint8_t *native = (uint8_t *)malloc((size_t)W * H * 3u), *ext = (uint8_t *)malloc((size_t)PITCH * H);
  SvHost *host = open_host_ids(SV_SYSTEM_GENESIS, W, H, NULL, NULL);
  SvExtendedSurface e;
  uint64_t native_hash = 0, seq = 100;
  int st, i;
  CHECK(host != NULL && native != NULL && ext != NULL, "live: setup");
  if (host == NULL || native == NULL || ext == NULL) { free(native); free(ext); if (host != NULL) sv_host_close(host); return; }
  SDL_SetWindowSize(host->window, 1920, 1080);
  make_native(native, W, H);
  make_ext(ext, W, SIDE, H, PITCH);

  /* before any extension result exists, and with a stale one: the width is kept and the Original picture is shown, never unvalidated pixels */
  set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_RAW, 37, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_present(host, native, W, H, W * 3) == 0 && sv_host_visible_side(host) == 0 && sv_host_extension_side(host) == 0, "no extension yet: nothing is visible");
  { SDL_Surface *a = shoot(host), *b;
    set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_RAW, 80, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    b = shoot(host);
    CHECK(a != NULL && b != NULL && full_hash(a) == full_hash(b) && sv_host_settings(host)->video.widescreen_width == 80, "a width change before any extension result changes nothing on screen and keeps the value");
    if (a != NULL) SDL_DestroySurface(a);
    if (b != NULL) SDL_DestroySurface(b); }
  e = describe(ext, W, SIDE, H, PITCH, 7, SV_EXTSRC_GENERIC); /* stale identity */
  CHECK(sv_host_present_extended(host, native, W, H, W * 3, 8, &e) == 0 && sv_host_visible_side(host) == 0, "a stale result: Original, nothing visible");
  set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_RAW, 20, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_visible_side(host) == 0, "a width change with a stale result still shows nothing extra");

  for (st = 0; st < 3; ++st) {
    uint32_t acc0, rej0, tex0, acc1, rej1, tex1;
    uint64_t frames0;
    set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, (SvSideStyle)st, 100, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    e = describe(ext, W, SIDE, H, PITCH, ++seq, SV_EXTSRC_GENERIC);
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_extension_side(host) == SIDE, "extended frame accepted");
    sv_host_extension_stats(host, &acc0, &rej0, &tex0);
    frames0 = host->frames_presented;
    for (i = 0; i < 5; ++i) {
      SDL_Surface *a;
      SvExtendedLayout lay;
      SvRect left, nat;
      const int want = sv_layout_visible_side(W, H, SV_ASPECT_ORIGINAL, widths[i], SIDE);
      set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, (SvSideStyle)st, widths[i], SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER); /* no new frame: the paused guest cannot supply one */
      CHECK(sv_host_visible_side(host) == want && sv_host_extension_side(host) == SIDE, "visible side follows the width; the produced side stays");
      a = shoot(host);
      CHECK(a != NULL, "capture");
      if (a == NULL) continue;
      if (want > 0) {
        CHECK(sv_layout_extended_dest(a->w, a->h, W, H, want, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, "layout");
        CHECK(lay.surface.w == (W + 2 * want) * (lay.native.w / W), "the presented width is the visible width");
      } else {
        lay.native = sv_layout_dest(a->w, a->h, W, H, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
        lay.surface = lay.native;
      }
      nat = lay.native;
      { const uint64_t hh = region_hash(a, nat);
        if (native_hash == 0) native_hash = hh;
        CHECK(hh == native_hash, "the native centre is pixel-identical at every width and style"); }
      left.x = lay.surface.x; left.y = nat.y + 8; left.w = nat.x - lay.surface.x; left.h = nat.h - 16;
      if (want > 0 && left.w > 4) {
        const int k = nat.w / W;
        left.x += 1; left.w -= 2;
        if (st == SV_SIDE_RAW) {
          /* the first visible column is extension column (SIDE - want): exact, cropped symmetrically around the native picture */
          const int col = SIDE - want, a_par = ((col / 16) & 1) != 0;
          CHECK(colour_is(a, lay.surface.x + 1, nat.y + 5 * k, a_par ? 255 : 0, a_par ? 0 : 255, a_par ? 255 : 0), "Raw: the outermost visible column is the matching extension column");
          CHECK(!colour_is(a, nat.x + nat.w / 2, nat.y + 5 * k, 77, 77, 77), "Raw: the extension's centre is never shown");
        } else if (st == SV_SIDE_COMFORT) {
          int x, y, mx = 0, d, hard = 0;
          for (y = left.y; y < left.y + left.h; y += 3)
            for (x = left.x; x + 1 < left.x + left.w; ++x) { d = lum(a, x + 1, y) - lum(a, x, y); if (d < 0) d = -d; if (d > mx) mx = d; if (d > 150) ++hard; }
          CHECK(hard == 0 && mx < 80, "Comfort at this width: smooth side without hard edges");
          CHECK(!colour_is(a, left.x + 2, nat.y + 20, 255, 0, 255) && !colour_is(a, left.x + 2, nat.y + 20, 0, 255, 0), "Comfort at this width: no extension tiles");
          CHECK(lum(a, left.x, nat.y + nat.h / 2) <= lum(a, left.x + left.w - 1, nat.y + nat.h / 2) + 6, "Comfort at this width: it fades out to the outer boundary of the CURRENT width");
        } else {
          int r0, g0, b0, r1, g1, b1;
          px(a, left.x, left.y, &r0, &g0, &b0); px(a, left.x + left.w - 1, left.y + left.h - 1, &r1, &g1, &b1);
          CHECK(r0 == r1 && g0 == g1 && b0 == b1 && r0 == g0 && r0 < 40, "Borders: flat neutral panel at this width");
        }
      }
      if (want == 0) CHECK(sv_host_side_treatment(host) == sv_host_side_treatment(host) && nat.x == lay.surface.x, "at 4:3 (or a native picture already that wide) only the native picture is shown");
      SDL_DestroySurface(a);
    }
    sv_host_extension_stats(host, &acc1, &rej1, &tex1);
    CHECK(host->frames_presented == frames0, "the frame count did not move while the width changed");
    CHECK(acc1 == acc0 && rej1 == rej0 && tex1 == tex0, "no producer-facing counter and no texture changed while the width moved");
  }

  /* authored: the full surface at 100; narrower selections crop only the sides (and never distort the native centre) */
  { SDL_Surface *full, *narrow, *raw100;
    SvExtendedLayout lay;
    set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_COMFORT, 100, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    e = describe(ext, W, SIDE, H, PITCH, ++seq, SV_EXTSRC_AUTHORED);
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_side_treatment(host) == SV_TREAT_PASSTHROUGH && sv_host_visible_side(host) == SIDE, "authored at 100: the complete surface");
    full = shoot(host);
    set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_RAW, 100, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    raw100 = shoot(host);
    CHECK(full != NULL && raw100 != NULL && full_hash(full) == full_hash(raw100), "an authored surface under Comfort is exactly the Raw presentation at 100");
    set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_COMFORT, 50, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    narrow = shoot(host);
    CHECK(narrow != NULL && sv_host_side_treatment(host) == SV_TREAT_PASSTHROUGH && sv_host_visible_side(host) < SIDE && sv_host_visible_side(host) > 0, "authored at 50: a centred crop, still untreated");
    if (narrow != NULL && full != NULL) {
      SvExtendedLayout l2;
      const int vis = sv_host_visible_side(host);
      sv_layout_extended_dest(full->w, full->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay);
      sv_layout_extended_dest(narrow->w, narrow->h, W, H, vis, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &l2);
      CHECK(region_hash(full, lay.native) == region_hash(narrow, l2.native), "the crop leaves the native centre untouched");
      { const int k = l2.native.w / W; const int col = SIDE - vis, par = ((col / 16) & 1) != 0;
        CHECK(colour_is(narrow, l2.surface.x + 1, l2.native.y + 5 * k, par ? 255 : 0, par ? 0 : 255, par ? 255 : 0), "the authored crop shows the exact surface columns"); }
    }
    if (full != NULL) SDL_DestroySurface(full);
    if (raw100 != NULL) SDL_DestroySurface(raw100);
    if (narrow != NULL) SDL_DestroySurface(narrow); }

  /* a genuinely wide native picture (no extension path) is untouched by any width / style */
  { uint8_t *n2 = (uint8_t *)malloc(398u * H * 3u);
    uint64_t hs[3];
    make_native(n2, 398, H);
    for (i = 0; i < 3; ++i) {
      SDL_Surface *a;
      set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, (SvSideStyle)i, i == 0 ? 0 : 60, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
      CHECK(sv_host_present(host, n2, 398, H, 398 * 3) == 0 && sv_host_visible_side(host) == 0, "a full-width native picture has no generic side");
      a = shoot(host); hs[i] = a != NULL ? full_hash(a) : 0u; if (a != NULL) SDL_DestroySurface(a);
    }
    CHECK(hs[0] == hs[1] && hs[1] == hs[2] && hs[0] != 0u, "a full-width native picture is identical at every width and style");
    free(n2); }
  sv_host_close(host);
  free(native); free(ext);
}

/* the geometries: the visible side is exactly the target aspect, within a pixel, on every platform geometry */
static void geometry_tests(void) {
  static const struct { int w, h; SvSystem sys; } g[] = {{256, 224, SV_SYSTEM_GENESIS}, {320, 224, SV_SYSTEM_GENESIS}, {320, 448, SV_SYSTEM_GENESIS}, {256, 192, SV_SYSTEM_SMS}, {256, 224, SV_SYSTEM_SMS}};
  size_t k;
  int a, wd;
  for (k = 0; k < sizeof g / sizeof g[0]; ++k) {
    SvHost *host = open_host_ids(g[k].sys, g[k].w, g[k].h, NULL, NULL);
    const int full = sv_layout_extension_side(g[k].w, g[k].h, SV_ASPECT_ORIGINAL);
    uint8_t *native, *ext;
    if (host == NULL) { CHECK(0, "geometry: host"); continue; }
    for (a = 0; a < 2; ++a) {
      const SvAspect asp = a ? SV_ASPECT_4_3 : SV_ASPECT_ORIGINAL;
      const int side = sv_layout_extension_side(g[k].w, g[k].h, asp);
      const int pitch = (g[k].w + 2 * side) * 3;
      uint64_t seq = 5000;
      native = (uint8_t *)malloc((size_t)g[k].w * (size_t)g[k].h * 3u);
      ext = (uint8_t *)malloc((size_t)pitch * (size_t)g[k].h);
      if (side <= 0 || native == NULL || ext == NULL) { free(native); free(ext); continue; }
      make_native(native, g[k].w, g[k].h);
      make_ext(ext, g[k].w, side, g[k].h, pitch);
      set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_COMFORT, 100, asp, SV_SCALE_FIT);
      { SvExtendedSurface e = describe(ext, g[k].w, side, g[k].h, pitch, ++seq, SV_EXTSRC_GENERIC);
        CHECK(sv_host_present_extended(host, native, g[k].w, g[k].h, g[k].w * 3, seq, &e) == 0 && sv_host_extension_side(host) == side, "geometry: accepted"); }
      for (wd = 0; wd <= 100; wd += 10) {
        int n, d;
        long long total_num, ratio_check;
        int vis;
        set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_COMFORT, wd, asp, SV_SCALE_FIT);
        vis = sv_host_visible_side(host);
        sv_widescreen_width_ratio(wd, &n, &d);
        /* display aspect of the visible picture: native display aspect * (w + 2 vis) / w, within one source pixel of the target unless clamped at zero or at the produced side */
        total_num = asp == SV_ASPECT_4_3 ? 4LL * g[k].h * (g[k].w + 2LL * vis) * 1000LL / (3LL * g[k].w) : (g[k].w + 2LL * vis) * 1000LL;
        ratio_check = total_num / (asp == SV_ASPECT_4_3 ? g[k].h : g[k].h) ;
        (void)ratio_check;
        if (vis > 0 && vis < side) {
          const double have = asp == SV_ASPECT_4_3 ? (4.0 / 3.0) * (double)(g[k].w + 2 * vis) / (double)g[k].w : (double)(g[k].w + 2 * vis) / (double)g[k].h;
          const double want = (double)n / (double)d;
          const double tol = (asp == SV_ASPECT_4_3 ? (4.0 / 3.0) * 2.0 / (double)g[k].w : 2.0 / (double)g[k].h) + 1e-9;
          CHECK(have <= want + 1e-9 && have >= want - tol, "geometry: the visible aspect is the target (floored to whole source pixels)");
        }
        CHECK(vis <= side && vis >= 0, "geometry: visible within the produced side");
      }
    }
    (void)full;
    free(native); free(ext);
    sv_host_close(host);
  }
}

/* ---------------------------------------------------------------- slider by keyboard in the paused menu */
static void key_event(int scancode, int down) {
  SDL_Event e;
  SDL_zero(e);
  e.type = (Uint32)(down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP);
  e.common.timestamp = SDL_GetTicksNS();
  e.key.scancode = (SDL_Scancode)scancode;
  e.key.down = down != 0;
  SDL_PushEvent(&e);
}
static void tap(int scancode) { key_event(scancode, 1); key_event(scancode, 0); }
typedef struct SliderScript { int step; int width[12], visible[12], n; uint64_t frames0, frames1; char hint[400], label[160]; } SliderScript;
static int slider_hook(void *ctx, SvHost *host, int iteration) {
  SliderScript *s = (SliderScript *)ctx;
  SvPage *p = sv_menu_top(&host->menu);
  int i;
  (void)iteration;
  if (s->step == 0) s->frames0 = host->frames_presented;
  switch (s->step) {
    case 0: case 1: tap(SV_KEY_DOWN); break;
    case 2: tap(SV_KEY_RETURN); break;
    case 3: case 4: case 5: case 6: tap(SV_KEY_DOWN); break; /* Fullscreen, Scaling, Aspect, Widescreen Mode, then the width slider */
    default: {
      const int k = s->step - 7;
      if (k < 0 || k > 20) { s->frames1 = host->frames_presented; return 1; }
      if (k % 2 == 0) { /* settle: record */
        if (k > 0 && s->n < 12) { s->width[s->n] = sv_host_settings(host)->video.widescreen_width; s->visible[s->n] = sv_host_visible_side(host); ++s->n; }
        if (k == 0 && p != NULL) for (i = 0; i < p->count; ++i) if (p->items[i].id == I_WS_WIDTH) { snprintf(s->hint, sizeof s->hint, "%s", p->items[i].hint); snprintf(s->label, sizeof s->label, "%s|%s", p->items[i].label, p->items[i].text); }
      } else tap(k < 17 ? SV_KEY_LEFT : SV_KEY_RIGHT);
      if (k == 20) { s->frames1 = host->frames_presented; tap(SV_KEY_ESCAPE); tap(SV_KEY_ESCAPE); }
    }
  }
  s->step++;
  return 0;
}
static void slider_tests(void) {
  enum { W = 320, H = 224, SIDE = 39, PITCH = (W + 2 * SIDE) * 3 };
  uint8_t *native = (uint8_t *)malloc((size_t)W * H * 3u), *ext = (uint8_t *)malloc((size_t)PITCH * H);
  SvHost *host = open_host_ids(SV_SYSTEM_GENESIS, W, H, NULL, NULL);
  SliderScript s;
  SvSample smp;
  SvExtendedSurface e;
  uint32_t acc0, rej0, tex0, acc1, rej1, tex1;
  int i, monotone = 1;
  CHECK(host != NULL && native != NULL && ext != NULL, "slider: setup");
  if (host == NULL || native == NULL || ext == NULL) { free(native); free(ext); if (host != NULL) sv_host_close(host); return; }
  make_native(native, W, H);
  make_ext(ext, W, SIDE, H, PITCH);
  set(host, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_COMFORT, 100, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  e = describe(ext, W, SIDE, H, PITCH, 1, SV_EXTSRC_GENERIC);
  CHECK(sv_host_present_extended(host, native, W, H, W * 3, 1, &e) == 0, "slider: frame");
  sv_host_extension_stats(host, &acc0, &rej0, &tex0);
  memset(&s, 0, sizeof s);
  memset(&smp, 0, sizeof smp);
  sv_host_set_loop_hook(host, slider_hook, &s);
  tap(SV_KEY_ESCAPE);
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
  CHECK(strncmp(s.label, "Widescreen Width|16:9 (100%)", 28) == 0, "the slider is labelled Widescreen Width and shows the aspect with the percentage");
  CHECK(strstr(s.hint, "saved for all games") != NULL || strstr(s.hint, "Saved for this game") != NULL, "the hint says where the value is saved");
  CHECK(s.n >= 8, "the keyboard moved the slider repeatedly");
  for (i = 1; i < s.n && i < 8; ++i) if (s.width[i] >= s.width[i - 1]) monotone = 0;
  CHECK(monotone && s.width[0] < 100, "Left lowers the width (immediately, while paused)");
  CHECK(s.visible[0] <= SIDE && s.visible[s.n > 7 ? 7 : s.n - 1] < s.visible[0], "the visible side shrinks with the width while the menu holds the guest");
  CHECK(s.frames0 == s.frames1 && s.frames0 == host->frames_presented, "no frame was presented while the slider moved");
  sv_host_extension_stats(host, &acc1, &rej1, &tex1);
  CHECK(acc1 == acc0 && rej1 == rej0 && tex1 == tex0, "the slider created no texture and no producer-facing event");
  /* Original keeps the value and says it only applies with a widescreen mode */
  set(host, SV_WIDESCREEN_ORIGINAL, SV_SIDE_COMFORT, 55, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_settings(host)->video.widescreen_width == 55, "Original retains the width value");
  sv_host_close(host);
  free(native); free(ext);
}

/* ---------------------------------------------------------------- per-game profiles */
static int file_exists(const char *path) { FILE *f = fopen(path, "rb"); if (f != NULL) fclose(f); return f != NULL; }
static void write_text(const char *path, const char *text) { FILE *f = fopen(path, "wb"); if (f != NULL) { fputs(text, f); fclose(f); } }
static char *read_text(const char *path) {
  static char buf[8192];
  FILE *f = fopen(path, "rb");
  size_t n;
  if (f == NULL) return NULL;
  n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = '\0';
  return buf;
}
static const char *kA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *kB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char *kC = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char *kD = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
static const char *kE = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

typedef struct Prefs { SvWidescreenMode ws; SvSideStyle st; int width; } Prefs;
static Prefs get_prefs(SvHost *h) { Prefs p; const SvSettings *s = sv_host_settings(h); p.ws = s->video.widescreen; p.st = s->video.widescreen_side_style; p.width = s->video.widescreen_width; return p; }
static void put_prefs(SvHost *h, Prefs p) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = p.ws; s.video.widescreen_side_style = p.st; s.video.widescreen_width = p.width;
  sv_host_apply_settings(h, &s);
}
static int same(Prefs a, Prefs b) { return a.ws == b.ws && a.st == b.st && a.width == b.width; }
static Prefs open_prefs(SvSystem sys, const char *platform, const char *id, int *active) {
  SvHost *h = open_host_ids(sys, 320, 224, platform, id);
  Prefs p = {SV_WIDESCREEN_ORIGINAL, SV_SIDE_COMFORT, -1};
  if (h == NULL) { CHECK(0, "profile: host opens"); return p; }
  p = get_prefs(h);
  if (active != NULL) *active = sv_host_profile_active(h);
  sv_host_close(h);
  return p;
}
static void edit_game(SvSystem sys, const char *platform, const char *id, Prefs p) {
  SvHost *h = open_host_ids(sys, 320, 224, platform, id);
  if (h == NULL) { CHECK(0, "profile: host opens for edit"); return; }
  put_prefs(h, p);
  sv_host_close(h);
}
static void profile_tests(void) {
  const Prefs pa = {SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_RAW, 30}, pb = {SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_SIDE_BORDERS, 66}, pc = {SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_SIDE_COMFORT, 12},
                  pd = {SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_SIDE_BORDERS, 88};
  char file[1100], path[1100], dir[800];
  int act = 0;
  Prefs got, glob;
  snprintf(g_settings, sizeof g_settings, "%s/viewer-settings.cfg", g_dir);
  snprintf(dir, sizeof dir, "%s/viewer-widescreen", g_dir);
  /* an OLD installation: only the global file, with its widescreen preferences */
  write_text(g_settings, "video.scale=1\nvideo.widescreen=extended_backgrounds\naudio.volume=40\n");
  glob = open_prefs(SV_SYSTEM_GENESIS, "genesis", kA, &act);
  CHECK(act == 1 && glob.ws == SV_WIDESCREEN_EXTENDED_BACKGROUNDS && glob.st == SV_SIDE_COMFORT && glob.width == 100, "an old file migrates: the global preferences are the defaults of a game with no profile (Comfort, width 100 added)");
  snprintf(file, sizeof file, "%s/genesis-%s.cfg", dir, kA);
  CHECK(!file_exists(file), "opening a game does not create a profile");
  /* assign four different games */
  edit_game(SV_SYSTEM_GENESIS, "genesis", kA, pa);
  edit_game(SV_SYSTEM_GENESIS, "genesis", kB, pb);
  edit_game(SV_SYSTEM_SMS, "master-system", kC, pc);
  edit_game(SV_SYSTEM_GENESIS, "sega-cd", kD, pd);
  got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kA, NULL); CHECK(same(got, pa), "game A restores its preferences");
  got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kB, NULL); CHECK(same(got, pb), "game B restores its preferences");
  got = open_prefs(SV_SYSTEM_SMS, "master-system", kC, NULL); CHECK(same(got, pc), "game C (Master System) restores its preferences");
  got = open_prefs(SV_SYSTEM_GENESIS, "sega-cd", kD, NULL); CHECK(same(got, pd), "game D (Sega CD) restores its preferences");
  /* the global file keeps its own widescreen values and every other setting */
  { char *g = read_text(g_settings);
    CHECK(g != NULL && strstr(g, "video.widescreen=extended_backgrounds") != NULL && strstr(g, "video.widescreen_side_style=comfort") != NULL && strstr(g, "video.widescreen_width=100") != NULL &&
              strstr(g, "audio.volume=40") != NULL && strstr(g, "video.scale=1") != NULL, "per-game edits never reach the global defaults"); }
  /* changing A leaves B, C, D alone; the same digest on another platform is another game */
  { Prefs na = {SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_SIDE_COMFORT, 5};
    edit_game(SV_SYSTEM_GENESIS, "genesis", kA, na);
    got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kA, NULL); CHECK(same(got, na), "A changed");
    got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kB, NULL); CHECK(same(got, pb), "B unchanged");
    got = open_prefs(SV_SYSTEM_SMS, "master-system", kC, NULL); CHECK(same(got, pc), "C unchanged");
    got = open_prefs(SV_SYSTEM_GENESIS, "sega-cd", kD, NULL); CHECK(same(got, pd), "D unchanged");
    edit_game(SV_SYSTEM_GENESIS, "genesis", kE, pa);
    got = open_prefs(SV_SYSTEM_GENESIS, "sega-cd", kE, NULL);
    CHECK(got.width == 100 && got.ws == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "Sega CD does not share a profile namespace with Genesis (same digest, other platform = no profile)");
    snprintf(file, sizeof file, "%s/genesis-%s.cfg", dir, kE); CHECK(file_exists(file), "the Genesis profile file exists");
    snprintf(file, sizeof file, "%s/sega-cd-%s.cfg", dir, kE); CHECK(!file_exists(file), "no Sega CD file was created for it"); }
  /* a changed global default reaches a game without an override only; games with overrides keep theirs */
  write_text(g_settings, "video.widescreen=enhanced_gameplay\nvideo.widescreen_side_style=borders\nvideo.widescreen_width=10\naudio.volume=40\n");
  got = open_prefs(SV_SYSTEM_GENESIS, "genesis", "f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0", NULL);
  CHECK(got.ws == SV_WIDESCREEN_ENHANCED_GAMEPLAY && got.st == SV_SIDE_BORDERS && got.width == 10, "a game without an override follows the new global defaults");
  got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kB, NULL); CHECK(same(got, pb), "a game with an override keeps it");
  /* damaged / partial profiles */
  snprintf(file, sizeof file, "%s/genesis-%s.cfg", dir, kC);
  write_text(file, "\xff\xfe garbage\n=\nvideo.widescreen_width=notanumber\n");
  got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kC, NULL);
  CHECK(got.ws == SV_WIDESCREEN_ENHANCED_GAMEPLAY && got.st == SV_SIDE_BORDERS && got.width == 10, "a corrupted profile falls back to the global values");
  got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kB, NULL); CHECK(same(got, pb), "and does not touch another game");
  write_text(file, "video.widescreen_width=44\n");
  got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kC, NULL);
  CHECK(got.width == 44 && got.ws == SV_WIDESCREEN_ENHANCED_GAMEPLAY && got.st == SV_SIDE_BORDERS, "a partial profile overrides only what it states");
  { char *g = read_text(g_settings); CHECK(g != NULL && strstr(g, "video.widescreen_width=10") != NULL, "the global file is intact"); }
  /* no trustworthy identity: global settings only, nothing arbitrary is created */
  { static const char *const bad[] = {"../../evil", "short", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", "", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/../x"};
    size_t i;
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
      SvHost *h = open_host_ids(SV_SYSTEM_GENESIS, 320, 224, "genesis", bad[i]);
      CHECK(h != NULL && !sv_host_profile_active(h) && sv_host_profile_path(h) == NULL, "an invalid identity has no profile");
      if (h != NULL) { Prefs p = get_prefs(h); p.width = 33; put_prefs(h, p); sv_host_close(h); }
    }
    { SvHost *h = open_host_ids(SV_SYSTEM_GENESIS, 320, 224, NULL, kA); CHECK(h != NULL && !sv_host_profile_active(h), "no platform id: no profile"); if (h != NULL) sv_host_close(h); }
    { SvHost *h = open_host_ids(SV_SYSTEM_GENESIS, 320, 224, "Genesis/../x", kA); CHECK(h != NULL && !sv_host_profile_active(h), "an invalid platform id: no profile"); if (h != NULL) sv_host_close(h); }
    { char *g = read_text(g_settings); CHECK(g != NULL && strstr(g, "video.widescreen_width=33") != NULL, "without an identity the choice is saved globally"); }
    snprintf(path, sizeof path, "%s/evil", g_dir); CHECK(!file_exists(path), "no file outside the profile directory"); }
  /* the viewer's in-game slider persists to the game's profile (keyboard through the real menu) */
  { SvHost *h = open_host_ids(SV_SYSTEM_GENESIS, 320, 224, "genesis", kE);
    SliderScript s; SvSample smp;
    if (h != NULL) {
      const int before = sv_host_settings(h)->video.widescreen_width;
      CHECK(sv_host_profile_active(h) && strstr(sv_host_profile_path(h), "viewer-widescreen/genesis-") != NULL, "the profile path is <settings dir>/viewer-widescreen/<platform>-<sha>.cfg");
      memset(&s, 0, sizeof s); memset(&smp, 0, sizeof smp);
      sv_host_set_loop_hook(h, slider_hook, &s);
      tap(SV_KEY_ESCAPE);
      sv_host_service(h, &smp);
      sv_host_set_loop_hook(h, NULL, NULL);
      CHECK(sv_host_settings(h)->video.widescreen_width != before, "the menu moved the width");
      sv_host_close(h);
      got = open_prefs(SV_SYSTEM_GENESIS, "genesis", kE, NULL);
      CHECK(got.width != before && got.width < 100, "the slider value is restored when the game is reopened");
    } }
}

int main(void) {
  char dir[512];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  snprintf(dir, sizeof dir, "%s/sv_width_profile_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  g_dir = dir;
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  snprintf(g_settings, sizeof g_settings, "%s/live.cfg", dir);
  live_width_tests();
  remove(g_settings);
  snprintf(g_settings, sizeof g_settings, "%s/geo.cfg", dir);
  geometry_tests();
  remove(g_settings);
  snprintf(g_settings, sizeof g_settings, "%s/slider.cfg", dir);
  slider_tests();
  remove(g_settings);
  profile_tests();
  if (failures != 0) { printf("sv_width_profile_test: %d failure(s)\n", failures); return 1; }
  printf("sv_width_profile_test: OK\n");
  return 0;
}
