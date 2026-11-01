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

void sv_host_draw_text(SvHost *h, float x, float y, int scale, SDL_Color c, const char *text) { sv_ui_text(h, SV_FONT_BODY, scale, (int)x, (int)y, c, text); }

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

void sv_host_round_fill(SvHost *h, float x, float y, float w, float hh, float r, SDL_Color c) {
  enum { SEG = 6 };
  SDL_Vertex v[1 + (SEG + 1)];
  int corner, i;
  SDL_FColor fc;
  if (r > w * 0.5f) r = w * 0.5f;
  if (r > hh * 0.5f) r = hh * 0.5f;
  fc.r = (float)c.r / 255.0f; fc.g = (float)c.g / 255.0f; fc.b = (float)c.b / 255.0f; fc.a = (float)c.a / 255.0f;
  SDL_SetRenderDrawBlendMode(h->renderer, SDL_BLENDMODE_BLEND);
  {
    SDL_FRect a = {x + r, y, w - 2 * r, hh}, b = {x, y + r, r, hh - 2 * r}, d = {x + w - r, y + r, r, hh - 2 * r};
    SDL_SetRenderDrawColor(h->renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(h->renderer, &a);
    SDL_RenderFillRect(h->renderer, &b);
    SDL_RenderFillRect(h->renderer, &d);
  }
  for (corner = 0; corner < 4; ++corner) {
    const float cx = (corner & 1) ? x + w - r : x + r, cy = (corner & 2) ? y + hh - r : y + r;
    const float start = (float)(corner == 0 ? 180 : corner == 1 ? 270 : corner == 2 ? 90 : 0) * 3.14159265f / 180.0f;
    int idx[SEG * 3];
    v[0].position.x = cx; v[0].position.y = cy; v[0].color = fc; v[0].tex_coord.x = v[0].tex_coord.y = 0.0f;
    for (i = 0; i <= SEG; ++i) {
      const float ang = start + (float)i * (3.14159265f * 0.5f) / (float)SEG;
      v[1 + i].position.x = cx + r * SDL_cosf(ang); v[1 + i].position.y = cy + r * SDL_sinf(ang);
      v[1 + i].color = fc; v[1 + i].tex_coord.x = v[1 + i].tex_coord.y = 0.0f;
    }
    for (i = 0; i < SEG; ++i) { idx[i * 3] = 0; idx[i * 3 + 1] = 1 + i; idx[i * 3 + 2] = 2 + i; }
    SDL_RenderGeometry(h->renderer, NULL, v, 1 + SEG + 1, idx, SEG * 3);
  }
}

void sv_host_round_stroke(SvHost *h, float x, float y, float w, float hh, float r, int th, SDL_Color c) {
  /* an outline as an outer round fill with an inner fill punched back to the background is not possible with alpha, so draw the four straight
   * sides and the corner arcs as short lines */
  enum { SEG = 6 };
  int corner, i, t;
  SDL_SetRenderDrawBlendMode(h->renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(h->renderer, c.r, c.g, c.b, c.a);
  if (r > w * 0.5f) r = w * 0.5f;
  if (r > hh * 0.5f) r = hh * 0.5f;
  for (t = 0; t < th; ++t) {
    const float o = (float)t;
    SDL_FRect top = {x + r, y + o, w - 2 * r, 1}, bot = {x + r, y + hh - 1 - o, w - 2 * r, 1}, lef = {x + o, y + r, 1, hh - 2 * r}, rig = {x + w - 1 - o, y + r, 1, hh - 2 * r};
    SDL_RenderFillRect(h->renderer, &top); SDL_RenderFillRect(h->renderer, &bot); SDL_RenderFillRect(h->renderer, &lef); SDL_RenderFillRect(h->renderer, &rig);
    for (corner = 0; corner < 4; ++corner) {
      const float cx = (corner & 1) ? x + w - r : x + r, cy = (corner & 2) ? y + hh - r : y + r;
      const float start = (float)(corner == 0 ? 180 : corner == 1 ? 270 : corner == 2 ? 90 : 0) * 3.14159265f / 180.0f;
      float px_ = cx + (r - o) * SDL_cosf(start), py = cy + (r - o) * SDL_sinf(start);
      for (i = 1; i <= SEG; ++i) {
        const float ang = start + (float)i * (3.14159265f * 0.5f) / (float)SEG;
        const float nx = cx + (r - o) * SDL_cosf(ang), ny = cy + (r - o) * SDL_sinf(ang);
        SDL_RenderLine(h->renderer, px_, py, nx, ny);
        px_ = nx; py = ny;
      }
    }
  }
}

/* Loads `<SEGARECOMP_ASSETS>/<relative>` as a nearest-filtered texture; NULL when the assets folder or the file is missing (the caller omits the graphic). */
SDL_Texture *sv_host_load_asset(SvHost *h, const char *relative) {
  SDL_Texture *tex = NULL;
#if SDL_VERSION_ATLEAST(3, 4, 0)
  const char *dir = SDL_getenv("SEGARECOMP_ASSETS");
  if (dir != NULL && *dir != '\0' && relative != NULL) {
    char path[900];
    SDL_Surface *s;
    snprintf(path, sizeof path, "%s/%s", dir, relative);
    s = SDL_LoadSurface(path);
    if (s != NULL) { tex = SDL_CreateTextureFromSurface(h->renderer, s); SDL_DestroySurface(s); }
    if (tex != NULL) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  }
#else
  (void)h; (void)relative;
#endif
  return tex;
}

static SDL_Texture *load_icon(SvHost *h, const char *name) {
  int i;
  for (i = 0; i < h->icon_count; ++i) if (strcmp(h->icon_names[i], name) == 0) return h->icons[i];
  if (h->icon_count >= 24) return NULL;
  {
    char rel[96];
    SDL_Texture *tex;
    snprintf(rel, sizeof rel, "ui/icon-%s.png", name);
    tex = sv_host_load_asset(h, rel);
    snprintf(h->icon_names[h->icon_count], sizeof h->icon_names[0], "%s", name);
    h->icons[h->icon_count++] = tex; /* a missing icon is remembered as NULL: the UI simply omits it */
    return tex;
  }
}

/* File reads belong to host setup, never the presentation/event hot path. */
static void preload_ui_assets(SvHost *h) {
  static const char *const names[] = {"controller", "fullscreen", "video", "settings", "reset", "info", "audio", "screenshot", "play", "quit", "warning", "check"};
  size_t i;
  (void)sv_ui_wordmark(h);
  for (i = 0; i < sizeof names / sizeof names[0]; ++i) (void)load_icon(h, names[i]);
}

void sv_host_icon_draw(SvHost *h, const char *name, float x, float y, float size, SDL_Color tint) {
  SDL_Texture *tex = NULL;
  int i;
  SDL_FRect d;
  for (i = 0; name != NULL && i < h->icon_count; ++i)
    if (strcmp(name, h->icon_names[i]) == 0) { tex = h->icons[i]; break; }
  if (tex == NULL) return;
  d.x = x; d.y = y; d.w = size; d.h = size;
  SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
  SDL_SetTextureAlphaMod(tex, tint.a);
  SDL_RenderTexture(h->renderer, tex, NULL, &d);
}

void sv_host_draw_picture(SvHost *h, float x, float y, float w, float hh) {
  SvRect r;
  SDL_FRect d;
  if (h->frame_tex == NULL || h->last_w <= 0) return;
  r = sv_layout_dest((int)w, (int)hh, h->last_w, h->last_h, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  d.x = x + (float)r.x; d.y = y + (float)r.y; d.w = (float)r.w; d.h = (float)r.h;
  SDL_SetTextureColorMod(h->frame_tex, 255, 255, 255);
  SDL_SetTextureAlphaMod(h->frame_tex, 255);
  SDL_SetTextureBlendMode(h->frame_tex, SDL_BLENDMODE_NONE);
  { /* menu thumbnails show the native picture even when the texture holds an extended one */
    SDL_FRect src;
    src.x = (float)h->ext_side; src.y = 0.0f; src.w = (float)h->last_w; src.h = (float)h->last_h;
    SDL_RenderTexture(h->renderer, h->frame_tex, h->ext_side > 0 ? &src : NULL, &d);
  }
}

/* Whole-number UI scale from the OUTPUT size in pixels (so a 4K or a 1.5x-DPI window grows its text with the display): 13-pixel body text at 1x up to 540 px of
 * window height, 2x from ~900, 3x from ~1440, ... A wide-but-short window (the Master System's 256x192 default is 768x576) stays at 1x. */
int sv_host_ui_scale(int out_w, int out_h) {
  int s = (out_h + 180) / 540;
  const int by_w = out_w / 330;   /* the narrowest page (the main menu) is ~330 units wide */
  if (by_w < s) s = by_w;
  if (s < 1) s = 1;
  if (s > 6) s = 6;
  return s;
}

/* ------------------------------------------------------------------------------------------------------------------ gamepads */

static void pad_slot_release(SvPadSlot *slot) {
  slot->buttons = 0;
  slot->latched = 0;
  slot->ax = slot->ay = 0;
}

static void pad_open(SvHost *h, SDL_JoystickID id) {
  const SvDiagDevice *dev;
  int i;
  if (h->pad_count >= SV_MAX_PADS) return;
  for (i = 0; i < h->pad_count; ++i) if (h->pads[i].id == id) return;
  dev = sv_diag_find(h->diag, id);
  if (dev == NULL) { sv_diag_scan(h->diag); dev = sv_diag_find(h->diag, id); }
  if (dev == NULL || !dev->open_ok || sv_diag_gamepad(h->diag, id) == NULL) return; /* the diagnostics recorded and logged why */
  memset(&h->pads[h->pad_count], 0, sizeof h->pads[0]);
  h->pads[h->pad_count].id = id;
  snprintf(h->pads[h->pad_count].name, sizeof h->pads[0].name, "%s", dev->name[0] != '\0' ? dev->name : "Gamepad");
  h->pad_count++;
}

static void pad_close(SvHost *h, SDL_JoystickID id) { /* the diagnostics close the SDL handle when the device vanishes */
  int i;
  for (i = 0; i < h->pad_count; ++i) {
    if (h->pads[i].id != id) continue;
    memmove(&h->pads[i], &h->pads[i + 1], (size_t)(h->pad_count - i - 1) * sizeof h->pads[0]);
    h->pad_count--;
    return;
  }
}

/* Tell the user right away about a controller that is present but unusable (the failure must be visible, never silent). */
static void announce_pad_problem(SvHost *h, SDL_JoystickID id, int hot_plug) {
  const SvDiagDevice *dev = sv_diag_find(h->diag, id);
  char note[96];
  if (dev == NULL) return;
  if (!dev->is_gamepad) snprintf(note, sizeof note, "Controller \"%.28s\" is not a recognised gamepad (see Controls)", dev->name);
  else if (!dev->open_ok) snprintf(note, sizeof note, "Controller \"%.28s\" could not be opened (see Controls)", dev->name);
  else if (hot_plug) snprintf(note, sizeof note, "Controller connected: %.40s", dev->name);
  else return;
  sv_host_toast(h, note);
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
  sv_diag_event(h->diag, e); /* first: opens/closes the gamepads, counts the events */
  switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: h->quit = 1; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: sv_host_release_inputs(h); sv_diag_release(h->diag); break; /* nothing stays "held" behind the user's back */
    case SDL_EVENT_WINDOW_RESIZED:
      if ((SDL_GetWindowFlags(h->window) & SDL_WINDOW_FULLSCREEN) == 0 && e->window.data1 > 0 && e->window.data2 > 0) {
        h->settings.video.window_w = e->window.data1;
        h->settings.video.window_h = e->window.data2;
        sv_host_mark_dirty(h);
      }
      break;
    case SDL_EVENT_JOYSTICK_ADDED: announce_pad_problem(h, e->jdevice.which, 0); break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_open(h, e->gdevice.which); announce_pad_problem(h, e->gdevice.which, 1); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_close(h, e->gdevice.which); break;
    case SDL_EVENT_KEY_DOWN:
      if ((int)e->key.scancode >= 0 && e->key.scancode < 512) {
        if (!e->key.repeat) h->key_latch[e->key.scancode] = 1;
        h->key_held[e->key.scancode] = 1;
      }
      break;
    case SDL_EVENT_KEY_UP: if ((int)e->key.scancode >= 0 && e->key.scancode < 512) h->key_held[e->key.scancode] = 0; break;
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

static void capture_parse(SvHost *h);

static void ensure_texture(SvHost *h, int w, int hgt) {
  if (h->frame_tex != NULL && h->tex_w == w && h->tex_h == hgt) return;
  if (h->frame_tex != NULL) SDL_DestroyTexture(h->frame_tex);
  h->frame_tex = SDL_CreateTexture(h->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, hgt);
  if (h->tex_creations != UINT32_MAX) h->tex_creations++;
  h->tex_w = w;
  h->tex_h = hgt;
  if (h->frame_tex != NULL) SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_NEAREST);
}

static SDL_Texture *make_rgba_texture(SvHost *h, int w, int hgt, SDL_TextureAccess access, SDL_BlendMode blend) {
  SDL_Texture *t = SDL_CreateTexture(h->renderer, SDL_PIXELFORMAT_RGBA32, access, w, hgt);
  if (t != NULL) SDL_SetTextureBlendMode(t, blend);
  return t;
}

static void ensure_crt_targets(SvHost *h, SvCrtState *c, SvRect dest, int src_h) {
  const SvVideoSettings *v = &h->settings.video;
  if (c->rt == NULL || c->rt_w != dest.w || c->rt_h != dest.h) {
    if (c->rt != NULL) SDL_DestroyTexture(c->rt);
    c->rt = make_rgba_texture(h, dest.w, dest.h, SDL_TEXTUREACCESS_TARGET, SDL_BLENDMODE_NONE);
    c->rt_w = dest.w;
    c->rt_h = dest.h;
  }
  if (v->scanlines > 0 && (c->scan_tex == NULL || c->scan_h != dest.h || c->scan_src_h != src_h || c->scan_strength != v->scanlines)) {
    const float period = (float)dest.h / (float)src_h;
    uint32_t *pixels = (uint32_t *)malloc((size_t)dest.h * sizeof *pixels);
    int y;
    if (c->scan_tex != NULL) SDL_DestroyTexture(c->scan_tex);
    c->scan_tex = NULL;
    if (pixels != NULL) {
      for (y = 0; y < dest.h; ++y) {
        const uint8_t a = sv_crt_scanline_alpha(y, period, v->scanlines);
        const uint8_t bytes[4] = {0, 0, 0, a};
        memcpy(&pixels[y], bytes, 4); /* RGBA32 byte order: R,G,B,A */
      }
      c->scan_tex = make_rgba_texture(h, 1, dest.h, SDL_TEXTUREACCESS_STATIC, SDL_BLENDMODE_BLEND);
      if (c->scan_tex != NULL) SDL_UpdateTexture(c->scan_tex, NULL, pixels, 4);
      free(pixels);
    }
    c->scan_h = dest.h;
    c->scan_src_h = src_h;
    c->scan_strength = v->scanlines;
  }
  if (v->mask > 0 && (c->mask_tex == NULL || c->mask_w != dest.w || c->mask_strength != v->mask)) {
    uint8_t *pixels = (uint8_t *)malloc((size_t)dest.w * 4u);
    int x;
    if (c->mask_tex != NULL) SDL_DestroyTexture(c->mask_tex);
    c->mask_tex = NULL;
    if (pixels != NULL) {
      for (x = 0; x < dest.w; ++x) {
        pixels[x * 4 + 0] = sv_crt_mask_value(x, 0, v->mask);
        pixels[x * 4 + 1] = sv_crt_mask_value(x, 1, v->mask);
        pixels[x * 4 + 2] = sv_crt_mask_value(x, 2, v->mask);
        pixels[x * 4 + 3] = 255;
      }
      c->mask_tex = make_rgba_texture(h, dest.w, 1, SDL_TEXTUREACCESS_STATIC, SDL_BLENDMODE_MOD);
      if (c->mask_tex != NULL) SDL_UpdateTexture(c->mask_tex, NULL, pixels, dest.w * 4);
      free(pixels);
    }
    c->mask_w = dest.w;
    c->mask_strength = v->mask;
  }
}

static void crt_state_destroy(SvCrtState *c) {
  if (c->rt != NULL) SDL_DestroyTexture(c->rt);
  if (c->scan_tex != NULL) SDL_DestroyTexture(c->scan_tex);
  if (c->mask_tex != NULL) SDL_DestroyTexture(c->mask_tex);
  memset(c, 0, sizeof *c);
}

/* Draws `src` (NULL = the whole picture) of the last frame into `d` with the current video settings. Pixel Perfect (identity settings) is a single
 * nearest-neighbour copy; every other preset renders through a destination-sized target with real scanline, mask, bloom, brightness and curvature
 * passes. `src_rows` is the number of source lines `d` covers (it sets the scanline period). The main picture and the Video page's live preview share this
 * path and differ only in the target state `c`, the source crop and the destination. */
static void render_picture(SvHost *h, SvCrtState *c, const SDL_FRect *src, SDL_FRect d, int src_rows) {
  const SvVideoSettings *v = &h->settings.video;
  SvRect dest;
  dest.x = (int)d.x; dest.y = (int)d.y; dest.w = (int)d.w; dest.h = (int)d.h;
  SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_NEAREST);
  SDL_SetTextureColorMod(h->frame_tex, 255, 255, 255);
  SDL_SetTextureAlphaMod(h->frame_tex, 255);
  SDL_SetTextureBlendMode(h->frame_tex, SDL_BLENDMODE_NONE);
  if (sv_crt_is_identity(&h->settings.video)) {
    SDL_RenderTexture(h->renderer, h->frame_tex, src, &d);
    return;
  }
  ensure_crt_targets(h, c, dest, src_rows);
  if (c->rt == NULL) { SDL_RenderTexture(h->renderer, h->frame_tex, src, &d); return; }
  {
    SDL_FRect full;
    full.x = 0.0f; full.y = 0.0f; full.w = (float)dest.w; full.h = (float)dest.h;
    SDL_SetRenderTarget(h->renderer, c->rt);
    SDL_SetRenderDrawColor(h->renderer, 0, 0, 0, 255);
    SDL_SetRenderDrawBlendMode(h->renderer, SDL_BLENDMODE_NONE);
    SDL_RenderClear(h->renderer);
    { /* base picture, darkened when brightness < 100 */
      const int cc = v->brightness < 100 ? 255 * v->brightness / 100 : 255;
      SDL_SetTextureColorMod(h->frame_tex, (Uint8)cc, (Uint8)cc, (Uint8)cc);
      SDL_RenderTexture(h->renderer, h->frame_tex, src, &full);
    }
    SDL_SetTextureColorMod(h->frame_tex, 255, 255, 255);
    SDL_SetTextureBlendMode(h->frame_tex, SDL_BLENDMODE_ADD);
    if (v->brightness > 100) { /* add the picture to itself: color * (1 + a) */
      SDL_SetTextureAlphaMod(h->frame_tex, (Uint8)((v->brightness - 100) * 255 / 100));
      SDL_RenderTexture(h->renderer, h->frame_tex, src, &full);
    }
    if (v->bloom > 0) { /* soft glow: four offset, linearly filtered, additive taps */
      const float spread = fmaxf(1.0f, (float)dest.h / (float)src_rows * 0.7f);
      const Uint8 alpha = (Uint8)(v->bloom * 255 * 16 / 10000);
      static const float off[4][2] = {{-1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 1.0f}};
      int i;
      SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_LINEAR);
      SDL_SetTextureAlphaMod(h->frame_tex, alpha);
      for (i = 0; i < 4; ++i) {
        SDL_FRect t = full;
        t.x += off[i][0] * spread * 2.0f;
        t.y += off[i][1] * spread * 2.0f;
        SDL_RenderTexture(h->renderer, h->frame_tex, src, &t);
      }
      SDL_SetTextureScaleMode(h->frame_tex, SDL_SCALEMODE_NEAREST);
    }
    SDL_SetTextureAlphaMod(h->frame_tex, 255);
    SDL_SetTextureBlendMode(h->frame_tex, SDL_BLENDMODE_NONE);
    if (v->scanlines > 0 && c->scan_tex != NULL) SDL_RenderTexture(h->renderer, c->scan_tex, NULL, &full);
    if (v->mask > 0 && c->mask_tex != NULL) SDL_RenderTexture(h->renderer, c->mask_tex, NULL, &full);
    SDL_SetRenderTarget(h->renderer, NULL);
  }
  if (v->curvature <= 0) {
    SDL_SetTextureScaleMode(c->rt, SDL_SCALEMODE_NEAREST);
    SDL_RenderTexture(h->renderer, c->rt, NULL, &d);
    return;
  }
  { /* barrel-warped mesh of the finished picture */
    enum { N = 20 };
    SDL_Vertex verts[(N + 1) * (N + 1)];
    int indices[N * N * 6];
    int i, j, n = 0;
    SDL_SetTextureScaleMode(c->rt, SDL_SCALEMODE_LINEAR);
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
        const int a = j * (N + 1) + i, b = a + 1, cc = a + (N + 1), e = cc + 1;
        indices[n++] = a; indices[n++] = b; indices[n++] = cc; indices[n++] = b; indices[n++] = e; indices[n++] = cc;
      }
    SDL_RenderGeometry(h->renderer, c->rt, verts, (N + 1) * (N + 1), indices, n);
  }
}

static void render_game(SvHost *h, int out_w, int out_h) {
  const SvVideoSettings *v = &h->settings.video;
  SvRect dest;
  SDL_FRect d;
  if (h->frame_tex == NULL || h->last_w <= 0) return;
  if (h->ext_side > 0) {
    SvExtendedLayout el;
    if (sv_layout_extended_dest(out_w, out_h, h->last_w, h->last_h, h->ext_side, v->aspect, v->scale, &el) != 0) return;
    dest = el.surface;
  } else {
    dest = sv_layout_dest(out_w, out_h, h->last_w, h->last_h, v->aspect, v->scale);
  }
  d.x = (float)dest.x; d.y = (float)dest.y; d.w = (float)dest.w; d.h = (float)dest.h;
  render_picture(h, &h->crt, NULL, d, h->last_h);
}

/* The Video page's live preview: a centre crop of the picture (about 112 source pixels wide, so the scanlines and the mask are resolved) rendered with the
 * CURRENT settings through the same pipeline as the game. */
void sv_host_draw_preview(SvHost *h, int x, int y, int w, int hh) {
  SDL_FRect src, d;
  int cw, ch;
  if (h->frame_tex == NULL || h->last_w <= 0 || w < 8 || hh < 8) return;
  cw = h->last_w < 112 ? h->last_w : 112;
  ch = (int)((double)cw * (double)hh / (double)w + 0.5);
  if (ch > h->last_h) { ch = h->last_h; cw = (int)((double)ch * (double)w / (double)hh + 0.5); if (cw > h->last_w) cw = h->last_w; }
  if (ch < 1) ch = 1;
  src.x = (float)(h->ext_side + (h->last_w - cw) / 2); src.y = (float)((h->last_h - ch) / 2); src.w = (float)cw; src.h = (float)ch;
  d.x = (float)x; d.y = (float)y; d.w = (float)w; d.h = (float)hh;
  render_picture(h, &h->crt_preview, &src, d, ch);
}

/* The two always-available overlays: the optional FPS/status box (top left) and the transient notification (top right). Both use the bitmap font and the
 * stepped frames of the menu, scale with the display and never cover more than a small corner of the picture. */
static void draw_hud(SvHost *h, int out_w, int out_h) {
  const SDL_Color text = {220, 232, 255, 255}, dim = {169, 183, 221, 255}, box = {5, 12, 30, 224}, edge = {35, 62, 120, 255}, light = {77, 103, 167, 255};
  const int u = sv_host_ui_scale(out_w, out_h);
  const int lh = sv_ui_line_h(SV_FONT_BODY, u);
  if (h->settings.video.show_fps && !h->menu_open) {
    char l1[64], l2[64], l3[64];
    const char *aspect = h->settings.video.aspect == SV_ASPECT_4_3 ? "4:3" : "Square pixels";
    int w, k;
    snprintf(l1, sizeof l1, "System: %s   %.0f FPS", h->cfg.system == SV_SYSTEM_SMS ? "MASTER SYSTEM" : "GENESIS", h->fps);
    snprintf(l2, sizeof l2, "Aspect: %s", aspect);
    snprintf(l3, sizeof l3, "Effect: %s", sv_crt_preset_name(h->settings.video.preset));
    w = sv_ui_text_w(SV_FONT_BODY, u, l1);
    k = sv_ui_text_w(SV_FONT_BODY, u, l2); if (k > w) w = k;
    k = sv_ui_text_w(SV_FONT_BODY, u, l3); if (k > w) w = k;
    w += 16 * u;
    sv_ui_frame(h, 8 * u, 8 * u, w, 3 * lh + 12 * u, u, box, edge, light, 0);
    sv_ui_text_shadow(h, SV_FONT_BODY, u, 16 * u, 14 * u, text, l1);
    sv_ui_text_shadow(h, SV_FONT_BODY, u, 16 * u, 14 * u + lh, dim, l2);
    sv_ui_text_shadow(h, SV_FONT_BODY, u, 16 * u, 14 * u + 2 * lh, dim, l3);
  }
  {
    int by = 8 * u, i, live = 0;
    const Uint64 now_ms = SDL_GetTicks();
    for (i = 0; i < h->toast_count; ++i) /* drop the expired ones, keep the order */
      if (now_ms < h->toasts[i].until) { if (live != i) h->toasts[live] = h->toasts[i]; ++live; }
    h->toast_count = live;
    for (i = 0; i < h->toast_count; ++i) {
      const char *text_in = h->toasts[i].text;
      char title[96], sub[96];
      const char *colon = strstr(text_in, ": ");
      const int shot = strncmp(text_in, "Screenshot", 10) == 0;
      const int icon_w = 22 * u;
      int w, tw, sw = 0, bx, bh;
      int avail = out_w - 24 * u - icon_w - 20 * u;
      if (avail < 40 * u) avail = 40 * u;
      if (colon != NULL && (size_t)(colon - text_in) < sizeof title) {
        snprintf(title, sizeof title, "%.*s", (int)(colon - text_in), text_in);
        snprintf(sub, sizeof sub, "%s", colon + 2);
      } else { snprintf(title, sizeof title, "%s", text_in); sub[0] = '\0'; }
      { char t2[96]; sv_font_ellipsize(SV_FONT_BODY, title, u, avail, t2, (int)sizeof t2); snprintf(title, sizeof title, "%s", t2); }
      if (sub[0] != '\0') { char s2[96]; sv_font_ellipsize(SV_FONT_BODY, sub, u, avail, s2, (int)sizeof s2); snprintf(sub, sizeof sub, "%s", s2); }
      tw = sv_ui_text_w(SV_FONT_BODY, u, title);
      if (sub[0] != '\0') sw = sv_ui_text_w(SV_FONT_BODY, u, sub);
      if (sw > tw) tw = sw;
      w = tw + icon_w + 22 * u;
      bh = (sub[0] != '\0' ? 2 : 1) * lh + 14 * u;
      bx = out_w - w - 8 * u;
      sv_ui_frame(h, bx, by, w, bh, u, box, edge, light, 1);
      sv_host_icon_draw(h, shot ? "screenshot" : "info", (float)(bx + 8 * u), (float)(by + (bh - 16 * u) / 2), (float)(16 * u), text);
      sv_ui_text_shadow(h, SV_FONT_BODY, u, bx + icon_w + 12 * u, by + 7 * u, text, title);
      if (sub[0] != '\0') sv_ui_text_shadow(h, SV_FONT_BODY, u, bx + icon_w + 12 * u, by + 7 * u + lh, dim, sub);
      by += bh + 4 * u;
    }
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
    const SvPage *top = sv_menu_top(&h->menu);
    SDL_Color dim = {4, 8, 20, 150};
    if (top != NULL && top->page_id >= PAGE_CONFIRM_QUIT) dim.a = 196; /* a decision: the game recedes further */
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

static void push_toast(SvHost *h, const char *text, Uint64 ms) {
  int i;
  if (text == NULL || *text == '\0') { h->toast_count = 0; return; } /* an empty text clears every notification */
  for (i = 0; i < h->toast_count; ++i) /* the same text again refreshes it in place */
    if (strcmp(h->toasts[i].text, text) == 0) { h->toasts[i].until = SDL_GetTicks() + ms; return; }
  if (h->toast_count == (int)(sizeof h->toasts / sizeof h->toasts[0])) { /* full: the oldest makes room, the newest are never dropped */
    memmove(&h->toasts[0], &h->toasts[1], sizeof h->toasts[0] * (size_t)(h->toast_count - 1));
    h->toast_count--;
  }
  snprintf(h->toasts[h->toast_count].text, sizeof h->toasts[0].text, "%s", text);
  h->toasts[h->toast_count].until = SDL_GetTicks() + ms;
  h->toast_count++;
}

void sv_host_toast(SvHost *h, const char *text) { push_toast(h, text, 2600u); }

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

/* A user-supplied gamecontrollerdb.txt next to the viewer settings (or SEGARECOMP_GAMEPAD_MAPPINGS) lets a joystick SDL has no mapping for become an
 * explicit gamepad. Nothing is shipped or guessed. */
static void load_user_mappings(SvHost *h) {
  const char *env = SDL_getenv("SEGARECOMP_GAMEPAD_MAPPINGS");
  char path[1100];
  (void)h;
  if (env != NULL && *env != '\0') { (void)sv_diag_load_mapping_file(env); return; }
  if (h->settings_path[0] != '\0') {
    const char *slash = strrchr(h->settings_path, '/');
    const size_t dir_len = slash != NULL ? (size_t)(slash - h->settings_path) : 0;
    if (dir_len > 0 && dir_len < 1000) {
      SDL_PathInfo info;
      snprintf(path, sizeof path, "%.*s/gamecontrollerdb.txt", (int)dir_len, h->settings_path);
      if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE) (void)sv_diag_load_mapping_file(path);
    }
  }
}

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
  h->diag = sv_diag_create();
  if (h->diag == NULL) { free(h); return NULL; }
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    char why[160];
    snprintf(why, sizeof why, "%s", SDL_GetError());
    if (!SDL_Init(SDL_INIT_VIDEO)) { sv_diag_destroy(h->diag); free(h); return NULL; }
    sv_diag_note_init(h->diag, 1, 0, why); /* no gamepad subsystem: keyboard only, and it is logged and shown in Controls */
  }
  load_user_mappings(h);
  {
    const int scale = cfg->default_scale > 0 && cfg->default_scale <= 16 ? cfg->default_scale : 3;
    w = h->settings.video.window_w > 0 ? h->settings.video.window_w : cfg->picture_w * scale;
    hh = h->settings.video.window_h > 0 ? h->settings.video.window_h : cfg->picture_h * scale;
  }
  { const char *drv = SDL_GetCurrentVideoDriver(); const int headless = drv != NULL && (strcmp(drv, "dummy") == 0 || strcmp(drv, "offscreen") == 0); /* no real display to fit into */
  if (!headless && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0) {
    if (w > usable.w * 9 / 10) { hh = hh * (usable.w * 9 / 10) / w; w = usable.w * 9 / 10; }
    if (hh > usable.h * 9 / 10) { w = w * (usable.h * 9 / 10) / hh; hh = usable.h * 9 / 10; }
  }
  }
  h->window = SDL_CreateWindow(cfg->title != NULL ? cfg->title : "Segarecomp", w, hh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (h->window == NULL) { sv_diag_destroy(h->diag); SDL_Quit(); free(h); return NULL; }
  SDL_SetWindowMinimumSize(h->window, cfg->picture_w / 2 > 160 ? cfg->picture_w / 2 : 160, cfg->picture_h / 2 > 120 ? cfg->picture_h / 2 : 120);
  h->renderer = SDL_CreateRenderer(h->window, NULL);
  if (h->renderer == NULL) { SDL_DestroyWindow(h->window); sv_diag_destroy(h->diag); SDL_Quit(); free(h); return NULL; }
  h->last_rgb = (uint8_t *)malloc((size_t)(cfg->max_w > 0 ? cfg->max_w : 320) * (size_t)(cfg->max_h > 0 ? cfg->max_h : 240) * 3u);
  if (h->last_rgb == NULL) { sv_host_close(h); return NULL; }
  {
    const size_t mw = (size_t)(cfg->max_w > 0 ? cfg->max_w : 320), mh = (size_t)(cfg->max_h > 0 ? cfg->max_h : 240);
    h->ext_raw = (uint8_t *)malloc((mw + 2u * SV_EXT_MAX_SIDE) * mh * 3u);
    h->side_buf_cap = 2u * SV_EXT_MAX_SIDE * mh * 3u;
    h->side_buf = (uint8_t *)malloc(h->side_buf_cap);
    h->side_scratch = malloc(sv_side_scratch_bytes((int)mh) > 0 ? sv_side_scratch_bytes((int)mh) : 1u);
    if (h->ext_raw == NULL || h->side_buf == NULL || h->side_scratch == NULL) { sv_host_close(h); return NULL; }
  }
  sv_host_apply_video(h);
  capture_parse(h);
  preload_ui_assets(h);
  h->session_start = SDL_GetTicks();
  sv_diag_scan(h->diag);
  ids = SDL_GetGamepads(&count);
  if (ids != NULL) { for (i = 0; i < count; ++i) pad_open(h, ids[i]); SDL_free(ids); }
  { int k; for (k = 0; k < sv_diag_count(h->diag); ++k) { const SvDiagDevice *dv = sv_diag_device(h->diag, k); if (dv != NULL && (!dv->is_gamepad || !dv->open_ok)) announce_pad_problem(h, dv->id, 0); } }
  sv_diag_log_summary(h->diag, "viewer start");
  return h;
}

void sv_host_close(SvHost *h) {
  if (h == NULL) return;
  if (h->hid_thread != NULL) SDL_WaitThread(h->hid_thread, NULL);
  save_settings_if_dirty(h);
  sv_diag_destroy(h->diag);
  if (h->frame_tex != NULL) SDL_DestroyTexture(h->frame_tex);
  crt_state_destroy(&h->crt);
  crt_state_destroy(&h->crt_preview);
  sv_ui_destroy(h);
  { int k; for (k = 0; k < h->icon_count; ++k) if (h->icons[k] != NULL) SDL_DestroyTexture(h->icons[k]); }
  if (h->renderer != NULL) SDL_DestroyRenderer(h->renderer);
  if (h->window != NULL) SDL_DestroyWindow(h->window);
  free(h->last_rgb);
  free(h->ext_raw);
  free(h->side_buf);
  free(h->side_scratch);
  SDL_Quit();
  free(h);
}

void sv_host_set_audio(SvHost *h, SDL_AudioStream *stream) {
  h->audio = stream;
  sv_host_apply_audio(h);
}

const SvSettings *sv_host_settings(const SvHost *h) { return &h->settings; }

void sv_host_widescreen_caps(const SvHost *h, SvWidescreenCaps *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof *out); /* nothing reported == Unavailable */
  if (h != NULL && h->cfg.widescreen_caps != NULL) h->cfg.widescreen_caps(h->cfg.widescreen_ctx, out);
}
SvWidescreenMode sv_host_widescreen_effective(const SvHost *h) {
  SvWidescreenCaps caps;
  if (h == NULL) return SV_WIDESCREEN_ORIGINAL;
  sv_host_widescreen_caps(h, &caps);
  return sv_widescreen_effective(h->settings.video.widescreen, &caps);
}
uint32_t sv_host_widescreen_fallbacks(const SvHost *h) { return h != NULL ? h->widescreen_fallbacks : 0u; }

void sv_host_apply_settings(SvHost *h, const SvSettings *settings) {
  h->settings = *settings;
  sv_settings_sanitize(&h->settings);
  sv_host_apply_video(h);
  sv_host_apply_audio(h);
  sv_host_restyle(h);
  sv_host_mark_dirty(h);
}
const char *sv_host_renderer_name(const SvHost *h) { const char *n = SDL_GetRendererName(h->renderer); return n != NULL ? n : "unknown"; }
int sv_host_menu_open(const SvHost *h) { return h->menu_open; }

SDL_Surface *sv_host_capture_output(SvHost *h) {
  draw_scene(h, 1);
  return SDL_RenderReadPixels(h->renderer, NULL);
}

/* ------------------------------------------------------------------------------------------------------------------ present */

/* SEG-123: builds the frame texture from the retained native picture (+ retained raw extension). Deterministic in (style, source, native pixels, raw
 * extension); the native pixels are always the centre; no allocation. Returns 1 on success. */
static int upload_picture(SvHost *h) {
  const int w = h->last_w, hgt = h->last_h, side = h->ext_side;
  SDL_Rect centre, l, r;
  SvSideTreatment t = SV_TREAT_PASSTHROUGH;
  if (h->frame_tex == NULL || w <= 0 || hgt <= 0) return 0;
  if (side <= 0) {
    h->side_applied = SV_TREAT_PASSTHROUGH;
    return SDL_UpdateTexture(h->frame_tex, NULL, h->last_rgb, w * 3) ? 1 : 0;
  }
  centre.x = side; centre.y = 0; centre.w = w; centre.h = hgt;
  t = sv_side_treatment(h->settings.video.widescreen_side_style, h->ext_source);
  if (t != SV_TREAT_PASSTHROUGH &&
      (hgt > SV_SIDE_MAX_ROWS || (size_t)2u * (size_t)side * (size_t)hgt * 3u > h->side_buf_cap ||
       sv_side_fill(t, h->last_rgb, w, hgt, w * 3, side, h->side_buf, h->side_buf + (size_t)side * (size_t)hgt * 3u, h->side_scratch) != 0))
    t = SV_TREAT_PASSTHROUGH; /* unreachable for validated geometry */
  if (t == SV_TREAT_PASSTHROUGH) {
    if (!SDL_UpdateTexture(h->frame_tex, NULL, h->ext_raw, (w + 2 * side) * 3)) return 0;
  } else {
    l.x = 0; l.y = 0; l.w = side; l.h = hgt;
    r.x = side + w; r.y = 0; r.w = side; r.h = hgt;
    if (!SDL_UpdateTexture(h->frame_tex, &l, h->side_buf, side * 3)) return 0;
    if (!SDL_UpdateTexture(h->frame_tex, &r, h->side_buf + (size_t)side * (size_t)hgt * 3u, side * 3)) return 0;
  }
  if (!SDL_UpdateTexture(h->frame_tex, &centre, h->last_rgb, w * 3)) return 0; /* the native pixels always come from the native picture */
  h->side_applied = t;
  return 1;
}

/* Re-applies the current side style to the frame already shown (a style change while the pause menu holds the guest). A retained surface produced
 * under a higher level than the one now requested is not re-exposed. */
void sv_host_restyle(SvHost *h) {
  if (h == NULL || h->ext_side <= 0 || h->last_w <= 0) return;
  if ((int)h->ext_prod_mode > (int)h->settings.video.widescreen) return;
  (void)upload_picture(h);
}
SvSideTreatment sv_host_side_treatment(const SvHost *h) { return h != NULL ? h->side_applied : SV_TREAT_PASSTHROUGH; }
SvExtSource sv_host_extension_source(const SvHost *h) { return h != NULL && h->ext_side > 0 ? h->ext_source : SV_EXTSRC_GENERIC; }

/* Automation seam (SEG-123): SEGARECOMP_VIEWER_CAPTURE_DIR + SEGARECOMP_VIEWER_CAPTURE_FRAMES="n,n,..." save the composed window output (picture, side style,
 * CRT; no overlay) of those presented frames as BMP. Read-only: it never touches the guest, the picture or any setting. */
static void capture_parse(SvHost *h) {
  const char *dir = SDL_getenv("SEGARECOMP_VIEWER_CAPTURE_DIR"), *list = SDL_getenv("SEGARECOMP_VIEWER_CAPTURE_FRAMES");
  h->capture_count = 0;
  h->capture_all_styles = SDL_getenv("SEGARECOMP_VIEWER_CAPTURE_ALL_STYLES") != NULL;
  if (dir == NULL || list == NULL || *dir == '\0' || strlen(dir) >= sizeof h->capture_dir) return;
  snprintf(h->capture_dir, sizeof h->capture_dir, "%s", dir);
  while (*list != '\0' && h->capture_count < 8) {
    char *end = NULL;
    const unsigned long long n = strtoull(list, &end, 10);
    if (end == list) break;
    if (n > 0) h->capture_frames[h->capture_count++] = (uint64_t)n;
    list = *end == ',' ? end + 1 : end;
    if (*end != ',') break;
  }
}
static void capture_save(SvHost *h, const char *suffix) {
  SDL_Surface *s = SDL_RenderReadPixels(h->renderer, NULL);
  char path[700];
  if (s == NULL) return;
  snprintf(path, sizeof path, "%s/frame-%llu%s.bmp", h->capture_dir, (unsigned long long)h->frames_presented, suffix);
  (void)SDL_SaveBMP(s, path);
  SDL_DestroySurface(s);
}
static void capture_if_requested(SvHost *h) {
  int k;
  for (k = 0; k < h->capture_count; ++k)
    if (h->capture_frames[k] == h->frames_presented) {
      capture_save(h, "");
      if (h->capture_all_styles && h->ext_side > 0) {
        /* the SAME frame in every side style (and the current one restored), from the retained picture and extension: a timing-independent comparison */
        static const char *const suffix[SV_SIDE_COUNT] = {"-comfort", "-borders", "-raw"};
        const SvSideStyle keep = h->settings.video.widescreen_side_style;
        int st;
        for (st = 0; st < SV_SIDE_COUNT; ++st) {
          h->settings.video.widescreen_side_style = (SvSideStyle)st;
          if (upload_picture(h)) { draw_scene(h, 0); capture_save(h, suffix[st]); }
        }
        h->settings.video.widescreen_side_style = keep;
        (void)upload_picture(h);
        draw_scene(h, 0);
      }
    }
}

static int present_frame(SvHost *h, const uint8_t *rgb, int w, int hgt, int pitch, const SvExtendedSurface *ext) {
  int y;
  const Uint64 now = SDL_GetTicksNS();
  const int side = ext != NULL ? ext->side : 0;
  if (rgb == NULL || w <= 0 || hgt <= 0 || w > (h->cfg.max_w > 0 ? h->cfg.max_w : 320) || hgt > (h->cfg.max_h > 0 ? h->cfg.max_h : 240) || pitch < w * 3) return -1;
  ensure_texture(h, w + 2 * side, hgt);
  if (h->frame_tex == NULL) return -1;
  /* the native picture and (when accepted) a private copy of the extension are retained first: the texture is then built from them by upload_picture,
   * the same code that re-applies the side style at once when it changes in the paused menu */
  for (y = 0; y < hgt; ++y) memcpy(h->last_rgb + (size_t)y * (size_t)w * 3u, rgb + (size_t)y * (size_t)pitch, (size_t)w * 3u);
  h->last_w = w;
  h->last_h = hgt;
  h->ext_side = side;
  if (ext != NULL) {
    const size_t row = (size_t)(w + 2 * side) * 3u;
    for (y = 0; y < hgt; ++y) memcpy(h->ext_raw + (size_t)y * row, ext->rgb24 + (size_t)y * (size_t)ext->pitch, row);
    h->ext_source = ext->source == SV_EXTSRC_AUTHORED ? SV_EXTSRC_AUTHORED : SV_EXTSRC_GENERIC;
    h->ext_prod_mode = ext->mode;
  }
  if (!upload_picture(h)) return -1;
  h->frames_presented++;
  if (h->frames_presented == 1) { /* first picture: say once how to reach the pause menu (the keys that actually open it); shown beside any other toast */
    const char *custom = h->settings.menu_key > 0 && h->settings.menu_key != SV_KEY_ESCAPE ? SDL_GetScancodeName((SDL_Scancode)h->settings.menu_key) : NULL;
    char hint[96];
    if (custom != NULL && *custom != '\0') snprintf(hint, sizeof hint, "Pause menu: Esc, %.24s or the gamepad's Back button", custom);
    else snprintf(hint, sizeof hint, "Pause menu: Esc or the gamepad's Back button");
    push_toast(h, hint, 6000u);
  }
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
  if (h->capture_count > 0) capture_if_requested(h);
  return SDL_RenderPresent(h->renderer) ? 0 : -1;
}

int sv_host_present(SvHost *h, const uint8_t *rgb, int w, int hgt, int pitch) { return present_frame(h, rgb, w, hgt, pitch, NULL); }

int sv_host_present_extended(SvHost *h, const uint8_t *rgb, int w, int hgt, int pitch, uint64_t sequence, const SvExtendedSurface *ext) {
  if (ext != NULL) {
    /* SEG-121: a surface produced under a higher level than the one now requested (the user stepped down while a frame was pending) is stale
     * for presentation: the picture shown must never contain Level 3 sprites once Level 2 or Original is selected. */
    if ((int)ext->mode <= (int)h->settings.video.widescreen &&
        sv_extended_surface_check(ext, w, hgt, sequence, h->cfg.max_w > 0 ? h->cfg.max_w : 320) == SV_EXT_OK) {
      if (h->ext_accepted != UINT32_MAX) h->ext_accepted++;
      return present_frame(h, rgb, w, hgt, pitch, ext);
    }
    /* An UNAVAILABLE surface that was not even requested is the normal Original case; anything else offered and refused is a rejection. */
    if (ext->status != SV_WS_UNAVAILABLE && h->ext_rejected != UINT32_MAX) h->ext_rejected++;
  }
  return present_frame(h, rgb, w, hgt, pitch, NULL);
}

int sv_host_extension_side(const SvHost *h) { return h != NULL ? h->ext_side : 0; }
void sv_host_extension_stats(const SvHost *h, uint32_t *accepted, uint32_t *rejected, uint32_t *texture_creations) {
  if (accepted != NULL) *accepted = h != NULL ? h->ext_accepted : 0u;
  if (rejected != NULL) *rejected = h != NULL ? h->ext_rejected : 0u;
  if (texture_creations != NULL) *texture_creations = h != NULL ? h->tex_creations : 0u;
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

static int SDLCALL hid_worker(void *ctx) {
  SvHost *h = (SvHost *)ctx;
  /* No shared live diagnostic tables in the worker. Match against SDL on the host thread after completion. */
  h->hid_pending_count = sv_diag_hid_scan(NULL, h->hid_pending, SV_DIAG_MAX_HID, h->hid_error, sizeof h->hid_error);
  SDL_SetAtomicInt(&h->hid_done, 1);
  return 0;
}

void sv_host_start_hid_scan(SvHost *h) {
  if (h->hid_thread != NULL) return;
  SDL_SetAtomicInt(&h->hid_done, 0);
  h->hid_thread = SDL_CreateThread(hid_worker, "viewer-hid-scan", h);
  if (h->hid_thread == NULL) sv_host_toast(h, "Could not start OS device scan");
}

static void finish_hid_scan(SvHost *h) {
  int i, k;
  if (h->hid_thread == NULL || !SDL_GetAtomicInt(&h->hid_done)) return;
  SDL_WaitThread(h->hid_thread, NULL);
  h->hid_thread = NULL;
  h->hid_count = h->hid_pending_count;
  h->hid_scanned = 1;
  memcpy(h->hid, h->hid_pending, sizeof h->hid);
  for (i = 0; i < h->hid_count; ++i)
    for (k = 0; k < sv_diag_count(h->diag); ++k) {
      const SvDiagDevice *d = sv_diag_device(h->diag, k);
      if (d->vendor == h->hid[i].vendor && d->product == h->hid[i].product) h->hid[i].matched_joystick = 1;
    }
  if (h->hid_count < 0) sv_host_toast(h, h->hid_error[0] ? h->hid_error : "OS device scan failed");
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
        else if (b == SDL_GAMEPAD_BUTTON_BACK || b == SDL_GAMEPAD_BUTTON_GUIDE) sv_pages_close(h);
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
          if (!(e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && h->listen.kind == SV_LISTEN_NONE && sv_pages_click(h, mx, my)))
          for (i = 0; i < h->ui_rows; ++i)
            if (mx >= (float)h->ui_item_rect[i][0] && mx < (float)(h->ui_item_rect[i][0] + h->ui_item_rect[i][2]) && my >= (float)h->ui_item_rect[i][1] &&
                my < (float)(h->ui_item_rect[i][1] + h->ui_item_rect[i][3])) {
              SvPage *page = sv_menu_top(&h->menu);
              if (page != NULL) {
                const int index = h->ui_item_index[i];
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
      finish_hid_scan(h);
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
