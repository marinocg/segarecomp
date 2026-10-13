/* SEG-058: renders the shared viewer's screens from a FICTIONAL synthetic picture (no game, no ROM) under the dummy video driver, for
 * documentation screenshots and manual review. Not a test (nothing is asserted) and never shipped.
 *
 *   sv_gallery <out-dir> <genesis|sms|sms224> [WxH]   (sms224: the Master System's 224-line mode)
 *
 * WxH is the window (output) size in pixels (default: the viewer's own default window). SEGARECOMP_ASSETS points at the launcher assets (icons, wordmark);
 * SV_GALLERY_PAD = none | live | raw | locked picks the virtual controller shown on the Controller status page (live: a gamepad with some buttons held;
 * raw: a joystick SDL has no mapping for; locked: a gamepad whose SDL_OpenGamepad fails).
 * Writes 01_gameplay, 02_pause_menu, 03_quick_settings, 04_video_display, 05_controls, 05b_controller_status, 06_audio, 07_system, 08_quit_confirmation, 09_toast,
 * 10_hud and crt_<preset> images (PNG with SDL >= 3.4, BMP otherwise). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_input_diag.h"

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
    e.type = (Uint32)(i == 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP);
    e.common.timestamp = SDL_GetTicksNS();
    e.key.scancode = (SDL_Scancode)sc;
    e.key.down = i == 0;
    SDL_PushEvent(&e);
  }
}

/* A fictional scene (not any game): stepped dusk sky, a sun, parallax hills, a checkered ground, a few blocks and a HUD bar. */
static void scene(uint8_t *rgb, int w, int h) {
  int x, y;
  for (y = 0; y < h; ++y)
    for (x = 0; x < w; ++x) {
      uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
      const int band = y * 10 / h;
      int r = 18 + band * 20, g = 26 + band * 13, b = 104 + band * 10;
      const int horizon = h * 11 / 20;
      if ((x - w * 3 / 4) * (x - w * 3 / 4) + (y - h / 4) * (y - h / 4) < (h / 7) * (h / 7)) { r = 255; g = 214; b = 110; }
      if (y > horizon - 26 - ((x * 7 / 11) % 29) && y <= horizon) { r = 52; g = 58; b = 128; }                       /* far hills */
      if (y > horizon - 12 - ((x * 5 / 7 + 9) % 19) && y <= horizon + 2) { r = 34; g = 96; b = 98; }                 /* near hills */
      if (y > horizon + 2) { const int k = (x / 16 + y / 8) % 2; r = 44 + k * 20; g = 138 + k * 26; b = 74 + k * 8; }  /* checkered ground */
      if (y > h * 15 / 16) { r = 96; g = 62; b = 40; }
      if (x > w / 5 && x < w / 5 + 14 && y > horizon - 24 && y < horizon + 2) { r = 238; g = 90; b = 72; }          /* a character-sized block */
      if (x > w / 2 && x < w / 2 + 28 && y > horizon + 10 && y < horizon + 26) { r = 232; g = 176; b = 64; }         /* a crate */
      if (y > 7 && y < 15 && x > 8 && x < 8 + w / 3) { r = 250; g = 230; b = 120; }                                  /* HUD bar */
      if (y > 7 && y < 15 && x > w - 8 - w / 5 && x < w - 8) { r = 120; g = 220; b = 240; }
      p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
    }
}

typedef struct Step { const char *capture; int keys[10]; } Step;

static const Step script[] = {
    {"02_pause_menu", {SV_KEY_DOWN, SV_KEY_RETURN, 0}},                          /* -> Quick Settings */
    {"03_quick_settings", {SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},        /* back, -> Video & Display */
    {"04_video_display", {SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},         /* back, -> Controls */
    {"05_controls", {SV_KEY_DOWN, SV_KEY_RETURN, 0}},                             /* Player -> Controller status & test */
    {"05b_controller_status", {SV_KEY_ESCAPE, SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}}, /* back, back, -> Audio */
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
  int win_w = 0, win_h = 0;
  SDL_JoystickID pad_id = 0;
  SDL_Joystick *pad_joy = NULL;
  const char *pad_mode = SDL_getenv("SV_GALLERY_PAD");
  if (argc < 3) { fprintf(stderr, "usage: %s <out-dir> <genesis|sms|sms224> [WxH]\n", argv[0]); return 2; }
  if (argc > 3 && sscanf(argv[3], "%dx%d", &win_w, &win_h) != 2) { fprintf(stderr, "bad size %s\n", argv[3]); return 2; }
  g_out = argv[1];
  sms = strcmp(argv[2], "sms") == 0 || strcmp(argv[2], "sms224") == 0;
#if SDL_VERSION_ATLEAST(3, 4, 0)
  g_ext = "png";
#else
  g_ext = "bmp";
#endif
  SDL_CreateDirectory(g_out);
  snprintf(settings, sizeof settings, "%s/viewer-settings.cfg", g_out);
  remove(settings);
  if (win_w > 0 && win_h > 0) { /* a saved window size is what the viewer opens at */
    SvSettings pre;
    sv_settings_defaults(&pre);
    pre.video.window_w = win_w;
    pre.video.window_h = win_h;
    sv_settings_save(&pre, settings);
  }
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings, 1);
  SDL_setenv_unsafe("SEGARECOMP_ROM_SHA256", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
  SDL_setenv_unsafe("SEGARECOMP_LAUNCHED", "1", 1);
  w = sms ? 256 : 320; h = sms ? (strcmp(argv[2], "sms224") == 0 ? 224 : 192) : 224;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = sms ? SV_SYSTEM_SMS : SV_SYSTEM_GENESIS;
  cfg.title = "gallery";
  cfg.picture_w = w; cfg.picture_h = h; cfg.max_w = 320; cfg.max_h = 240; cfg.default_scale = 3;
  cfg.can_reset = sms;
  cfg.rom_sha256 = SDL_getenv("SEGARECOMP_ROM_SHA256");
  if (pad_mode != NULL && strcmp(pad_mode, "locked") == 0) sv_diag_set_open_hook(NULL); /* replaced below once SDL is up */
  host = sv_host_open(&cfg);
  if (host == NULL) { fprintf(stderr, "cannot open the host\n"); return 1; }
  if (pad_mode != NULL && strcmp(pad_mode, "none") != 0 && *pad_mode != '\0') {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = (Uint16)(strcmp(pad_mode, "raw") == 0 ? SDL_JOYSTICK_TYPE_UNKNOWN : SDL_JOYSTICK_TYPE_GAMEPAD);
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = strcmp(pad_mode, "raw") == 0 ? "Fictional Raw Stick" : (strcmp(pad_mode, "locked") == 0 ? "Fictional Locked Pad" : "Fictional Pro Pad");
    desc.vendor_id = strcmp(pad_mode, "live") == 0 ? 0x057e : 0x1234;
    desc.product_id = strcmp(pad_mode, "live") == 0 ? 0x2009 : 0x0001;
    pad_id = SDL_AttachVirtualJoystick(&desc);
    pad_joy = pad_id != 0 ? SDL_OpenJoystick(pad_id) : NULL;
    if (pad_joy != NULL && strcmp(pad_mode, "live") == 0) {
      SDL_SetJoystickVirtualButton(pad_joy, SDL_GAMEPAD_BUTTON_EAST, true);
      SDL_SetJoystickVirtualButton(pad_joy, SDL_GAMEPAD_BUTTON_START, true);
      SDL_SetJoystickVirtualAxis(pad_joy, SDL_GAMEPAD_AXIS_LEFTX, 21000);
    }
    SDL_UpdateJoysticks();
  }
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
  s.video.show_fps = 1;
  sv_host_apply_settings(host, &s);
  save(sv_host_capture_output(host), "10_hud");
  s.video.show_fps = 0;
  sv_host_apply_settings(host, &s);
  sv_host_set_loop_hook(host, hook, &c);
  key(SV_KEY_ESCAPE);
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
  if (pad_joy != NULL) SDL_CloseJoystick(pad_joy);
  if (pad_id != 0) SDL_DetachVirtualJoystick(pad_id);
  sv_host_close(host);
  free(rgb);
  return 0;
}
