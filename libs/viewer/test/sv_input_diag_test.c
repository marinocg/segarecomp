/* SEG-058 corrective delivery: the input diagnostics (the SDL layer under the launcher's Input page and the viewer's Controller status) against
 * SDL's virtual joysticks. It proves the A-F classification with REAL SDL events (not a mock of the diagnostics): no joystick, a joystick SDL
 * has no gamepad mapping for, a recognised gamepad whose SDL_OpenGamepad fails (SDL's error text is kept), an opened pad that is silent, live
 * input, input with broken game bindings, hot-unplug/replug, and a user mapping file turning a raw joystick into a gamepad. No window, no
 * display, no physical device: this does NOT prove any real controller works (see docs/testing/input-diagnostics.md). Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_input_diag.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static void pump(SvDiag *d) {
  SDL_Event e;
  SDL_UpdateJoysticks();
  SDL_PumpEvents();
  while (SDL_PollEvent(&e)) sv_diag_event(d, &e);
}

static SDL_JoystickID attach(SDL_JoystickType type, const char *name, Uint16 vid, Uint16 pid) {
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = (Uint16)type;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.vendor_id = vid;
  desc.product_id = pid;
  desc.name = name;
  return SDL_AttachVirtualJoystick(&desc);
}

static SDL_Gamepad *failing_open(SDL_JoystickID id) {
  (void)id;
  SDL_SetError("simulated: access to the device was denied");
  return NULL;
}

int main(void) {
  SvDiag *d;
  SvSettings settings;
  SvDiagContext ctx;
  SDL_JoystickID raw, pad;
  SDL_Joystick *handle;
  char text[1500];
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0"); /* no real device may influence the test */
  if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) { printf("SDL_Init failed: %s\n", SDL_GetError()); return 2; }
  d = sv_diag_create();
  CHECK(d != NULL, "create");
  sv_diag_note_init(d, 1, 1, NULL);
  sv_settings_defaults(&settings);
  ctx.settings = &settings; ctx.system = SV_SYSTEM_GENESIS; ctx.player = 0; ctx.nav_checked = 0;

  /* A: nothing */
  sv_diag_scan(d);
  CHECK(sv_diag_count(d) == 0 && sv_diag_overall(d, &ctx) == SV_PAD_NO_JOYSTICK, "A: no joystick is reported as such");
  CHECK(strcmp(sv_pad_state_code(SV_PAD_NO_JOYSTICK), "A") == 0, "A code");

  /* B: a joystick without a gamepad mapping (type unknown, no mapping line) */
  raw = attach(SDL_JOYSTICK_TYPE_UNKNOWN, "Fictional Raw Stick", 0x1234, 0x0001);
  CHECK(raw != 0, "the raw virtual joystick attaches");
  pump(d);
  {
    const SvDiagDevice *v = sv_diag_find(d, raw);
    CHECK(v != NULL, "B: the diagnostics learned of the joystick through the hot-plug event");
    CHECK(v != NULL && !v->is_gamepad, "B: it is not a gamepad");
    CHECK(v != NULL && v->vendor == 0x1234 && v->product == 0x0001 && v->guid[0] != '\0', "B: vendor/product/GUID are exposed");
    CHECK(v != NULL && sv_diag_state_of(d, v, &ctx) == SV_PAD_NOT_A_GAMEPAD, "B: classified as a joystick that is not a gamepad");
    CHECK(sv_diag_gamepad(d, raw) == NULL, "B: no gamepad handle is invented");
    CHECK(v != NULL && v->raw_buttons > 0, "B: the raw shape is reported");
  }
  CHECK(sv_diag_overall(d, &ctx) == SV_PAD_NOT_A_GAMEPAD, "B: overall");
  /* raw events are counted but never become gameplay input */
  handle = SDL_OpenJoystick(raw);
  SDL_SetJoystickVirtualButton(handle, 0, true);
  pump(d);
  {
    const SvDiagDevice *v = sv_diag_find(d, raw);
    CHECK(v != NULL && v->raw_events > 0 && v->events == 0 && v->buttons == 0, "B: raw events are counted separately and never as gamepad buttons");
    CHECK(v != NULL && sv_diag_state_of(d, v, &ctx) == SV_PAD_NOT_A_GAMEPAD, "B: raw input does not make it a working gamepad");
  }
  SDL_CloseJoystick(handle);

  /* a user mapping turns it into a gamepad explicitly */
  {
    const SvDiagDevice *v = sv_diag_find(d, raw);
    char path[256], line[400];
    FILE *f;
    snprintf(path, sizeof path, "sv_diag_test_mapping_%u.txt", (unsigned)SDL_GetTicks());
    f = fopen(path, "wb");
    CHECK(f != NULL, "the mapping file can be written");
    if (f != NULL && v != NULL) {
      snprintf(line, sizeof line, "%s,Fictional Raw Stick,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,platform:%s,\n", v->guid, SDL_GetPlatform()); /* SDL ignores mapping lines of another platform */
      fputs(line, f);
      fclose(f);
      CHECK(sv_diag_load_mapping_file(path) >= 1, "the user's mapping file loads");
      pump(d);
      v = sv_diag_find(d, raw);
      CHECK(v != NULL && v->is_gamepad && v->open_ok, "B->OK: after the mapping the joystick is an opened gamepad");
      CHECK(v != NULL && sv_diag_state_of(d, v, &ctx) == SV_PAD_NO_EVENTS, "D: the freshly mapped pad is opened but silent");
      remove(path);
    }
  }
  SDL_DetachVirtualJoystick(raw);
  pump(d);
  CHECK(sv_diag_count(d) == 0, "hot-unplug drops the device and closes its handles");

  /* C: recognised but SDL_OpenGamepad fails */
  sv_diag_set_open_hook(failing_open);
  pad = attach(SDL_JOYSTICK_TYPE_GAMEPAD, "Fictional Locked Pad", 0x057e, 0x2009);
  pump(d);
  {
    const SvDiagDevice *v = sv_diag_find(d, pad);
    CHECK(v != NULL && v->is_gamepad && v->open_attempted && !v->open_ok, "C: an open failure is recorded");
    CHECK(v != NULL && strstr(v->open_error, "access to the device was denied") != NULL, "C: SDL's exact error text is kept");
    CHECK(v != NULL && sv_diag_state_of(d, v, &ctx) == SV_PAD_OPEN_FAILED, "C: classified as open failed");
    CHECK(sv_diag_gamepad(d, pad) == NULL, "C: no handle");
  }
  sv_diag_summary(d, text, sizeof text);
  CHECK(strstr(text, "OPEN FAILED") != NULL && strstr(text, "057e:2009") != NULL, "C: the plain-text summary names the failure and the VID:PID");
  SDL_DetachVirtualJoystick(pad);
  pump(d);
  sv_diag_set_open_hook(NULL);

  /* D, then live: a working virtual gamepad */
  pad = attach(SDL_JOYSTICK_TYPE_GAMEPAD, "Fictional Live Pad", 0x057e, 0x2069);
  pump(d);
  {
    const SvDiagDevice *v = sv_diag_find(d, pad);
    CHECK(v != NULL && v->open_ok && sv_diag_gamepad(d, pad) != NULL, "D: the gamepad opens");
    CHECK(v != NULL && sv_diag_state_of(d, v, &ctx) == SV_PAD_NO_EVENTS, "D: enumerated and opened is NOT reported as working");
  }
  handle = SDL_OpenJoystick(pad);
  SDL_SetJoystickVirtualButton(handle, SDL_GAMEPAD_BUTTON_EAST, true);
  SDL_SetJoystickVirtualAxis(handle, SDL_GAMEPAD_AXIS_LEFTX, 20000);
  pump(d);
  {
    const SvDiagDevice *v = sv_diag_find(d, pad);
    CHECK(v != NULL && v->events >= 2, "live: button and axis events are counted");
    CHECK(v != NULL && (v->buttons & (1u << SDL_GAMEPAD_BUTTON_EAST)) != 0 && v->last_button == SDL_GAMEPAD_BUTTON_EAST, "live: the pressed button is tracked by SDL position");
    CHECK(v != NULL && v->axis[SDL_GAMEPAD_AXIS_LEFTX] > 15000, "live: the stick value is tracked");
    CHECK(v != NULL && sv_diag_state_of(d, v, &ctx) == SV_PAD_LIVE, "live: classified as receiving input with usable defaults");
  }
  CHECK(strcmp(sv_pad_nintendo_label(SDL_GAMEPAD_BUTTON_EAST), "A") == 0 && strcmp(sv_pad_nintendo_label(SDL_GAMEPAD_BUTTON_SOUTH), "B") == 0, "East is the printed Nintendo A, South is B");
  /* focus loss clears pressed state */
  sv_diag_release(d);
  CHECK(sv_diag_find(d, pad)->buttons == 0 && sv_diag_find(d, pad)->axis[SDL_GAMEPAD_AXIS_LEFTX] == 0, "focus loss releases everything");

  /* E: live but the game bindings are broken (a button bound twice; an action unbound) */
  settings.bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON2] = settings.bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON1];
  settings.bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_START] = -1;
  CHECK(sv_diag_state_of(d, sv_diag_find(d, pad), &ctx) == SV_PAD_BINDINGS, "E: live input with conflicting or missing bindings");
  sv_settings_defaults(&settings);
  CHECK(sv_diag_state_of(d, sv_diag_find(d, pad), &ctx) == SV_PAD_LIVE, "E: restoring the defaults clears it");

  /* F: SDL delivers, the launcher's menu navigation has not reacted */
  ctx.settings = NULL; ctx.nav_checked = 1;
  CHECK(sv_diag_state_of(d, sv_diag_find(d, pad), &ctx) == SV_PAD_MENU_NAV, "F: SDL input without menu navigation is its own class");
  sv_diag_note_nav(d, pad);
  CHECK(sv_diag_state_of(d, sv_diag_find(d, pad), &ctx) == SV_PAD_LIVE, "F: cleared once navigation reacted");
  ctx.settings = &settings; ctx.nav_checked = 0;

  /* Switch 2 diagnostics from the same facts (a virtual device with the Switch 2 Pro vendor/product proves the DIAGNOSTIC chain only, never the
   * Nintendo protocol or a physical controller). Gamepad opened and live: the working verdict, no failure text. */
  {
    char msg[1536];
    CHECK(sv_diag_switch2_message(d, &ctx, NULL, 0, msg, sizeof msg) == SV_S2_WORKING && strstr(msg, "events are arriving") != NULL, "Switch 2: an opened, live pad is the working verdict");
    CHECK(strstr(msg, "Connection:") == NULL, "Switch 2: no connection type is invented when SDL does not report one");
    sv_diag_summary(d, text, sizeof text);
    CHECK(strstr(text, "Nintendo Switch 2 controller detected.") != NULL, "Switch 2: the copyable summary carries the verdict");
  }
  /* unplug / replug keeps working and does not leak the old slot */
  SDL_CloseJoystick(handle);
  SDL_DetachVirtualJoystick(pad);
  pump(d);
  CHECK(sv_diag_count(d) == 0 && sv_diag_overall(d, &ctx) == SV_PAD_NO_JOYSTICK, "unplug returns to state A");
  pad = attach(SDL_JOYSTICK_TYPE_GAMEPAD, "Fictional Live Pad", 0x057e, 0x2069);
  pump(d);
  CHECK(sv_diag_count(d) == 1 && sv_diag_find(d, pad) != NULL && sv_diag_find(d, pad)->open_ok, "replug opens again");

  /* a second pad: both are tracked and Player 2 can be told apart by order */
  {
    SDL_JoystickID second = attach(SDL_JOYSTICK_TYPE_GAMEPAD, "Fictional Second Pad", 0x045e, 0x028e);
    pump(d);
    CHECK(sv_diag_count(d) == 2, "two controllers are listed");
    SDL_DetachVirtualJoystick(second);
    pump(d);
    CHECK(sv_diag_count(d) == 1, "removing one leaves the other");
  }
  /* Switch 2 chain with a raw (no gamepad mapping) device and with a failing open; transport only when reported */
  SDL_DetachVirtualJoystick(pad);
  pump(d);
  {
    char msg[1536];
    SvDiagHid hid;
    SDL_JoystickID s2 = attach(SDL_JOYSTICK_TYPE_UNKNOWN, "Virtual Switch 2 Pro (diagnostic path only)", 0x057e, 0x2069);
    const SvSwitch2Verdict expected = !sv_diag_switch2_driver_declared() ? SV_S2_DRIVER_ABSENT : (!sv_diag_libusb_available(d) ? SV_S2_LIBUSB_MISSING : SV_S2_USB_NOT_CLAIMED);
    pump(d);
    CHECK(sv_diag_switch2_message(d, &ctx, NULL, 0, msg, sizeof msg) == expected, "Switch 2: a raw joystick is attributed to the first missing link (driver, libusb, USB claim)");
    CHECK(strstr(msg, "Connection:") == NULL, "Switch 2: transport is not guessed without a report");
    memset(&hid, 0, sizeof hid);
    hid.vendor = 0x057e; hid.product = 0x2069; hid.bus = 2;
    CHECK(sv_diag_switch2_message(d, &ctx, &hid, 1, msg, sizeof msg) == SV_S2_BLUETOOTH && strstr(msg, "Connection: Bluetooth.") != NULL && strstr(msg, "does not provide native Switch 2 Bluetooth") != NULL,
          "Switch 2: a Bluetooth OS-scan entry yields the Bluetooth message");
    SDL_SetHint("SEGARECOMP_HIDAPI_SETUP_057e_2069", "begin iface=0 | stage=init_device FAILED driver=Switch2 error=\"Couldn't claim interface 1: -12\"");
    CHECK(sv_diag_switch2_message(d, &ctx, NULL, 0, msg, sizeof msg) == (expected == SV_S2_USB_NOT_CLAIMED ? SV_S2_SETUP_FAILED : expected), "Switch 2: SDL's published setup trace selects the setup verdict only when driver and libusb are present");
    if (expected == SV_S2_USB_NOT_CLAIMED) CHECK(strstr(msg, "interface 1: -12 (NOT_SUPPORTED)") != NULL, "Switch 2: the trace reaches the message with the libusb error named");
    SDL_ResetHint("SEGARECOMP_HIDAPI_SETUP_057e_2069");
    hid.bus = 1;
    CHECK(sv_diag_switch2_message(d, &ctx, &hid, 1, msg, sizeof msg) == expected && strstr(msg, "Connection: USB.") != NULL, "Switch 2: a USB OS-scan entry keeps the chain and states the connection");
    SDL_DetachVirtualJoystick(s2);
    pump(d);
    CHECK(sv_diag_switch2_message(d, &ctx, NULL, 0, msg, sizeof msg) == SV_S2_NOT_SEEN && msg[0] == '\0', "Switch 2: nothing is reported when no Switch 2 device is visible");
    CHECK(sv_diag_switch2_message(d, &ctx, &hid, 1, msg, sizeof msg) == expected, "Switch 2: an OS-scan-only controller is still attributed");
    sv_diag_set_open_hook(failing_open);
    s2 = attach(SDL_JOYSTICK_TYPE_GAMEPAD, "Virtual Switch 2 Pro (diagnostic path only)", 0x057e, 0x2069);
    pump(d);
    CHECK(sv_diag_switch2_message(d, &ctx, NULL, 0, msg, sizeof msg) == SV_S2_OPEN_FAILED && strstr(msg, "Gamepad initialization: failed.") != NULL && strstr(msg, "Details: simulated: access to the device was denied") != NULL,
          "Switch 2: an open failure shows SDL's own error text");
    SDL_DetachVirtualJoystick(s2);
    pump(d);
    sv_diag_set_open_hook(NULL);
    pad = attach(SDL_JOYSTICK_TYPE_GAMEPAD, "Fictional Live Pad", 0x057e, 0x2069);
    pump(d);
  }
  /* the state ranking: the most advanced device wins */
  {
    const SvPadState mix[3] = {SV_PAD_NO_JOYSTICK, SV_PAD_OPEN_FAILED, SV_PAD_LIVE};
    CHECK(sv_pad_overall(mix, 3) == SV_PAD_LIVE && sv_pad_overall(mix, 2) == SV_PAD_OPEN_FAILED && sv_pad_overall(mix, 0) == SV_PAD_NO_JOYSTICK, "overall state ranking");
  }
  /* init failure is visible */
  sv_diag_note_init(d, 1, 0, "simulated: no gamepad support");
  sv_diag_summary(d, text, sizeof text);
  CHECK(!sv_diag_gamepad_subsystem_ok(d) && strstr(text, "gamepad subsystem FAILED") != NULL && strstr(text, "simulated: no gamepad support") != NULL, "an SDL_INIT_GAMEPAD failure is part of the summary");
  CHECK(strstr(text, "SDL ") != NULL, "the SDL version is part of the summary");
  CHECK(strstr(text, "/") == NULL || strstr(text, ".z64") == NULL, "the summary carries no file paths");
  /* the OS-level scan is read-only and must not crash with no Nintendo hardware */
  {
    SvDiagHid hid[SV_DIAG_MAX_HID];
    char err[96];
    const int n = sv_diag_hid_scan(d, hid, SV_DIAG_MAX_HID, err, sizeof err);
    CHECK(n >= -1, "the OS-level scan returns a count or an error");
  }
  sv_diag_destroy(d);
  SDL_Quit();
  if (failures != 0) { printf("%d failure(s)\n", failures); return 1; }
  printf("sv_input_diag_test: ok\n");
  return 0;
}
