#ifndef SEGARECOMP_VIEWER_SV_SETTINGS_H
#define SEGARECOMP_VIEWER_SV_SETTINGS_H

/*
 * Shared in-game viewer foundation: persistent viewer settings (SDL-free, strict C11).
 *
 * The settings are PRESENTATION and INPUT-MAPPING choices of the host. They never reach guest state: a binding only changes which physical
 * input produces which console button, and the console adapters sample the resulting mask at the same deterministic points as before.
 * Keys are SDL3 scancodes (USB HID usage ids, a stable ABI; sv_host.c asserts the values) and gamepad buttons are SDL_GamepadButton
 * values, stored as plain ints so this file needs no SDL header.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SvSystem { SV_SYSTEM_GENESIS = 0, SV_SYSTEM_SMS = 1, SV_SYSTEM_COUNT = 2 } SvSystem;

/* Generic console actions. Each console adapter maps the bits to its own pad mask. */
typedef enum SvAction {
  SV_ACT_UP = 0, SV_ACT_DOWN, SV_ACT_LEFT, SV_ACT_RIGHT,
  SV_ACT_BUTTON1, SV_ACT_BUTTON2, SV_ACT_BUTTON3, /* Genesis A/B/C, SMS 1/2 (BUTTON3 unused) */
  SV_ACT_START,                                   /* Genesis Start */
  SV_ACT_PAUSE,                                   /* SMS hardware Pause button (console button, not the host menu) */
  SV_ACT_COUNT
} SvAction;
#define SV_ACTION_BIT(a) ((uint16_t)(1u << (unsigned)(a)))

typedef enum SvScaleMode { SV_SCALE_FIT = 0, SV_SCALE_INTEGER = 1 } SvScaleMode;
typedef enum SvAspect { SV_ASPECT_ORIGINAL = 0, SV_ASPECT_4_3 = 1 } SvAspect;
typedef enum SvCrtPreset { SV_CRT_PIXEL_PERFECT = 0, SV_CRT_SOFT, SV_CRT_CLASSIC, SV_CRT_ARCADE, SV_CRT_CUSTOM, SV_CRT_COUNT } SvCrtPreset;

/* Widescreen Mode: ONE mutually exclusive presentation preference (never a set of booleans). Level 2 extends background scenery at the
 * sides, Level 3 additionally attempts enhanced gameplay. It is only the REQUESTED mode; what is actually shown (the effective mode) is
 * derived from the platform's reported capability (sv_widescreen_effective) and is Original wherever the platform cannot honour it. */
typedef enum SvWidescreenMode {
  SV_WIDESCREEN_ORIGINAL = 0,
  SV_WIDESCREEN_EXTENDED_BACKGROUNDS = 1, /* Level 2 */
  SV_WIDESCREEN_ENHANCED_GAMEPLAY = 2,    /* Level 3 */
  SV_WIDESCREEN_COUNT
} SvWidescreenMode;
/* SEG-123: how the EXTRA side area of a generic Level 2/3 extension is displayed. Presentation only: it never reaches the producer, the guest or the
 * native pixels. COMFORT (default, value 0 so an unconfigured/zeroed struct is Comfort) subdues the side regions, BORDERS shows neutral panels and RAW
 * shows the extension exactly as produced. */
typedef enum SvSideStyle { SV_SIDE_COMFORT = 0, SV_SIDE_BORDERS = 1, SV_SIDE_RAW = 2, SV_SIDE_COUNT } SvSideStyle;
const char *sv_side_style_name(SvSideStyle style);        /* menu / launcher label */
const char *sv_side_style_description(SvSideStyle style); /* one-sentence explanation */

/* What a platform viewer observes it can do for the running game, per enhanced level. Reporting nothing means UNAVAILABLE. */
typedef enum SvWidescreenSupport { SV_WS_UNAVAILABLE = 0, SV_WS_PARTIAL = 1, SV_WS_AVAILABLE = 2 } SvWidescreenSupport;
/* `partial_note` (optional, NULL = the generic wording) is a static platform sentence shown when the selected level is Partial, for a platform whose
 * limitation is specific (Master System: the extension repeats scenery). It must outlive the call and never promise more than the capability. */
/* `enhanced_note` (optional, falls back to `partial_note`) is the same for a Partial Level 3 (SEG-121). */
typedef struct SvWidescreenCaps { SvWidescreenSupport extended, enhanced; const char *partial_note; const char *enhanced_note; } SvWidescreenCaps;

typedef struct SvVideoSettings {
  int fullscreen;               /* borderless desktop fullscreen (never a display mode change) */
  SvScaleMode scale;
  SvAspect aspect;
  SvCrtPreset preset;
  int scanlines, mask, curvature, bloom; /* 0..100 */
  int brightness;                        /* 50..150 percent */
  int vsync, show_fps;
  SvWidescreenMode widescreen;  /* requested mode, default Original */
  SvSideStyle widescreen_side_style; /* SEG-123: side-area presentation of a generic extension, default Comfort */
  int window_w, window_h;                /* last windowed size in points, 0 = default */
} SvVideoSettings;

typedef struct SvAudioSettings {
  int volume; /* 0..100, playback gain only */
  int muted;
} SvAudioSettings;

#define SV_PLAYERS 2
typedef struct SvBindings {
  int key[SV_PLAYERS][SV_ACT_COUNT]; /* SDL scancode, 0 = unbound */
  int pad[SV_PLAYERS][SV_ACT_COUNT]; /* SDL_GamepadButton, -1 = unbound */
} SvBindings;

typedef struct SvSettings {
  SvVideoSettings video;
  SvAudioSettings audio;
  int deadzone;      /* analog stick dead zone, percent 5..60 */
  int menu_key;      /* scancode of the pause-menu key; Escape always works too */
  SvBindings bind[SV_SYSTEM_COUNT];
  char pad_name[64]; /* preferred gamepad name for Player 1, empty = first connected */
} SvSettings;

/* Scancodes (USB HID) and buttons used by the defaults and by host hotkeys; sv_host.c static-asserts them against SDL. */
enum {
  SV_KEY_A = 4, SV_KEY_C = 6, SV_KEY_I = 12, SV_KEY_J = 13, SV_KEY_K = 14, SV_KEY_L = 15, SV_KEY_O = 18, SV_KEY_P = 19,
  SV_KEY_U = 24, SV_KEY_X = 27, SV_KEY_Z = 29, SV_KEY_RETURN = 40, SV_KEY_ESCAPE = 41, SV_KEY_BACKSPACE = 42, SV_KEY_DELETE = 76,
  SV_KEY_F11 = 68, SV_KEY_F12 = 69, SV_KEY_RIGHT = 79, SV_KEY_LEFT = 80, SV_KEY_DOWN = 81, SV_KEY_UP = 82, SV_KEY_RSHIFT = 229,
  SV_KEY_F1 = 58, SV_KEY_F10 = 67
};
enum {
  SV_PAD_SOUTH = 0, SV_PAD_EAST = 1, SV_PAD_WEST = 2, SV_PAD_NORTH = 3, SV_PAD_BACK = 4, SV_PAD_GUIDE = 5, SV_PAD_START = 6,
  SV_PAD_DPAD_UP = 11, SV_PAD_DPAD_DOWN = 12, SV_PAD_DPAD_LEFT = 13, SV_PAD_DPAD_RIGHT = 14
};

/* Which actions a console exposes for a player, and their labels there. */
int sv_action_available(SvSystem system, int player, SvAction action);
const char *sv_action_label(SvSystem system, SvAction action);
const char *sv_system_name(SvSystem system);

void sv_settings_defaults(SvSettings *settings);
void sv_bindings_defaults(SvSettings *settings, SvSystem system);
void sv_video_defaults(SvVideoSettings *video);
/* Loads the CRT tuning of `preset` into the video settings (and selects it). SV_CRT_CUSTOM leaves the numbers untouched. */
void sv_apply_crt_preset(SvVideoSettings *video, SvCrtPreset preset);
const char *sv_crt_preset_name(SvCrtPreset preset);

/* Fallback Level 3 -> Level 2 -> Original: the mode that is actually rendered. NULL `caps` (no provider) means nothing is supported. */
SvWidescreenMode sv_widescreen_effective(SvWidescreenMode requested, const SvWidescreenCaps *caps);
const char *sv_widescreen_name(SvWidescreenMode mode);        /* menu / launcher label */
const char *sv_widescreen_description(SvWidescreenMode mode); /* one-sentence explanation shown beneath the selector */
/* Honest per-selection support text (requested vs capability); empty string for Original with nothing to explain. */
const char *sv_widescreen_status(SvWidescreenMode requested, const SvWidescreenCaps *caps);

/* Clamps every field into its documented range. Called after every parse and every edit. */
void sv_settings_sanitize(SvSettings *settings);

/* Text form: `key=value` lines. Unknown keys and malformed lines are ignored; values are clamped. Returns the number of recognised
 * keys. Never fails on bad content (a damaged file leaves defaults), and bounds its work (input capped at 32 KiB). */
size_t sv_settings_parse(SvSettings *settings, const char *text, size_t length);
/* Serializes into `out` (NUL terminated). Returns the length, or 0 when `cap` is too small. */
size_t sv_settings_format(const SvSettings *settings, char *out, size_t cap);
/* File helpers: load keeps defaults when the file is missing or damaged; save is atomic (temporary file + rename). 0 == ok. */
int sv_settings_load(SvSettings *settings, const char *path);
int sv_settings_save(const SvSettings *settings, const char *path);

/* Conflicts. A key or pad button may drive only one action of one player of a console. Returns 1 and fills the owner when `code` is
 * already bound to something other than (player, action). */
typedef struct SvBindingOwner { int player; SvAction action; } SvBindingOwner;
int sv_find_key_conflict(const SvSettings *settings, SvSystem system, int key, int player, SvAction action, SvBindingOwner *owner);
int sv_find_pad_conflict(const SvSettings *settings, SvSystem system, int button, int player, SvAction action, SvBindingOwner *owner);
/* Keys that can never be game bindings (menu key, Escape, host hotkeys). */
int sv_key_reserved(const SvSettings *settings, int key);
/* Assigns with conflict resolution: the previous owner of `code` becomes unbound and is reported through `moved_from` (may be NULL).
 * Returns 0 ok, -1 reserved/invalid code. */
int sv_assign_key(SvSettings *settings, SvSystem system, int player, SvAction action, int key, SvBindingOwner *moved_from, int *had_conflict);
int sv_assign_pad(SvSettings *settings, SvSystem system, int player, SvAction action, int button, SvBindingOwner *moved_from, int *had_conflict);

/* Stick/dpad helper: converts a signed 16-bit axis to -1/0/+1 with the dead zone `percent` (hysteresis-free, pure). */
int sv_axis_direction(int value, int deadzone_percent);

#ifdef __cplusplus
}
#endif
#endif
