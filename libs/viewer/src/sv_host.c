#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_internal.h"

/* The settings store scancodes and buttons as plain ints (so sv_settings.c needs no SDL): pin them to the SDL3 values. */
#define SV_PIN(a, b) typedef char sv_pin_##a[((int)(a) == (int)(b)) ? 1 : -1]
SV_PIN(SV_KEY_A, SDL_SCANCODE_A); SV_PIN(SV_KEY_C, SDL_SCANCODE_C); SV_PIN(SV_KEY_I, SDL_SCANCODE_I); SV_PIN(SV_KEY_J, SDL_SCANCODE_J);
SV_PIN(SV_KEY_K, SDL_SCANCODE_K); SV_PIN(SV_KEY_L, SDL_SCANCODE_L); SV_PIN(SV_KEY_O, SDL_SCANCODE_O); SV_PIN(SV_KEY_P, SDL_SCANCODE_P);
SV_PIN(SV_KEY_U, SDL_SCANCODE_U); SV_PIN(SV_KEY_X, SDL_SCANCODE_X); SV_PIN(SV_KEY_Z, SDL_SCANCODE_Z); SV_PIN(SV_KEY_RETURN, SDL_SCANCODE_RETURN);
SV_PIN(SV_KEY_ESCAPE, SDL_SCANCODE_ESCAPE); SV_PIN(SV_KEY_BACKSPACE, SDL_SCANCODE_BACKSPACE); SV_PIN(SV_KEY_DELETE, SDL_SCANCODE_DELETE);
SV_PIN(SV_KEY_F11, SDL_SCANCODE_F11); SV_PIN(SV_KEY_F12, SDL_SCANCODE_F12); SV_PIN(SV_KEY_RIGHT, SDL_SCANCODE_RIGHT); SV_PIN(SV_KEY_LEFT, SDL_SCANCODE_LEFT);
SV_PIN(SV_KEY_DOWN, SDL_SCANCODE_DOWN); SV_PIN(SV_KEY_UP, SDL_SCANCODE_UP); SV_PIN(SV_KEY_RSHIFT, SDL_SCANCODE_RSHIFT); SV_PIN(SV_KEY_F1, SDL_SCANCODE_F1);
SV_PIN(SV_KEY_F10, SDL_SCANCODE_F10);
SV_PIN(SV_PAD_SOUTH, SDL_GAMEPAD_BUTTON_SOUTH); SV_PIN(SV_PAD_EAST, SDL_GAMEPAD_BUTTON_EAST); SV_PIN(SV_PAD_WEST, SDL_GAMEPAD_BUTTON_WEST);
SV_PIN(SV_PAD_NORTH, SDL_GAMEPAD_BUTTON_NORTH); SV_PIN(SV_PAD_BACK, SDL_GAMEPAD_BUTTON_BACK); SV_PIN(SV_PAD_GUIDE, SDL_GAMEPAD_BUTTON_GUIDE);
SV_PIN(SV_PAD_START, SDL_GAMEPAD_BUTTON_START); SV_PIN(SV_PAD_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_UP); SV_PIN(SV_PAD_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
SV_PIN(SV_PAD_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT); SV_PIN(SV_PAD_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

/* ------------------------------------------------------------------------------------------------------------------ helpers */

void sv_host_mark_dirty(SvHost *h) { h->settings_dirty = 1; }

static void save_settings_if_dirty(SvHost *h) {
  if (h->settings_dirty && h->settings_path[0] != '\0') {
    if (sv_settings_save(&h->settings, h->settings_path) != 0) SDL_Log("viewer: could not save settings to %s", h->settings_path);
  }
  h->settings_dirty = 0;
}

static void resolve_settings_path(SvHost *h) {
  const char *override_path = SDL_getenv("SEGARECOMP_VIEWER_SETTINGS");
  h->settings_path[0] = '\0';
  if (override_path != NULL && *override_path != '\0') {
    if (strlen(override_path) < sizeof h->settings_path) snprintf(h->settings_path, sizeof h->settings_path, "%s", override_path);
    return;
  }
  {
    char *pref = SDL_GetPrefPath(NULL, "Segarecomp"); /* ends with a separator */
    if (pref != NULL) {
      char dir[900];
      snprintf(dir, sizeof dir, "%slibrary", pref);
      SDL_CreateDirectory(dir);
      snprintf(h->settings_path, sizeof h->settings_path, "%s/viewer-settings.cfg", dir);
      SDL_free(pref);
    }
  }
}

void sv_host_draw_text(SvHost *h, float x, float y, int scale, SDL_Color c, const char *text) {
  const float s = (float)(scale < 1 ? 1 : scale);
  SDL_SetRenderScale(h->renderer, s, s);
  SDL_SetRenderDrawColor(h->renderer, c.r, c.g, c.b, c.a);
  SDL_RenderDebugText(h->renderer, x / s, y / s, text);
  SDL_SetRenderScale(h->renderer, 1.0f, 1.0f);
}

void sv_host_fill(SvHost *h, int x, int y, int w, int ht, SDL_Color c) {
  SDL_FRect r;
  r.x = (float)x; r.y = (float)y; r.w = (float)w; r.h = (float)ht;
  SDL_SetRenderDrawBlendMode(h->renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(h->renderer, c.r, c.g, c.b, c.a);
  SDL_RenderFillRect(h->renderer, &r);
}

/* Chunky pixel bevel in the launcher's palette: dark outline, edge, light top/left, dark bottom/right, fill. */
void sv_host_bevel(SvHost *h, int x, int y, int w, int ht, int step, SDL_Color fill, SDL_Color edge, SDL_Color highlight, SDL_Color shadow) {
  const SDL_Color outline = {7, 16, 38, 255};
  sv_host_fill(h, x, y, w, ht, outline);
  sv_host_fill(h, x + step, y + step, w - 2 * step, ht - 2 * step, edge);
  sv_host_fill(h, x + step, y + step, w - 2 * step, step, highlight);
  sv_host_fill(h, x + step, y + step, step, ht - 2 * step, highlight);
  sv_host_fill(h, x + step, y + ht - 2 * step, w - 2 * step, step, shadow);
  sv_host_fill(h, x + w - 2 * step, y + step, step, ht - 2 * step, shadow);
  sv_host_fill(h, x + 2 * step, y + 2 * step, w - 4 * step, ht - 4 * step, fill);
}

int sv_host_ui_scale(int out_w, int out_h) {
  const int by_w = (int)((double)out_w * 0.92 / 380.0);
  const int by_h = (out_h + 140) / 300;
  int s = by_w < by_h ? by_w : by_h;
  if (s < 1) s = 1;
  if (s > 5) s = 5;
  return s;
}

/* ------------------------------------------------------------------------------------------------------------------ gamepads */

static void pad_slot_release(SvPadSlot *slot) {
  slot->buttons = 0;
  slot->latched = 0;
  slot->ax = slot->ay = 0;
}

static void pad_open(SvHost *h, SDL_JoystickID id) {
  SDL_Gamepad *pad;
  int i;
  if (h->pad_count >= SV_MAX_PADS) return;
  for (i = 0; i < h->pad_count; ++i) if (h->pads[i].id == id) return;
  pad = SDL_OpenGamepad(id);
  if (pad == NULL) return;
  memset(&h->pads[h->pad_count], 0, sizeof h->pads[0]);
  h->pads[h->pad_count].id = id;
  h->pads[h->pad_count].pad = pad;
  snprintf(h->pads[h->pad_count].name, sizeof h->pads[0].name, "%s", SDL_GetGamepadName(pad) != NULL ? SDL_GetGamepadName(pad) : "Gamepad");
  h->pad_count++;
}

static void pad_close(SvHost *h, SDL_JoystickID id) {
  int i;
  for (i = 0; i < h->pad_count; ++i) {
    if (h->pads[i].id != id) continue;
    SDL_CloseGamepad(h->pads[i].pad);
    memmove(&h->pads[i], &h->pads[i + 1], (size_t)(h->pad_count - i - 1) * sizeof h->pads[0]);
    h->pad_count--;
    return;
  }
}

int sv_host_pad_assigned(const SvHost *h, int player) {
  int first = -1, i, preferred = -1;
  if (h->pad_count == 0) return -1;
  if (h->settings.pad_name[0] != '\0')
    for (i = 0; i < h->pad_count; ++i)
      if (strcmp(h->pads[i].name, h->settings.pad_name) == 0) { preferred = i; break; }
  first = preferred >= 0 ? preferred : 0;
  if (player == 0) return first;
  /* Player 2 takes the first remaining device. */
  for (i = 0; i < h->pad_count; ++i) if (i != first) return i;
  return -1;
}

void sv_host_release_inputs(SvHost *h) {
  int i;
  memset(h->key_held, 0, sizeof h->key_held);
  memset(h->key_latch, 0, sizeof h->key_latch);
  for (i = 0; i < h->pad_count; ++i) pad_slot_release(&h->pads[i]);
}

/* ------------------------------------------------------------------------------------------------------------------ events */

static void handle_pad_button(SvHost *h, SDL_JoystickID id, int button, int down) {
  int i;
  for (i = 0; i < h->pad_count; ++i)
    if (h->pads[i].id == id && button >= 0 && button < 32) {
      if (down) { h->pads[i].buttons |= 1u << (unsigned)button; h->pads[i].latched |= 1u << (unsigned)button; } else h->pads[i].buttons &= ~(1u << (unsigned)button);
    }
}

/* Events that matter in every state: window/focus, hotplug, held-state tracking. Returns 1 when the event was a key/pad press the caller
 * may also interpret (the held tables are already updated). */
static void common_event(SvHost *h, const SDL_Event *e) {
  h->event_ts = e->common.timestamp;
  switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: h->quit = 1; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: sv_host_release_inputs(h); break; /* nothing stays "held" behind the user's back */
    case SDL_EVENT_WINDOW_RESIZED:
      if ((SDL_GetWindowFlags(h->window) & SDL_WINDOW_FULLSCREEN) == 0 && e->window.data1 > 0 && e->window.data2 > 0) {
        h->settings.video.window_w = e->window.data1;
        h->settings.video.window_h = e->window.data2;
        sv_host_mark_dirty(h);
      }
      break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_open(h, e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_close(h, e->gdevice.which); break;
    case SDL_EVENT_KEY_DOWN:
      if (e->key.scancode < 512) {
        if (!e->key.repeat) h->key_latch[e->key.scancode] = 1;
        h->key_held[e->key.scancode] = 1;
      }
      break;
    case SDL_EVENT_KEY_UP: if (e->key.scancode < 512) h->key_held[e->key.scancode] = 0; break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: handle_pad_button(h, e->gbutton.which, e->gbutton.button, 1); break;
    case SDL_EVENT_GAMEPAD_BUTTON_UP: handle_pad_button(h, e->gbutton.which, e->gbutton.button, 0); break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      int i;
      for (i = 0; i < h->pad_count; ++i)
        if (h->pads[i].id == e->gaxis.which) {
          if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) h->pads[i].ax = e->gaxis.value;
          else if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) h->pads[i].ay = e->gaxis.value;
        }
      break;
    }
    default: break;
  }
}

static int is_menu_key(const SvHost *h, int scancode) { return scancode == SV_KEY_ESCAPE || scancode == h->settings.menu_key; }

/* ------------------------------------------------------------------------------------------------------------------ sampling */

static uint16_t sample_player(SvHost *h, int player) {
  const SvBindings *b = &h->settings.bind[h->cfg.system];
  const int slot = sv_host_pad_assigned(h, player);
  uint16_t mask = 0;
  int a, dx = 0, dy = 0;
  if (slot >= 0) {
    dx = sv_axis_direction(h->pads[slot].ax, h->settings.deadzone);
    dy = sv_axis_direction(h->pads[slot].ay, h->settings.deadzone);
  }
  for (a = 0; a < SV_ACT_COUNT; ++a) {
    int on = 0;
    if (!sv_action_available(h->cfg.system, player, (SvAction)a)) continue;
    if (b->key[player][a] > 0 && b->key[player][a] < 512) on = h->key_held[b->key[player][a]] != 0;
    if (!on && slot >= 0 && b->pad[player][a] >= 0 && b->pad[player][a] < 32) on = (h->pads[slot].buttons >> (unsigned)b->pad[player][a]) & 1u;
    if (!on && slot >= 0) { /* the left stick doubles as the D-pad when the D-pad directions are bound */
      if (a == SV_ACT_LEFT && dx < 0) on = 1;
      if (a == SV_ACT_RIGHT && dx > 0) on = 1;
      if (a == SV_ACT_UP && dy < 0) on = 1;
      if (a == SV_ACT_DOWN && dy > 0) on = 1;
    }
    if (a == SV_ACT_PAUSE && !on) { /* a tap shorter than a sample still presses the console Pause button once */
      if (b->key[player][a] > 0 && b->key[player][a] < 512 && h->key_latch[b->key[player][a]]) on = 1;
      if (slot >= 0 && b->pad[player][a] >= 0 && b->pad[player][a] < 32 && ((h->pads[slot].latched >> (unsigned)b->pad[player][a]) & 1u)) on = 1;
    }
    if (on) mask = (uint16_t)(mask | SV_ACTION_BIT(a));
  }
  return mask;
}

/* ------------------------------------------------------------------------------------------------------------------ video */

static void ensure_texture(SvHost *h, int w, int hgt) {
  if (h->frame_tex != NULL && h->tex_w == w && h->tex_h == hgt) return;
  if (h->frame_tex != NULL) SDL_DestroyTexture(h->frame_tex);
  h->frame_tex = SDL_CreateTexture(h->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, hgt);
  h->tex_w = w;
  h->tex_h = hgt;
  if (h->frame_tex != NULL) SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_NEAREST);
}

static SDL_Texture *make_rgba_texture(SvHost *h, int w, int hgt, SDL_TextureAccess access, SDL_BlendMode blend) {
  SDL_Texture *t = SDL_CreateTexture(h->renderer, SDL_PIXELFORMAT_RGBA32, access, w, hgt);
  if (t != NULL) SDL_SetTextureBlendMode(t, blend);
  return t;
}

static void ensure_crt_targets(SvHost *h, SvRect dest, int src_h) {
  const SvVideoSettings *v = &h->settings.video;
  if (h->rt == NULL || h->rt_w != dest.w || h->rt_h != dest.h) {
    if (h->rt != NULL) SDL_DestroyTexture(h->rt);
    h->rt = make_rgba_texture(h, dest.w, dest.h, SDL_TEXTUREACCESS_TARGET, SDL_BLENDMODE_NONE);
    h->rt_w = dest.w;
    h->rt_h = dest.h;
  }
  if (v->scanlines > 0 && (h->scan_tex == NULL || h->scan_h != dest.h || h->scan_src_h != src_h || h->scan_strength != v->scanlines)) {
    const float period = (float)dest.h / (float)src_h;
    uint32_t *pixels = (uint32_t *)malloc((size_t)dest.h * sizeof *pixels);
    int y;
    if (h->scan_tex != NULL) SDL_DestroyTexture(h->scan_tex);
    h->scan_tex = NULL;
    if (pixels != NULL) {
      for (y = 0; y < dest.h; ++y) {
        const uint8_t a = sv_crt_scanline_alpha(y, period, v->scanlines);
        const uint8_t bytes[4] = {0, 0, 0, a};
        memcpy(&pixels[y], bytes, 4); /* RGBA32 byte order: R,G,B,A */
      }
      h->scan_tex = make_rgba_texture(h, 1, dest.h, SDL_TEXTUREACCESS_STATIC, SDL_BLENDMODE_BLEND);
      if (h->scan_tex != NULL) SDL_UpdateTexture(h->scan_tex, NULL, pixels, 4);
      free(pixels);
    }
    h->scan_h = dest.h;
    h->scan_src_h = src_h;
    h->scan_strength = v->scanlines;
  }
  if (v->mask > 0 && (h->mask_tex == NULL || h->mask_w != dest.w || h->mask_strength != v->mask)) {
    uint8_t *pixels = (uint8_t *)malloc((size_t)dest.w * 4u);
    int x;
    if (h->mask_tex != NULL) SDL_DestroyTexture(h->mask_tex);
    h->mask_tex = NULL;
    if (pixels != NULL) {
      for (x = 0; x < dest.w; ++x) {
        pixels[x * 4 + 0] = sv_crt_mask_value(x, 0, v->mask);
        pixels[x * 4 + 1] = sv_crt_mask_value(x, 1, v->mask);
        pixels[x * 4 + 2] = sv_crt_mask_value(x, 2, v->mask);
        pixels[x * 4 + 3] = 255;
      }
      h->mask_tex = make_rgba_texture(h, dest.w, 1, SDL_TEXTUREACCESS_STATIC, SDL_BLENDMODE_MOD);
      if (h->mask_tex != NULL) SDL_UpdateTexture(h->mask_tex, NULL, pixels, dest.w * 4);
      free(pixels);
    }
    h->mask_w = dest.w;
    h->mask_strength = v->mask;
  }
}

/* Draws the picture into the back buffer. Pixel Perfect (identity settings) is a single nearest-neighbour copy; every other preset renders
 * through a window-sized target with real scanline, mask, bloom, brightness and curvature passes. */
static void render_game(SvHost *h, int out_w, int out_h) {
  const SvVideoSettings *v = &h->settings.video;
  SvRect dest;
  SDL_FRect d;
  if (h->frame_tex == NULL || h->last_w <= 0) return;
  dest = sv_layout_dest(out_w, out_h, h->last_w, h->last_h, v->aspect, v->scale);
  d.x = (float)dest.x; d.y = (float)dest.y; d.w = (float)dest.w; d.h = (float)dest.h;
  SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_NEAREST);
  SDL_SetTextureColorMod(h->frame_tex, 255, 255, 255);
  SDL_SetTextureAlphaMod(h->frame_tex, 255);
  SDL_SetTextureBlendMode(h->frame_tex, SDL_BLENDMODE_NONE);
  if (sv_crt_is_identity(&h->settings.video)) {
    SDL_RenderTexture(h->renderer, h->frame_tex, NULL, &d);
    return;
  }
  ensure_crt_targets(h, dest, h->last_h);
  if (h->rt == NULL) { SDL_RenderTexture(h->renderer, h->frame_tex, NULL, &d); return; }
  {
    SDL_FRect full;
    full.x = 0.0f; full.y = 0.0f; full.w = (float)dest.w; full.h = (float)dest.h;
    SDL_SetRenderTarget(h->renderer, h->rt);
    SDL_SetRenderDrawColor(h->renderer, 0, 0, 0, 255);
    SDL_SetRenderDrawBlendMode(h->renderer, SDL_BLENDMODE_NONE);
    SDL_RenderClear(h->renderer);
    { /* base picture, darkened when brightness < 100 */
      const int c = v->brightness < 100 ? 255 * v->brightness / 100 : 255;
      SDL_SetTextureColorMod(h->frame_tex, (Uint8)c, (Uint8)c, (Uint8)c);
      SDL_RenderTexture(h->renderer, h->frame_tex, NULL, &full);
    }
    SDL_SetTextureColorMod(h->frame_tex, 255, 255, 255);
    SDL_SetTextureBlendMode(h->frame_tex, SDL_BLENDMODE_ADD);
    if (v->brightness > 100) { /* add the picture to itself: color * (1 + a) */
      SDL_SetTextureAlphaMod(h->frame_tex, (Uint8)((v->brightness - 100) * 255 / 100));
      SDL_RenderTexture(h->renderer, h->frame_tex, NULL, &full);
    }
    if (v->bloom > 0) { /* soft glow: four offset, linearly filtered, additive taps */
      const float spread = fmaxf(1.0f, (float)dest.h / (float)h->last_h * 0.7f);
      const Uint8 alpha = (Uint8)(v->bloom * 255 * 16 / 10000);
      static const float off[4][2] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}};
      int i;
      SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_LINEAR);
      SDL_SetTextureAlphaMod(h->frame_tex, alpha);
      for (i = 0; i < 4; ++i) {
        SDL_FRect t = full;
        t.x += off[i][0] * spread * 2.0f;
        t.y += off[i][1] * spread * 2.0f;
        SDL_RenderTexture(h->renderer, h->frame_tex, NULL, &t);
      }
      SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_NEAREST);
    }
    SDL_SetTextureAlphaMod(h->frame_tex, 255);
    SDL_SetTextureBlendMode(h->frame_tex, SDL_BLENDMODE_NONE);
    if (v->scanlines > 0 && h->scan_tex != NULL) SDL_RenderTexture(h->renderer, h->scan_tex, NULL, &full);
    if (v->mask > 0 && h->mask_tex != NULL) SDL_RenderTexture(h->renderer, h->mask_tex, NULL, &full);
    SDL_SetRenderTarget(h->renderer, NULL);
  }
  if (v->curvature <= 0) {
    SDL_SetTextureScaleMode(h->rt, SDL_SCALEMODE_NEAREST);
    SDL_RenderTexture(h->renderer, h->rt, NULL, &d);
    return;
  }
  { /* barrel-warped mesh of the finished picture */
    enum { N = 20 };
    SDL_Vertex verts[(N + 1) * (N + 1)];
    int indices[N * N * 6];
    int i, j, n = 0;
    SDL_SetTextureScaleMode(h->rt, SDL_SCALEMODE_LINEAR);
    for (j = 0; j <= N; ++j)
      for (i = 0; i <= N; ++i) {
        float wx, wy, vig;
        SDL_Vertex *vt = &verts[j * (N + 1) + i];
        const float nx = (float)i / (float)N * 2.0f - 1.0f, ny = (float)j / (float)N * 2.0f - 1.0f;
        sv_crt_warp(nx, ny, v->curvature, &wx, &wy, &vig);
        vt->position.x = d.x + (wx * 0.5f + 0.5f) * d.w;
        vt->position.y = d.y + (wy * 0.5f + 0.5f) * d.h;
        vt->tex_coord.x = (float)i / (float)N;
        vt->tex_coord.y = (float)j / (float)N;
        vt->color.r = vig; vt->color.g = vig; vt->color.b = vig; vt->color.a = 1.0f;
      }
    for (j = 0; j < N; ++j)
      for (i = 0; i < N; ++i) {
        const int a = j * (N + 1) + i, b = a + 1, c = a + (N + 1), e = c + 1;
        indices[n++] = a; indices[n++] = b; indices[n++] = c; indices[n++] = b; indices[n++] = e; indices[n++] = c;
      }
    SDL_RenderGeometry(h->renderer, h->rt, verts, (N + 1) * (N + 1), indices, n);
  }
}

static void draw_hud(SvHost *h, int out_w, int out_h) {
  const SDL_Color text = {220, 232, 255, 255}, fill = {8, 20, 47, 220}, edge = {35, 62, 120, 255}, hi = {77, 103, 167, 255}, sh = {7, 16, 38, 255};
  const int ui = sv_host_ui_scale(out_w, out_h) > 1 ? sv_host_ui_scale(out_w, out_h) - 1 : 1;
  (void)out_h;
  if (h->settings.video.show_fps && !h->menu_open) {
    char line[64];
    const int w = (int)(strlen("000.0 FPS  00.0 ms") * 8u) * ui + 8 * ui;
    snprintf(line, sizeof line, "%5.1f FPS %5.1f ms", h->fps, h->frame_ms);
    sv_host_bevel(h, 6, 6, w, 8 * ui + 8 * ui, ui, fill, edge, hi, sh);
    sv_host_draw_text(h, (float)(6 + 5 * ui), (float)(6 + 4 * ui + ui), ui, text, line);
  }
  if (h->toast[0] != '\0' && SDL_GetTicks() < h->toast_until) {
    const int tui = sv_host_ui_scale(out_w, out_h);
    const int w = (int)strlen(h->toast) * 8 * tui + 12 * tui, hh = 8 * tui + 10 * tui;
    sv_host_bevel(h, (out_w - w) / 2, out_h - hh - 12 * tui, w, hh, tui, fill, edge, hi, sh);
    sv_host_draw_text(h, (float)((out_w - w) / 2 + 6 * tui), (float)(out_h - hh - 12 * tui + 5 * tui), tui, text, h->toast);
  }
}

static void draw_scene(SvHost *h, int overlay) {
  int out_w = 0, out_h = 0;
  SDL_GetCurrentRenderOutputSize(h->renderer, &out_w, &out_h);
  SDL_SetRenderTarget(h->renderer, NULL);
  SDL_SetRenderDrawBlendMode(h->renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(h->renderer, 0, 0, 0, 255);
  SDL_RenderClear(h->renderer);
  render_game(h, out_w, out_h);
  if (overlay && h->menu_open) {
    SDL_Color dim = {4, 8, 20, 150};
    if (!sv_pages_wants_preview(h)) sv_host_fill(h, 0, 0, out_w, out_h, dim);
    sv_pages_draw(h, out_w, out_h);
  }
  draw_hud(h, out_w, out_h);
}

void sv_host_apply_video(SvHost *h) {
  const SvVideoSettings *v = &h->settings.video;
  const int want = v->fullscreen != 0;
  if (((SDL_GetWindowFlags(h->window) & SDL_WINDOW_FULLSCREEN) != 0) != want) SDL_SetWindowFullscreen(h->window, want);
  SDL_SetRenderVSync(h->renderer, v->vsync ? 1 : 0);
}

void sv_host_apply_audio(SvHost *h) {
  if (h->audio != NULL) SDL_SetAudioStreamGain(h->audio, h->settings.audio.muted ? 0.0f : (float)h->settings.audio.volume / 100.0f);
}

void sv_host_toggle_fullscreen(SvHost *h) {
  h->settings.video.fullscreen = !h->settings.video.fullscreen;
  sv_host_apply_video(h);
  sv_host_mark_dirty(h);
}

void sv_host_set_loop_hook(SvHost *h, int (*hook)(void *ctx, SvHost *host, int iteration), void *ctx) {
  h->hook = hook;
  h->hook_ctx = ctx;
}

void sv_host_toast(SvHost *h, const char *text) {
  snprintf(h->toast, sizeof h->toast, "%s", text);
  h->toast_until = SDL_GetTicks() + 2600u;
}

/* ------------------------------------------------------------------------------------------------------------------ screenshots */

int sv_host_screenshot(SvHost *h, char *path_out, size_t cap) {
  char dir[700], path[800];
  SDL_Surface *surface;
  SDL_DateTime dt;
  SDL_Time now = 0;
  const char *override_dir = SDL_getenv("SEGARECOMP_SCREENSHOT_DIR");
  int ok;
  if (h->last_rgb == NULL || h->last_w <= 0) return -1;
  if (override_dir != NULL && *override_dir != '\0') snprintf(dir, sizeof dir, "%s", override_dir);
  else {
    const char *pictures = SDL_GetUserFolder(SDL_FOLDER_PICTURES);
    if (pictures != NULL) snprintf(dir, sizeof dir, "%sSegarecomp", pictures);
    else {
      char *pref = SDL_GetPrefPath(NULL, "Segarecomp");
      snprintf(dir, sizeof dir, "%sscreenshots", pref != NULL ? pref : "");
      SDL_free(pref);
    }
  }
  if (!SDL_CreateDirectory(dir)) return -1;
  SDL_GetCurrentTime(&now);
  if (!SDL_TimeToDateTime(now, &dt, true)) memset(&dt, 0, sizeof dt);
#if SDL_VERSION_ATLEAST(3, 4, 0)
  snprintf(path, sizeof path, "%s/%s-%04d%02d%02d-%02d%02d%02d-%llu.png", dir, h->cfg.system == SV_SYSTEM_SMS ? "sms" : "genesis", dt.year, dt.month, dt.day,
           dt.hour, dt.minute, dt.second, (unsigned long long)h->frames_presented);
#else
  snprintf(path, sizeof path, "%s/%s-%04d%02d%02d-%02d%02d%02d-%llu.bmp", dir, h->cfg.system == SV_SYSTEM_SMS ? "sms" : "genesis", dt.year, dt.month, dt.day,
           dt.hour, dt.minute, dt.second, (unsigned long long)h->frames_presented);
#endif
  surface = SDL_CreateSurfaceFrom(h->last_w, h->last_h, SDL_PIXELFORMAT_RGB24, h->last_rgb, h->last_w * 3);
  if (surface == NULL) return -1;
#if SDL_VERSION_ATLEAST(3, 4, 0)
  ok = SDL_SavePNG(surface, path);
#else
  ok = SDL_SaveBMP(surface, path);
#endif
  SDL_DestroySurface(surface);
  if (!ok) return -1;
  snprintf(h->last_screenshot, sizeof h->last_screenshot, "%.500s", path);
  if (path_out != NULL && cap > 0) snprintf(path_out, cap, "%s", path);
  return 0;
}

void sv_host_take_screenshot(SvHost *h) {
  char path[800], note[96];
  if (sv_host_screenshot(h, path, sizeof path) == 0) {
    const char *base = strrchr(path, '/');
    snprintf(note, sizeof note, "Screenshot saved: %.60s", base != NULL ? base + 1 : path);
    sv_host_toast(h, note);
  } else {
    sv_host_toast(h, "Screenshot failed (no frame yet or folder not writable)");
  }
}

/* ------------------------------------------------------------------------------------------------------------------ open / close */

SvHost *sv_host_open(const SvHostConfig *cfg) {
  SvHost *h;
  int w, hh, count = 0, i;
  SDL_JoystickID *ids;
  SDL_Rect usable;
  if (cfg == NULL || cfg->picture_w <= 0 || cfg->picture_h <= 0) return NULL;
  h = (SvHost *)calloc(1, sizeof *h);
  if (h == NULL) return NULL;
  h->cfg = *cfg;
  sv_settings_defaults(&h->settings);
  resolve_settings_path(h);
  (void)sv_settings_load(&h->settings, h->settings_path);
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { free(h); return NULL; } /* no gamepad subsystem: keyboard only */
  }
  {
    const int scale = cfg->default_scale > 0 && cfg->default_scale <= 16 ? cfg->default_scale : 3;
    w = h->settings.video.window_w > 0 ? h->settings.video.window_w : cfg->picture_w * scale;
    hh = h->settings.video.window_h > 0 ? h->settings.video.window_h : cfg->picture_h * scale;
  }
  if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
    if (w > usable.w * 9 / 10) { hh = hh * (usable.w * 9 / 10) / w; w = usable.w * 9 / 10; }
    if (hh > usable.h * 9 / 10) { w = w * (usable.h * 9 / 10) / hh; hh = usable.h * 9 / 10; }
  }
  h->window = SDL_CreateWindow(cfg->title != NULL ? cfg->title : "Segarecomp", w, hh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (h->window == NULL) { SDL_Quit(); free(h); return NULL; }
  SDL_SetWindowMinimumSize(h->window, cfg->picture_w / 2 > 160 ? cfg->picture_w / 2 : 160, cfg->picture_h / 2 > 120 ? cfg->picture_h / 2 : 120);
  h->renderer = SDL_CreateRenderer(h->window, NULL);
  if (h->renderer == NULL) { SDL_DestroyWindow(h->window); SDL_Quit(); free(h); return NULL; }
  h->last_rgb = (uint8_t *)malloc((size_t)(cfg->max_w > 0 ? cfg->max_w : 320) * (size_t)(cfg->max_h > 0 ? cfg->max_h : 240) * 3u);
  if (h->last_rgb == NULL) { sv_host_close(h); return NULL; }
  sv_host_apply_video(h);
  ids = SDL_GetGamepads(&count);
  if (ids != NULL) { for (i = 0; i < count; ++i) pad_open(h, ids[i]); SDL_free(ids); }
  return h;
}

void sv_host_close(SvHost *h) {
  int i;
  if (h == NULL) return;
  save_settings_if_dirty(h);
  for (i = 0; i < h->pad_count; ++i) SDL_CloseGamepad(h->pads[i].pad);
  if (h->frame_tex != NULL) SDL_DestroyTexture(h->frame_tex);
  if (h->rt != NULL) SDL_DestroyTexture(h->rt);
  if (h->scan_tex != NULL) SDL_DestroyTexture(h->scan_tex);
  if (h->mask_tex != NULL) SDL_DestroyTexture(h->mask_tex);
  if (h->renderer != NULL) SDL_DestroyRenderer(h->renderer);
  if (h->window != NULL) SDL_DestroyWindow(h->window);
  free(h->last_rgb);
  SDL_Quit();
  free(h);
}

void sv_host_set_audio(SvHost *h, SDL_AudioStream *stream) {
  h->audio = stream;
  sv_host_apply_audio(h);
}

const SvSettings *sv_host_settings(const SvHost *h) { return &h->settings; }

void sv_host_apply_settings(SvHost *h, const SvSettings *settings) {
  h->settings = *settings;
  sv_settings_sanitize(&h->settings);
  sv_host_apply_video(h);
  sv_host_apply_audio(h);
  sv_host_mark_dirty(h);
}
const char *sv_host_renderer_name(const SvHost *h) { const char *n = SDL_GetRendererName(h->renderer); return n != NULL ? n : "unknown"; }
int sv_host_menu_open(const SvHost *h) { return h->menu_open; }

SDL_Surface *sv_host_capture_output(SvHost *h) {
  draw_scene(h, 1);
  return SDL_RenderReadPixels(h->renderer, NULL);
}

/* ------------------------------------------------------------------------------------------------------------------ present */

int sv_host_present(SvHost *h, const uint8_t *rgb, int w, int hgt, int pitch) {
  int y;
  const Uint64 now = SDL_GetTicksNS();
  if (rgb == NULL || w <= 0 || hgt <= 0 || w > (h->cfg.max_w > 0 ? h->cfg.max_w : 320) || hgt > (h->cfg.max_h > 0 ? h->cfg.max_h : 240) || pitch < w * 3) return -1;
  ensure_texture(h, w, hgt);
  if (h->frame_tex == NULL) return -1;
  if (!SDL_UpdateTexture(h->frame_tex, NULL, rgb, pitch)) return -1;
  for (y = 0; y < hgt; ++y) memcpy(h->last_rgb + (size_t)y * (size_t)w * 3u, rgb + (size_t)y * (size_t)pitch, (size_t)w * 3u);
  h->last_w = w;
  h->last_h = hgt;
  h->frames_presented++;
  if (h->last_present_ns != 0) h->frame_ms = h->frame_ms * 0.9 + (double)(now - h->last_present_ns) / 1.0e6 * 0.1;
  h->last_present_ns = now;
  h->fps_frames++;
  if (h->fps_window_start == 0) h->fps_window_start = now;
  if (now - h->fps_window_start >= 1000000000ULL) {
    h->fps = (double)h->fps_frames * 1.0e9 / (double)(now - h->fps_window_start);
    h->fps_frames = 0;
    h->fps_window_start = now;
  }
  draw_scene(h, 0);
  return SDL_RenderPresent(h->renderer) ? 0 : -1;
}

/* ------------------------------------------------------------------------------------------------------------------ service + menu loop */

static SvNav key_to_nav(int sc) {
  switch (sc) {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_W: return SV_NAV_UP;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: return SV_NAV_DOWN;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: return SV_NAV_LEFT;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: return SV_NAV_RIGHT;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: return SV_NAV_ACCEPT;
    case SDL_SCANCODE_BACKSPACE: return SV_NAV_BACK;
    default: return SV_NAV_NONE;
  }
}

static SvNav button_to_nav(int b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return SV_NAV_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return SV_NAV_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return SV_NAV_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return SV_NAV_RIGHT;
    case SDL_GAMEPAD_BUTTON_SOUTH: return SV_NAV_ACCEPT;
    case SDL_GAMEPAD_BUTTON_EAST: return SV_NAV_BACK;
    default: return SV_NAV_NONE;
  }
}

/* Runs the pause overlay until it is closed. The caller (the console adapter) is blocked here, so the guest does not advance. */
static void run_menu(SvHost *h) {
  SDL_Event e;
  int stick_dir_x = 0, stick_dir_y = 0, iteration = 0;
  h->menu_open = 1;
  sv_host_release_inputs(h);
  if (h->audio != NULL) { SDL_PauseAudioStreamDevice(h->audio); SDL_ClearAudioStream(h->audio); }
  sv_pages_open_main(h);
  while (h->menu_open && !h->quit) {
    int got = SDL_WaitEventTimeout(&e, 16);
    while (got) {
      common_event(h, &e);
      if (e.type == SDL_EVENT_KEY_DOWN) {
        const int sc = (int)e.key.scancode;
        h->last_input_pad = 0;
        if (h->listen.kind != SV_LISTEN_NONE) {
          if (!e.key.repeat) sv_pages_listen_key(h, sc);
        } else if (!e.key.repeat && is_menu_key(h, sc) && sc != SV_KEY_ESCAPE) {
          sv_pages_close(h);
        } else if (sc == SDL_SCANCODE_ESCAPE) {
          if (!e.key.repeat) sv_pages_back(h);
        } else if (sc == SDL_SCANCODE_F11 && !e.key.repeat) {
          sv_host_toggle_fullscreen(h);
        } else if (sc == SDL_SCANCODE_F12 && !e.key.repeat) {
          sv_host_take_screenshot(h);
        } else {
          const SvNav nav = key_to_nav(sc);
          if (nav == SV_NAV_BACK) sv_pages_back(h);
          else if (nav != SV_NAV_NONE) sv_pages_apply(h, sv_menu_navigate(&h->menu, nav));
        }
      } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const int b = e.gbutton.button;
        h->last_input_pad = 1;
        if (h->listen.kind != SV_LISTEN_NONE) sv_pages_listen_pad(h, b);
        else if (b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_GUIDE) sv_pages_close(h);
        else if (button_to_nav(b) == SV_NAV_BACK) sv_pages_back(h);
        else if (button_to_nav(b) != SV_NAV_NONE) sv_pages_apply(h, sv_menu_navigate(&h->menu, button_to_nav(b)));
      } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && h->listen.kind == SV_LISTEN_NONE) {
        const int dir = sv_axis_direction(e.gaxis.value, h->settings.deadzone);
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY && dir != stick_dir_y) {
          stick_dir_y = dir;
          h->last_input_pad = 1;
          if (dir != 0) sv_pages_apply(h, sv_menu_navigate(&h->menu, dir < 0 ? SV_NAV_UP : SV_NAV_DOWN));
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX && dir != stick_dir_x) {
          stick_dir_x = dir;
          h->last_input_pad = 1;
          if (dir != 0) sv_pages_apply(h, sv_menu_navigate(&h->menu, dir < 0 ? SV_NAV_LEFT : SV_NAV_RIGHT));
        }
      } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int i;
        SDL_ConvertEventToRenderCoordinates(h->renderer, &e);
        {
          const float mx = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x, my = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
          h->mouse_x = mx;
          h->mouse_y = my;
          for (i = 0; i < h->ui_rows; ++i)
            if (mx >= (float)h->ui_item_rect[i][0] && mx < (float)(h->ui_item_rect[i][0] + h->ui_item_rect[i][2]) && my >= (float)h->ui_item_rect[i][1] &&
                my < (float)(h->ui_item_rect[i][1] + h->ui_item_rect[i][3])) {
              SvPage *page = sv_menu_top(&h->menu);
              if (page != NULL) {
                const int index = page->scroll + i;
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && h->listen.kind == SV_LISTEN_NONE)
                  sv_pages_apply(h, sv_menu_pick(&h->menu, index, 1));
                else if (e.type == SDL_EVENT_MOUSE_MOTION) sv_menu_pick(&h->menu, index, 0);
              }
              break;
            }
        }
      } else if (e.type == SDL_EVENT_MOUSE_WHEEL && h->listen.kind == SV_LISTEN_NONE) {
        if (e.wheel.y > 0) sv_menu_navigate(&h->menu, SV_NAV_UP); else if (e.wheel.y < 0) sv_menu_navigate(&h->menu, SV_NAV_DOWN);
      }
      got = SDL_PollEvent(&e);
    }
    if (h->listen.kind != SV_LISTEN_NONE && SDL_GetTicks() - h->listen.started > 8000u) sv_pages_listen_cancel(h, "Binding cancelled (no input)");
    if (h->menu_open) {
      sv_pages_rebuild(h);
      draw_scene(h, 1);
      if (h->hook != NULL && h->hook(h->hook_ctx, h, iteration++) != 0) sv_pages_close(h);
      SDL_RenderPresent(h->renderer);
    }
  }
  sv_host_release_inputs(h);
  h->menu_open = 0;
  memset(&h->menu, 0, sizeof h->menu);
  save_settings_if_dirty(h);
  if (h->audio != NULL) { SDL_ClearAudioStream(h->audio); SDL_ResumeAudioStreamDevice(h->audio); }
  sv_host_apply_audio(h);
}

void sv_host_service(SvHost *h, SvSample *out) {
  SDL_Event e;
  int open_menu = 0, p;
  memset(out, 0, sizeof *out);
  while (SDL_PollEvent(&e)) {
    common_event(h, &e);
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
      const int sc = (int)e.key.scancode;
      if (is_menu_key(h, sc)) open_menu = 1;
      else if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT) != 0)) {
        if (sc == SDL_SCANCODE_RETURN) { h->key_held[sc] = 0; h->key_latch[sc] = 0; }
        sv_host_toggle_fullscreen(h);
      } else if (sc == SDL_SCANCODE_F12) sv_host_take_screenshot(h);
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
      if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK || e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) open_menu = 1;
    }
  }
  if (open_menu && !h->quit) {
    run_menu(h);
    out->menu_was_open = 1;
  }
  if (h->quit) { out->quit = 1; return; }
  for (p = 0; p < SV_PLAYERS; ++p) out->pad[p] = sample_player(h, p);
  memset(h->key_latch, 0, sizeof h->key_latch);
  { int i; for (i = 0; i < h->pad_count; ++i) h->pads[i].latched = 0; }
  if (h->reset) { out->reset = 1; h->reset = 0; }
}

/* ------------------------------------------------------------------------------------------------------------------ stop screen */

void sv_host_stop_screen(SvHost *h, const char *title, const char *detail) {
  const char *driver = SDL_GetCurrentVideoDriver();
  SDL_Event e;
  if (h == NULL || SDL_getenv("SEGARECOMP_VIEWER_NO_STOP_SCREEN") != NULL) return;
  if (driver != NULL && (strcmp(driver, "dummy") == 0 || strcmp(driver, "offscreen") == 0)) return;
  h->stop_screen = 1;
  sv_host_release_inputs(h);
  if (h->audio != NULL) SDL_PauseAudioStreamDevice(h->audio);
  while (!h->quit && h->stop_screen) {
    int out_w = 0, out_h = 0;
    if (SDL_WaitEventTimeout(&e, 33)) {
      do {
        common_event(h, &e);
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_SPACE)) h->stop_screen = 0;
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) h->stop_screen = 0;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat) sv_host_take_screenshot(h);
      } while (SDL_PollEvent(&e));
    }
    SDL_GetCurrentRenderOutputSize(h->renderer, &out_w, &out_h);
    draw_scene(h, 0);
    sv_draw_stop_screen(h, out_w, out_h, title, detail);
    if (h->hook != NULL && h->hook(h->hook_ctx, h, 0) != 0) h->stop_screen = 0;
    SDL_RenderPresent(h->renderer);
  }
  h->stop_screen = 0;
  save_settings_if_dirty(h);
}
