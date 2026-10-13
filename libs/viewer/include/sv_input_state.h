#ifndef SEGARECOMP_VIEWER_SV_INPUT_STATE_H
#define SEGARECOMP_VIEWER_SV_INPUT_STATE_H

/*
 * SDL-free classification of "why does my controller not work": the pure half of the input diagnostics (sv_input_diag.h gathers the facts
 * from SDL). A controller is never reported as "working" from enumeration alone; the states below are ordered by how far the chain
 *   OS/SDL sees a joystick -> SDL has a gamepad mapping -> SDL_OpenGamepad works -> events arrive -> the game bindings use them
 * has been proven.
 */
#include "sv_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SvPadState {
  SV_PAD_NO_JOYSTICK = 0,   /* A: no joystick is visible to SDL */
  SV_PAD_NOT_A_GAMEPAD,     /* B: a joystick is enumerated but SDL has no gamepad mapping/driver for it */
  SV_PAD_OPEN_FAILED,       /* C: recognised as a gamepad, but SDL_OpenGamepad failed */
  SV_PAD_NO_EVENTS,         /* D: opened, but no button/axis event has arrived yet */
  SV_PAD_BINDINGS,          /* E: live input, but the game bindings are missing or conflicting */
  SV_PAD_MENU_NAV,          /* F: live in SDL, but the launcher's menu navigation does not receive it */
  SV_PAD_LIVE,              /* live input and usable bindings (and menu navigation where it applies) */
  SV_PAD_STATE_COUNT
} SvPadState;

typedef struct SvPadFacts {
  int enumerated;            /* SDL lists the joystick */
  int is_gamepad;            /* SDL has a gamepad mapping for it */
  int open_attempted, open_ok;
  unsigned long events;      /* gamepad button/axis events received since it was opened */
  int bindings_checked;      /* the host supplies the game bindings (the launcher has none) */
  int bindings_missing;      /* console actions without a gamepad button */
  int bindings_conflicts;    /* gamepad buttons driving more than one action */
  int nav_checked;           /* the host feeds menu navigation (launcher) */
  int nav_ok;                /* menu navigation reacted to this pad */
} SvPadFacts;

SvPadState sv_pad_classify(const SvPadFacts *facts);
/* Overall state of a set of devices: the most advanced one wins; an empty set is SV_PAD_NO_JOYSTICK. */
SvPadState sv_pad_overall(const SvPadState *states, int count);
/* "A".."F" for the failure classes, "OK" for SV_PAD_LIVE. */
const char *sv_pad_state_code(SvPadState state);
const char *sv_pad_state_title(SvPadState state);
const char *sv_pad_state_advice(SvPadState state);

/* Audits one player's gamepad bindings of a console: actions the console exposes but leaves unbound, and buttons bound twice. */
void sv_pad_binding_audit(const SvSettings *settings, SvSystem system, int player, int *missing, int *conflicts);

/* The label a Nintendo controller prints on the SDL button at this position ("A" for East, ...), or NULL for non-face/non-Nintendo layouts. SDL's
 * names are positional (South/East/West/North); Nintendo prints A on the right (East) and B at the bottom (South). */
const char *sv_pad_nintendo_label(int sdl_button);
/* The positional name used for every controller ("South", "East", "Back", "D-pad Up", ...). */
const char *sv_pad_position_name(int sdl_button);

#ifdef __cplusplus
}
#endif
#endif
