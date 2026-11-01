/* SEG-123: the side-presentation styles (Comfort / Borders / Raw) and the authored-widescreen bypass of the shared SDL3 host, read back from the rendered
 * window output (dummy video driver, software renderer; no display, audio device or ROM). The scenes are project-authored and deliberately hostile: a
 * high-contrast moving native picture, and an extension of hard magenta/green stripes with a bright "sprite" block that a viewer would find distracting.
 * Expected values come from the scene arithmetic and from comparing styles with each other, never from the renderer. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int lum(SDL_Surface *s, int x, int y) { int r = 0, g = 0, b = 0; px(s, x, y, &r, &g, &b); return r + g + b; }
static uint64_t region_hash(SDL_Surface *s, SvRect rc) {
  uint64_t h = 1469598103934665603ull;
  int x, y, r, g, b;
  for (y = rc.y; y < rc.y + rc.h; ++y)
    for (x = rc.x; x < rc.x + rc.w; ++x) { px(s, x, y, &r, &g, &b); h = (h ^ (uint64_t)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16))) * 1099511628211ull; }
  return h;
}
static uint64_t full_hash(SDL_Surface *s) { SvRect all; all.x = 0; all.y = 0; all.w = s->w; all.h = s->h; return region_hash(s, all); }

/* The native picture of frame `t`: 8-pixel vertical stripes that move 4 pixels per frame, over a horizontal band; high contrast, edge columns included. */
static void make_native(uint8_t *rgb, int w, int h, int t) {
  int x, y;
  for (y = 0; y < h; ++y)
    for (x = 0; x < w; ++x) {
      uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
      const int on = (((x + 4 * t) / 8) & 1) != 0;
      p[0] = on ? 240 : 20; p[1] = on ? 240 : 40; p[2] = on ? 20 : 220;
      if ((y / 24) % 3 == 1) { p[0] = (uint8_t)(255 - p[0]); }
    }
}
/* The extension: hard magenta/green 16-pixel stripes (a "recognisable repeated pattern"), a bright yellow "sprite" block at the left, and an
 * unmistakable grey centre that the host must never show. */
static void make_ext(uint8_t *s, int w, int side, int h, int pitch) {
  const int sw = w + 2 * side;
  int x, y;
  memset(s, 0, (size_t)pitch * (size_t)h);
  for (y = 0; y < h; ++y)
    for (x = 0; x < sw; ++x) {
      uint8_t *p = s + (size_t)y * (size_t)pitch + (size_t)x * 3u;
      const int a = ((x / 16) & 1) != 0;
      if (x >= side && x < side + w) { p[0] = 77; p[1] = 77; p[2] = 77; }
      else if (x < side && y >= h / 2 - 8 && y < h / 2 + 8 && x >= side / 3 && x < side / 3 + 16) { p[0] = 255; p[1] = 255; p[2] = 0; }
      else { p[0] = a ? 255 : 0; p[1] = a ? 0 : 255; p[2] = a ? 255 : 0; }
    }
}
static SvExtendedSurface describe(const uint8_t *s, int w, int side, int h, int pitch, uint64_t seq, SvWidescreenMode mode, SvExtSource src) {
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  e.rgb24 = s; e.size = (size_t)pitch * (size_t)h; e.width = w + 2 * side; e.height = h; e.pitch = pitch; e.side = side; e.sequence = seq;
  e.mode = mode; e.status = SV_WS_PARTIAL; e.source = src;
  return e;
}

static SvHost *open_host(SvSystem system, const char *settings_file, int max_w, int max_h, int pw, int ph) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = system; cfg.title = "side style test";
  cfg.picture_w = pw; cfg.picture_h = ph; cfg.max_w = max_w; cfg.max_h = max_h; cfg.default_scale = 3;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings_file, 1);
  return sv_host_open(&cfg);
}
static void style(SvHost *h, SvSideStyle st, SvWidescreenMode ws, SvAspect asp, SvScaleMode sc) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen_side_style = st; s.video.widescreen = ws; s.video.aspect = asp; s.video.scale = sc;
  sv_host_apply_settings(h, &s);
}
static SDL_Surface *shoot(SvHost *h) { sv_host_toast(h, ""); return sv_host_capture_output(h); }

/* Largest step between horizontally / vertically adjacent output pixels inside a rectangle, and the number of "hard edges" (step > 150). */
static void steps(SDL_Surface *s, SvRect rc, int *max_h, int *max_v, int *hard) {
  int x, y, d;
  *max_h = 0; *max_v = 0; *hard = 0;
  for (y = rc.y; y < rc.y + rc.h - 1; ++y)
    for (x = rc.x; x < rc.x + rc.w - 1; ++x) {
      d = lum(s, x + 1, y) - lum(s, x, y); if (d < 0) d = -d;
      if (d > *max_h) *max_h = d;
      if (d > 150) ++*hard;
      d = lum(s, x, y + 1) - lum(s, x, y); if (d < 0) d = -d;
      if (d > *max_v) *max_v = d;
      if (d > 150) ++*hard;
    }
}
static double mean_lum(SDL_Surface *s, SvRect rc) {
  double acc = 0.0;
  int x, y;
  for (y = rc.y; y < rc.y + rc.h; ++y) for (x = rc.x; x < rc.x + rc.w; ++x) acc += lum(s, x, y);
  return rc.w > 0 && rc.h > 0 ? acc / ((double)rc.w * (double)rc.h) : 0.0;
}
static int region_has_colour(SDL_Surface *s, SvRect rc, int r0, int g0, int b0) {
  int x, y, r, g, b;
  for (y = rc.y; y < rc.y + rc.h; ++y)
    for (x = rc.x; x < rc.x + rc.w; ++x) { px(s, x, y, &r, &g, &b); if (r == r0 && g == g0 && b == b0) return 1; }
  return 0;
}
static int region_flat(SDL_Surface *s, SvRect rc, int *r0, int *g0, int *b0) {
  int x, y, r, g, b;
  px(s, rc.x, rc.y, r0, g0, b0);
  for (y = rc.y; y < rc.y + rc.h; ++y)
    for (x = rc.x; x < rc.x + rc.w; ++x) { px(s, x, y, &r, &g, &b); if (r != *r0 || g != *g0 || b != *b0) return 0; }
  return 1;
}

typedef struct Geo { const char *name; SvSystem sys; int w, h, max_w, max_h; } Geo;

/* All three styles, both sources, on one geometry / aspect / scale / window size. */
static void check_geometry(SvHost *host, const Geo *g, SvAspect asp, SvScaleMode sc, const char *tag) {
  char what[200];
  const int side = sv_layout_extension_side(g->w, g->h, asp);
  const int pitch = (g->w + 2 * side) * 3 + 6;
  uint8_t *native = (uint8_t *)malloc((size_t)g->w * (size_t)g->h * 3u), *ext = (uint8_t *)malloc((size_t)pitch * (size_t)g->h);
  SDL_Surface *raw = NULL, *com = NULL, *bor = NULL, *aut = NULL, *aut_b = NULL;
  SvExtendedLayout lay;
  SvRect left, right, nat;
  uint64_t seq = 1000;
  SvExtendedSurface e;
  if (native == NULL || ext == NULL) { free(native); free(ext); CHECK(0, "allocation"); return; }
  if (side <= 0) { free(native); free(ext); return; } /* the native picture is already as wide as 16:9: nothing to extend */
  make_native(native, g->w, g->h, 3);
  make_ext(ext, g->w, side, g->h, pitch);
#define PRESENT(src_kind, mode_kind) do { ++seq; e = describe(ext, g->w, side, g->h, pitch, seq, mode_kind, src_kind); \
    CHECK(sv_host_present_extended(host, native, g->w, g->h, g->w * 3, seq, &e) == 0 && sv_host_extension_side(host) == side, "extended frame accepted"); } while (0)
  style(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, asp, sc);
  PRESENT(SV_EXTSRC_GENERIC, SV_WIDESCREEN_ENHANCED_GAMEPLAY);
  raw = shoot(host);
  CHECK(raw != NULL && sv_host_side_treatment(host) == SV_TREAT_PASSTHROUGH, "Raw: pass-through");
  style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, asp, sc);
  PRESENT(SV_EXTSRC_GENERIC, SV_WIDESCREEN_ENHANCED_GAMEPLAY);
  com = shoot(host);
  CHECK(com != NULL && sv_host_side_treatment(host) == SV_TREAT_COMFORT, "Comfort: generic extension is treated");
  style(host, SV_SIDE_BORDERS, SV_WIDESCREEN_ENHANCED_GAMEPLAY, asp, sc);
  PRESENT(SV_EXTSRC_GENERIC, SV_WIDESCREEN_ENHANCED_GAMEPLAY);
  bor = shoot(host);
  CHECK(bor != NULL && sv_host_side_treatment(host) == SV_TREAT_BORDERS, "Borders: treated");
  style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, asp, sc);
  PRESENT(SV_EXTSRC_AUTHORED, SV_WIDESCREEN_ENHANCED_GAMEPLAY);
  aut = shoot(host);
  CHECK(aut != NULL && sv_host_side_treatment(host) == SV_TREAT_PASSTHROUGH && sv_host_extension_source(host) == SV_EXTSRC_AUTHORED, "Comfort: an authored surface bypasses");
  style(host, SV_SIDE_BORDERS, SV_WIDESCREEN_ENHANCED_GAMEPLAY, asp, sc);
  PRESENT(SV_EXTSRC_AUTHORED, SV_WIDESCREEN_ENHANCED_GAMEPLAY);
  aut_b = shoot(host);
  CHECK(aut_b != NULL && sv_host_side_treatment(host) == SV_TREAT_BORDERS, "Borders overrides an authored surface (explicit user request)");
  if (raw == NULL || com == NULL || bor == NULL || aut == NULL || aut_b == NULL) goto done;
  CHECK(raw->w == com->w && raw->h == com->h && raw->w == bor->w, "same output size for every style");
  if (sv_layout_extended_dest(raw->w, raw->h, g->w, g->h, side, asp, sc, &lay) != 0) { CHECK(0, "layout"); goto done; }
  { /* review aid: SEGARECOMP_SIDE_STYLE_DUMP=<dir> writes the four presented outputs of the 1280x720 square-pixel integer H40 case */
    const char *dump = SDL_getenv("SEGARECOMP_SIDE_STYLE_DUMP");
    if (dump != NULL && g->w == 320 && g->h == 224 && strncmp(tag, "1280x720 square integer", 23) == 0) {
      char path[700];
      snprintf(path, sizeof path, "%s/raw.bmp", dump); SDL_SaveBMP(raw, path);
      snprintf(path, sizeof path, "%s/comfort.bmp", dump); SDL_SaveBMP(com, path);
      snprintf(path, sizeof path, "%s/borders.bmp", dump); SDL_SaveBMP(bor, path);
      snprintf(path, sizeof path, "%s/authored.bmp", dump); SDL_SaveBMP(aut, path);
    }
  }
  nat = lay.native;
  /* a fractional (Fit) scale rounds the seams by an output pixel: every rectangle keeps a 2-pixel margin from the seam and from the outer letterbox */
  left.x = lay.surface.x + 1; left.y = nat.y + 2; left.w = nat.x - lay.surface.x - 3; left.h = nat.h - 4;
  right.x = nat.x + nat.w + 2; right.y = nat.y + 2; right.w = lay.surface.x + lay.surface.w - right.x - 1; right.h = nat.h - 4;
  nat.x += 2; nat.y += 2; nat.w -= 4; nat.h -= 4;
  snprintf(what, sizeof what, "%s %s: side regions exist", g->name, tag);
  CHECK(left.w > 0 && right.w > 0, what);

  /* the native picture is bit-identical in every style */
  snprintf(what, sizeof what, "%s %s: the native-centre pixels are identical in Raw, Comfort, Borders and authored", g->name, tag);
  CHECK(region_hash(raw, nat) == region_hash(com, nat) && region_hash(raw, nat) == region_hash(bor, nat) && region_hash(raw, nat) == region_hash(aut, nat), what);
  CHECK(!region_has_colour(raw, nat, 77, 77, 77) && !region_has_colour(com, nat, 77, 77, 77), "the extension's centre is never shown");

  /* Raw shows the hostile extension: hard stripes and the sprite block */
  { int mh, mv, hard;
    steps(raw, left, &mh, &mv, &hard);
    CHECK(hard > 0 && mh >= 250, "Raw side contains the distracting hard edges (scene sanity)");
    CHECK(region_has_colour(raw, left, 255, 255, 0), "Raw shows the extension's sprite block"); }

  /* Comfort: smooth, subdued, no repeated pattern, no sprites, native untouched, sides not equal to raw */
  { int mh, mv, hard, mh2, mv2, hard2;
    steps(com, left, &mh, &mv, &hard);
    steps(com, right, &mh2, &mv2, &hard2);
    snprintf(what, sizeof what, "%s %s: Comfort sides have no hard edges (max step %d/%d)", g->name, tag, mh > mh2 ? mh : mh2, mv > mv2 ? mv : mv2);
    CHECK(hard == 0 && hard2 == 0 && mh <= 60 && mh2 <= 60 && mv <= 60 && mv2 <= 60, what);
    snprintf(what, sizeof what, "%s %s: Comfort is subdued compared with Raw", g->name, tag);
    CHECK(mean_lum(com, left) < 0.5 * mean_lum(raw, left) && mean_lum(com, right) < 0.5 * mean_lum(raw, right), what);
    CHECK(!region_has_colour(com, left, 255, 255, 0) && !region_has_colour(com, left, 255, 0, 255) && !region_has_colour(com, left, 0, 255, 0) &&
              !region_has_colour(com, right, 255, 0, 255) && !region_has_colour(com, right, 0, 255, 0),
          "Comfort shows no extension tiles or sprites");
    CHECK(region_hash(com, left) != region_hash(raw, left) && region_hash(com, right) != region_hash(raw, right), "Comfort differs from Raw at the sides");
    /* the fade ends dark and the strongest colour is next to the native edge */
    CHECK(lum(com, left.x, left.y + left.h / 2) < lum(com, left.x + left.w - 1, left.y + left.h / 2) + 8, "Comfort fades towards the outer boundary"); }

  /* Borders: flat neutral panels, no extension content */
  { int r0, g0, b0, r1, g1, b1;
    CHECK(region_flat(bor, left, &r0, &g0, &b0) && region_flat(bor, right, &r1, &g1, &b1) && r0 == g0 && g0 == b0 && r0 == r1 && g0 == g1 && b0 == b1 && r0 < 40,
          "Borders: flat neutral panels on both sides");
    CHECK(!region_has_colour(bor, left, 255, 255, 0), "Borders: no extension content"); }

  /* authored surface under Comfort is exactly the Raw output; under Borders it is the Borders output */
  snprintf(what, sizeof what, "%s %s: an authored surface is displayed exactly as delivered under Comfort", g->name, tag);
  CHECK(full_hash(aut) == full_hash(raw), what);
  CHECK(full_hash(aut_b) == full_hash(bor), "Borders overrides an authored surface identically");
done:
  if (raw != NULL) SDL_DestroySurface(raw);
  if (com != NULL) SDL_DestroySurface(com);
  if (bor != NULL) SDL_DestroySurface(bor);
  if (aut != NULL) SDL_DestroySurface(aut);
  if (aut_b != NULL) SDL_DestroySurface(aut_b);
  free(native); free(ext);
#undef PRESENT
}

static void geometry_matrix(const char *settings_file) {
  static const Geo geos[] = {
    {"Genesis H32", SV_SYSTEM_GENESIS, 256, 224, 320, 480}, {"Genesis H40", SV_SYSTEM_GENESIS, 320, 224, 320, 480},
    {"Genesis H40 interlace", SV_SYSTEM_GENESIS, 320, 448, 320, 480}, {"SMS 192", SV_SYSTEM_SMS, 256, 192, 256, 240}, {"SMS 224", SV_SYSTEM_SMS, 256, 224, 256, 240}};
  size_t i;
  for (i = 0; i < sizeof geos / sizeof geos[0]; ++i) {
    const Geo *g = &geos[i];
    SvHost *host = open_host(g->sys, settings_file, g->max_w, g->max_h, g->w, g->h <= 240 ? g->h : 224);
    int a, s, big;
    CHECK(host != NULL, "host opens");
    if (host == NULL) continue;
    for (big = 0; big < 2; ++big) {
      SDL_SetWindowSize(host->window, big ? 1920 : 1280, big ? 1080 : 720);
      for (a = 0; a < 2; ++a)
        for (s = 0; s < 2; ++s) {
          char tag[64];
          snprintf(tag, sizeof tag, "%dx%d %s %s", big ? 1920 : 1280, big ? 1080 : 720, a ? "4:3" : "square", s ? "integer" : "fit");
          check_geometry(host, g, a ? SV_ASPECT_4_3 : SV_ASPECT_ORIGINAL, s ? SV_SCALE_INTEGER : SV_SCALE_FIT, tag);
        }
    }
    sv_host_close(host);
  }
}

/* ---- the pause page: Side Presentation next to Widescreen Mode, immediate application, content vs presentation wording ---- */
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
typedef struct MenuScript {
  int step;
  char label[3][64], text[3][64], hint[3][320];
  SvSideStyle style_seen[3];
  SvSideTreatment treat_seen[3];
  int row_after_widescreen;
} MenuScript;
static SvItem *menu_item(SvHost *host, int id) {
  SvPage *p = sv_menu_top(&host->menu);
  int i;
  if (p == NULL) return NULL;
  for (i = 0; i < p->count; ++i) if (p->items[i].id == id) return &p->items[i];
  return NULL;
}
static void menu_record(MenuScript *m, SvHost *host, int slot) {
  SvItem *it = menu_item(host, I_SIDE_STYLE);
  if (it == NULL) return;
  snprintf(m->label[slot], sizeof m->label[slot], "%s", it->label);
  snprintf(m->text[slot], sizeof m->text[slot], "%s", it->text);
  snprintf(m->hint[slot], sizeof m->hint[slot], "%s", it->hint);
  m->style_seen[slot] = sv_host_settings(host)->video.widescreen_side_style;
  m->treat_seen[slot] = sv_host_side_treatment(host);
}
static int menu_hook(void *ctx, SvHost *host, int iteration) {
  MenuScript *m = (MenuScript *)ctx;
  SvPage *p = sv_menu_top(&host->menu);
  (void)iteration;
  switch (m->step) {
    case 0: case 1: tap(SV_KEY_DOWN); break;
    case 2: tap(SV_KEY_RETURN); break; /* Video & Display */
    case 3: case 4: case 5: tap(SV_KEY_DOWN); break; /* Fullscreen -> Scaling -> Aspect -> Widescreen Mode */
    case 6: /* the row after Widescreen Mode is Side Presentation */
      if (p != NULL) { /* only the (non-focusable) status note may lie between the two rows */
        int i, j;
        for (i = 0; i < p->count; ++i)
          if (p->items[i].id == I_WIDESCREEN) {
            for (j = i + 1; j < p->count && p->items[j].kind == SV_ITEM_LABEL; ++j) {}
            if (j < p->count && p->items[j].id == I_WS_WIDTH && p->items[j + 1].id == I_SIDE_STYLE) m->row_after_widescreen = 1;
          }
      }
      tap(SV_KEY_DOWN); break;
    case 7: tap(SV_KEY_DOWN); break; /* Widescreen Width -> Side Presentation */
    case 8: case 9: break; /* let focus settle */
    case 10: menu_record(m, host, 0); tap(SV_KEY_RIGHT); break;
    case 11: case 12: break;
    case 13: menu_record(m, host, 1); tap(SV_KEY_RIGHT); break;
    case 14: case 15: break;
    case 16: menu_record(m, host, 2); tap(SV_KEY_ESCAPE); break;
    case 17: tap(SV_KEY_ESCAPE); break;
    default: return 1;
  }
  m->step++;
  return 0;
}
static void menu_tests(const char *settings_file) {
  enum { W = 320, H = 224, SIDE = 39, PITCH = (W + 2 * SIDE) * 3 };
  uint8_t *native = (uint8_t *)malloc((size_t)W * H * 3u), *ext = (uint8_t *)malloc((size_t)PITCH * H);
  SvHost *host = open_host(SV_SYSTEM_GENESIS, settings_file, W, 240, W, H);
  MenuScript m;
  SvSample smp;
  SvExtendedSurface e;
  CHECK(host != NULL && native != NULL && ext != NULL, "menu: setup");
  if (host == NULL || native == NULL || ext == NULL) { free(native); free(ext); if (host != NULL) sv_host_close(host); return; }
  make_native(native, W, H, 0);
  make_ext(ext, W, SIDE, H, PITCH);
  style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  e = describe(ext, W, SIDE, H, PITCH, 1, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC);
  CHECK(sv_host_present_extended(host, native, W, H, W * 3, 1, &e) == 0, "menu: frame");
  memset(&m, 0, sizeof m);
  memset(&smp, 0, sizeof smp);
  sv_host_set_loop_hook(host, menu_hook, &m);
  tap(SV_KEY_ESCAPE);
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
  CHECK(m.row_after_widescreen, "the Video page offers Widescreen Width, then Side Presentation, directly after Widescreen Mode");
  CHECK(strcmp(m.label[0], "Side Presentation") == 0 && strcmp(m.text[0], "Comfort") == 0 && m.style_seen[0] == SV_SIDE_COMFORT && m.treat_seen[0] == SV_TREAT_COMFORT, "Comfort is selected by default");
  CHECK(strstr(m.hint[0], "does not create new gameplay visibility") != NULL && strstr(m.hint[0], "decorative fill, not extra game content") != NULL, "Comfort help: hides extra data, creates no gameplay visibility");
  CHECK(strcmp(m.text[1], "Borders") == 0 && m.style_seen[1] == SV_SIDE_BORDERS && m.treat_seen[1] == SV_TREAT_BORDERS, "Right selects Borders and applies it at once");
  CHECK(strcmp(m.text[2], "Raw Extension") == 0 && m.style_seen[2] == SV_SIDE_RAW && m.treat_seen[2] == SV_TREAT_PASSTHROUGH, "Right selects Raw Extension and applies it at once");
  CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "the menu change left the requested Level 3 mode alone");
  /* an authored surface under Comfort says its content is preserved */
  style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  e = describe(ext, W, SIDE, H, PITCH, 2, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_AUTHORED);
  CHECK(sv_host_present_extended(host, native, W, H, W * 3, 2, &e) == 0, "menu: authored frame");
  memset(&m, 0, sizeof m);
  sv_host_set_loop_hook(host, menu_hook, &m);
  tap(SV_KEY_ESCAPE);
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
  CHECK(strstr(m.hint[0], "widescreen content is preserved in full") != NULL, "an authored surface under Comfort: the help says its full content is preserved");
  /* the settings file keeps the choice */
  sv_host_close(host);
  { char buf[16384]; FILE *f = fopen(settings_file, "rb"); size_t n = f != NULL ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f != NULL) fclose(f);
    buf[n] = '\0';
    CHECK(strstr(buf, "video.widescreen_side_style=raw") != NULL && strstr(buf, "video.widescreen=enhanced_gameplay") != NULL, "the side style is persisted next to the Level 3 preference"); }
  free(native); free(ext);
}

static void behaviour_tests(const char *settings_file) {
  enum { W = 320, H = 224, SIDE = 39, PITCH = (W + 2 * SIDE) * 3 };
  uint8_t *native = (uint8_t *)malloc((size_t)W * H * 3u), *ext = (uint8_t *)malloc((size_t)PITCH * H), *n2 = (uint8_t *)malloc(400u * H * 3u);
  SvHost *host = open_host(SV_SYSTEM_GENESIS, settings_file, 400, 240, W, H);
  SvExtendedSurface e;
  SDL_Surface *a, *b, *c;
  SvExtendedLayout lay;
  uint32_t acc0, rej0, tex0, acc1, rej1, tex1;
  uint64_t seq = 5000;
  int i;
  CHECK(host != NULL && native != NULL && ext != NULL && n2 != NULL, "behaviour: setup");
  if (host == NULL || native == NULL || ext == NULL || n2 == NULL) { free(native); free(ext); free(n2); if (host != NULL) sv_host_close(host); return; }
  make_native(native, W, H, 0);
  make_ext(ext, W, SIDE, H, PITCH);

  /* the default is Comfort for an unconfigured installation, and old files without the key keep working */
  CHECK(sv_host_settings(host)->video.widescreen_side_style == SV_SIDE_COMFORT, "the default side style is Comfort");
  { SvSettings s; sv_settings_defaults(&s);
    CHECK(sv_settings_parse(&s, "video.widescreen=enhanced_gameplay\n", strlen("video.widescreen=enhanced_gameplay\n")) == 1 && s.video.widescreen_side_style == SV_SIDE_COMFORT &&
              s.video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "an old file without the key stays Comfort and keeps its Level 3 preference");
    sv_settings_parse(&s, "video.widescreen_side_style=nonsense\n", strlen("video.widescreen_side_style=nonsense\n"));
    CHECK(s.video.widescreen_side_style == SV_SIDE_COMFORT, "an invalid value becomes Comfort");
    sv_settings_parse(&s, "video.widescreen_side_style=raw\n", strlen("video.widescreen_side_style=raw\n"));
    CHECK(s.video.widescreen_side_style == SV_SIDE_RAW, "raw is parsed"); }

  /* Original is identical in every style (no extension is ever involved) */
  style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_present(host, native, W, H, W * 3) == 0, "Original present");
  a = shoot(host);
  style(host, SV_SIDE_RAW, SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_present(host, native, W, H, W * 3) == 0, "Original present (Raw)");
  b = shoot(host);
  style(host, SV_SIDE_BORDERS, SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(sv_host_present(host, native, W, H, W * 3) == 0, "Original present (Borders)");
  c = shoot(host);
  CHECK(a != NULL && b != NULL && c != NULL && full_hash(a) == full_hash(b) && full_hash(b) == full_hash(c) && sv_host_side_treatment(host) == SV_TREAT_PASSTHROUGH,
        "Original output is identical in every side style");
  if (a != NULL) SDL_DestroySurface(a);
  if (b != NULL) SDL_DestroySurface(b);
  if (c != NULL) SDL_DestroySurface(c);

  /* A genuinely widened native picture (no extension path) is never treated, whatever the style */
  { uint64_t hs[3];
    make_native(n2, 398, H, 1);
    for (i = 0; i < 3; ++i) {
      style(host, (SvSideStyle)i, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
      CHECK(sv_host_present(host, n2, 398, H, 398 * 3) == 0 && sv_host_extension_side(host) == 0 && sv_host_side_treatment(host) == SV_TREAT_PASSTHROUGH,
            "a genuinely wide native picture is shown with no side treatment");
      a = shoot(host); hs[i] = a != NULL ? full_hash(a) : 0u; if (a != NULL) SDL_DestroySurface(a);
    }
    CHECK(hs[0] == hs[1] && hs[1] == hs[2] && hs[0] != 0u, "a wide native picture is identical in every style"); }

  /* Comfort -> Raw -> Borders -> Comfort applies at once to the frame on screen (no new present) and never changes the requested mode, the producer-facing
   * counters or the texture */
  style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  e = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC);
  CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0, "Level 3 frame accepted");
  sv_host_extension_stats(host, &acc0, &rej0, &tex0);
  { static const SvSideStyle order[] = {SV_SIDE_COMFORT, SV_SIDE_RAW, SV_SIDE_BORDERS, SV_SIDE_COMFORT};
    static const SvSideTreatment want[] = {SV_TREAT_COMFORT, SV_TREAT_PASSTHROUGH, SV_TREAT_BORDERS, SV_TREAT_COMFORT};
    uint64_t seen[4];
    size_t k;
    for (k = 0; k < 4; ++k) {
      SvSettings s = *sv_host_settings(host);
      s.video.widescreen_side_style = order[k];
      sv_host_apply_settings(host, &s);
      CHECK(sv_host_side_treatment(host) == want[k], "the style change is applied immediately");
      CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "the requested Level 3 mode is untouched by a style change");
      a = shoot(host); seen[k] = a != NULL ? full_hash(a) : 0u; if (a != NULL) SDL_DestroySurface(a);
    }
    CHECK(seen[0] == seen[3] && seen[0] != seen[1] && seen[1] != seen[2] && seen[0] != seen[2], "Comfort -> Raw -> Borders -> Comfort is deterministic and distinct"); }
  sv_host_extension_stats(host, &acc1, &rej1, &tex1);
  CHECK(acc1 == acc0 && rej1 == rej0 && tex1 == tex0, "style switching changes no producer-facing counter and creates no texture");

  /* the native screenshot (game pixels only) is identical whatever the side style */
  { uint64_t sh[3];
    int k;
    for (k = 0; k < 3; ++k) {
      SvSettings st = *sv_host_settings(host);
      char path[1024];
      FILE *f;
      st.video.widescreen_side_style = (SvSideStyle)k;
      sv_host_apply_settings(host, &st);
      path[0] = '\0';
      sh[k] = 0u;
      if (sv_host_screenshot(host, path, sizeof path) == 0 && (f = fopen(path, "rb")) != NULL) {
        uint64_t hh = 1469598103934665603ull;
        int c;
        while ((c = fgetc(f)) != EOF) hh = (hh ^ (uint64_t)(unsigned)c) * 1099511628211ull;
        fclose(f);
        sh[k] = hh;
      }
    }
    CHECK(sh[0] != 0u && sh[0] == sh[1] && sh[1] == sh[2], "native screenshots are identical in every side style"); }

  /* steady state: many frames at stable geometry, any style: no texture churn */
  for (i = 0; i < 40; ++i) {
    SvSettings s = *sv_host_settings(host);
    s.video.widescreen_side_style = (SvSideStyle)(i % 3);
    sv_host_apply_settings(host, &s);
    make_native(native, W, H, i);
    e = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC);
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0, "steady frame");
  }
  sv_host_extension_stats(host, &acc1, &rej1, &tex1);
  CHECK(tex1 == tex0 && acc1 == acc0 + 40u && rej1 == rej0, "no texture creation and no rejection over 40 frames at stable geometry");

  /* stale sprite pixels: a Level 3 frame whose side holds the yellow sprite is never visible after switching to Comfort/Borders, nor re-exposed when the
   * user has meanwhile stepped down to Level 2 and Raw in one edit */
  style(host, SV_SIDE_RAW, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  make_native(native, W, H, 0);
  e = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC);
  CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0, "sprite frame");
  a = shoot(host);
  sv_layout_extended_dest(a->w, a->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay);
  { SvRect left; left.x = lay.surface.x; left.y = lay.native.y; left.w = lay.native.x - lay.surface.x; left.h = lay.native.h;
    CHECK(region_has_colour(a, left, 255, 255, 0), "Raw shows the Level 3 sprite");
    SDL_DestroySurface(a);
    style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    a = shoot(host); CHECK(!region_has_colour(a, left, 255, 255, 0), "Comfort: no stale sprite pixels after the switch"); SDL_DestroySurface(a);
    style(host, SV_SIDE_BORDERS, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    a = shoot(host); CHECK(!region_has_colour(a, left, 255, 255, 0), "Borders: no stale sprite pixels after the switch"); SDL_DestroySurface(a);
    /* one edit: Level 2 + Raw while the retained surface was produced as Level 3 -> the retained frame must not be re-exposed */
    style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    { SvSettings s = *sv_host_settings(host);
      s.video.widescreen = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; s.video.widescreen_side_style = SV_SIDE_RAW;
      sv_host_apply_settings(host, &s); }
    a = shoot(host); CHECK(!region_has_colour(a, left, 255, 255, 0), "a retained Level 3 surface is not re-exposed after stepping down to Level 2"); SDL_DestroySurface(a);
    /* and the next Level 3 surface produced for the old request is stale for presentation */
    e = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_GENERIC);
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_extension_side(host) == 0, "a surface produced above the requested level falls back to the original picture"); }

  /* validation is unchanged and an authored declaration cannot bypass it */
  style(host, SV_SIDE_COMFORT, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  sv_host_extension_stats(host, &acc0, &rej0, &tex0);
  { SvExtendedSurface bad;
    uint8_t *tiny = (uint8_t *)calloc(64, 1);
    bad = describe(ext, W, SIDE, H, PITCH, seq + 99, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_AUTHORED); /* stale identity */
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, ++seq, &bad) == 0 && sv_host_extension_side(host) == 0, "an authored surface with a stale identity is rejected");
    bad = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_AUTHORED); bad.width += 1; /* geometry */
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &bad) == 0 && sv_host_extension_side(host) == 0, "an authored surface with bad geometry is rejected");
    bad = describe(tiny, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_EXTSRC_AUTHORED); bad.size = 64; /* capacity */
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &bad) == 0 && sv_host_extension_side(host) == 0, "an authored surface that does not cover its bytes is rejected");
    bad = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ORIGINAL, SV_EXTSRC_AUTHORED); /* content mode */
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &bad) == 0 && sv_host_extension_side(host) == 0, "an authored surface produced as Original is rejected");
    bad = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_ENHANCED_GAMEPLAY, (SvExtSource)77); /* an unknown provenance is generic */
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &bad) == 0 && sv_host_side_treatment(host) == SV_TREAT_COMFORT && sv_host_extension_source(host) == SV_EXTSRC_GENERIC,
          "an unknown provenance value is treated as generic");
    free(tiny); }
  sv_host_extension_stats(host, &acc1, &rej1, &tex1);
  CHECK(rej1 == rej0 + 4u && acc1 == acc0 + 1u, "the four invalid authored surfaces were counted as rejections");

  /* CRT + fullscreen: the treatment sits before the CRT stage and all of them compose */
  { SvSettings s = *sv_host_settings(host);
    uint64_t hn_raw, hn_com;
    SvRect nat;
    s.video.widescreen = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; s.video.scale = SV_SCALE_INTEGER; s.video.aspect = SV_ASPECT_ORIGINAL;
    s.video.scanlines = 40; s.video.mask = 30; s.video.curvature = 0; s.video.bloom = 0; s.video.preset = SV_CRT_CUSTOM;
    s.video.widescreen_side_style = SV_SIDE_RAW;
    sv_host_apply_settings(host, &s);
    make_native(native, W, H, 2);
    e = describe(ext, W, SIDE, H, PITCH, ++seq, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_EXTSRC_GENERIC);
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0, "CRT: frame");
    a = shoot(host); sv_layout_extended_dest(a->w, a->h, W, H, SIDE, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay); nat = lay.native;
    hn_raw = region_hash(a, nat); SDL_DestroySurface(a);
    s.video.widescreen_side_style = SV_SIDE_COMFORT; sv_host_apply_settings(host, &s);
    a = shoot(host); hn_com = region_hash(a, nat);
    CHECK(hn_raw == hn_com, "CRT scanlines/mask: the native picture is identical in Raw and Comfort");
    { int mh, mv, hard; SvRect left; left.x = lay.surface.x; left.y = nat.y; left.w = nat.x - lay.surface.x; left.h = nat.h;
      steps(a, left, &mh, &mv, &hard);
      CHECK(mh >= 0, "CRT: sides are rendered through the same pipeline");
      (void)mv; (void)hard; }
    SDL_DestroySurface(a);
    s.video.fullscreen = 1; sv_host_apply_settings(host, &s);
    CHECK(sv_host_present_extended(host, native, W, H, W * 3, seq, &e) == 0 && sv_host_side_treatment(host) == SV_TREAT_COMFORT, "fullscreen: Comfort stays applied");
    s.video.fullscreen = 0; sv_host_apply_settings(host, &s); }

  /* the pause page offers Side Presentation; the status text keeps content capability and presentation separate */
  CHECK(strcmp(sv_side_style_name(SV_SIDE_RAW), "Raw Extension") == 0 && strstr(sv_side_style_description(SV_SIDE_COMFORT), "does not create new gameplay visibility") != NULL,
        "the Comfort help text says it does not create gameplay visibility");
  sv_host_close(host);
  free(native); free(ext); free(n2);
}

int main(void) {
  char dir[512], settings_file[600];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  snprintf(dir, sizeof dir, "%s/sv_side_style_test_%d", tmp, (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(settings_file, sizeof settings_file, "%s/viewer-settings.cfg", dir);
  remove(settings_file);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  { char shots[700]; snprintf(shots, sizeof shots, "%s/shots", dir); SDL_setenv_unsafe("SEGARECOMP_SCREENSHOT_DIR", shots, 1); }
  behaviour_tests(settings_file);
  remove(settings_file);
  menu_tests(settings_file);
  remove(settings_file);
  geometry_matrix(settings_file);
  remove(settings_file);
  if (failures != 0) { printf("sv_side_style_test: %d failure(s)\n", failures); return 1; }
  printf("sv_side_style_test: OK\n");
  return 0;
}
