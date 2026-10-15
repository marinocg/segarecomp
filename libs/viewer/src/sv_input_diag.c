#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_input_diag.h"

struct SvDiag {
  SvDiagDevice dev[SV_DIAG_MAX_DEVICES];
  SDL_Gamepad *pad[SV_DIAG_MAX_DEVICES];
  SDL_Joystick *joy[SV_DIAG_MAX_DEVICES]; /* opened raw joystick of a device that is not a gamepad (to count raw events) */
  int count;
  int video_ok, gamepad_ok;
  char gamepad_error[160];
  int trace;
  int yielded;         /* sv_diag_yield_exclusive_devices is in effect */
  char saved_hint[16]; /* the Switch 2 driver hint before the yield; "" = it was unset */
  int saved_hint_set;
  int libusb_loadable; /* probed once at setup, never during hotplug/event logging */
};

static SDL_Gamepad *(*g_open_hook)(SDL_JoystickID id) = NULL;

void sv_diag_set_open_hook(SDL_Gamepad *(*hook)(SDL_JoystickID id)) { g_open_hook = hook; }

static void copy_text(char *dst, size_t cap, const char *src) {
  if (cap == 0) return;
  snprintf(dst, cap, "%s", src != NULL ? src : "");
}

SvDiag *sv_diag_create(void) {
  SvDiag *d = (SvDiag *)calloc(1, sizeof *d);
  if (d != NULL) {
    d->libusb_loadable = sv_diag_libusb_loadable();
    const char *t = SDL_getenv("SEGARECOMP_INPUT_TRACE");
    d->trace = (t != NULL && *t != '\0' && *t != '0');
    if (d->trace) SDL_SetLogPriority(SDL_LOG_CATEGORY_INPUT, SDL_LOG_PRIORITY_DEBUG); /* SDL's own HIDAPI/libusb messages (for example "Couldn't load libusb") */
    d->video_ok = 1;
    d->gamepad_ok = 1;
  }
  return d;
}

static void close_slot(SvDiag *d, int i) {
  if (d->pad[i] != NULL) SDL_CloseGamepad(d->pad[i]);
  if (d->joy[i] != NULL) SDL_CloseJoystick(d->joy[i]);
  d->pad[i] = NULL;
  d->joy[i] = NULL;
}

void sv_diag_destroy(SvDiag *d) {
  int i;
  if (d == NULL) return;
  for (i = 0; i < d->count; ++i) close_slot(d, i);
  free(d);
}

void sv_diag_note_init(SvDiag *d, int video_ok, int gamepad_ok, const char *gamepad_error) {
  if (d == NULL) return;
  d->video_ok = video_ok != 0;
  d->gamepad_ok = gamepad_ok != 0;
  copy_text(d->gamepad_error, sizeof d->gamepad_error, gamepad_ok ? "" : gamepad_error);
  if (!gamepad_ok) SDL_Log("input: SDL gamepad subsystem failed to initialise: %s", d->gamepad_error[0] != '\0' ? d->gamepad_error : "(no SDL error text)");
}

static int find_index(const SvDiag *d, SDL_JoystickID id) {
  int i;
  for (i = 0; i < d->count; ++i) if (d->dev[i].id == id) return i;
  return -1;
}

static void fill_identity(SvDiagDevice *v, SDL_JoystickID id) {
  SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
  const char *name = SDL_GetGamepadNameForID(id);
  if (name == NULL) name = SDL_GetJoystickNameForID(id);
  copy_text(v->name, sizeof v->name, name != NULL ? name : "Unnamed device");
  SDL_GUIDToString(guid, v->guid, (int)sizeof v->guid);
  v->vendor = SDL_GetJoystickVendorForID(id);
  v->product = SDL_GetJoystickProductForID(id);
  v->version = SDL_GetJoystickProductVersionForID(id);
  v->is_gamepad = SDL_IsGamepad(id) ? 1 : 0;
  v->gamepad_type = v->is_gamepad ? (int)SDL_GetRealGamepadTypeForID(id) : (int)SDL_GAMEPAD_TYPE_UNKNOWN;
  v->player_index = SDL_GetJoystickPlayerIndexForID(id);
  v->connection = SV_DIAG_NOT_OPENED;
  v->last_button = -1;
  v->last_raw_button = -1;
}

static void try_open(SvDiag *d, int i) {
  SvDiagDevice *v = &d->dev[i];
  SDL_Joystick *joy = NULL;
  if (v->open_attempted) return;
  v->open_attempted = 1;
  v->open_error[0] = '\0';
  if (v->is_gamepad) {
    SDL_Gamepad *pad;
    SDL_ClearError();
    pad = g_open_hook != NULL ? g_open_hook(v->id) : SDL_OpenGamepad(v->id);
    if (pad == NULL) {
      copy_text(v->open_error, sizeof v->open_error, SDL_GetError()[0] != '\0' ? SDL_GetError() : "SDL_OpenGamepad failed without an error text");
      SDL_Log("input: SDL_OpenGamepad failed for \"%s\" (%04x:%04x): %s", v->name, v->vendor, v->product, v->open_error);
      return;
    }
    d->pad[i] = pad;
    v->open_ok = 1;
    joy = SDL_GetGamepadJoystick(pad);
    v->connection = (int)SDL_GetGamepadConnectionState(pad);
    {
      const int idx = SDL_GetGamepadPlayerIndex(pad);
      v->player_index = idx;
    }
    {
      const char *n = SDL_GetGamepadName(pad);
      if (n != NULL) copy_text(v->name, sizeof v->name, n);
    }
  } else {
    /* A raw joystick: open it only to count raw events and report its shape; nothing here feeds gameplay. */
    SDL_ClearError();
    joy = SDL_OpenJoystick(v->id);
    if (joy == NULL) {
      copy_text(v->open_error, sizeof v->open_error, SDL_GetError()[0] != '\0' ? SDL_GetError() : "SDL_OpenJoystick failed without an error text");
      SDL_Log("input: SDL_OpenJoystick failed for \"%s\" (%04x:%04x): %s", v->name, v->vendor, v->product, v->open_error);
      return;
    }
    d->joy[i] = joy;
    v->open_ok = 1;
    v->connection = (int)SDL_GetJoystickConnectionState(joy);
  }
  if (joy != NULL) {
    v->raw_buttons = SDL_GetNumJoystickButtons(joy);
    v->raw_axes = SDL_GetNumJoystickAxes(joy);
    v->raw_hats = SDL_GetNumJoystickHats(joy);
  }
}

static void add_device(SvDiag *d, SDL_JoystickID id) {
  int i;
  if (find_index(d, id) >= 0 || d->count >= SV_DIAG_MAX_DEVICES) return;
  i = d->count++;
  memset(&d->dev[i], 0, sizeof d->dev[i]);
  d->pad[i] = NULL;
  d->joy[i] = NULL;
  d->dev[i].id = id;
  d->dev[i].added_ms = SDL_GetTicks();
  fill_identity(&d->dev[i], id);
  try_open(d, i);
}

static void remove_device(SvDiag *d, SDL_JoystickID id) {
  const int i = find_index(d, id);
  if (i < 0) return;
  close_slot(d, i);
  memmove(&d->dev[i], &d->dev[i + 1], (size_t)(d->count - i - 1) * sizeof d->dev[0]);
  memmove(&d->pad[i], &d->pad[i + 1], (size_t)(d->count - i - 1) * sizeof d->pad[0]);
  memmove(&d->joy[i], &d->joy[i + 1], (size_t)(d->count - i - 1) * sizeof d->joy[0]);
  d->count--;
  d->pad[d->count] = NULL;
  d->joy[d->count] = NULL;
}

void sv_diag_scan(SvDiag *d) {
  int n = 0, i;
  SDL_JoystickID *ids;
  if (d == NULL) return;
  ids = SDL_GetJoysticks(&n);
  /* drop what vanished */
  for (i = d->count - 1; i >= 0; --i) {
    int k, found = 0;
    for (k = 0; ids != NULL && k < n; ++k) if (ids[k] == d->dev[i].id) { found = 1; break; }
    if (!found) remove_device(d, d->dev[i].id);
  }
  for (i = 0; ids != NULL && i < n; ++i) add_device(d, ids[i]);
  if (ids != NULL) SDL_free(ids);
}

static void note_pad_event(SvDiag *d, int i) {
  d->dev[i].events++;
  d->dev[i].last_event_ms = SDL_GetTicks();
}

void sv_diag_event(SvDiag *d, const SDL_Event *e) {
  int i;
  if (d == NULL || e == NULL) return;
  switch (e->type) {
    case SDL_EVENT_JOYSTICK_ADDED:
      add_device(d, e->jdevice.which);
      sv_diag_log_summary(d, "device added");
      break;
    case SDL_EVENT_JOYSTICK_REMOVED:
      remove_device(d, e->jdevice.which);
      sv_diag_log_summary(d, "device removed");
      break;
    case SDL_EVENT_GAMEPAD_REMAPPED: {
      /* a mapping arrived (for example from a user's gamecontrollerdb.txt): re-read the identity and open it now */
      i = find_index(d, e->gdevice.which);
      if (i >= 0 && !d->dev[i].is_gamepad) {
        fill_identity(&d->dev[i], e->gdevice.which);
        if (d->joy[i] != NULL) { SDL_CloseJoystick(d->joy[i]); d->joy[i] = NULL; }
        d->dev[i].open_attempted = 0;
        try_open(d, i);
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
      i = find_index(d, e->gdevice.which);
      if (i < 0) add_device(d, e->gdevice.which);
      else if (!d->dev[i].is_gamepad) { /* JOYSTICK_ADDED saw it as a plain joystick; a mapping now exists */
        fill_identity(&d->dev[i], e->gdevice.which);
        if (d->joy[i] != NULL) { SDL_CloseJoystick(d->joy[i]); d->joy[i] = NULL; }
        d->dev[i].open_attempted = 0;
        try_open(d, i);
      }
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      i = find_index(d, e->gbutton.which);
      if (i >= 0 && e->gbutton.button < 32) {
        const Uint32 bit = 1u << (unsigned)e->gbutton.button;
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) { d->dev[i].buttons |= bit; d->dev[i].last_button = e->gbutton.button; } else d->dev[i].buttons &= ~bit;
        note_pad_event(d, i);
        if (d->trace) SDL_Log("input: %s button %s (%s) on \"%s\"", e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "down" : "up", sv_pad_position_name(e->gbutton.button),
                              sv_pad_nintendo_label(e->gbutton.button) != NULL ? sv_pad_nintendo_label(e->gbutton.button) : "-", d->dev[i].name);
      }
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      i = find_index(d, e->gaxis.which);
      if (i >= 0 && e->gaxis.axis < 6) {
        d->dev[i].axis[e->gaxis.axis] = e->gaxis.value;
        note_pad_event(d, i);
        if (d->trace && (e->gaxis.value > 16000 || e->gaxis.value < -16000)) SDL_Log("input: axis %d = %d on \"%s\"", (int)e->gaxis.axis, (int)e->gaxis.value, d->dev[i].name);
      }
      break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
      i = find_index(d, e->jbutton.which);
      if (i >= 0 && !d->dev[i].is_gamepad) { d->dev[i].raw_events++; d->dev[i].last_event_ms = SDL_GetTicks(); d->dev[i].last_raw_button = e->jbutton.button; }
      break;
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
      i = find_index(d, e->jbutton.which);
      if (i >= 0 && !d->dev[i].is_gamepad) { d->dev[i].raw_events++; d->dev[i].last_event_ms = SDL_GetTicks(); }
      break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
      i = find_index(d, e->jaxis.which);
      if (i >= 0 && !d->dev[i].is_gamepad) { d->dev[i].raw_events++; d->dev[i].last_event_ms = SDL_GetTicks(); }
      break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
      i = find_index(d, e->jhat.which);
      if (i >= 0 && !d->dev[i].is_gamepad) { d->dev[i].raw_events++; d->dev[i].last_event_ms = SDL_GetTicks(); }
      break;
    default: break;
  }
}

void sv_diag_release(SvDiag *d) {
  int i;
  if (d == NULL) return;
  for (i = 0; i < d->count; ++i) { d->dev[i].buttons = 0; memset(d->dev[i].axis, 0, sizeof d->dev[i].axis); }
}

void sv_diag_note_nav(SvDiag *d, SDL_JoystickID id) {
  const int i = d != NULL ? find_index(d, id) : -1;
  if (i >= 0) d->dev[i].nav_ok = 1;
}

int sv_diag_count(const SvDiag *d) { return d != NULL ? d->count : 0; }
const SvDiagDevice *sv_diag_device(const SvDiag *d, int index) { return d != NULL && index >= 0 && index < d->count ? &d->dev[index] : NULL; }
const SvDiagDevice *sv_diag_find(const SvDiag *d, SDL_JoystickID id) { const int i = d != NULL ? find_index(d, id) : -1; return i >= 0 ? &d->dev[i] : NULL; }
SDL_Gamepad *sv_diag_gamepad(const SvDiag *d, SDL_JoystickID id) { const int i = d != NULL ? find_index(d, id) : -1; return i >= 0 ? d->pad[i] : NULL; }

SvPadState sv_diag_state_of(const SvDiag *d, const SvDiagDevice *v, const SvDiagContext *ctx) {
  SvPadFacts f;
  (void)d;
  memset(&f, 0, sizeof f);
  if (v == NULL) return SV_PAD_NO_JOYSTICK;
  f.enumerated = 1;
  f.is_gamepad = v->is_gamepad;
  f.open_attempted = v->open_attempted;
  f.open_ok = v->open_ok;
  f.events = v->events;
  if (ctx != NULL && ctx->settings != NULL) {
    f.bindings_checked = 1;
    sv_pad_binding_audit(ctx->settings, ctx->system, ctx->player, &f.bindings_missing, &f.bindings_conflicts);
  }
  if (ctx != NULL && ctx->nav_checked) { f.nav_checked = 1; f.nav_ok = v->nav_ok; }
  return sv_pad_classify(&f);
}

SvPadState sv_diag_overall(const SvDiag *d, const SvDiagContext *ctx) {
  SvPadState states[SV_DIAG_MAX_DEVICES];
  int i;
  if (d == NULL) return SV_PAD_NO_JOYSTICK;
  for (i = 0; i < d->count; ++i) states[i] = sv_diag_state_of(d, &d->dev[i], ctx);
  return sv_pad_overall(states, d->count);
}

int sv_diag_video_ok(const SvDiag *d) { return d != NULL && d->video_ok; }
int sv_diag_gamepad_subsystem_ok(const SvDiag *d) { return d != NULL && d->gamepad_ok; }
const char *sv_diag_gamepad_init_error(const SvDiag *d) { return d != NULL ? d->gamepad_error : ""; }

/* ------------------------------------------------------------------------------------------------------------------ OS-level view */

static const char *nintendo_family(unsigned product) {
  switch (product) {
    case 0x2069: return "Switch 2 Pro Controller";
    case 0x2066: return "Switch 2 Joy-Con (R)";
    case 0x2067: return "Switch 2 Joy-Con (L)";
    case 0x2068: return "Switch 2 Joy-Con pair";
    case 0x2073: return "Switch 2 GameCube controller";
    case 0x2009: return "Switch Pro Controller";
    case 0x2006: return "Switch Joy-Con (L)";
    case 0x2007: return "Switch Joy-Con (R)";
    case 0x200e: return "Switch Charging Grip";
    default: return "";
  }
}

int sv_diag_hid_scan(const SvDiag *d, SvDiagHid *out, int cap, char *error, size_t error_cap) {
  SDL_hid_device_info *list, *it;
  int n = 0, i;
  if (error != NULL && error_cap > 0) error[0] = '\0';
  if (SDL_hid_init() != 0) {
    if (error != NULL) copy_text(error, error_cap, SDL_GetError()[0] != '\0' ? SDL_GetError() : "SDL_hid_init failed");
    return -1;
  }
  list = SDL_hid_enumerate(0x057e, 0);
  for (it = list; it != NULL && n < cap; it = it->next) {
    int dup = 0;
    for (i = 0; i < n; ++i) if (out[i].product == it->product_id && out[i].bus == (int)it->bus_type && out[i].interface_number == it->interface_number) dup = 1;
    if (dup) continue;
    memset(&out[n], 0, sizeof out[n]);
    out[n].vendor = it->vendor_id;
    out[n].product = it->product_id;
    out[n].bus = (int)it->bus_type;
    out[n].interface_number = it->interface_number;
    if (it->product_string != NULL) {
      /* wchar_t product string: keep printable ASCII only */
      size_t k, o = 0;
      for (k = 0; it->product_string[k] != 0 && o + 1 < sizeof out[n].product_string; ++k)
        if (it->product_string[k] >= 32 && it->product_string[k] < 127) out[n].product_string[o++] = (char)it->product_string[k];
      out[n].product_string[o] = '\0';
    }
    copy_text(out[n].family, sizeof out[n].family, nintendo_family(it->product_id));
    if (d != NULL)
      for (i = 0; i < d->count; ++i)
        if (d->dev[i].vendor == it->vendor_id && d->dev[i].product == it->product_id) out[n].matched_joystick = 1;
    ++n;
  }
  if (list != NULL) SDL_hid_free_enumeration(list);
  (void)SDL_hid_exit();
  return n;
}

int sv_diag_libusb_available(const SvDiag *d) { return d != NULL && d->libusb_loadable; }

int sv_diag_libusb_loadable(void) {
#if defined(_WIN32)
  static const char *names[] = {"libusb-1.0.dll", "libusb-1.0"};
#elif defined(__APPLE__)
  static const char *names[] = {"libusb-1.0.0.dylib", "libusb-1.0.dylib", "/opt/homebrew/lib/libusb-1.0.0.dylib", "/usr/local/lib/libusb-1.0.0.dylib"};
#else
  static const char *names[] = {"libusb-1.0.so.0", "libusb-1.0.so"};
#endif
  size_t i;
  for (i = 0; i < sizeof names / sizeof names[0]; ++i) {
    SDL_SharedObject *so = SDL_LoadObject(names[i]);
    if (so != NULL) { SDL_UnloadObject(so); return 1; }
  }
  SDL_ClearError();
  return 0;
}

const char *sv_diag_switch2_hint_state(void) {
#ifdef SDL_HINT_JOYSTICK_HIDAPI_SWITCH2
  const char *v = SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH2);
  if (v == NULL) return "default";
  return *v != '\0' && *v != '0' ? "enabled" : "disabled";
#else
  return "not known to this SDL";
#endif
}

int sv_diag_switch2_driver_declared(void) {
  const char *rev = SDL_GetRevision();
  return rev != NULL && strstr(rev, "segarecomp-switch2-usb") != NULL;
}

SvSwitch2Verdict sv_diag_switch2_message(const SvDiag *d, const SvDiagContext *ctx, const SvDiagHid *hid, int hid_count, char *out, size_t cap) {
  SvSwitch2Facts f;
  const char *detail = NULL;
  int i;
  memset(&f, 0, sizeof f);
  f.state = SV_PAD_NO_JOYSTICK;
  f.driver_declared = sv_diag_switch2_driver_declared();
  f.libusb_loadable = d != NULL && d->libusb_loadable;
  for (i = 0x2066; i <= 0x2073; ++i) { /* the patched Segarecomp SDL publishes how Switch 2 driver setup went, per product, as a hint */
    char name[48];
    const char *trace;
    snprintf(name, sizeof name, "SEGARECOMP_HIDAPI_SETUP_057e_%04x", (unsigned)i);
    trace = SDL_GetHint(name);
    if (trace != NULL && *trace != '\0') { copy_text(f.setup_trace, sizeof f.setup_trace, trace); f.seen = 1; break; }
  }
  for (i = 0; hid != NULL && i < hid_count; ++i) {
    if (!sv_switch2_is_product(hid[i].vendor, hid[i].product)) continue;
    f.seen = 1;
    if (f.transport != 1 && (hid[i].bus == 1 || hid[i].bus == 2)) f.transport = hid[i].bus; /* USB wins when both buses are listed */
  }
  for (i = 0; d != NULL && i < d->count; ++i) {
    const SvDiagDevice *v = &d->dev[i];
    if (!sv_switch2_is_product(v->vendor, v->product)) continue;
    f.seen = 1;
    f.state = sv_diag_state_of(d, v, ctx);
    if (v->connection == SDL_JOYSTICK_CONNECTION_WIRED) f.transport = 1;
    else if (v->connection == SDL_JOYSTICK_CONNECTION_WIRELESS) f.transport = 2;
    detail = v->open_error;
    break;
  }
  if (out != NULL && cap > 0) (void)sv_switch2_message(&f, detail, out, cap);
  return sv_switch2_assess(&f);
}

void sv_diag_sdl_version(char *out, size_t cap) {
  const int v = SDL_GetVersion();
  snprintf(out, cap, "SDL %d.%d.%d (built against %d.%d.%d)", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_MICRO_VERSION);
}

int sv_diag_load_mapping_file(const char *path) {
  int n;
  SDL_ClearError();
  n = SDL_AddGamepadMappingsFromFile(path);
  if (n < 0) return -1;
  SDL_Log("input: loaded %d gamepad mapping(s) from the user's mapping file", n);
  return n;
}

/* ------------------------------------------------------------------------------------------------------------------ text */

static const char *connection_text(int c) {
  switch (c) {
    case SDL_JOYSTICK_CONNECTION_WIRED: return "wired (USB)";
    case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
    case SDL_JOYSTICK_CONNECTION_UNKNOWN: return "unknown";
    case SV_DIAG_NOT_OPENED: return "not opened";
    default: return "invalid";
  }
}

size_t sv_diag_summary(const SvDiag *d, char *out, size_t cap) {
  size_t used = 0;
  int i;
  char ver[96];
#define ADD(...) do { if (used < cap) { const int w = snprintf(out + used, cap - used, __VA_ARGS__); if (w > 0) used += (size_t)w < cap - used ? (size_t)w : cap - used - 1; } } while (0)
  if (cap == 0) return 0;
  out[0] = '\0';
  sv_diag_sdl_version(ver, sizeof ver);
  ADD("%s; video %s; gamepad subsystem %s%s%s\n", ver, sv_diag_video_ok(d) ? "ok" : "FAILED", sv_diag_gamepad_subsystem_ok(d) ? "ok" : "FAILED",
      sv_diag_gamepad_subsystem_ok(d) ? "" : ": ", sv_diag_gamepad_subsystem_ok(d) ? "" : sv_diag_gamepad_init_error(d));
  ADD("Switch 2 HIDAPI hint: %s; native Switch 2 USB driver declared by this SDL build: %s; libusb-1.0 loadable: %s; devices: %d\n", sv_diag_switch2_hint_state(),
      sv_diag_switch2_driver_declared() ? "yes" : "no", d != NULL && d->libusb_loadable ? "yes" : "no", sv_diag_count(d));
  for (i = 0; i < sv_diag_count(d); ++i) {
    const SvDiagDevice *v = sv_diag_device(d, i);
    ADD("[%d] \"%s\" id=%u vid:pid=%04x:%04x guid=%s %s, %s, %s", i, v->name, (unsigned)v->id, v->vendor, v->product, v->guid, v->is_gamepad ? "gamepad" : "raw joystick (no gamepad mapping)",
        connection_text(v->connection), v->open_attempted ? (v->open_ok ? "opened" : "OPEN FAILED") : "not opened");
    if (v->open_error[0] != '\0') ADD(" [%s]", v->open_error);
    ADD(", events=%lu%s\n", v->is_gamepad ? v->events : v->raw_events, v->is_gamepad ? "" : " (raw)");
  }
  {
    char s2[1536];
    SvDiagContext ctx;
    memset(&ctx, 0, sizeof ctx);
    if (sv_diag_switch2_message(d, &ctx, NULL, 0, s2, sizeof s2) != SV_S2_NOT_SEEN) ADD("%s\n", s2);
  }
#undef ADD
  return used;
}

void sv_diag_log_summary(const SvDiag *d, const char *why) {
  char text[2600];
  if (d == NULL) return;
  (void)sv_diag_summary(d, text, sizeof text);
  SDL_Log("input diagnostics (%s):\n%s", why != NULL ? why : "summary", text);
}

/* ------------------------------------------------------------------------------------------------------------------ exclusive devices */

void sv_diag_yield_exclusive_devices(SvDiag *d) {
#ifdef SDL_HINT_JOYSTICK_HIDAPI_SWITCH2
  const char *prior;
  if (d == NULL || d->yielded) return;
  prior = SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH2);
  d->saved_hint_set = prior != NULL;
  copy_text(d->saved_hint, sizeof d->saved_hint, prior);
  d->yielded = 1;
  (void)SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_SWITCH2, "0", SDL_HINT_OVERRIDE);
  SDL_UpdateJoysticks(); /* SDL applies a driver hint change during a joystick update: the libusb handle is closed before this returns */
  SDL_Log("input: released exclusive (libusb) controllers for the game process");
#else
  (void)d;
#endif
}

void sv_diag_reclaim_exclusive_devices(SvDiag *d) {
#ifdef SDL_HINT_JOYSTICK_HIDAPI_SWITCH2
  if (d == NULL || !d->yielded) return;
  d->yielded = 0;
  if (d->saved_hint_set) (void)SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_SWITCH2, d->saved_hint, SDL_HINT_OVERRIDE);
  else (void)SDL_ResetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH2);
  SDL_UpdateJoysticks();
  SDL_Log("input: took the exclusive (libusb) controllers back from the game process");
#else
  (void)d;
#endif
}

int sv_diag_exclusive_devices_yielded(const SvDiag *d) { return d != NULL && d->yielded; }
