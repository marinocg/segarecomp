#include <stddef.h>

#include "sv_input_state.h"

SvPadState sv_pad_classify(const SvPadFacts *f) {
  if (f == NULL || !f->enumerated) return SV_PAD_NO_JOYSTICK;
  if (!f->is_gamepad) return SV_PAD_NOT_A_GAMEPAD;
  if (!f->open_attempted || !f->open_ok) return f->open_attempted ? SV_PAD_OPEN_FAILED : SV_PAD_NO_EVENTS;
  if (f->events == 0) return SV_PAD_NO_EVENTS;
  if (f->nav_checked && !f->nav_ok) return SV_PAD_MENU_NAV;
  if (f->bindings_checked && (f->bindings_missing > 0 || f->bindings_conflicts > 0)) return SV_PAD_BINDINGS;
  return SV_PAD_LIVE;
}

SvPadState sv_pad_overall(const SvPadState *states, int count) {
  static const int rank[SV_PAD_STATE_COUNT] = {0, 1, 2, 3, 5, 4, 6}; /* NO_JOYSTICK < NOT_A_GAMEPAD < OPEN_FAILED < NO_EVENTS < MENU_NAV < BINDINGS < LIVE */
  SvPadState best = SV_PAD_NO_JOYSTICK;
  int i;
  for (i = 0; states != NULL && i < count; ++i)
    if ((int)states[i] >= 0 && (int)states[i] < SV_PAD_STATE_COUNT && rank[states[i]] >= rank[best]) best = states[i];
  return best;
}

const char *sv_pad_state_code(SvPadState s) {
  switch (s) {
    case SV_PAD_NO_JOYSTICK: return "A";
    case SV_PAD_NOT_A_GAMEPAD: return "B";
    case SV_PAD_OPEN_FAILED: return "C";
    case SV_PAD_NO_EVENTS: return "D";
    case SV_PAD_BINDINGS: return "E";
    case SV_PAD_MENU_NAV: return "F";
    case SV_PAD_LIVE: return "OK";
    default: return "?";
  }
}

const char *sv_pad_state_title(SvPadState s) {
  switch (s) {
    case SV_PAD_NO_JOYSTICK: return "No controller seen by SDL";
    case SV_PAD_NOT_A_GAMEPAD: return "Joystick found, but not a gamepad";
    case SV_PAD_OPEN_FAILED: return "Gamepad recognised, but it could not be opened";
    case SV_PAD_NO_EVENTS: return "Gamepad opened, waiting for input";
    case SV_PAD_BINDINGS: return "Input works, but the game bindings need attention";
    case SV_PAD_MENU_NAV: return "Input works in SDL, but menu navigation does not receive it";
    case SV_PAD_LIVE: return "Input received";
    default: return "Unknown";
  }
}

const char *sv_pad_state_advice(SvPadState s) {
  switch (s) {
    case SV_PAD_NO_JOYSTICK:
      return "SDL lists no joystick or gamepad. Connect the controller (press a button to wake it), try another cable or port, and check the "
             "OS-level device list below. A controller can be visible to the OS yet unsupported by the bundled SDL on this connection type.";
    case SV_PAD_NOT_A_GAMEPAD:
      return "SDL sees a raw joystick but has no gamepad mapping or driver for it, so its buttons cannot be trusted as gameplay input. "
             "Segarecomp does not guess raw axes as buttons. A mapping line for the GUID below in gamecontrollerdb.txt makes it a gamepad.";
    case SV_PAD_OPEN_FAILED:
      return "SDL recognised the device but refused to open it (see the SDL error). Another program may hold it exclusively, or the OS denies access "
             "(permissions / udev on Linux).";
    case SV_PAD_NO_EVENTS:
      return "The device is open. Press buttons or move a stick: nothing has arrived yet. If that stays so, the driver opened the device but "
             "produces no input.";
    case SV_PAD_BINDINGS:
      return "Buttons arrive, but a console action has no gamepad button or one button drives two actions. Fix it in Controls (or Restore Defaults).";
    case SV_PAD_MENU_NAV:
      return "SDL delivers this pad's input, but the launcher's menu navigation has not reacted. Use the menu test below and report this state.";
    case SV_PAD_LIVE:
      return "Button and axis events from this device are arriving.";
    default: return "";
  }
}

void sv_pad_binding_audit(const SvSettings *settings, SvSystem system, int player, int *missing, int *conflicts) {
  int a, b, miss = 0, conf = 0;
  if (settings != NULL && system >= 0 && system < SV_SYSTEM_COUNT && player >= 0 && player < SV_PLAYERS) {
    const SvBindings *bind = &settings->bind[system];
    for (a = 0; a < SV_ACT_COUNT; ++a) {
      if (!sv_action_available(system, player, (SvAction)a)) continue;
      if (a <= SV_ACT_RIGHT) continue; /* D-pad directions may rely on the left stick / D-pad defaults */
      if (bind->pad[player][a] < 0) { ++miss; continue; }
      for (b = a + 1; b < SV_ACT_COUNT; ++b)
        if (sv_action_available(system, player, (SvAction)b) && bind->pad[player][b] == bind->pad[player][a]) { ++conf; break; }
    }
  }
  if (missing != NULL) *missing = miss;
  if (conflicts != NULL) *conflicts = conf;
}

const char *sv_pad_nintendo_label(int b) {
  switch (b) {
    case SV_PAD_SOUTH: return "B";
    case SV_PAD_EAST: return "A";
    case SV_PAD_WEST: return "Y";
    case SV_PAD_NORTH: return "X";
    case SV_PAD_BACK: return "-";
    case SV_PAD_START: return "+";
    case SV_PAD_GUIDE: return "Home";
    default: return NULL;
  }
}

const char *sv_pad_position_name(int b) {
  switch (b) {
    case SV_PAD_SOUTH: return "South";
    case SV_PAD_EAST: return "East";
    case SV_PAD_WEST: return "West";
    case SV_PAD_NORTH: return "North";
    case SV_PAD_BACK: return "Back";
    case SV_PAD_GUIDE: return "Guide";
    case SV_PAD_START: return "Start";
    case 7: return "Left stick";
    case 8: return "Right stick";
    case 9: return "Left shoulder";
    case 10: return "Right shoulder";
    case SV_PAD_DPAD_UP: return "D-pad Up";
    case SV_PAD_DPAD_DOWN: return "D-pad Down";
    case SV_PAD_DPAD_LEFT: return "D-pad Left";
    case SV_PAD_DPAD_RIGHT: return "D-pad Right";
    case 15: return "Misc 1";
    case 16: return "Right paddle 1";
    case 17: return "Left paddle 1";
    case 18: return "Right paddle 2";
    case 19: return "Left paddle 2";
    case 20: return "Touchpad";
    default: return "Button";
  }
}
