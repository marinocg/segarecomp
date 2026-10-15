#include <stddef.h>
#include <stdio.h>

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

/* ------------------------------------------------------------------------------------------------------------------ Switch 2 */

int sv_switch2_is_product(unsigned vendor, unsigned product) { return vendor == 0x057eu && product >= 0x2066u && product <= 0x2073u; }

SvSwitch2Verdict sv_switch2_assess(const SvSwitch2Facts *f) {
  if (f == NULL || !f->seen) return SV_S2_NOT_SEEN;
  switch (f->state) {
    case SV_PAD_OPEN_FAILED: return SV_S2_OPEN_FAILED;
    case SV_PAD_NO_EVENTS: return SV_S2_NO_EVENTS;
    case SV_PAD_BINDINGS: return SV_S2_BINDINGS;
    case SV_PAD_MENU_NAV: return SV_S2_NAV_PENDING;
    case SV_PAD_LIVE: return SV_S2_WORKING;
    default: break; /* SV_PAD_NO_JOYSTICK / SV_PAD_NOT_A_GAMEPAD: SDL has no gamepad for it */
  }
  if (f->transport == 2) return SV_S2_BLUETOOTH;
  if (!f->driver_declared) return SV_S2_DRIVER_ABSENT;
  if (!f->libusb_loadable) return SV_S2_LIBUSB_MISSING;
  return SV_S2_USB_NOT_CLAIMED;
}

size_t sv_switch2_message(const SvSwitch2Facts *f, const char *detail, char *out, size_t cap) {
  size_t used = 0;
  SvSwitch2Verdict v;
#define ADD(...) do { if (used < cap) { const int w = snprintf(out + used, cap - used, __VA_ARGS__); if (w > 0) used += (size_t)w < cap - used ? (size_t)w : cap - used - 1; } } while (0)
  if (cap == 0) return 0;
  out[0] = '\0';
  v = sv_switch2_assess(f);
  if (v == SV_S2_NOT_SEEN) return 0;
  ADD("Nintendo Switch 2 controller detected.\n");
  if (f->transport == 1 || f->transport == 2) ADD("Connection: %s.\n", f->transport == 2 ? "Bluetooth" : "USB");
  switch (v) {
    case SV_S2_BLUETOOTH:
      ADD("This SDL version does not provide native Switch 2 Bluetooth support. Connect the controller with a USB-C cable, or use Steam Input or a "
          "compatible controller bridge instead.");
      break;
    case SV_S2_DRIVER_ABSENT:
      ADD("Native USB support: NOT declared by this SDL build (the official SDL binaries for Windows do not contain the Switch 2 driver). Use the SDL "
          "that ships in the Segarecomp package, or Steam Input or a compatible controller bridge.");
      break;
    case SV_S2_LIBUSB_MISSING:
      ADD("Native USB support is available in this SDL build.\nlibusb runtime: NOT found, so the driver cannot start. Keep libusb-1.0 where SDL can load it "
          "(the Windows package ships libusb-1.0.dll beside the launcher and in sdl3/lib).");
      break;
    case SV_S2_USB_NOT_CLAIMED:
      ADD("Native USB support is available in this SDL build.\nlibusb runtime: available.\nSDL did not turn the controller into a gamepad. The USB interface may not be "
          "accessible to libusb (on Windows it must be bound to WinUSB; Segarecomp never installs or changes drivers; on Linux check udev), or another program "
          "holds it.\nRun with SEGARECOMP_INPUT_TRACE=1 for SDL's own message.");
      break;
    case SV_S2_OPEN_FAILED:
      if (f->driver_declared) ADD("Native USB support is available in this SDL build.\nlibusb runtime: %s\n", f->libusb_loadable ? "available." : "NOT found.");
      ADD("Gamepad initialization: failed.\nDetails: %s", detail != NULL && detail[0] != '\0' ? detail : "SDL gave no error text");
      break;
    case SV_S2_NO_EVENTS:
      ADD("Gamepad initialization: succeeded. No button or stick event has arrived yet: press a button. If nothing ever arrives the driver opened the "
          "device but receives no input.");
      break;
    case SV_S2_BINDINGS:
      ADD("Input events arrive, but the game bindings are missing or conflicting. Fix them in Controls (or Restore Defaults).");
      break;
    case SV_S2_NAV_PENDING:
      ADD("Input events arrive. Use the menu test to confirm that the launcher's own navigation receives them.");
      break;
    case SV_S2_WORKING:
      ADD("Gamepad initialization: succeeded; button and stick events are arriving through SDL's gamepad layer%s.",
          f->driver_declared ? " (this SDL build includes the native Switch 2 USB driver)" : "");
      break;
    default: break;
  }
#undef ADD
  return used;
}
