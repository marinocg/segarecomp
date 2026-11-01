#include "sv_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SV_SETTINGS_MAX_TEXT 32768u

static const char *const system_keys[SV_SYSTEM_COUNT] = {"genesis", "sms"};
static const char *const action_keys[SV_ACT_COUNT] = {"up", "down", "left", "right", "b1", "b2", "b3", "start", "pause"};
static const char *const preset_names[SV_CRT_COUNT] = {"Pixel Perfect", "CRT Soft", "CRT Classic", "CRT Arcade", "Custom"};

const char *sv_system_name(SvSystem system) { return system == SV_SYSTEM_SMS ? "Master System" : "Genesis"; }
const char *sv_crt_preset_name(SvCrtPreset preset) { return (unsigned)preset < SV_CRT_COUNT ? preset_names[preset] : preset_names[0]; }

const char *sv_action_label(SvSystem system, SvAction action) {
  static const char *const genesis[SV_ACT_COUNT] = {"Up", "Down", "Left", "Right", "A", "B", "C", "Start", "-"};
  static const char *const sms[SV_ACT_COUNT] = {"Up", "Down", "Left", "Right", "Button 1", "Button 2", "-", "-", "Pause"};
  if ((unsigned)action >= SV_ACT_COUNT) return "-";
  return system == SV_SYSTEM_SMS ? sms[action] : genesis[action];
}

int sv_action_available(SvSystem system, int player, SvAction action) {
  if (player < 0 || player >= SV_PLAYERS || (unsigned)action >= SV_ACT_COUNT) return 0;
  switch (action) {
    case SV_ACT_UP: case SV_ACT_DOWN: case SV_ACT_LEFT: case SV_ACT_RIGHT: case SV_ACT_BUTTON1: case SV_ACT_BUTTON2: return 1;
    case SV_ACT_BUTTON3: case SV_ACT_START: return system == SV_SYSTEM_GENESIS;
    case SV_ACT_PAUSE: return system == SV_SYSTEM_SMS && player == 0; /* the Pause button is a console button, wired to Player 1's slot only */
    default: return 0;
  }
}

void sv_video_defaults(SvVideoSettings *v) {
  memset(v, 0, sizeof *v);
  v->scale = SV_SCALE_FIT;
  v->aspect = SV_ASPECT_ORIGINAL;
  v->preset = SV_CRT_PIXEL_PERFECT;
  v->brightness = 100;
}

void sv_apply_crt_preset(SvVideoSettings *v, SvCrtPreset preset) {
  static const int table[SV_CRT_COUNT][5] = {/* scanlines, mask, curvature, bloom, brightness */
                                             {0, 0, 0, 0, 100}, {28, 0, 0, 12, 105}, {48, 35, 12, 20, 110}, {64, 55, 24, 32, 118}, {0, 0, 0, 0, 100}};
  if ((unsigned)preset >= SV_CRT_COUNT) preset = SV_CRT_PIXEL_PERFECT;
  v->preset = preset;
  if (preset == SV_CRT_CUSTOM) return;
  v->scanlines = table[preset][0];
  v->mask = table[preset][1];
  v->curvature = table[preset][2];
  v->bloom = table[preset][3];
  v->brightness = table[preset][4];
}

void sv_bindings_defaults(SvSettings *s, SvSystem system) {
  SvBindings *b = &s->bind[system];
  int p, a;
  for (p = 0; p < SV_PLAYERS; ++p)
    for (a = 0; a < SV_ACT_COUNT; ++a) { b->key[p][a] = 0; b->pad[p][a] = -1; }
  /* Player 1 keyboard: arrows, Z/X/C, Return; the SMS Pause button is P (a console button, distinct from the host menu). */
  b->key[0][SV_ACT_UP] = SV_KEY_UP; b->key[0][SV_ACT_DOWN] = SV_KEY_DOWN; b->key[0][SV_ACT_LEFT] = SV_KEY_LEFT; b->key[0][SV_ACT_RIGHT] = SV_KEY_RIGHT;
  b->key[0][SV_ACT_BUTTON1] = SV_KEY_Z; b->key[0][SV_ACT_BUTTON2] = SV_KEY_X;
  if (system == SV_SYSTEM_GENESIS) { b->key[0][SV_ACT_BUTTON3] = SV_KEY_C; b->key[0][SV_ACT_START] = SV_KEY_RETURN; }
  else b->key[0][SV_ACT_PAUSE] = SV_KEY_P;
  /* Player 2 keyboard: I/K/J/L, U/O(/P), Right Shift. */
  b->key[1][SV_ACT_UP] = SV_KEY_I; b->key[1][SV_ACT_DOWN] = SV_KEY_K; b->key[1][SV_ACT_LEFT] = SV_KEY_J; b->key[1][SV_ACT_RIGHT] = SV_KEY_L;
  b->key[1][SV_ACT_BUTTON1] = SV_KEY_U; b->key[1][SV_ACT_BUTTON2] = SV_KEY_O;
  if (system == SV_SYSTEM_GENESIS) { b->key[1][SV_ACT_BUTTON3] = SV_KEY_P; b->key[1][SV_ACT_START] = SV_KEY_RSHIFT; }
  /* Gamepads: the same layout for both players (each player uses the pad assigned to its slot). */
  for (p = 0; p < SV_PLAYERS; ++p) {
    b->pad[p][SV_ACT_UP] = SV_PAD_DPAD_UP; b->pad[p][SV_ACT_DOWN] = SV_PAD_DPAD_DOWN; b->pad[p][SV_ACT_LEFT] = SV_PAD_DPAD_LEFT;
    b->pad[p][SV_ACT_RIGHT] = SV_PAD_DPAD_RIGHT; b->pad[p][SV_ACT_BUTTON1] = SV_PAD_SOUTH; b->pad[p][SV_ACT_BUTTON2] = SV_PAD_EAST;
    if (system == SV_SYSTEM_GENESIS) { b->pad[p][SV_ACT_BUTTON3] = SV_PAD_WEST; b->pad[p][SV_ACT_START] = SV_PAD_START; }
  }
  if (system == SV_SYSTEM_SMS) b->pad[0][SV_ACT_PAUSE] = SV_PAD_START;
}

void sv_settings_defaults(SvSettings *s) {
  memset(s, 0, sizeof *s);
  sv_video_defaults(&s->video);
  s->audio.volume = 100;
  s->deadzone = 25;
  s->menu_key = SV_KEY_ESCAPE;
  sv_bindings_defaults(s, SV_SYSTEM_GENESIS);
  sv_bindings_defaults(s, SV_SYSTEM_SMS);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void sv_settings_sanitize(SvSettings *s) {
  SvVideoSettings *v = &s->video;
  int sys, p, a;
  v->fullscreen = v->fullscreen != 0;
  v->scale = (v->scale == SV_SCALE_INTEGER) ? SV_SCALE_INTEGER : SV_SCALE_FIT;
  v->aspect = (v->aspect == SV_ASPECT_4_3) ? SV_ASPECT_4_3 : SV_ASPECT_ORIGINAL;
  if ((unsigned)v->widescreen >= SV_WIDESCREEN_COUNT) v->widescreen = SV_WIDESCREEN_ORIGINAL;
  if ((unsigned)v->widescreen_side_style >= SV_SIDE_COUNT) v->widescreen_side_style = SV_SIDE_COMFORT;
  if ((unsigned)v->preset >= SV_CRT_COUNT) v->preset = SV_CRT_PIXEL_PERFECT;
  v->scanlines = clampi(v->scanlines, 0, 100);
  v->mask = clampi(v->mask, 0, 100);
  v->curvature = clampi(v->curvature, 0, 100);
  v->bloom = clampi(v->bloom, 0, 100);
  v->brightness = clampi(v->brightness, 50, 150);
  v->vsync = v->vsync != 0;
  v->show_fps = v->show_fps != 0;
  v->window_w = (v->window_w >= 160 && v->window_w <= 16384) ? v->window_w : 0;
  v->window_h = (v->window_h >= 120 && v->window_h <= 16384) ? v->window_h : 0;
  s->audio.volume = clampi(s->audio.volume, 0, 100);
  s->audio.muted = s->audio.muted != 0;
  s->deadzone = clampi(s->deadzone, 5, 60);
  if (s->menu_key <= 0 || s->menu_key >= 512) s->menu_key = SV_KEY_ESCAPE;
  s->pad_name[sizeof s->pad_name - 1u] = '\0';
  for (sys = 0; sys < SV_SYSTEM_COUNT; ++sys)
    for (p = 0; p < SV_PLAYERS; ++p)
      for (a = 0; a < SV_ACT_COUNT; ++a) {
        int *k = &s->bind[sys].key[p][a], *b = &s->bind[sys].pad[p][a];
        if (*k < 0 || *k >= 512 || sv_key_reserved(s, *k)) *k = 0;
        if (*b < -1 || *b > 30) *b = -1;
        if (!sv_action_available((SvSystem)sys, p, (SvAction)a)) { *k = 0; *b = -1; }
      }
}

int sv_key_reserved(const SvSettings *s, int key) {
  return key == SV_KEY_ESCAPE || key == s->menu_key || key == SV_KEY_F11 || key == SV_KEY_F12 || key == SV_KEY_F10 || key == SV_KEY_F1;
}

/* ---- text form ---- */

static int parse_int(const char *text, int *out) {
  char *end = NULL;
  long v;
  if (*text == '\0') return 0;
  v = strtol(text, &end, 10);
  if (end == text || *end != '\0' || v < -100000L || v > 100000L) return 0;
  *out = (int)v;
  return 1;
}

static int match_binding(const char *key, int *sys, int *player, int *action, int *is_pad) {
  /* bind.<system>.p<1|2>.<key|pad>.<action> */
  char buffer[64];
  char *part[5];
  int n = 0, i;
  size_t len = strlen(key);
  if (len == 0 || len >= sizeof buffer) return 0;
  memcpy(buffer, key, len + 1u);
  part[n++] = buffer;
  for (i = 0; buffer[i] != '\0' && n < 5; ++i)
    if (buffer[i] == '.') { buffer[i] = '\0'; part[n++] = buffer + i + 1; }
  if (n != 5 || strcmp(part[0], "bind") != 0) return 0;
  for (i = 0; i < SV_SYSTEM_COUNT; ++i) if (strcmp(part[1], system_keys[i]) == 0) *sys = i;
  if (strcmp(part[1], system_keys[0]) != 0 && strcmp(part[1], system_keys[1]) != 0) return 0;
  if (strcmp(part[2], "p1") == 0) *player = 0; else if (strcmp(part[2], "p2") == 0) *player = 1; else return 0;
  if (strcmp(part[3], "key") == 0) *is_pad = 0; else if (strcmp(part[3], "pad") == 0) *is_pad = 1; else return 0;
  for (i = 0; i < SV_ACT_COUNT; ++i)
    if (strcmp(part[4], action_keys[i]) == 0) { *action = i; return 1; }
  return 0;
}

/* Stable text names of the widescreen modes (the settings file stores these, never the enum number). */
static const char *const widescreen_keys[SV_WIDESCREEN_COUNT] = {"original", "extended_backgrounds", "enhanced_gameplay"};

static const char *const side_style_keys[SV_SIDE_COUNT] = {"comfort", "borders", "raw"};

static int apply_pair(SvSettings *s, const char *key, const char *value) {
  int v, sys = 0, player = 0, action = 0, is_pad = 0;
  if (strcmp(key, "video.widescreen") == 0) { /* an unknown value is recognised and becomes Original */
    int m;
    s->video.widescreen = SV_WIDESCREEN_ORIGINAL;
    for (m = 0; m < SV_WIDESCREEN_COUNT; ++m) if (strcmp(value, widescreen_keys[m]) == 0) s->video.widescreen = (SvWidescreenMode)m;
    return 1;
  }
  if (strcmp(key, "video.widescreen_side_style") == 0) { /* an unknown value is recognised and becomes the default Comfort */
    int m;
    s->video.widescreen_side_style = SV_SIDE_COMFORT;
    for (m = 0; m < SV_SIDE_COUNT; ++m) if (strcmp(value, side_style_keys[m]) == 0) s->video.widescreen_side_style = (SvSideStyle)m;
    return 1;
  }
  if (strcmp(key, "pad_name") == 0) { strncpy(s->pad_name, value, sizeof s->pad_name - 1u); s->pad_name[sizeof s->pad_name - 1u] = '\0'; return 1; }
  if (!parse_int(value, &v)) return 0;
  if (strcmp(key, "video.fullscreen") == 0) s->video.fullscreen = v;
  else if (strcmp(key, "video.scale") == 0) s->video.scale = (SvScaleMode)v;
  else if (strcmp(key, "video.aspect") == 0) s->video.aspect = (SvAspect)v;
  else if (strcmp(key, "video.preset") == 0) s->video.preset = (SvCrtPreset)v;
  else if (strcmp(key, "video.scanlines") == 0) s->video.scanlines = v;
  else if (strcmp(key, "video.mask") == 0) s->video.mask = v;
  else if (strcmp(key, "video.curvature") == 0) s->video.curvature = v;
  else if (strcmp(key, "video.bloom") == 0) s->video.bloom = v;
  else if (strcmp(key, "video.brightness") == 0) s->video.brightness = v;
  else if (strcmp(key, "video.vsync") == 0) s->video.vsync = v;
  else if (strcmp(key, "video.show_fps") == 0) s->video.show_fps = v;
  else if (strcmp(key, "video.window_w") == 0) s->video.window_w = v;
  else if (strcmp(key, "video.window_h") == 0) s->video.window_h = v;
  else if (strcmp(key, "audio.volume") == 0) s->audio.volume = v;
  else if (strcmp(key, "audio.muted") == 0) s->audio.muted = v;
  else if (strcmp(key, "input.deadzone") == 0) s->deadzone = v;
  else if (strcmp(key, "input.menu_key") == 0) s->menu_key = v;
  else if (match_binding(key, &sys, &player, &action, &is_pad)) { if (is_pad) s->bind[sys].pad[player][action] = v; else s->bind[sys].key[player][action] = v; }
  else return 0;
  return 1;
}

size_t sv_settings_parse(SvSettings *s, const char *text, size_t length) {
  size_t recognised = 0, at = 0;
  if (text == NULL) return 0;
  if (length > SV_SETTINGS_MAX_TEXT) length = SV_SETTINGS_MAX_TEXT;
  while (at < length) {
    char line[160];
    size_t n = 0;
    char *eq;
    while (at < length && text[at] != '\n') { if (n + 1u < sizeof line) line[n++] = text[at]; ++at; }
    ++at;
    while (n > 0 && (line[n - 1u] == '\r' || line[n - 1u] == ' ')) --n;
    line[n] = '\0';
    if (n == 0 || line[0] == '#') continue;
    eq = strchr(line, '=');
    if (eq == NULL) continue;
    *eq = '\0';
    if (apply_pair(s, line, eq + 1)) ++recognised;
  }
  sv_settings_sanitize(s);
  return recognised;
}

size_t sv_settings_format(const SvSettings *s, char *out, size_t cap) {
  size_t used = 0;
  int sys, p, a;
#define EMIT(...) do { int n_ = snprintf(out + used, cap - used, __VA_ARGS__); if (n_ < 0 || (size_t)n_ >= cap - used) { if (cap) out[0] = '\0'; return 0; } used += (size_t)n_; } while (0)
  if (out == NULL || cap == 0) return 0;
  EMIT("# Segarecomp viewer settings (presentation and input mapping only; never part of guest state)\n");
  EMIT("video.fullscreen=%d\nvideo.scale=%d\nvideo.aspect=%d\nvideo.preset=%d\nvideo.scanlines=%d\nvideo.mask=%d\nvideo.curvature=%d\n",
       s->video.fullscreen, (int)s->video.scale, (int)s->video.aspect, (int)s->video.preset, s->video.scanlines, s->video.mask, s->video.curvature);
  EMIT("video.bloom=%d\nvideo.brightness=%d\nvideo.vsync=%d\nvideo.show_fps=%d\nvideo.window_w=%d\nvideo.window_h=%d\n", s->video.bloom, s->video.brightness,
       s->video.vsync, s->video.show_fps, s->video.window_w, s->video.window_h);
  EMIT("video.widescreen=%s\n", widescreen_keys[(unsigned)s->video.widescreen < SV_WIDESCREEN_COUNT ? s->video.widescreen : SV_WIDESCREEN_ORIGINAL]);
  EMIT("video.widescreen_side_style=%s\n", side_style_keys[(unsigned)s->video.widescreen_side_style < SV_SIDE_COUNT ? s->video.widescreen_side_style : SV_SIDE_COMFORT]);
  EMIT("audio.volume=%d\naudio.muted=%d\ninput.deadzone=%d\ninput.menu_key=%d\n", s->audio.volume, s->audio.muted, s->deadzone, s->menu_key);
  if (s->pad_name[0] != '\0') EMIT("pad_name=%s\n", s->pad_name);
  for (sys = 0; sys < SV_SYSTEM_COUNT; ++sys)
    for (p = 0; p < SV_PLAYERS; ++p)
      for (a = 0; a < SV_ACT_COUNT; ++a) {
        if (!sv_action_available((SvSystem)sys, p, (SvAction)a)) continue;
        EMIT("bind.%s.p%d.key.%s=%d\n", system_keys[sys], p + 1, action_keys[a], s->bind[sys].key[p][a]);
        EMIT("bind.%s.p%d.pad.%s=%d\n", system_keys[sys], p + 1, action_keys[a], s->bind[sys].pad[p][a]);
      }
#undef EMIT
  return used;
}

int sv_settings_load(SvSettings *settings, const char *path) {
  FILE *f;
  static char buffer[SV_SETTINGS_MAX_TEXT + 1u];
  size_t n;
  if (path == NULL || *path == '\0') return -1;
  f = fopen(path, "rb");
  if (f == NULL) return -1;
  n = fread(buffer, 1, SV_SETTINGS_MAX_TEXT, f);
  fclose(f);
  buffer[n] = '\0';
  (void)sv_settings_parse(settings, buffer, n);
  return 0;
}

int sv_settings_save(const SvSettings *settings, const char *path) {
  static char buffer[SV_SETTINGS_MAX_TEXT];
  char temporary[1100];
  size_t n;
  FILE *f;
  if (path == NULL || *path == '\0' || strlen(path) > 1000u) return -1;
  n = sv_settings_format(settings, buffer, sizeof buffer);
  if (n == 0) return -1;
  snprintf(temporary, sizeof temporary, "%s.tmp", path);
  f = fopen(temporary, "wb");
  if (f == NULL) return -1;
  if (fwrite(buffer, 1, n, f) != n || fflush(f) != 0) { fclose(f); remove(temporary); return -1; }
  if (fclose(f) != 0) { remove(temporary); return -1; }
#if defined(_WIN32)
  remove(path); /* rename() does not replace on the Windows C runtime */
#endif
  if (rename(temporary, path) != 0) { remove(temporary); return -1; }
  return 0;
}

/* ---- conflicts ---- */

static int find_conflict(const int table[SV_PLAYERS][SV_ACT_COUNT], SvSystem system, int none, int code, int player, SvAction action, int same_player, SvBindingOwner *owner) {
  int p, a;
  if (code == none) return 0;
  for (p = 0; p < SV_PLAYERS; ++p)
    for (a = 0; a < SV_ACT_COUNT; ++a) {
      if (same_player && p != player) continue;
      if ((p == player && a == (int)action) || !sv_action_available(system, p, (SvAction)a)) continue;
      if (table[p][a] == code) { if (owner != NULL) { owner->player = p; owner->action = (SvAction)a; } return 1; }
    }
  return 0;
}

int sv_find_key_conflict(const SvSettings *s, SvSystem system, int key, int player, SvAction action, SvBindingOwner *owner) {
  return find_conflict(s->bind[system].key, system, 0, key, player, action, 0, owner);
}
int sv_find_pad_conflict(const SvSettings *s, SvSystem system, int button, int player, SvAction action, SvBindingOwner *owner) {
  return find_conflict(s->bind[system].pad, system, -1, button, player, action, 1, owner);
}

int sv_assign_key(SvSettings *s, SvSystem system, int player, SvAction action, int key, SvBindingOwner *moved_from, int *had_conflict) {
  SvBindingOwner owner;
  int conflict;
  if (had_conflict != NULL) *had_conflict = 0;
  if (player < 0 || player >= SV_PLAYERS || !sv_action_available(system, player, action)) return -1;
  if (key == 0) { s->bind[system].key[player][action] = 0; return 0; } /* clear */
  if (key < 0 || key >= 512 || sv_key_reserved(s, key)) return -1;
  conflict = sv_find_key_conflict(s, system, key, player, action, &owner);
  if (conflict) { s->bind[system].key[owner.player][owner.action] = 0; if (moved_from != NULL) *moved_from = owner; }
  if (had_conflict != NULL) *had_conflict = conflict;
  s->bind[system].key[player][action] = key;
  return 0;
}

int sv_assign_pad(SvSettings *s, SvSystem system, int player, SvAction action, int button, SvBindingOwner *moved_from, int *had_conflict) {
  SvBindingOwner owner;
  int conflict;
  if (had_conflict != NULL) *had_conflict = 0;
  if (player < 0 || player >= SV_PLAYERS || !sv_action_available(system, player, action)) return -1;
  if (button == -1) { s->bind[system].pad[player][action] = -1; return 0; }
  if (button < 0 || button > 30) return -1;
  conflict = sv_find_pad_conflict(s, system, button, player, action, &owner);
  /* Two PLAYERS may legitimately share a pad layout (each reads its own device): only a clash inside the same player is a conflict. */
  if (conflict) { s->bind[system].pad[owner.player][owner.action] = -1; if (moved_from != NULL) *moved_from = owner; }
  if (had_conflict != NULL) *had_conflict = conflict;
  s->bind[system].pad[player][action] = button;
  return 0;
}

int sv_axis_direction(int value, int deadzone_percent) {
  const long threshold = (32767L * (long)clampi(deadzone_percent, 5, 90)) / 100L;
  if ((long)value > threshold) return 1;
  if ((long)value < -threshold) return -1;
  return 0;
}

SvWidescreenMode sv_widescreen_effective(SvWidescreenMode requested, const SvWidescreenCaps *caps) {
  if (caps == NULL) return SV_WIDESCREEN_ORIGINAL;
  if (requested == SV_WIDESCREEN_ENHANCED_GAMEPLAY && caps->enhanced != SV_WS_UNAVAILABLE) return SV_WIDESCREEN_ENHANCED_GAMEPLAY;
  if (requested != SV_WIDESCREEN_ORIGINAL && (unsigned)requested < SV_WIDESCREEN_COUNT && caps->extended != SV_WS_UNAVAILABLE)
    return SV_WIDESCREEN_EXTENDED_BACKGROUNDS;
  return SV_WIDESCREEN_ORIGINAL;
}

const char *sv_widescreen_name(SvWidescreenMode mode) {
  switch (mode) {
    case SV_WIDESCREEN_EXTENDED_BACKGROUNDS: return "Extended Backgrounds (Level 2)";
    case SV_WIDESCREEN_ENHANCED_GAMEPLAY: return "Enhanced Widescreen (Level 3)";
    default: return "Original";
  }
}

const char *sv_widescreen_description(SvWidescreenMode mode) {
  switch (mode) {
    case SV_WIDESCREEN_EXTENDED_BACKGROUNDS:
      return "Shows more background scenery at the sides. Gameplay stays in the original area; some scenery may be incomplete.";
    case SV_WIDESCREEN_ENHANCED_GAMEPLAY:
      return "Also shows sprites the game already places beside the picture, where supported. It does not make the game world wider.";
    default: return "Original game picture. Preserves the intended display area and compatibility.";
  }
}

const char *sv_widescreen_status(SvWidescreenMode requested, const SvWidescreenCaps *caps) {
  const SvWidescreenMode eff = sv_widescreen_effective(requested, caps);
  if (requested == SV_WIDESCREEN_ORIGINAL || (unsigned)requested >= SV_WIDESCREEN_COUNT) return "";
  if (eff == SV_WIDESCREEN_ORIGINAL) return "Not yet available for this game. Showing the original picture.";
  if (eff != requested) return "Level 3 is not available for this game. Showing Extended Backgrounds instead.";
  if ((eff == SV_WIDESCREEN_ENHANCED_GAMEPLAY ? caps->enhanced : caps->extended) == SV_WS_PARTIAL) {
    if (eff == SV_WIDESCREEN_ENHANCED_GAMEPLAY && caps->enhanced_note != NULL && caps->enhanced_note[0] != '\0') return caps->enhanced_note;
    if (caps->partial_note != NULL && caps->partial_note[0] != '\0') return caps->partial_note;
    return "Partly available for this game: some of the extra picture may be missing.";
  }
  return "Available for this game.";
}

const char *sv_side_style_name(SvSideStyle style) {
  switch (style) {
    case SV_SIDE_BORDERS: return "Borders";
    case SV_SIDE_RAW: return "Raw Extension";
    default: return "Comfort";
  }
}

const char *sv_side_style_description(SvSideStyle style) {
  switch (style) {
    case SV_SIDE_BORDERS: return "Plain neutral side panels around the original picture. No extra game data is shown.";
    case SV_SIDE_RAW: return "Shows the extension exactly as the game produced it, including distracting extra data. Useful for debugging.";
    default: return "Hides distracting extra game data with a soft ambient fill. It does not create new gameplay visibility.";
  }
}
