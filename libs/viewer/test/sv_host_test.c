/* SEG-058: the SDL3 host of the shared viewer under the dummy video driver and the software renderer: presenter output is read back and
 * compared pixel for pixel, the pause overlay is driven with synthetic events, and gamepads are virtual. No display, no audio device,
 * no ROM. Strict C11 like the code under test. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static void key_event(int scancode, int down, int mod) {
  SDL_Event e;
  SDL_zero(e);
  e.type = (Uint32)(down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP);
  e.common.timestamp = SDL_GetTicksNS();
  e.key.scancode = (SDL_Scancode)scancode;
  e.key.down = down != 0;
  e.key.mod = (SDL_Keymod)mod;
  SDL_PushEvent(&e);
}
static void tap(int scancode) { key_event(scancode, 1, 0); key_event(scancode, 0, 0); }
static void window_event(SDL_EventType type) { SDL_Event e; SDL_zero(e); e.type = (Uint32)type; e.common.timestamp = SDL_GetTicksNS(); SDL_PushEvent(&e); }

static void fill_pattern(uint8_t *rgb, int w, int h) {
  int x, y;
  for (y = 0; y < h; ++y)
    for (x = 0; x < w; ++x) {
      uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3u;
      p[0] = (uint8_t)(x * 5 + 7); p[1] = (uint8_t)(y * 9 + 3); p[2] = (uint8_t)((x ^ y) * 11 + 1);
    }
}
static void fill_flat(uint8_t *rgb, int w, int h, int r, int g, int b) {
  int i;
  for (i = 0; i < w * h; ++i) { rgb[i * 3] = (uint8_t)r; rgb[i * 3 + 1] = (uint8_t)g; rgb[i * 3 + 2] = (uint8_t)b; }
}

typedef struct Shot { SDL_Surface *s; } Shot;
static int px(SDL_Surface *s, int x, int y, int *r, int *g, int *b) {
  Uint8 rr = 0, gg = 0, bb = 0, aa = 0;
  if (!SDL_ReadSurfacePixel(s, x, y, &rr, &gg, &bb, &aa)) return 0;
  *r = rr; *g = gg; *b = bb;
  return 1;
}
static uint64_t surface_hash(SDL_Surface *s) {
  uint64_t h = 1469598103934665603ull;
  int x, y, r, g, b;
  for (y = 0; y < s->h; ++y)
    for (x = 0; x < s->w; ++x) { px(s, x, y, &r, &g, &b); h = (h ^ (uint64_t)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16))) * 1099511628211ull; }
  return h;
}

static SvHost *open_host(SvSystem system, const char *settings_file, int can_reset) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = system;
  cfg.title = "viewer host test";
  cfg.picture_w = 320; cfg.picture_h = 224; cfg.max_w = 320; cfg.max_h = 240;
  if (system == SV_SYSTEM_SMS) { cfg.picture_w = 256; cfg.picture_h = 192; cfg.max_w = 256; }
  cfg.default_scale = 3;
  cfg.can_reset = can_reset;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", settings_file, 1);
  return sv_host_open(&cfg);
}

static void set_window(SvHost *host, int w, int h) { (void)host; (void)w; (void)h; }

/* ---- Video page: the preset buttons are real controls, and the page lays out from pixels at every size ---- */
typedef struct PresetScript { int step; int rect[4]; int found; int chosen; } PresetScript;
static void click_at(float x, float y) {
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_EVENT_MOUSE_MOTION;
  e.common.timestamp = SDL_GetTicksNS();
  e.motion.x = x; e.motion.y = y;
  SDL_PushEvent(&e);
  SDL_zero(e);
  e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  e.common.timestamp = SDL_GetTicksNS();
  e.button.button = SDL_BUTTON_LEFT; e.button.down = true; e.button.clicks = 1; e.button.x = x; e.button.y = y;
  SDL_PushEvent(&e);
}
static int preset_hook(void *ctx, SvHost *host, int iteration) {
  PresetScript *p = (PresetScript *)ctx;
  (void)iteration;
  switch (p->step++) {
    case 0: tap(SV_KEY_DOWN); tap(SV_KEY_DOWN); tap(SV_KEY_RETURN); return 0;   /* Video & Display */
    case 1:
      p->found = sv_host_ui_preset_rect(host, 3, p->rect);
      if (p->found) click_at((float)(p->rect[0] + p->rect[2] / 2), (float)(p->rect[1] + p->rect[3] / 2));  /* "CRT Arcade" */
      return 0;
    case 2: p->chosen = (int)sv_host_settings(host)->video.preset; tap(SV_KEY_ESCAPE); tap(SV_KEY_ESCAPE); return 0;
    default: return 1;
  }
}

static void ui_scale_tests(void) {
  CHECK(sv_host_ui_scale(320, 240) == 1 && sv_host_ui_scale(768, 576) == 1 && sv_host_ui_scale(1280, 720) == 1, "small and 720p windows use 1x text (13 px)");
  CHECK(sv_host_ui_scale(1920, 1080) == 2 && sv_host_ui_scale(2560, 1440) == 3 && sv_host_ui_scale(3840, 2160) == 4, "the text grows with the display: 2x at 1080p, 3x at 1440p, 4x at 4K");
  CHECK(sv_host_ui_scale(600, 2000) == 1, "a narrow window never scales past what fits");
}

static void page_fit_tests(SvHost *host) {
  static const int sizes[][2] = {{720, 520}, {1080, 740}, {1440, 1040}, {2160, 1480}, {1920, 1080}, {2560, 1440}};
  int size, page, i;
  for (size = 0; size < (int)(sizeof sizes / sizeof sizes[0]); ++size)
    for (page = PAGE_MAIN; page <= PAGE_CONFIRM_CONTROLS; ++page) {
      sv_menu_reset(&host->menu);
      sv_menu_push(&host->menu, page, "REVIEW PAGE");
      sv_pages_rebuild(host);
      sv_pages_draw(host, sizes[size][0], sizes[size][1]);
      for (i = 0; i < host->ui_rows; ++i) {
        const int *r = host->ui_item_rect[i];
        CHECK(r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && r[0] + r[2] <= sizes[size][0] && r[1] + r[3] <= sizes[size][1], "responsive page hit rectangles stay inside the output at logical/HiDPI review sizes");
      }
    }
  sv_menu_reset(&host->menu);
}


/* ---- gamepad remap script: one step per drawn overlay frame ---- */
typedef struct PadScript { SDL_Joystick *joy; SvHost *host; int step; int activation_ignored; int north_bound; int conflict_moved; int a_stayed_south; } PadScript;
static void pad_button(PadScript *p, int button, int down) { SDL_SetJoystickVirtualButton(p->joy, button, down != 0); }
static int pad_hook(void *ctx, SvHost *host, int iteration) {
  PadScript *p = (PadScript *)ctx;
  const SvSettings *st = sv_host_settings(host);
  int i;
  (void)iteration;
  switch (p->step++) {
    case 0:
      for (i = 0; i < 3; ++i) tap(SV_KEY_DOWN);
      tap(SV_KEY_RETURN);                       /* Controls */
      for (i = 0; i < 6; ++i) tap(SV_KEY_DOWN); /* Player, (Device label skipped), Controller status button, Up, Down, Left, Right, A */
      tap(SV_KEY_RIGHT);                         /* the gamepad column of the A row */
      return 0;
    case 1: pad_button(p, SDL_GAMEPAD_BUTTON_SOUTH, 1); return 0;        /* activate "A pad" with the PAD itself */
    case 2:
      p->a_stayed_south = st->bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON1] == SV_PAD_SOUTH;
      p->activation_ignored = p->a_stayed_south;                           /* the activating press must not become the binding */
      pad_button(p, SDL_GAMEPAD_BUTTON_SOUTH, 0); return 0;
    case 3: pad_button(p, SDL_GAMEPAD_BUTTON_NORTH, 1); return 0;
    case 4:
      p->north_bound = st->bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON1] == SV_PAD_NORTH;
      pad_button(p, SDL_GAMEPAD_BUTTON_NORTH, 0); tap(SV_KEY_DOWN); tap(SV_KEY_RIGHT); return 0;   /* now on the B row, gamepad column */
    case 5: pad_button(p, SDL_GAMEPAD_BUTTON_SOUTH, 1); return 0;
    case 6: pad_button(p, SDL_GAMEPAD_BUTTON_SOUTH, 0); return 0;
    case 7: pad_button(p, SDL_GAMEPAD_BUTTON_WEST, 1); return 0;              /* WEST is Genesis C by default: a duplicate */
    case 8:
      p->conflict_moved = st->bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON2] == SV_PAD_WEST && st->bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON3] == -1;
      pad_button(p, SDL_GAMEPAD_BUTTON_WEST, 0); tap(SV_KEY_ESCAPE); tap(SV_KEY_ESCAPE); return 0;
    default: return 1;
  }
}

/* ---- scripted pause menu ---- */
typedef struct Script { int step; int quit_after; const int *keys; int n; SDL_Surface *first; int saw_open; int captured; } Script;
static int menu_hook(void *ctx, SvHost *host, int iteration) {
  Script *s = (Script *)ctx;
  (void)iteration;
  if (sv_host_menu_open(host)) s->saw_open = 1;
  if (s->step == 0) { s->first = sv_host_capture_output(host); s->captured = 1; }
  if (s->step < s->n) { tap(s->keys[s->step]); s->step++; return 0; }
  return s->quit_after ? 0 : 1;
}


/* ---- Widescreen Mode (SEG-117): requested vs capability vs effective, the Video page, keyboard and gamepad, persistence, no gameplay overlay ---- */
typedef struct WsCaps { SvWidescreenCaps caps; int calls; } WsCaps;
static void ws_caps_fn(void *ctx, SvWidescreenCaps *out) { WsCaps *c = (WsCaps *)ctx; c->calls++; *out = c->caps; }

typedef struct WsScript {
  int step;
  SDL_Joystick *joy;       /* NULL: keyboard script */
  int saw_row, label_ok, hint_original_ok, hint_l2_ok, hint_l3_ok, note_ok, quick_row;
  char l3_hint[320];
} WsScript;
static SvItem *ws_item(SvHost *host) {
  SvPage *p = sv_menu_top(&host->menu);
  int i;
  if (p == NULL) return NULL;
  for (i = 0; i < p->count; ++i) if (p->items[i].id == I_WIDESCREEN) return &p->items[i];
  return NULL;
}
static int ws_has_note(SvHost *host, const char *text) {
  SvPage *p = sv_menu_top(&host->menu);
  int i;
  if (p == NULL) return 0;
  for (i = 0; i < p->count; ++i) if (p->items[i].kind == SV_ITEM_LABEL && strcmp(p->items[i].icon, "note") == 0 && strcmp(p->items[i].hint, text) == 0) return 1;
  return 0;
}
static void ws_move(WsScript *w, int key, int pad_button_id) {
  if (w->joy == NULL) { tap(key); return; }
  SDL_SetJoystickVirtualButton(w->joy, pad_button_id, true);
}
/* Opens Video & Display (Main: Resume, Quick settings, Video & Display) -> Fullscreen, Scaling, Aspect, then Widescreen -> Right, Right. */
static int ws_hook(void *ctx, SvHost *host, int iteration) {
  WsScript *w = (WsScript *)ctx;
  SvItem *it;
  (void)iteration;
  if (w->joy != NULL) { /* a gamepad press is held for one drawn frame and released on the next */
    int b;
    for (b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) SDL_SetJoystickVirtualButton(w->joy, b, false);
    SDL_UpdateJoysticks();
    if (w->step % 2 == 1) { ++w->step; return 0; }
  }
  {
    const int s = w->joy != NULL ? w->step / 2 : w->step;
    switch (s) {
      case 0: ws_move(w, SV_KEY_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN); break;
      case 1: ws_move(w, SV_KEY_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN); break;
      case 2: ws_move(w, SV_KEY_RETURN, SDL_GAMEPAD_BUTTON_SOUTH); break; /* Video & Display */
      case 3: case 4: case 5: ws_move(w, SV_KEY_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN); break; /* -> Widescreen Mode */
      case 6:
        it = ws_item(host);
        w->saw_row = it != NULL;
        w->label_ok = it != NULL && strcmp(it->label, "Widescreen Mode") == 0 && strcmp(it->text, "Original") == 0;
        w->hint_original_ok = it != NULL && strcmp(it->hint, "Original game picture. Preserves the intended display area and compatibility.") == 0;
        ws_move(w, SV_KEY_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        break;
      case 7:
        it = ws_item(host);
        w->hint_l2_ok = it != NULL && strncmp(it->text, "Extended Backgrounds (Level 2)", 30) == 0 &&
                        strstr(it->hint, "Shows more background scenery at the sides.") != NULL && strstr(it->hint, "Not yet available for this game. Showing the original picture.") != NULL;
        w->note_ok = ws_has_note(host, "Not yet available for this game. Showing the original picture.");
        ws_move(w, SV_KEY_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        break;
      case 8:
        it = ws_item(host);
        w->hint_l3_ok = it != NULL && strncmp(it->text, "Enhanced Widescreen (Level 3)", 29) == 0 && strstr(it->hint, "Expands the playable view where supported.") != NULL;
        if (it != NULL) snprintf(w->l3_hint, sizeof w->l3_hint, "%s", it->hint);
        ws_move(w, SV_KEY_ESCAPE, SDL_GAMEPAD_BUTTON_EAST);
        break;
      case 9: ws_move(w, SV_KEY_ESCAPE, SDL_GAMEPAD_BUTTON_EAST); break;
      default: return 1;
    }
  }
  if (w->joy != NULL) SDL_UpdateJoysticks();
  ++w->step;
  return 0;
}

/* A fake guest: counts frames, folds the sampled pad masks into a digest and presents a deterministic picture. `change_at` (>= 0) selects Level 3 through
 * the real pause menu at that frame; the result must equal an uninterrupted run (the menu blocks sv_host_service, so the guest does not advance meanwhile; opening it releases held keys by design, so the held key ends before). */
typedef struct FakeGuest { uint64_t frames, pad_hash, presented; } FakeGuest;
static int pick_l3_hook(void *ctx, SvHost *host, int iteration) {
  int *step = (int *)ctx;
  static const int keys[] = {SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_RETURN, SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_RIGHT, SV_KEY_RIGHT, SV_KEY_ESCAPE, SV_KEY_ESCAPE};
  (void)host; (void)iteration;
  if (*step < (int)(sizeof keys / sizeof keys[0])) { tap(keys[(*step)++]); return 0; }
  return 1;
}
static FakeGuest run_fake_guest(SvHost *host, uint8_t *frame, int change_at, uint64_t *out_hash) {
  FakeGuest g = {0, 1469598103934665603ull, 0};
  int i;
  for (i = 0; i < 30; ++i) {
    SvSample smp;
    if (i == change_at) { int step = 0; tap(SV_KEY_ESCAPE); sv_host_set_loop_hook(host, pick_l3_hook, &step); }
    sv_host_service(host, &smp);
    if (i == change_at) sv_host_set_loop_hook(host, NULL, NULL);
    if (i == 5) key_event(SV_KEY_RIGHT, 1, 0);
    if (i == 12) key_event(SV_KEY_RIGHT, 0, 0);
    g.pad_hash = (g.pad_hash ^ smp.pad[0]) * 1099511628211ull;
    g.pad_hash = (g.pad_hash ^ smp.pad[1]) * 1099511628211ull;
    g.frames++;
    fill_pattern(frame, 320, 224);
    frame[0] = (uint8_t)i;
    if (sv_host_present(host, frame, 320, 224, 320 * 3) == 0) g.presented++;
  }
  { SDL_Surface *o = sv_host_capture_output(host); *out_hash = o != NULL ? surface_hash(o) : 0; if (o != NULL) SDL_DestroySurface(o); }
  return g;
}

static void widescreen_live_tests(const char *dir, uint8_t *frame) {
  char file[600];
  SvHost *host;
  FakeGuest control, live;
  uint64_t hc = 0, hl = 0;
  snprintf(file, sizeof file, "%s/ws-live.cfg", dir);
  remove(file);
  host = open_host(SV_SYSTEM_GENESIS, file, 0);
  if (host == NULL) { CHECK(0, "widescreen live: host opens"); return; }
  control = run_fake_guest(host, frame, -1, &hc);
  sv_host_close(host);
  remove(file);
  host = open_host(SV_SYSTEM_GENESIS, file, 0);
  if (host == NULL) { CHECK(0, "widescreen live: host reopens"); return; }
  live = run_fake_guest(host, frame, 14, &hl);
  CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "widescreen live: the mid-run menu change took effect");
  CHECK(live.frames == control.frames && live.presented == control.presented, "widescreen live: guest frame count and presented frames are unchanged by the switch");
  CHECK(live.pad_hash == control.pad_hash, "widescreen live: the sampled input stream (the recording source) is unchanged by the switch");
  CHECK(hl == hc, "widescreen live: the next presented frame is the Original picture while no extension exists");
  sv_host_close(host);
  remove(file);
}

static void widescreen_tests(const char *dir, uint8_t *frame) {
  char file[600];
  SvHost *host;
  SvSettings st;
  SvSample smp;
  WsScript w;
  int r, g, b;
  snprintf(file, sizeof file, "%s/ws-settings.cfg", dir);
  remove(file);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", file, 1);

  /* 1. no capability provider (every platform in this milestone): Level 2 and Level 3 are selectable, persisted, and the effective mode stays Original */
  host = open_host(SV_SYSTEM_GENESIS, file, 0);
  CHECK(host != NULL, "widescreen: host opens");
  if (host == NULL) return;
  CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ORIGINAL && sv_host_widescreen_effective(host) == SV_WIDESCREEN_ORIGINAL, "widescreen: default is Original (requested and effective)");
  { SvWidescreenCaps caps; sv_host_widescreen_caps(host, &caps);
    CHECK(caps.extended == SV_WS_UNAVAILABLE && caps.enhanced == SV_WS_UNAVAILABLE, "widescreen: no provider means both enhanced levels are Unavailable"); }
  fill_pattern(frame, 320, 224);
  CHECK(sv_host_present(host, frame, 320, 224, 320 * 3) == 0, "widescreen: present");
  { SDL_Surface *base = sv_host_capture_output(host);
    uint64_t h0 = base != NULL ? surface_hash(base) : 0;
    memset(&w, 0, sizeof w);
    sv_host_set_loop_hook(host, ws_hook, &w);
    tap(SV_KEY_ESCAPE);
    sv_host_service(host, &smp);
    sv_host_set_loop_hook(host, NULL, NULL);
    CHECK(w.saw_row && w.label_ok, "widescreen: the Video page offers 'Widescreen Mode' (Original selected by default)");
    CHECK(w.hint_original_ok, "widescreen: the Original description is shown");
    CHECK(w.hint_l2_ok, "widescreen: Level 2 description and the honest Unavailable message are shown");
    CHECK(w.note_ok, "widescreen: the status note row carries the Unavailable message on the Video page");
    CHECK(w.hint_l3_ok, "widescreen: Level 3 description is shown");
    CHECK(strstr(w.l3_hint, "Not yet available for this game. Showing the original picture.") != NULL, "widescreen: Level 3 reports Unavailable with the original picture");
    CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "widescreen: keyboard Right twice selects Level 3 immediately");
    CHECK(sv_host_widescreen_effective(host) == SV_WIDESCREEN_ORIGINAL, "widescreen: requested Level 3 stays Original effectively (preference preserved)");
    CHECK(sv_host_widescreen_fallbacks(host) == 2u, "widescreen: each unsupported selection is counted (bounded counter)");
    if (smp.menu_was_open) { /* the gameplay frame carries no widescreen text or control: pixel-identical to the Original-mode frame */
      SDL_Surface *after = sv_host_capture_output(host);
      CHECK(after != NULL && base != NULL && surface_hash(after) == h0, "widescreen: with the menu closed the output equals the Original output (no overlay)");
      if (after != NULL) SDL_DestroySurface(after);
    }
    if (base != NULL) SDL_DestroySurface(base);
  }
  sv_host_close(host); /* saves */
  { char buf[16384]; FILE *f = fopen(file, "rb"); size_t n = f != NULL ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f != NULL) fclose(f);
    buf[n] = '\0';
    CHECK(strstr(buf, "video.widescreen=enhanced_gameplay") != NULL, "widescreen: the selection is persisted by the in-game menu"); }
  host = open_host(SV_SYSTEM_SMS, file, 1);
  CHECK(host != NULL && sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY && sv_host_widescreen_effective(host) == SV_WIDESCREEN_ORIGINAL,
        "widescreen: the same saved value is read by the Master System host, effective Original");
  if (host != NULL) {
    st = *sv_host_settings(host);
    st.video.widescreen = SV_WIDESCREEN_EXTENDED_BACKGROUNDS;
    sv_host_apply_settings(host, &st); /* live change through the host */
    CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_EXTENDED_BACKGROUNDS && sv_host_widescreen_effective(host) == SV_WIDESCREEN_ORIGINAL, "widescreen: live change keeps Original effective");
    sv_host_close(host);
  }

  /* 2. a provider: fallback Level 3 -> Level 2 -> Original, status text, requested preserved */
  { WsCaps wc;
    SvHostConfig cfg;
    memset(&wc, 0, sizeof wc);
    memset(&cfg, 0, sizeof cfg);
    cfg.system = SV_SYSTEM_GENESIS; cfg.title = "widescreen caps"; cfg.picture_w = 320; cfg.picture_h = 224; cfg.max_w = 320; cfg.max_h = 240; cfg.default_scale = 3;
    cfg.widescreen_caps = ws_caps_fn; cfg.widescreen_ctx = &wc;
    wc.caps.extended = SV_WS_AVAILABLE; wc.caps.enhanced = SV_WS_UNAVAILABLE;
    host = sv_host_open(&cfg);
    CHECK(host != NULL, "widescreen: host with a provider opens");
    if (host != NULL) {
      st = *sv_host_settings(host);
      st.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY;
      sv_host_apply_settings(host, &st);
      CHECK(sv_host_widescreen_effective(host) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS && wc.calls > 0, "widescreen: Level 3 requested, only Level 2 available -> effective Level 2");
      wc.caps.extended = SV_WS_PARTIAL; wc.caps.enhanced = SV_WS_PARTIAL;
      CHECK(sv_host_widescreen_effective(host) == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "widescreen: Partial Level 3 is effective Level 3");
      wc.caps.extended = SV_WS_UNAVAILABLE; wc.caps.enhanced = SV_WS_UNAVAILABLE;
      CHECK(sv_host_widescreen_effective(host) == SV_WIDESCREEN_ORIGINAL && sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "widescreen: capability loss falls back to Original and keeps the preference");
      sv_host_close(host);
    }
  }

  /* 3. gamepad: the same selector is reachable and cycles with a virtual pad (D-pad, South to enter, East to go back) */
  remove(file);
  host = open_host(SV_SYSTEM_GENESIS, file, 0);
  if (host != NULL) {
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id;
    SDL_Joystick *joy;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes = SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT; desc.name = "Fictional Widescreen Pad";
    id = SDL_AttachVirtualJoystick(&desc);
    joy = id ? SDL_OpenJoystick(id) : NULL;
    CHECK(joy != NULL, "widescreen: virtual gamepad opens");
    if (joy != NULL) {
      sv_host_service(host, &smp);
      memset(&w, 0, sizeof w);
      w.joy = joy;
      sv_host_set_loop_hook(host, ws_hook, &w);
      tap(SV_KEY_ESCAPE);
      sv_host_service(host, &smp);
      sv_host_set_loop_hook(host, NULL, NULL);
      CHECK(w.saw_row && w.hint_l2_ok && w.hint_l3_ok && sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "widescreen: all three modes are reachable and selectable with a gamepad");
      SDL_CloseJoystick(joy);
    }
    if (id) SDL_DetachVirtualJoystick(id);
    sv_host_close(host);
  }

  /* 4. restore video defaults restores Original; existing aspect/scale untouched by the new field */
  host = open_host(SV_SYSTEM_GENESIS, file, 0);
  if (host != NULL) {
    st = *sv_host_settings(host);
    st.video.widescreen = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; st.video.aspect = SV_ASPECT_4_3; st.video.scale = SV_SCALE_INTEGER;
    sv_host_apply_settings(host, &st);
    CHECK(sv_host_settings(host)->video.aspect == SV_ASPECT_4_3 && sv_host_settings(host)->video.scale == SV_SCALE_INTEGER, "widescreen: aspect and scaling still apply independently");
    sv_video_defaults(&st.video);
    sv_host_apply_settings(host, &st);
    CHECK(sv_host_settings(host)->video.widescreen == SV_WIDESCREEN_ORIGINAL, "widescreen: video defaults restore Original");
    sv_host_close(host);
  }
  (void)r; (void)g; (void)b;
  remove(file);
}

int main(void) {
  char dir[512], settings_file[600], shots[600];
  uint8_t *frame = (uint8_t *)malloc(320u * 240u * 3u);
  SvHost *host;
  SvSettings s;
  SDL_Surface *a;
  int r, g, bl, x, y, ok, k = 1, dest_w_cache = 0;
  SvRect dest = {0, 0, 0, 0};
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  (void)set_window; (void)fill_flat;
  snprintf(dir, sizeof dir, "%s/sv_host_test_%d", tmp, (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  snprintf(settings_file, sizeof settings_file, "%s/viewer-settings.cfg", dir);
  remove(settings_file); /* a previous run may have left a remapped file: every run starts from the defaults */
  snprintf(shots, sizeof shots, "%s/shots", dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_SCREENSHOT_DIR", shots, 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);

  host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
  CHECK(host != NULL, "the host opens under the dummy driver");
  if (host == NULL) return 1;
  CHECK(strcmp(sv_host_renderer_name(host), "software") == 0, "the software renderer is the fallback in use");

  /* ---- Pixel Perfect: byte-exact nearest-neighbour output at an integer scale ---- */
  fill_pattern(frame, 320, 224);
  s = *sv_host_settings(host);
  s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(host, &s);
  CHECK(sv_host_present(host, frame, 320, 224, 320 * 3) == 0, "present a 320x224 frame");
  CHECK(host->toast_count >= 1 && strstr(host->toasts[host->toast_count - 1].text, "Pause menu: Esc") != NULL && strstr(host->toasts[host->toast_count - 1].text, "Back") != NULL &&
            host->toasts[host->toast_count - 1].until > SDL_GetTicks(),
        "the first picture shows a hint naming the keys that open the pause menu");
  /* notifications stack: a second one never hides the first, the same text refreshes in place, a full stack drops only the oldest */
  sv_host_toast(host, "");
  sv_host_toast(host, "Controller connected: Pad");
  sv_host_toast(host, "Second: toast");
  CHECK(host->toast_count == 2 && strstr(host->toasts[0].text, "Controller connected") != NULL && strstr(host->toasts[1].text, "Second") != NULL, "two toasts coexist");
  sv_host_toast(host, "Second: toast");
  CHECK(host->toast_count == 2, "the same text refreshes instead of duplicating");
  sv_host_toast(host, "Third");
  sv_host_toast(host, "Fourth");
  CHECK(host->toast_count == 3 && strcmp(host->toasts[0].text, "Second: toast") == 0 && strcmp(host->toasts[2].text, "Fourth") == 0, "a full stack drops only the oldest");
  sv_host_toast(host, ""); /* the byte-exact check below is about the picture, not the overlay */
  a = sv_host_capture_output(host);
  CHECK(a != NULL && a->w >= 640 && a->h >= 448, "the window is at least 2x the picture");
  ok = a != NULL;
  if (a != NULL) dest = sv_layout_dest(a->w, a->h, 320, 224, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  k = dest.h / 224;
  dest_w_cache = a != NULL ? a->w : 0;
  CHECK(k >= 2 && dest.w == 320 * k && dest.h == 224 * k, "integer scaling picks a whole-number multiple");
  for (y = 0; ok && y < 224; ++y)
    for (x = 0; ok && x < 320; ++x) {
      const uint8_t *p = frame + ((size_t)y * 320u + (size_t)x) * 3u;
      int sx, sy;
      for (sy = 0; ok && sy < k; ++sy)
        for (sx = 0; ok && sx < k; ++sx) { px(a, dest.x + x * k + sx, dest.y + y * k + sy, &r, &g, &bl); if (r != p[0] || g != p[1] || bl != p[2]) ok = 0; }
    }
  CHECK(ok, "Pixel Perfect output equals the source, replicated exactly (no filtering, no shader)");
  { const uint64_t baseline = surface_hash(a);
    /* ---- the four presets are four different pictures ---- */
    uint64_t hashes[4];
    int i, j;
    hashes[0] = baseline;
    s = *sv_host_settings(host);
    for (i = 1; i < 4; ++i) {
      SDL_Surface *c;
      sv_apply_crt_preset(&s.video, (SvCrtPreset)i);
      sv_host_apply_settings(host, &s);
      c = sv_host_capture_output(host);
      hashes[i] = c != NULL ? surface_hash(c) : 0;
      if (c != NULL) SDL_DestroySurface(c);
    }
    for (i = 0; i < 4; ++i) for (j = i + 1; j < 4; ++j) CHECK(hashes[i] != hashes[j], "every CRT preset renders a different picture from every other");
  }
  SDL_DestroySurface(a);
  sv_video_defaults(&s.video);
  s.video.scale = SV_SCALE_INTEGER;
  sv_host_apply_settings(host, &s);

  /* ---- individual passes on a flat bright frame ---- */
  fill_flat(frame, 320, 224, 200, 200, 200);
  sv_host_present(host, frame, 320, 224, 320 * 3);
  s = *sv_host_settings(host);
  s.video.preset = SV_CRT_CUSTOM; s.video.scanlines = 80;
  sv_host_apply_settings(host, &s);
  a = sv_host_capture_output(host);
  { long dark = 0, light = 0; int n = 0;
    for (y = dest.y + 3 * k; y < dest.y + dest.h - 3 * k - 3; y += k) { if (((y - dest.y) % k) != 0) continue; px(a, dest.x + 40, y, &r, &g, &bl); light += r; px(a, dest.x + 40, y + k / 2, &r, &g, &bl); dark += r; ++n; }
    CHECK(n > 0 && dark / n < light / n - 20, "scanlines darken the gap between source lines and keep the centre bright"); }
  SDL_DestroySurface(a);
  s.video.scanlines = 0; s.video.mask = 70;
  sv_host_apply_settings(host, &s);
  a = sv_host_capture_output(host);
  { int r1, g1, b1, r2, g2, b2;
    px(a, dest.x + 100, dest.y + dest.h / 2 + 1, &r1, &g1, &b1); px(a, dest.x + 101, dest.y + dest.h / 2 + 1, &r2, &g2, &b2);
    CHECK(r1 != g1 || g1 != b1 || r2 != g2, "the aperture mask tints alternating columns"); }
  SDL_DestroySurface(a);
  s.video.mask = 0; s.video.curvature = 100;
  sv_host_apply_settings(host, &s);
  a = sv_host_capture_output(host);
  px(a, dest.x + 2, dest.y + 2, &r, &g, &bl);
  if (SDL_getenv("SV_TEST_DUMP")) SDL_SaveBMP(a, SDL_getenv("SV_TEST_DUMP"));
  CHECK(r < 20, "curvature pulls the picture off the corners of its rectangle");
  px(a, dest.x + dest.w / 2, dest.y + dest.h / 2, &r, &g, &bl);
  CHECK(r > 100, "the centre of a curved picture stays bright");
  SDL_DestroySurface(a);
  s.video.curvature = 0; s.video.brightness = 130;
  sv_host_apply_settings(host, &s);
  fill_flat(frame, 320, 224, 100, 100, 100);
  sv_host_present(host, frame, 320, 224, 320 * 3);
  a = sv_host_capture_output(host);
  px(a, dest.x + dest.w / 2, dest.y + dest.h / 2, &r, &g, &bl);
  CHECK(r >= 125 && r <= 135, "brightness 130% lifts a 100 grey to about 130");
  SDL_DestroySurface(a);
  s.video.brightness = 100; s.video.bloom = 100;
  sv_host_apply_settings(host, &s);
  { int xx;
    for (y = 0; y < 224; ++y) for (xx = 0; xx < 320; ++xx) { uint8_t *p = frame + ((size_t)y * 320u + (size_t)xx) * 3u; p[0] = p[1] = p[2] = xx < 160 ? 255 : 0; }
    sv_host_present(host, frame, 320, 224, 320 * 3);
    a = sv_host_capture_output(host);
    px(a, dest.x + dest.w / 2 + 2, dest.y + dest.h / 2, &r, &g, &bl); /* just inside the dark half */
    CHECK(r > 0, "bloom bleeds light across a bright edge");
    SDL_DestroySurface(a); }
  sv_video_defaults(&s.video);
  sv_host_apply_settings(host, &s);

  /* ---- input: held state, focus loss, host hotkeys ---- */
  { SvSample smp;
    sv_host_service(host, &smp);
    CHECK(smp.pad[0] == 0 && smp.pad[1] == 0 && !smp.quit, "nothing held at rest");
    key_event(SV_KEY_Z, 1, 0); key_event(SV_KEY_RETURN, 1, 0); key_event(SV_KEY_UP, 1, 0);
    sv_host_service(host, &smp);
    CHECK((smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1)) && (smp.pad[0] & SV_ACTION_BIT(SV_ACT_START)) && (smp.pad[0] & SV_ACTION_BIT(SV_ACT_UP)), "Genesis Z=A, Return=Start, Up map to the Player 1 mask");
    key_event(SV_KEY_I, 1, 0);
    sv_host_service(host, &smp);
    CHECK(smp.pad[1] & SV_ACTION_BIT(SV_ACT_UP), "I is Player 2 Up (Genesis port 2 is real)");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    sv_host_service(host, &smp);
    CHECK(smp.pad[0] == 0 && smp.pad[1] == 0, "focus loss releases every held input at the next sampling point");
    key_event(SV_KEY_Z, 1, 0);
    sv_host_service(host, &smp);
    key_event(SV_KEY_Z, 0, 0);
    sv_host_service(host, &smp);
    CHECK(smp.pad[0] == 0, "release after press");
    /* Alt+Enter is the fullscreen hotkey: it must not press Start */
    key_event(SV_KEY_RETURN, 1, SDL_KMOD_LALT);
    sv_host_service(host, &smp);
    CHECK(!(smp.pad[0] & SV_ACTION_BIT(SV_ACT_START)) && sv_host_settings(host)->video.fullscreen == 1, "Alt+Enter toggles fullscreen and does not press Start");
    key_event(SV_KEY_RETURN, 1, SDL_KMOD_LALT);
    sv_host_service(host, &smp);
    CHECK(sv_host_settings(host)->video.fullscreen == 0, "Alt+Enter toggles back");
  }

  /* ---- pause overlay: scripted navigation (the guest does not advance: service() does not return until the menu closes) ---- */
  { static const int to_video_and_back[] = {SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_RETURN /* Video & Display */, SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_DOWN, SV_KEY_DOWN,
                                            SV_KEY_RIGHT /* Video Effect: after Fullscreen, Scaling, Aspect, Widescreen Mode, VSync, FPS (the "CRT effect" heading is not focusable) */,
                                            SV_KEY_ESCAPE /* back to main */, SV_KEY_ESCAPE /* resume */};
    Script sc;
    SvSample smp;
    memset(&sc, 0, sizeof sc);
    sc.keys = to_video_and_back; sc.n = (int)(sizeof to_video_and_back / sizeof to_video_and_back[0]);
    sv_host_set_loop_hook(host, menu_hook, &sc);
    tap(SV_KEY_ESCAPE);
    sv_host_service(host, &smp);
    sv_host_set_loop_hook(host, NULL, NULL);
    CHECK(smp.menu_was_open && sc.saw_open && !sv_host_menu_open(host), "Esc opens the pause overlay, blocks, and closes it again");
    CHECK(sc.first != NULL && sc.first->w == dest_w_cache, "the overlay renders into the window");
    if (sc.first != NULL) {
      px(sc.first, 4, 4, &r, &g, &bl);
      CHECK(r < 160, "the picture is dimmed behind the main menu");
      SDL_DestroySurface(sc.first);
    }
    CHECK(sv_host_settings(host)->video.preset == SV_CRT_SOFT, "a Left/Right change in Video & Display is applied immediately");
    { SvSettings back = *sv_host_settings(host); sv_video_defaults(&back.video); sv_host_apply_settings(host, &back); }
    /* held keys do not leak across the overlay */
    key_event(SV_KEY_Z, 1, 0);
    tap(SV_KEY_ESCAPE);
    { static const int resume[] = {SV_KEY_RETURN /* Resume */};
      Script s2; memset(&s2, 0, sizeof s2); s2.keys = resume; s2.n = 1;
      sv_host_set_loop_hook(host, menu_hook, &s2);
      sv_host_service(host, &smp);
      sv_host_set_loop_hook(host, NULL, NULL);
      if (s2.first != NULL) SDL_DestroySurface(s2.first); }
    sv_host_service(host, &smp);
    CHECK(smp.pad[0] == 0, "a key held when the overlay opened reads released after it closes (deterministic)");
    key_event(SV_KEY_Z, 0, 0);
  }
  /* quit goes through a confirmation */
  { static const int quit_flow[] = {SV_KEY_UP /* wraps to the last item */, SV_KEY_RETURN /* Quit... */, SV_KEY_DOWN /* confirm */, SV_KEY_RETURN};
    Script sc; SvSample smp;
    memset(&sc, 0, sizeof sc); sc.keys = quit_flow; sc.n = 4;
    sv_host_set_loop_hook(host, menu_hook, &sc);
    tap(SV_KEY_ESCAPE);
    sv_host_service(host, &smp);
    sv_host_set_loop_hook(host, NULL, NULL);
    if (sc.first != NULL) SDL_DestroySurface(sc.first);
    CHECK(smp.quit, "Quit requires the confirmation page and then quits");
  }
  sv_host_close(host);

  /* ---- persistence across a restart, hostile settings file ---- */
  host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
  CHECK(host != NULL, "reopen");
  if (host != NULL) {
    s = *sv_host_settings(host);
    s.video.scale = SV_SCALE_INTEGER; s.video.aspect = SV_ASPECT_4_3; s.audio.volume = 40;
    sv_host_apply_settings(host, &s);
    sv_host_close(host);
    host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
    CHECK(host != NULL && sv_host_settings(host)->video.scale == SV_SCALE_INTEGER && sv_host_settings(host)->video.aspect == SV_ASPECT_4_3 && sv_host_settings(host)->audio.volume == 40, "settings persist across a restart");
    sv_host_close(host);
  }
  { FILE *f = fopen(settings_file, "wb"); if (f != NULL) { fputs("video.scanlines=oops\nbind.genesis.p1.key.up=-77\n\xff\xfe\n", f); fclose(f); } }
  host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
  CHECK(host != NULL && sv_host_settings(host)->video.scanlines == 0, "a damaged settings file opens with defaults");
  sv_host_close(host);
  remove(settings_file);

  /* ---- Master System: Pause tap latch, Player 2, active height 192 <-> 224, reset gating, screenshot ---- */
  host = open_host(SV_SYSTEM_SMS, settings_file, 1);
  CHECK(host != NULL, "SMS host opens");
  if (host != NULL) {
    SvSample smp;
    uint8_t *sms = (uint8_t *)malloc(256u * 224u * 3u);
    { SDL_VirtualJoystickDesc desc;
      SDL_JoystickID id;
      SDL_Joystick *joy;
      SDL_INIT_INTERFACE(&desc);
      desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
      desc.naxes = SDL_GAMEPAD_AXIS_COUNT; desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
      desc.name = "Fictional SMS Pause Pad";
      id = SDL_AttachVirtualJoystick(&desc);
      joy = id ? SDL_OpenJoystick(id) : NULL;
      CHECK(joy != NULL, "virtual SMS gamepad opens");
      sv_host_service(host, &smp);
      if (joy != NULL) {
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, true); SDL_UpdateJoysticks();
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, false); SDL_UpdateJoysticks();
        sv_host_service(host, &smp);
        CHECK(!smp.menu_was_open && (smp.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE)), "SMS Start/Plus short tap is latched guest Pause, never host menu");
        sv_host_service(host, &smp);
        CHECK(!(smp.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE)), "SMS virtual Pause tap releases after one sample");
        SDL_CloseJoystick(joy);
      }
      if (id) SDL_DetachVirtualJoystick(id);
      sv_host_service(host, &smp);
      sv_host_toast(host, ""); /* presentation geometry below measures game pixels, not the hotplug notification */
    }
    fill_flat(sms, 256, 224, 255, 255, 255);
    tap(SV_KEY_P);   /* a tap shorter than a sample */
    sv_host_service(host, &smp);
    CHECK(smp.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE), "a short tap of the console Pause button is latched for one sample");
    sv_host_service(host, &smp);
    CHECK(!(smp.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE)), "and it is released by the next");
    key_event(SV_KEY_P, 1, 0);
    sv_host_service(host, &smp); sv_host_service(host, &smp);
    CHECK(smp.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE), "a held Pause key stays pressed");
    key_event(SV_KEY_P, 0, 0);
    sv_host_service(host, &smp);
    tap(SV_KEY_ESCAPE);   /* the host menu is NOT the console Pause button */
    { static const int resume[] = {SV_KEY_RETURN}; Script sc; memset(&sc, 0, sizeof sc); sc.keys = resume; sc.n = 1;
      sv_host_set_loop_hook(host, menu_hook, &sc); sv_host_service(host, &smp); sv_host_set_loop_hook(host, NULL, NULL);
      if (sc.first != NULL) SDL_DestroySurface(sc.first);
      CHECK(smp.menu_was_open && !(smp.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE)), "opening the host menu never presses the console Pause button"); }
    key_event(SV_KEY_J, 1, 0); key_event(SV_KEY_U, 1, 0);
    sv_host_service(host, &smp);
    CHECK((smp.pad[1] & SV_ACTION_BIT(SV_ACT_LEFT)) && (smp.pad[1] & SV_ACTION_BIT(SV_ACT_BUTTON1)) && !(smp.pad[0] & SV_ACTION_BIT(SV_ACT_LEFT)), "SMS Player 2 keys reach the second port");
    key_event(SV_KEY_J, 0, 0); key_event(SV_KEY_U, 0, 0);
    sv_host_service(host, &smp);
    /* geometry of the two active heights (measured on the picture: the start-up hint toast is cleared first) */
    sv_host_present(host, sms, 256, 192, 256 * 3); /* this host's first picture: it raises the hint */
    sv_host_toast(host, "");
    a = sv_host_capture_output(host);
    { int minx = 9999, maxx = -1, miny = 9999, maxy = -1;
      for (y = 0; y < a->h; ++y) for (x = 0; x < a->w; ++x) { px(a, x, y, &r, &g, &bl); if (r > 128) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; } }
      CHECK(a->w == 768 && a->h == 576 && (maxx - minx + 1) == 768 && (maxy - miny + 1) == 576, "192-line picture fills a 768x576 window"); }
    SDL_DestroySurface(a);
    sv_host_present(host, sms, 256, 224, 256 * 3);
    a = sv_host_capture_output(host);
    { int minx = 9999, maxx = -1, miny = 9999, maxy = -1;
      for (y = 0; y < a->h; ++y) for (x = 0; x < a->w; ++x) { px(a, x, y, &r, &g, &bl); if (r > 128) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; } }
      CHECK(((maxx - minx + 1) * 224 - (maxy - miny + 1) * 256) <= 224 && ((maxy - miny + 1) * 256 - (maxx - minx + 1) * 224) <= 256, "224-line picture keeps 256:224 (letterboxed, not stretched)"); }
    SDL_DestroySurface(a);
    /* screenshot of the game pixels only */
    { char path[800];
      CHECK(sv_host_screenshot(host, path, sizeof path) == 0, "screenshot is written");
      { FILE *f = fopen(path, "rb"); unsigned char sig[8] = {0}; size_t n = f ? fread(sig, 1, 8, f) : 0; if (f) fclose(f);
        CHECK(n == 8 && (memcmp(sig, "\x89PNG", 4) == 0 || memcmp(sig, "BM", 2) == 0), "the screenshot is an image file"); }
#if SDL_VERSION_ATLEAST(3, 4, 0)
      { SDL_Surface *shot = SDL_LoadSurface(path);
        CHECK(shot != NULL && shot->w == 256 && shot->h == 224, "the screenshot has the game's own pixel size (no menu, no CRT)");
        if (shot != NULL) SDL_DestroySurface(shot); }
#endif
    }
    /* the stop screen never blocks a headless run */
    { const Uint64 t0 = SDL_GetTicks();
      sv_host_stop_screen(host, "The game stopped", "typed guest error");
      CHECK(SDL_GetTicks() - t0 < 500, "the stop screen is skipped on a headless video driver"); }
    free(sms);
    sv_host_close(host);
  }

  /* ---- gamepad: hotplug, mapping, remap listener ignores the activating press, conflict moves the binding ---- */
  remove(settings_file);
  host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
  CHECK(host != NULL, "reopen for gamepad tests");
  if (host != NULL) {
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id;
    SDL_Joystick *joy = NULL;
    SvSample smp;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Fictional Test Pad";
    id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0, "a virtual gamepad attaches (hotplug)");
    if (id != 0) {
      joy = SDL_OpenJoystick(id);
      sv_host_service(host, &smp);
      SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
      SDL_UpdateJoysticks();
      sv_host_service(host, &smp);
      CHECK(smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1), "the gamepad's South button is Genesis A");
      SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, true);
      SDL_UpdateJoysticks();
      sv_host_service(host, &smp);
      CHECK(!smp.menu_was_open && (smp.pad[0] & SV_ACTION_BIT(SV_ACT_START)), "Start/Plus reaches Genesis Start, never the host menu");
      SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, false);
      SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
      SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 30000);
      SDL_UpdateJoysticks();
      sv_host_service(host, &smp);
      CHECK(smp.pad[0] & SV_ACTION_BIT(SV_ACT_RIGHT), "the left stick doubles as the D-pad beyond the dead zone");
      SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 3000);
      SDL_UpdateJoysticks();
      sv_host_service(host, &smp);
      CHECK(!(smp.pad[0] & SV_ACTION_BIT(SV_ACT_RIGHT)), "inside the dead zone nothing registers");

      /* remap through the overlay, opened with the pad's Back button */
      { PadScript ps;
        memset(&ps, 0, sizeof ps);
        ps.joy = joy; ps.host = host;
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_BACK, true);
        SDL_UpdateJoysticks();
        sv_host_set_loop_hook(host, pad_hook, &ps);
        sv_host_service(host, &smp);
        sv_host_set_loop_hook(host, NULL, NULL);
        CHECK(smp.menu_was_open, "the pad's Back button opens the pause overlay");
        CHECK(ps.activation_ignored, "the remap listener ignores the press that activated it");
        CHECK(ps.north_bound, "the next fresh pad press becomes the binding");
        CHECK(ps.conflict_moved, "a duplicate pad binding moves from its previous owner (which becomes unbound)");
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_BACK, false);
        SDL_UpdateJoysticks();
      }
      { Script sc;
        memset(&sc, 0, sizeof sc);
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, true);
        SDL_UpdateJoysticks();
        sv_host_set_loop_hook(host, menu_hook, &sc);
        sv_host_service(host, &smp);
        sv_host_set_loop_hook(host, NULL, NULL);
        CHECK(smp.menu_was_open, "Guide/Home also opens the menu (real virtual setter events)");
        if (sc.first != NULL) SDL_DestroySurface(sc.first);
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_GUIDE, false);
        SDL_UpdateJoysticks();
      }
      SDL_CloseJoystick(joy);
      SDL_DetachVirtualJoystick(id);
      sv_host_service(host, &smp);
      CHECK(smp.pad[0] == 0, "unplugging releases the pad");
    }
    sv_host_close(host);
  }
  /* ---- Video page preset buttons (mouse), and the UI scale ---- */
  ui_scale_tests();
  remove(settings_file);
  host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
  CHECK(host != NULL, "reopen for the preset test");
  if (host != NULL) {
    PresetScript ps;
    SvSample smp;
    memset(&ps, 0, sizeof ps);
    fill_pattern(frame, 320, 224);
    page_fit_tests(host);
    host->settings.audio.volume = 35; host->settings.audio.muted = 1;
    sv_pages_open_main(host);
    sv_pages_apply(host, (SvMenuEvent){SV_EVENT_ACTIVATE, I_GO_AUDIO, 0});
    sv_pages_apply(host, (SvMenuEvent){SV_EVENT_ACTIVATE, I_AUDIO_DEFAULTS, 0});
    CHECK(host->settings.audio.volume == 100 && !host->settings.audio.muted, "audio restore resets gain/mute through the actual page action, even without an audio device");
    sv_menu_reset(&host->menu);
    sv_host_present(host, frame, 320, 224, 960);
    CHECK(sv_host_settings(host)->video.preset == SV_CRT_PIXEL_PERFECT, "starts on Pixel Perfect");
    sv_host_set_loop_hook(host, preset_hook, &ps);
    tap(SV_KEY_ESCAPE);
    sv_host_service(host, &smp);
    sv_host_set_loop_hook(host, NULL, NULL);
    CHECK(ps.found, "the Video page draws four preset buttons with real hit rectangles");
    CHECK(ps.chosen == SV_CRT_ARCADE, "clicking a preset button selects that preset (it is a real control, not decoration)");
    sv_host_close(host);
  }
  /* ---- two gamepads: controller order, the persisted Player 1 choice, hot-unplug of the preferred one ---- */
  remove(settings_file);
  {
    SDL_VirtualJoystickDesc da, db;
    SDL_JoystickID ida, idb;
    SDL_Joystick *ja, *jb;
    SvSample smp;
    SDL_INIT_INTERFACE(&da); SDL_INIT_INTERFACE(&db);
    da.type = db.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    da.naxes = db.naxes = SDL_GAMEPAD_AXIS_COUNT; da.nbuttons = db.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    da.name = "Fictional Pad A"; db.name = "Fictional Pad B";
    da.vendor_id = 0x1111; da.product_id = 0x0001; db.vendor_id = 0x2222; db.product_id = 0x0002; /* distinct ids: SDL derives the gamepad name from the mapping */
    ida = SDL_AttachVirtualJoystick(&da); idb = SDL_AttachVirtualJoystick(&db);
    ja = SDL_OpenJoystick(ida); jb = SDL_OpenJoystick(idb);
    CHECK(ida != 0 && idb != 0 && ja != NULL && jb != NULL, "two virtual gamepads attach");
    host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
    CHECK(host != NULL, "open with two controllers already connected");
    if (host != NULL && ja != NULL && jb != NULL) {
      SvSettings st;
      sv_host_service(host, &smp);
      SDL_SetJoystickVirtualButton(ja, SDL_GAMEPAD_BUTTON_SOUTH, true); SDL_UpdateJoysticks(); sv_host_service(host, &smp);
      CHECK((smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1)) && !(smp.pad[1] & SV_ACTION_BIT(SV_ACT_BUTTON1)), "without a preference the first connected pad is Player 1");
      SDL_SetJoystickVirtualButton(ja, SDL_GAMEPAD_BUTTON_SOUTH, false); SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, true); SDL_UpdateJoysticks(); sv_host_service(host, &smp);
      CHECK((smp.pad[1] & SV_ACTION_BIT(SV_ACT_BUTTON1)) && !(smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1)), "the second pad is Player 2 (truthful port wiring)");
      SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, false); SDL_UpdateJoysticks(); sv_host_service(host, &smp);
      st = *sv_host_settings(host);
      snprintf(st.pad_name, sizeof st.pad_name, "Fictional Pad B");
      sv_host_apply_settings(host, &st);
      SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, true); SDL_UpdateJoysticks(); sv_host_service(host, &smp);
      CHECK((smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1)) && !(smp.pad[1] & SV_ACTION_BIT(SV_ACT_BUTTON1)), "the chosen device becomes Player 1 and the other one Player 2");
      SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, false); SDL_UpdateJoysticks(); sv_host_service(host, &smp);
      sv_host_close(host);  /* SDL_Quit: the virtual pads vanish with it (their handles are stale from here on) */
      ja = jb = NULL;
      /* the choice is persisted: the same two pads plugged in again, then a restart */
      ida = SDL_AttachVirtualJoystick(&da); idb = SDL_AttachVirtualJoystick(&db);
      ja = SDL_OpenJoystick(ida); jb = SDL_OpenJoystick(idb);
      host = open_host(SV_SYSTEM_GENESIS, settings_file, 0);
      CHECK(host != NULL && ja != NULL && jb != NULL, "reopen with the pads connected");
      if (host != NULL && ja != NULL && jb != NULL) {
        sv_host_service(host, &smp);
        CHECK(strcmp(sv_host_settings(host)->pad_name, "Fictional Pad B") == 0, "the Player 1 device choice persists across restarts");
        SDL_SetJoystickVirtualButton(jb, SDL_GAMEPAD_BUTTON_SOUTH, true); SDL_UpdateJoysticks(); sv_host_service(host, &smp);
        CHECK(smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1), "after the restart the persisted device is Player 1 again");
        /* unplug the preferred one while a button is held: it releases, Player 1 falls back to the remaining pad */
        SDL_CloseJoystick(jb); jb = NULL;
        SDL_DetachVirtualJoystick(idb);
        idb = 0;
        sv_host_service(host, &smp);
        CHECK(!(smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1)), "unplugging the pad releases what it was holding");
        SDL_SetJoystickVirtualButton(ja, SDL_GAMEPAD_BUTTON_SOUTH, true); SDL_UpdateJoysticks(); sv_host_service(host, &smp);
        CHECK(smp.pad[0] & SV_ACTION_BIT(SV_ACT_BUTTON1), "with the preferred pad gone Player 1 falls back to the remaining one");
      }
      if (host != NULL) sv_host_close(host);
      host = NULL;
    } else if (host != NULL) sv_host_close(host);
  }
  widescreen_tests(dir, frame);
  widescreen_live_tests(dir, frame);
  free(frame);
  if (failures != 0) { printf("sv_host_test: %d failure(s)\n", failures); return 1; }
  printf("sv_host_test: ok\n");
  return 0;
}
