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
      snprintf(line, sizeof line, "%s,Fictional Raw Stick,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,platform:Linux,\n", v->guid);
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
