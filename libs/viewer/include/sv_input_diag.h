#ifndef SEGARECOMP_VIEWER_SV_INPUT_DIAG_H
#define SEGARECOMP_VIEWER_SV_INPUT_DIAG_H

/*
 * Input diagnostics (SDL3): the single detection engine shared by the launcher and the in-game viewer. It records the result of the
 * SDL subsystem initialisation, every joystick SDL enumerates (also those SDL cannot treat as a gamepad), the outcome of every
 * SDL_OpenGamepad attempt with SDL's own error text, live button/axis state and event counts, and an on-demand read-only list of the
 * Nintendo HID devices the OS exposes (bus type USB/Bluetooth), which explains a controller the OS sees but SDL does not.
 *
 * It never reinterprets a raw joystick as gameplay input, never stores ROM content or paths, and the pure classification of the facts
 * lives in sv_input_state.h (SDL free, unit tested). The gamepads it opens are handed to the host through sv_diag_gamepad().
 */
#include <SDL3/SDL.h>

#include "sv_input_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SV_DIAG_MAX_DEVICES 8
#define SV_DIAG_MAX_HID 12
#define SV_DIAG_NOT_OPENED (-2)

typedef struct SvDiagDevice {
  SDL_JoystickID id;
  char name[64];
  char guid[40];
  unsigned vendor, product, version;
  int is_gamepad;
  int gamepad_type;            /* SDL_GamepadType, SDL_GAMEPAD_TYPE_UNKNOWN when not a gamepad */
  int connection;              /* SDL_JoystickConnectionState (-1 invalid, 0 unknown, 1 wired, 2 wireless); SV_DIAG_NOT_OPENED when it is not open */
  int player_index;            /* SDL's player index, -1 when unset */
  int open_attempted, open_ok;
  char open_error[160];        /* SDL_GetError() of the failed open */
  int raw_buttons, raw_axes, raw_hats; /* of the opened joystick; 0 when it could not be opened */
  Uint32 buttons;              /* SDL_GamepadButton bitmask, live */
  Sint16 axis[6];              /* SDL_GamepadAxis, live (left x/y, right x/y, triggers) */
  unsigned long events;        /* gamepad button/axis events since opened */
  unsigned long raw_events;    /* raw joystick events of a device that is not a gamepad */
  Uint64 last_event_ms;        /* SDL_GetTicks() of the newest event, 0 = none */
  int last_button;             /* SDL button of the newest press, -1 = none */
  int last_raw_button;
  int nav_ok;                  /* the host's menu navigation reacted to this pad (set by sv_diag_note_nav) */
  Uint64 added_ms;
} SvDiagDevice;

typedef struct SvDiagHid {
  unsigned vendor, product;
  int bus;                     /* SDL_hid_bus_type: 1 USB, 2 Bluetooth, 0 unknown */
  int interface_number;
  char product_string[64];
  char family[32];             /* "Switch 2 Pro Controller", "Switch Pro Controller", ... or "" */
  int matched_joystick;        /* SDL exposes a joystick with this vendor/product */
} SvDiagHid;

typedef struct SvDiag SvDiag;

/* `open_gamepads` = 1: the diagnostics open every recognised gamepad (launcher). The viewer host opens through sv_diag_open_gamepad as well so a
 * failure is recorded; both end up with the same SvDiagDevice records. */
SvDiag *sv_diag_create(void);
void sv_diag_destroy(SvDiag *diag);
/* Records how SDL_Init went (call right after it). `error` is SDL_GetError() text of a failed gamepad init, may be NULL. */
void sv_diag_note_init(SvDiag *diag, int video_ok, int gamepad_ok, const char *gamepad_error);
/* Enumerates the joysticks present now (adds the new ones, drops the vanished ones) and tries to open each recognised gamepad once. */
void sv_diag_scan(SvDiag *diag);
/* Feeds every SDL event (joystick/gamepad hot-plug, buttons, axes, raw joystick events). */
void sv_diag_event(SvDiag *diag, const SDL_Event *event);
/* Focus loss / window blur: nothing stays "pressed" behind the user's back. */
void sv_diag_release(SvDiag *diag);
/* The host's menu navigation acted on a press of this pad. */
void sv_diag_note_nav(SvDiag *diag, SDL_JoystickID id);

int sv_diag_count(const SvDiag *diag);
const SvDiagDevice *sv_diag_device(const SvDiag *diag, int index);
const SvDiagDevice *sv_diag_find(const SvDiag *diag, SDL_JoystickID id);
/* The opened gamepad of a device (borrowed; closed by the diagnostics when the device disappears), NULL when none. */
SDL_Gamepad *sv_diag_gamepad(const SvDiag *diag, SDL_JoystickID id);

typedef struct SvDiagContext {
  const SvSettings *settings;  /* NULL in the launcher */
  SvSystem system;
  int player;                  /* the player whose gamepad bindings are audited */
  int nav_checked;             /* the launcher's menu navigation is expected to react */
} SvDiagContext;
SvPadState sv_diag_state_of(const SvDiag *diag, const SvDiagDevice *device, const SvDiagContext *context);
SvPadState sv_diag_overall(const SvDiag *diag, const SvDiagContext *context);

int sv_diag_video_ok(const SvDiag *diag);
int sv_diag_gamepad_subsystem_ok(const SvDiag *diag);
const char *sv_diag_gamepad_init_error(const SvDiag *diag);

/* Read-only OS-level scan of the Nintendo (VID 057E) HID devices through SDL's HIDAPI; fills `out` (up to `cap`), returns the count or -1 and
 * sets `error`. Explains a controller the OS sees but SDL does not expose, and the bus (USB / Bluetooth) it is on. */
int sv_diag_hid_scan(const SvDiag *diag, SvDiagHid *out, int cap, char *error, size_t error_cap);
/* Whether a shared libusb-1.0 can be loaded (the SDL driver for Switch 2 controllers talks USB through it). */
int sv_diag_libusb_loadable(void);
/* Whether the SDL build contains a given HIDAPI driver switch (hint is known to SDL); text such as "enabled/disabled/default". */
const char *sv_diag_switch2_hint_state(void);
/* "SDL 3.4.16 (compiled 3.4.16)". */
void sv_diag_sdl_version(char *out, size_t cap);

/* Loads `gamecontrollerdb.txt`-style mapping lines (a user-supplied file); returns the number of mappings added or -1. */
int sv_diag_load_mapping_file(const char *path);

/* A bounded plain-text summary: SDL version, init results, one block per device, no ROM names or file paths. */
size_t sv_diag_summary(const SvDiag *diag, char *out, size_t cap);
/* SDL_Log()s the summary (SEGARECOMP_INPUT_TRACE=1 additionally logs every event). Called once at start-up and on every hot-plug. */
void sv_diag_log_summary(const SvDiag *diag, const char *why);

/* Test seam: replaces SDL_OpenGamepad (NULL restores it) so the "recognised but cannot be opened" state can be produced deterministically. */
void sv_diag_set_open_hook(SDL_Gamepad *(*hook)(SDL_JoystickID id));

#ifdef __cplusplus
}
#endif
#endif
