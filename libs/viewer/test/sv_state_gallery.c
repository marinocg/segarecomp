/* SEG-131 (ADR 0278): renders the save-state and rewind screens of the shared viewer from a FICTIONAL, project-authored picture and a FAKE provider -- no game,
 * no ROM, no machine -- under the dummy video driver, for documentation screenshots and review at several window sizes. Not a test (the companion
 * tests/sv_state_gallery_test.py checks that every screen was produced) and never shipped.
 *
 *   sv_state_gallery <out-dir> [WxH]
 *
 * Writes 11_pause_menu_states, 12_save_slots, 13_overwrite_confirmation, 14_load_slots, 15_load_confirmation, 16_rewind_open, 17_rewind_back_1, 18_rewind_middle,
 * 19_rewind_oldest, 20_rewind_newest_moment, 21_rewind_short_history, 22_rewind_memory_limited, 23_rewind_error, 24_rewind_unavailable, 25_states_unavailable (PNG with
 * SDL >= 3.4, BMP otherwise). SEGARECOMP_ASSETS points at the launcher assets (icons, wordmark). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_gallery_scene.h"
#include "sv_host.h"
#include "../src/sv_internal.h"

static const char *g_out;
static const char *g_ext;
static int failures;

static void save(SDL_Surface *s, const char *name) {
  char path[600];
  if (s == NULL) { ++failures; return; }
  snprintf(path, sizeof path, "%s/%s.%s", g_out, name, g_ext);
#if SDL_VERSION_ATLEAST(3, 4, 0)
  if (!SDL_SavePNG(s, path)) ++failures;
#else
  if (!SDL_SaveBMP(s, path)) ++failures;
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

/* ---- the fake provider: original thumbnails derived from the fictional review picture (shifted and tinted per moment), never a game frame ---- */
#define MAX_HISTORY 160
typedef struct Fake {
  int available;
  int occupied[SV_STATE_SLOTS], damaged[SV_STATE_SLOTS];
  int history, rewind_ok, fail_restore, limited;
  uint8_t slot_thumb[SV_STATE_SLOTS][SV_STATE_THUMB_MAX_W * SV_STATE_THUMB_MAX_H * 2];
  uint8_t moment_thumb[MAX_HISTORY][SV_STATE_THUMB_MAX_W * SV_STATE_THUMB_MAX_H * 2];
} Fake;

static uint8_t g_scene[320 * 224 * 3];

static void make_thumb(uint8_t *out, int dx, int tint) {
  int x, y;
  for (y = 0; y < 112; ++y)
    for (x = 0; x < 160; ++x) {
      const int sx = (x * 2 + dx) % 320, sy = y * 2;
      const uint8_t *p = &g_scene[((size_t)sy * 320u + (size_t)(sx < 0 ? sx + 320 : sx)) * 3u];
      int r = p[0], g = p[1], b = p[2];
      r = (r * (256 - tint) + 40 * tint) / 256;
      b = (b * (256 - tint) + 200 * tint) / 256;
      out[((size_t)y * 160u + (size_t)x) * 2u] = (uint8_t)((((g >> 2) & 7) << 5) | (b >> 3));
      out[((size_t)y * 160u + (size_t)x) * 2u + 1u] = (uint8_t)(((r >> 3) << 3) | (g >> 5));
    }
}

static int f_available(void *ctx, char *why, size_t cap) {
  const Fake *f = (const Fake *)ctx;
  if (!f->available) snprintf(why, cap, "No save location is configured for this session.");
  return f->available;
}
static int f_slot_info(void *ctx, int slot, SvStateSlotInfo *out) {
  Fake *f = (Fake *)ctx;
  memset(out, 0, sizeof *out);
  if (f->occupied[slot]) {
    static const uint64_t played[SV_STATE_SLOTS] = {754000u, 3723000u, 0u, 45000u, 0u, 0u};
    out->occupied = 1;
    out->guest_ms = played[slot];
    out->thumb.w = 160; out->thumb.h = 112; out->thumb.rgb565 = f->slot_thumb[slot];
  } else if (f->damaged[slot]) {
    out->damaged = 1;
    snprintf(out->note, sizeof out->note, "The state file is corrupt.");
  }
  return 0;
}
static int f_save(void *ctx, int slot, char *err, size_t cap) { (void)ctx; (void)slot; (void)err; (void)cap; return 0; }
static int f_load(void *ctx, int slot, char *err, size_t cap) { (void)ctx; (void)slot; (void)err; (void)cap; return 0; }
static int f_rewind_available(void *ctx, char *why, size_t cap) {
  const Fake *f = (const Fake *)ctx;
  if (!f->rewind_ok) snprintf(why, cap, "Rewind is unavailable: one saved moment (180 KB) does not fit the 1 MB memory limit.");
  return f->rewind_ok;
}
static int f_history_count(void *ctx) { return ((Fake *)ctx)->history; }
static int f_history_info(void *ctx, int index, SvRewindEntry *out) {
  Fake *f = (Fake *)ctx;
  memset(out, 0, sizeof *out);
  if (index < 0 || index >= f->history) return -1;
  out->age_ms = 500 * (f->history - index) - 120;
  out->thumb.w = 160; out->thumb.h = 112; out->thumb.rgb565 = f->moment_thumb[index];
  return 0;
}
static int f_restore(void *ctx, int index, char *err, size_t cap) {
  Fake *f = (Fake *)ctx;
  (void)index;
  if (f->fail_restore) { snprintf(err, cap, "the saved moment is damaged"); return -1; }
  return 0;
}
static int f_history_note(void *ctx, char *note, size_t cap) {
  const Fake *f = (const Fake *)ctx;
  if (!f->limited) return 0;
  snprintf(note, cap, "Memory limit reached: the last 18 seconds are kept.");
  return 1;
}

/* ---- scripted captures ---- */
typedef struct Step { const char *capture; int keys[8]; } Step;
typedef struct Ctx { const Step *steps; int count, at, wait; Fake *fake; } Ctx;

static int hook(void *vctx, SvHost *host, int iteration) {
  Ctx *c = (Ctx *)vctx;
  int i;
  (void)iteration;
  if (host->rewind != NULL && host->rewind->anim < 1.0f) return 0;   /* the rewind surface settles first */
  if (c->wait > 0) { --c->wait; return 0; }
  if (c->at >= c->count) return 1;
  if (c->steps[c->at].capture != NULL) save(sv_host_capture_output(host), c->steps[c->at].capture);
  for (i = 0; c->steps[c->at].keys[i] != 0; ++i) key(c->steps[c->at].keys[i]);
  c->at++;
  c->wait = 2;
  return 0;
}

static void run(SvHost *host, const Step *steps, int count, Fake *fake, int open_with_f7) {
  Ctx c;
  SvSample smp;
  c.steps = steps; c.count = count; c.at = 0; c.wait = 1; c.fake = fake;
  sv_host_set_loop_hook(host, hook, &c);
  key(open_with_f7 ? SDL_SCANCODE_F7 : SV_KEY_ESCAPE);
  sv_host_service(host, &smp);
  sv_host_set_loop_hook(host, NULL, NULL);
}

int main(int argc, char **argv) {
  SvHostConfig cfg;
  SvHost *host;
  SvStateProvider provider;
  SvSample smp;
  Fake *fake;
  uint8_t *rgb;
  char settings[700];
  int win_w = 0, win_h = 0, i;
  if (argc < 2) { fprintf(stderr, "usage: %s <out-dir> [WxH]\n", argv[0]); return 2; }
  if (argc > 2 && sscanf(argv[2], "%dx%d", &win_w, &win_h) != 2) { fprintf(stderr, "bad size %s\n", argv[2]); return 2; }
  g_out = argv[1];
#if SDL_VERSION_ATLEAST(3, 4, 0)
  g_ext = "png";
#else
  g_ext = "bmp";
#endif
  SDL_CreateDirectory(g_out);
  snprintf(settings, sizeof settings, "%s/viewer-settings.cfg", g_out);
  remove(settings);
  if (win_w > 0 && win_h > 0) {
    SvSettings pre;
    sv_settings_defaults(&pre);
    pre.video.window_w = win_w;
    pre.video.window_h = win_h;
    sv_settings_save(&pre, settings);
  }
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings, 1);
  SDL_setenv_unsafe("SEGARECOMP_LAUNCHED", "1", 1);
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS;
  cfg.title = "state gallery";
  cfg.picture_w = 320; cfg.picture_h = 224; cfg.max_w = 320; cfg.max_h = 240; cfg.default_scale = 3;
  host = sv_host_open(&cfg);
  if (host == NULL) { fprintf(stderr, "cannot open the host\n"); return 1; }
  rgb = (uint8_t *)malloc(320u * 224u * 3u);
  if (!sv_gallery_scene(rgb, 320, 224)) memset(rgb, 16, 320u * 224u * 3u);
  memcpy(g_scene, rgb, sizeof g_scene);
  sv_host_present(host, rgb, 320, 224, 320 * 3);
  sv_host_toast(host, "");

  fake = (Fake *)calloc(1u, sizeof *fake);
  fake->available = 1; fake->rewind_ok = 1; fake->history = 119;
  fake->occupied[0] = fake->occupied[1] = fake->occupied[3] = 1;
  fake->damaged[5] = 1;
  for (i = 0; i < SV_STATE_SLOTS; ++i) make_thumb(fake->slot_thumb[i], i * 53, 20 + 10 * i);
  for (i = 0; i < MAX_HISTORY; ++i) make_thumb(fake->moment_thumb[i], (i * 7) % 320, 8 + (i % 6) * 18);
  memset(&provider, 0, sizeof provider);
  provider.ctx = fake; provider.states_available = f_available; provider.slot_info = f_slot_info; provider.save_slot = f_save; provider.load_slot = f_load;
  provider.rewind_available = f_rewind_available; provider.history_count = f_history_count; provider.history_info = f_history_info; provider.restore_entry = f_restore;
  provider.history_note = f_history_note;
  sv_host_set_state_provider(host, &provider);

  {  /* the pause menu and the slot lists (empty, occupied, unusable), the confirmations */
    static const Step steps[] = {
        {"11_pause_menu_states", {SV_KEY_DOWN, SV_KEY_RETURN, 0}},
        {"12_save_slots", {SV_KEY_DOWN, SV_KEY_RETURN, 0}},                      /* Slot 1 (occupied): the overwrite confirmation */
        {"13_overwrite_confirmation", {SV_KEY_ESCAPE, SV_KEY_ESCAPE, SV_KEY_DOWN, SV_KEY_RETURN, 0}},
        {"14_load_slots", {SV_KEY_RETURN, 0}},
        {"15_load_confirmation", {SV_KEY_ESCAPE, SV_KEY_ESCAPE, SV_KEY_ESCAPE, 0}},
    };
    run(host, steps, (int)(sizeof steps / sizeof steps[0]), fake, 0);
  }
  {  /* the rewind surface, a full minute of history: NOW, back one, the middle, the oldest, the newest moment */
    static const Step steps[] = {
        {"16_rewind_open", {SV_KEY_LEFT, 0}},
        {"17_rewind_back_1", {SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, 0}},
        {NULL, {SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEUP, 0}},
        {"18_rewind_middle", {SDL_SCANCODE_HOME, 0}},
        {"19_rewind_oldest", {SDL_SCANCODE_END, SV_KEY_LEFT, 0}},
        {"20_rewind_newest_moment", {SV_KEY_ESCAPE, 0}},
    };
    run(host, steps, (int)(sizeof steps / sizeof steps[0]), fake, 1);
  }
  {  /* a short history */
    static const Step steps[] = {{"21_rewind_short_history", {SV_KEY_LEFT, 0}}, {NULL, {SV_KEY_ESCAPE, 0}}};
    fake->history = 3;
    run(host, steps, 2, fake, 1);
  }
  {  /* reduced available history under memory pressure */
    static const Step steps[] = {{"22_rewind_memory_limited", {SV_KEY_LEFT, SV_KEY_LEFT, 0}}, {NULL, {SV_KEY_ESCAPE, 0}}};
    fake->history = 36; fake->limited = 1;
    run(host, steps, 2, fake, 1);
    fake->limited = 0;
  }
  {  /* a restore that fails stays in the surface and says so */
    static const Step steps[] = {{NULL, {SV_KEY_LEFT, SV_KEY_LEFT, SV_KEY_LEFT, SV_KEY_RETURN, 0}}, {"23_rewind_error", {SV_KEY_ESCAPE, 0}}, {NULL, {0}}};
    fake->history = 24; fake->fail_restore = 1;
    run(host, steps, 3, fake, 1);
    fake->fail_restore = 0;
  }
  {  /* rewind unavailable: the reason, in the toast and on the disabled menu row */
    fake->rewind_ok = 0;
    key(SDL_SCANCODE_F7);
    sv_host_service(host, &smp);
    save(sv_host_capture_output(host), "24_rewind_unavailable");
    fake->rewind_ok = 1;
  }
  {  /* no save location: every state row says why it is disabled */
    static const Step steps[] = {{"25_states_unavailable", {SV_KEY_ESCAPE, 0}}};
    fake->available = 0;
    sv_host_toast(host, "");
    run(host, steps, 1, fake, 0);
  }
  sv_host_close(host);
  free(fake);
  free(rgb);
  return failures ? 1 : 0;
}
