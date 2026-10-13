/* SEG-058: renders the shared viewer's screens from a FICTIONAL synthetic picture (no game, no ROM) under the dummy video driver, for
 * documentation screenshots and manual review. Not a test (nothing is asserted) and never shipped.
 *
 *   sv_gallery <out-dir> <genesis|sms>
 *
 * Writes 01_gameplay, 02_pause_menu, 03_quick_settings, 04_video_display, 05_controls, 06_audio, 07_system, 08_quit_confirmation and
 * crt_<preset> images (PNG with SDL >= 3.4, BMP otherwise). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"

static const char *g_out;
static const char *g_ext;

static void save(SDL_Surface *s, const char *name) {
  char path[600];
  if (s == NULL) return;
  snprintf(path, sizeof path, "%s/%s.%s", g_out, name, g_ext);
#if SDL_VERSION_ATLEAST(3, 4, 0)
  SDL_SavePNG(s, path);
#else
  SDL_SaveBMP(s, path);
#endif
  SDL_DestroySurface(s);
  printf("wrote %s\n", path);
}

static void key(int sc) {
  SDL_Event e;
  int i;
  for (i = 0; i < 2; ++i) {
    SDL_zero(e);
    e.type = i == 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.common.timestamp = SDL_GetTicksNS();
    e.key.scancode = (SDL_Scancode)sc;
    e.key.down = i == 0;
    SDL_PushEvent(&e);
  }
}

/* A fictional scene: stepped sky, sun, two hills, a "hero" block and HUD bars. */
static void scene(uint8_t *rgb, int w, int h) {
  int x, y;
  for (y = 0; y < h; ++y)
    for (x = 0; x < w; ++x) {
      uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
      const int band = y * 8 / h;
      int r = 20 + band * 22, g = 30 + band * 14, b = 110 + band * 12;
      if ((x - w * 3 / 4) * (x - w * 3 / 4) + (y - h / 4) * (y - h / 4) < (h / 8) * (h / 8)) { r = 255; g = 190; b = 80; }
      if (y > h * 5 / 8 + ((x / 16) % 3) * 4 - (x / 24) % 6) { r = 30 + (x / 16 + y / 16) % 2 * 24; g = 130 + (x / 16 + y / 16) % 2 * 30; b = 80; }
      if (y > h * 7 / 8) { r = 90; g = 60; b = 40; }
      if (x > w / 4 && x < w / 4 + 16 && y > h * 5 / 8 - 28 && y < h * 5 / 8) { r = 240; g = 80; b = 70; }
      if (y > 6 && y < 14 && x > 8 && x < 8 + w / 3) { r = 250; g = 230; b = 120; }
      p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
    }
}

typedef struct Step { const char *capture; int keys[10]; } Step;

static const Step script[] = {
    {"02_pause_menu", {SV_KEY_DOWN, SV_KEY_RETURN, 0}},                          /* -> Quick Settings */
    {"03_quick_settings", {SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},        /* back, -> Video & Display */
    {"04_video_display", {SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},         /* back, -> Controls */
    {"05_controls", {SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},              /* back, -> Audio */
    {"06_audio", {SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},                 /* back, -> System */
    {"07_system", {SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},                /* back, -> Quit confirmation */
    {"08_quit_confirmation", {SV_KEY_ESCAPE, SV_KEY_ESCAPE, 0}},                  /* back, resume */
};

typedef struct Ctx { int step, wait; } Ctx;

static int hook(void *ctx, SvHost *host, int iteration) {
  Ctx *c = (Ctx *)ctx;
  int i;
  (void)iteration;
  if (c->wait > 0) { --c->wait; return 0; }
  if (c->step >= (int)(sizeof script / sizeof script[0])) return 1;
  { /* the Video page previews a CRT preset; every other screen shows the plain picture */
    SvSettings s = *sv_host_settings(host);
    sv_apply_crt_preset(&s.video, c->step == 2 ? SV_CRT_CLASSIC : SV_CRT_PIXEL_PERFECT);
    sv_host_apply_settings(host, &s);
  }
  save(sv_host_capture_output(host), script[c->step].capture);
  for (i = 0; script[c->step].keys[i] != 0; ++i) key(script[c->step].keys[i]);
  c->step++;
  c->wait = 2; /* let the pushed events run and the next page draw */
  return 0;
}

int main(int argc, char **argv) {
  SvHostConfig cfg;
  SvHost *host;
  SvSettings s;
  SvSample smp;
  uint8_t *rgb;
  Ctx c = {0, 1};
  int sms, w, h, i;
  char settings[700];
  if (argc < 3) { fprintf(stderr, "usage: %s <out-dir> <genesis|sms>\n", argv[0]); return 2; }
  g_out = argv[1];
  sms = strcmp(argv[2], "sms") == 0;
#if SDL_VERSION_ATLEAST(3, 4, 0)
  g_ext = "png";
#else
  g_ext = "bmp";
#endif
  SDL_CreateDirectory(g_out);
  snprintf(settings, sizeof settings, "%s/viewer-settings.cfg", g_out);
  remove(settings);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings, 1);
  SDL_setenv_unsafe("SEGARECOMP_ROM_SHA256", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
  SDL_setenv_unsafe("SEGARECOMP_LAUNCHED", "1", 1);
  w = sms ? 256 : 320; h = sms ? 192 : 224;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = sms ? SV_SYSTEM_SMS : SV_SYSTEM_GENESIS;
  cfg.title = "gallery";
  cfg.picture_w = w; cfg.picture_h = h; cfg.max_w = 320; cfg.max_h = 240; cfg.default_scale = 3;
  cfg.can_reset = sms;
  cfg.rom_sha256 = SDL_getenv("SEGARECOMP_ROM_SHA256");
  host = sv_host_open(&cfg);
  if (host == NULL) { fprintf(stderr, "cannot open the host\n"); return 1; }
  rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
  scene(rgb, w, h);
  sv_host_present(host, rgb, w, h, w * 3);
  save(sv_host_capture_output(host), "01_gameplay");
  /* the CRT presets on the same picture */
  s = *sv_host_settings(host);
  for (i = 0; i < 4; ++i) {
    static const char *names[] = {"crt_pixel_perfect", "crt_soft", "crt_classic", "crt_arcade"};
    sv_apply_crt_preset(&s.video, (SvCrtPreset)i);
    sv_host_apply_settings(host, &s);
    save(sv_host_capture_output(host), names[i]);
  }
  sv_apply_crt_preset(&s.video, SV_CRT_CLASSIC); /* the Video page previews this one */
  s.video.show_fps = 0;
  sv_host_apply_settings(host, &s);
  sv_apply_crt_preset(&s.video, SV_CRT_CLASSIC);
  sv_host_apply_settings(host, &s);
  sv_host_toast(host, "Screenshot saved: genesis-20260101-120000-1.png");
  save(sv_host_capture_output(host), "09_toast");
  sv_host_toast(host, "");
  sv_host_set_loop_hook(host, hook, &c);
  key(SV_KEY_ESCAPE);
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
  sv_host_close(host);
  free(rgb);
  return 0;
}
