// SEG-058: the SDL-free logic of the shared viewer foundation (libs/viewer): settings persistence, bindings and conflicts, layout math,
// CRT profiles and the menu model. Everything is driven by synthetic values; no window, no clock.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "sv_font.h"
#include "sv_input_state.h"
#include "sv_layout.h"
#include "sv_menu.h"
#include "sv_settings.h"
}

namespace {
int failures = 0;
void check(bool ok, const std::string &what) {
  if (!ok) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

SvSettings defaults() { SvSettings s; sv_settings_defaults(&s); return s; }

void settings_tests() {
  SvSettings s = defaults();
  check(s.video.preset == SV_CRT_PIXEL_PERFECT && s.video.scanlines == 0 && s.video.brightness == 100 && !s.video.fullscreen, "defaults are the untouched picture");
  check(s.menu_key == SV_KEY_ESCAPE, "the menu key defaults to Escape");
  check(s.bind[SV_SYSTEM_GENESIS].key[0][SV_ACT_START] == SV_KEY_RETURN && s.bind[SV_SYSTEM_SMS].key[0][SV_ACT_PAUSE] == SV_KEY_P, "default maps: Genesis Start, SMS Pause");

  // round trip
  s.video.fullscreen = 1; s.video.scale = SV_SCALE_INTEGER; s.video.aspect = SV_ASPECT_4_3; sv_apply_crt_preset(&s.video, SV_CRT_CLASSIC);
  s.audio.volume = 35; s.audio.muted = 1; s.deadzone = 40; s.video.window_w = 900; s.video.window_h = 700;
  std::snprintf(s.pad_name, sizeof s.pad_name, "Fictional Pad");
  s.bind[SV_SYSTEM_SMS].key[0][SV_ACT_BUTTON1] = 20;
  char text[8192];
  const size_t n = sv_settings_format(&s, text, sizeof text);
  check(n > 100 && std::strlen(text) == n, "format returns the length");
  SvSettings t = defaults();
  const size_t recognised = sv_settings_parse(&t, text, n);
  check(recognised > 40, "every written key is recognised");
  check(t.video.fullscreen == 1 && t.video.scale == SV_SCALE_INTEGER && t.video.aspect == SV_ASPECT_4_3 && t.video.preset == SV_CRT_CLASSIC && t.video.scanlines == s.video.scanlines &&
            t.video.mask == s.video.mask && t.video.curvature == s.video.curvature && t.video.window_w == 900 && t.audio.volume == 35 && t.audio.muted == 1 && t.deadzone == 40 &&
            std::strcmp(t.pad_name, "Fictional Pad") == 0 && t.bind[SV_SYSTEM_SMS].key[0][SV_ACT_BUTTON1] == 20,
        "settings survive a text round trip");
  char tiny[16];
  check(sv_settings_format(&s, tiny, sizeof tiny) == 0, "a too-small buffer is reported, not truncated silently");

  // hostile / damaged input
  SvSettings d = defaults();
  const char *bad = "video.scanlines=999999\nvideo.brightness=-5\naudio.volume=abc\n=\nnonsense\nbind.genesis.p1.key.up=99999\nbind.genesis.p9.key.up=4\nvideo.preset=77\ninput.deadzone=1\n"
                    "bind.sms.p2.key.pause=40\ninput.menu_key=0\nvideo.window_w=3\n";
  sv_settings_parse(&d, bad, std::strlen(bad));
  check(d.video.scanlines == 0 || d.video.scanlines == 100, "out-of-range scanlines are rejected or clamped");
  check(d.video.brightness == 50, "brightness clamps to its range");
  check(d.audio.volume == 100, "a non-numeric value leaves the default");
  check(d.bind[SV_SYSTEM_GENESIS].key[0][SV_ACT_UP] == 0 || d.bind[SV_SYSTEM_GENESIS].key[0][SV_ACT_UP] < 512, "an out-of-range scancode never survives");
  check(d.video.preset == SV_CRT_PIXEL_PERFECT, "an unknown preset falls back to Pixel Perfect");
  check(d.deadzone == 5, "dead zone clamps");
  check(d.bind[SV_SYSTEM_SMS].key[1][SV_ACT_PAUSE] == 0, "the Pause button exists only for Player 1");
  check(d.menu_key == SV_KEY_ESCAPE, "an invalid menu key restores Escape (the menu stays reachable)");
  check(d.video.window_w == 0, "an absurd window size is dropped");
  SvSettings big = defaults();
  std::string huge(100000, 'x');
  check(sv_settings_parse(&big, huge.data(), huge.size()) == 0, "oversized junk is bounded and ignored");
  check(sv_settings_parse(&big, nullptr, 0) == 0, "null text is harmless");

  // file persistence (atomic)
  const char *path = "sv_core_test_settings.cfg";
  std::remove(path);
  SvSettings loaded = defaults();
  check(sv_settings_load(&loaded, path) != 0, "a missing file keeps the defaults and reports it");
  check(sv_settings_save(&s, path) == 0, "save");
  check(sv_settings_load(&loaded, path) == 0 && loaded.audio.volume == 35, "load");
  { FILE *f = std::fopen(path, "wb"); std::fputs("garbage \xff\xfe\n\n=\n", f); std::fclose(f); }
  SvSettings after_garbage = defaults();
  check(sv_settings_load(&after_garbage, path) == 0 && after_garbage.audio.volume == 100, "a damaged file loads as defaults");
  std::remove(path);
  std::remove("sv_core_test_settings.cfg.tmp");
}

void binding_tests() {
  SvSettings s = defaults();
  SvBindingOwner moved{};
  int conflict = 0;
  // a free key
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_BUTTON1, 20 /* Q */, &moved, &conflict) == 0 && !conflict && s.bind[SV_SYSTEM_GENESIS].key[0][SV_ACT_BUTTON1] == 20, "free key assigns");
  // Z is Genesis A by default... after the line above A is Q; X is B. Bind X to A: conflict with B.
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_BUTTON1, SV_KEY_X, &moved, &conflict) == 0 && conflict && moved.player == 0 && moved.action == SV_ACT_BUTTON2, "duplicate key is reported with its previous owner");
  check(s.bind[SV_SYSTEM_GENESIS].key[0][SV_ACT_BUTTON2] == 0 && s.bind[SV_SYSTEM_GENESIS].key[0][SV_ACT_BUTTON1] == SV_KEY_X, "the previous owner is unbound, never silently doubled");
  // across players
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 1, SV_ACT_BUTTON1, SV_KEY_X, &moved, &conflict) == 0 && conflict && moved.player == 0, "a key cannot drive two players");
  // reserved keys
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_UP, SV_KEY_ESCAPE, nullptr, nullptr) == -1, "Escape is reserved for the menu");
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_UP, SV_KEY_F12, nullptr, nullptr) == -1, "F12 is a host hotkey");
  s.menu_key = 20;
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_UP, 20, nullptr, nullptr) == -1, "the configured menu key is reserved too");
  // unavailable actions
  check(sv_assign_key(&s, SV_SYSTEM_SMS, 0, SV_ACT_START, 30, nullptr, nullptr) == -1, "SMS has no Start action");
  check(sv_assign_key(&s, SV_SYSTEM_SMS, 1, SV_ACT_PAUSE, 30, nullptr, nullptr) == -1, "Pause belongs to Player 1");
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_PAUSE, 30, nullptr, nullptr) == -1, "Genesis has no console Pause button");
  // clear
  check(sv_assign_key(&s, SV_SYSTEM_GENESIS, 0, SV_ACT_LEFT, 0, nullptr, nullptr) == 0 && s.bind[SV_SYSTEM_GENESIS].key[0][SV_ACT_LEFT] == 0, "clearing is allowed");
  // pads: same layout for both players is not a conflict, but inside one player it is
  SvSettings p = defaults();
  check(sv_assign_pad(&p, SV_SYSTEM_GENESIS, 1, SV_ACT_BUTTON1, SV_PAD_SOUTH, &moved, &conflict) == 0 && !conflict, "two players may share a pad layout (each reads its own device)");
  check(sv_assign_pad(&p, SV_SYSTEM_GENESIS, 0, SV_ACT_BUTTON2, SV_PAD_SOUTH, &moved, &conflict) == 0 && conflict && moved.action == SV_ACT_BUTTON1 && p.bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON1] == -1,
        "a duplicate pad button inside one player moves the binding");
  check(sv_assign_pad(&p, SV_SYSTEM_GENESIS, 0, SV_ACT_BUTTON1, 99, nullptr, nullptr) == -1, "an invalid button is refused");
  // axis helper
  for (SvSystem sys : {SV_SYSTEM_SMS, SV_SYSTEM_GENESIS}) {
    SvSettings second = defaults();
    check(sv_find_pad_conflict(&second, sys, SV_PAD_EAST, 1, SV_ACT_BUTTON1, &moved) == 1 && moved.player == 1 && moved.action == SV_ACT_BUTTON2,
          "pad lookup searches the requested player's device, not P1 first");
    check(sv_assign_pad(&second, sys, 1, SV_ACT_BUTTON1, SV_PAD_EAST, &moved, &conflict) == 0 && conflict && moved.player == 1 && moved.action == SV_ACT_BUTTON2 &&
              second.bind[sys].pad[1][SV_ACT_BUTTON2] == -1 && second.bind[sys].pad[1][SV_ACT_BUTTON1] == SV_PAD_EAST &&
              second.bind[sys].pad[0][SV_ACT_BUTTON2] == SV_PAD_EAST,
          "P2 conflict moves only P2's binding, including generic Genesis configuration");
    check(sv_assign_pad(&second, sys, 1, SV_ACT_BUTTON1, SV_PAD_EAST, &moved, &conflict) == 0 && !conflict, "reassigning the same pad action has no conflict");
  }
  check(sv_axis_direction(0, 25) == 0 && sv_axis_direction(8000, 25) == 0 && sv_axis_direction(9000, 25) == 1 && sv_axis_direction(-9000, 25) == -1 && sv_axis_direction(32767, 25) == 1, "stick dead zone");
  check(sv_axis_direction(9000, 60) == 0, "a larger dead zone ignores more travel");
  // sanitize drops reserved bindings from a loaded file
  SvSettings q = defaults();
  q.bind[SV_SYSTEM_SMS].key[0][SV_ACT_BUTTON1] = SV_KEY_ESCAPE;
  sv_settings_sanitize(&q);
  check(q.bind[SV_SYSTEM_SMS].key[0][SV_ACT_BUTTON1] == 0, "a reserved key in a loaded file is dropped");
}

void layout_tests() {
  // 256x192 SMS picture
  SvRect r = sv_layout_dest(768, 576, 256, 192, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  check(r.x == 0 && r.y == 0 && r.w == 768 && r.h == 576, "exact 3x fit");
  r = sv_layout_dest(1000, 576, 256, 192, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  check(r.w == 768 && r.h == 576 && r.x == 116 && r.y == 0, "letterboxed horizontally, centred");
  r = sv_layout_dest(1000, 700, 256, 192, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  check(r.w == 768 && r.h == 576 && r.x == 116 && r.y == 62, "integer scale keeps whole multiples");
  r = sv_layout_dest(200, 150, 256, 192, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  check(r.w <= 200 && r.h <= 150 && r.w > 0, "below 1x integer falls back to fit");
  // 192 <-> 224 active height keeps the right aspect
  const SvRect a = sv_layout_dest(1024, 768, 256, 192, SV_ASPECT_ORIGINAL, SV_SCALE_FIT), b = sv_layout_dest(1024, 768, 256, 224, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  check(a.w * 192 == a.h * 256 || std::abs(a.w * 192 - a.h * 256) <= 192, "192-line picture keeps 256:192");
  check(b.w == 877 || b.w == 878 || std::abs(b.w * 224 - b.h * 256) <= 224, "224-line picture keeps 256:224");
  check(b.h == 768 || b.w == 1024, "the 224-line picture still fills one axis");
  // 4:3 correction of a 320x224 Genesis picture
  r = sv_layout_dest(1280, 960, 320, 224, SV_ASPECT_4_3, SV_SCALE_FIT);
  check(r.w == 1280 && r.h == 960, "H40 corrected to a 4:3 display");
  r = sv_layout_dest(1280, 960, 320, 224, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  check(r.w == 1280 && r.h == 896 && r.y == 32, "original aspect keeps square pixels");
  r = sv_layout_dest(0, 100, 256, 192, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  check(r.w == 0 && r.h == 0, "an empty window gives an empty rectangle");
  r = sv_layout_dest(100, 100, 0, 0, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  check(r.w == 0, "an empty picture gives an empty rectangle");
}

// SEG-118 (ADR 0262): the 16:9 extension geometry, with exact numbers rather than "non-zero".
void extension_layout_tests() {
  struct Side { int w, h; SvAspect a; int side; } sides[] = {
    {320, 224, SV_ASPECT_ORIGINAL, 39},   // H40: 398 wide = 16:9 of 224 lines
    {256, 224, SV_ASPECT_ORIGINAL, 71},   // H32 needs more than H40: never a constant extra
    {320, 224, SV_ASPECT_4_3, 53},        // 4:3 pixel shape: 426 wide
    {256, 224, SV_ASPECT_4_3, 42},
    {256, 192, SV_ASPECT_ORIGINAL, 42},   // Master System height
    {256, 240, SV_ASPECT_ORIGINAL, 85},
    {320, 448, SV_ASPECT_ORIGINAL, 238},  // interlace mode 2 (square pixels, as the existing policy shows it)
    {256, 448, SV_ASPECT_ORIGINAL, 256},  // clamped at SV_EXT_MAX_SIDE
    {1000, 100, SV_ASPECT_ORIGINAL, 0},   // already wider than 16:9: nothing to add
    {0, 224, SV_ASPECT_ORIGINAL, 0}, {320, 0, SV_ASPECT_ORIGINAL, 0}, {-1, 224, SV_ASPECT_ORIGINAL, 0}, {100000, 224, SV_ASPECT_ORIGINAL, 0},
  };
  for (const Side &c : sides)
    check(sv_layout_extension_side(c.w, c.h, c.a) == c.side, "extension side for " + std::to_string(c.w) + "x" + std::to_string(c.h) + " aspect " + std::to_string((int)c.a));
  check(sv_layout_extension_side(320, 224, SV_ASPECT_ORIGINAL) != sv_layout_extension_side(256, 224, SV_ASPECT_ORIGINAL), "H32 and H40 get different extensions");
  {
    int w = -1, h = -1;
    sv_layout_extension_max(320, 448, &w, &h);
    check(w == 320 + 2 * SV_EXT_MAX_SIDE && h == 448, "maximum allocation");
    sv_layout_extension_max(0, 448, &w, &h);
    check(w == 0 && h == 0, "invalid maximum");
    // every geometry the producers use fits the maximum
    for (int hh = 1; hh <= 448; hh += 7)
      for (int ww : {160, 256, 320})
        check(ww + 2 * sv_layout_extension_side(ww, hh, SV_ASPECT_ORIGINAL) <= 320 + 2 * SV_EXT_MAX_SIDE, "bounded");
  }
  SvExtendedLayout l;
  struct Place { int ww, wh, w, h, side; SvAspect a; SvScaleMode sc; SvRect surface, native; } places[] = {
    // H40 on a 1080p display: fit and integer
    {1920, 1080, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_FIT, {0, 0, 1919, 1080}, {188, 0, 1543, 1080}},
    {1920, 1080, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, {164, 92, 1592, 896}, {320, 92, 1280, 896}},
    {1280, 720, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, {43, 24, 1194, 672}, {160, 24, 960, 672}},
    // a 4:3 window letterboxes the 16:9 surface; a 960x672 default window too
    {1024, 768, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_FIT, {0, 96, 1024, 576}, {100, 96, 823, 576}},
    {960, 672, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, {82, 112, 796, 448}, {160, 112, 640, 448}},
    // below 1x integer falls back to fit
    {398, 224, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, {0, 0, 398, 224}, {39, 0, 320, 224}},
    {199, 112, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, {0, 0, 199, 112}, {19, 0, 160, 112}},
    // 4:3 content is never stretched to 16:9: the native rectangle is exactly 4:3
    {1920, 1080, 320, 224, 53, SV_ASPECT_4_3, SV_SCALE_FIT, {1, 0, 1917, 1080}, {239, 0, 1440, 1080}},
    // tiny window
    {1, 1, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_FIT, {0, 0, 1, 1}, {0, 0, 1, 1}},
    // zero extension degenerates to the ordinary layout
    {960, 720, 320, 224, 0, SV_ASPECT_4_3, SV_SCALE_FIT, {0, 0, 960, 720}, {0, 0, 960, 720}},
  };
  for (const Place &c : places) {
    const int rc = sv_layout_extended_dest(c.ww, c.wh, c.w, c.h, c.side, c.a, c.sc, &l);
    const std::string name = "placement " + std::to_string(c.ww) + "x" + std::to_string(c.wh) + " side " + std::to_string(c.side);
    check(rc == 0, name + " ok");
    check(l.surface.x == c.surface.x && l.surface.y == c.surface.y && l.surface.w == c.surface.w && l.surface.h == c.surface.h, name + " surface");
    check(l.native.x == c.native.x && l.native.y == c.native.y && l.native.w == c.native.w && l.native.h == c.native.h, name + " native");
    // the native picture is centred on the surface (to within one display pixel) and never larger than it
    check(std::abs((l.native.x * 2 + l.native.w) - (l.surface.x * 2 + l.surface.w)) <= 1, name + " centred");
    check(l.native.y == l.surface.y && l.native.h == l.surface.h && l.native.w <= l.surface.w, name + " vertical");
    if (c.sc == SV_SCALE_INTEGER && l.surface.h >= c.h) check(l.native.w % c.w == 0 && l.native.h % c.h == 0, name + " integer scale keeps whole multiples");
  }
  // the native picture keeps the scale of the extension-less layout whenever the extension does not force a smaller one
  l = SvExtendedLayout();
  sv_layout_extended_dest(1920, 1080, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_FIT, &l);
  check(l.native.h == sv_layout_dest(1920, 1080, 320, 224, SV_ASPECT_ORIGINAL, SV_SCALE_FIT).h, "the native picture keeps its height on a 16:9 display");
  // invalid input -> empty rectangles, no overflow
  struct Bad { int ww, wh, w, h, side; } bad[] = {{0, 100, 320, 224, 39}, {100, 0, 320, 224, 39}, {100, 100, 0, 224, 39}, {100, 100, 320, 0, 39}, {100, 100, 320, 224, -1},
                                                  {100, 100, 320, 224, SV_EXT_MAX_SIDE + 1}, {2147483647, 2147483647, 320, 224, 39}, {100, 100, 2147483647, 224, 39}};
  for (const Bad &b : bad) {
    l.surface = SvRect{7, 7, 7, 7};
    l.native = SvRect{7, 7, 7, 7};
    check(sv_layout_extended_dest(b.ww, b.wh, b.w, b.h, b.side, SV_ASPECT_ORIGINAL, SV_SCALE_FIT, &l) == -1 && l.surface.w == 0 && l.native.w == 0, "invalid placement rejected");
  }
  check(sv_layout_extended_dest(100, 100, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_FIT, nullptr) == -1, "null output rejected");
  // integer scale exactly 1x for every window size sweep stays inside the window
  for (int ww = 1; ww < 2200; ww += 97)
    for (int wh = 1; wh < 1300; wh += 89)
      for (int sc = 0; sc < 2; ++sc) {
        sv_layout_extended_dest(ww, wh, 320, 224, 39, SV_ASPECT_ORIGINAL, sc ? SV_SCALE_INTEGER : SV_SCALE_FIT, &l);
        check(l.surface.x >= 0 && l.surface.y >= 0 && l.surface.x + l.surface.w <= ww && l.surface.y + l.surface.h <= wh && l.native.x >= l.surface.x &&
              l.native.x + l.native.w <= l.surface.x + l.surface.w, "inside the window");
      }
}

void extension_surface_check_tests() {
  static uint8_t px[(320 + 2 * 39) * 3 * 224];
  SvExtendedSurface e;
  std::memset(&e, 0, sizeof e);
  e.rgb24 = px; e.size = sizeof px; e.width = 398; e.height = 224; e.pitch = 398 * 3; e.side = 39; e.sequence = 9;
  e.mode = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; e.status = SV_WS_AVAILABLE;
  check(sv_extended_surface_check(&e, 320, 224, 9, 320) == SV_EXT_OK, "a consistent surface is accepted");
  e.status = SV_WS_PARTIAL;
  check(sv_extended_surface_check(&e, 320, 224, 9, 320) == SV_EXT_OK, "a partial surface is accepted");
  e.status = SV_WS_UNAVAILABLE;
  check(sv_extended_surface_check(&e, 320, 224, 9, 320) == SV_EXT_STATUS, "unavailable is refused");
  e.status = SV_WS_AVAILABLE;
  check(sv_extended_surface_check(nullptr, 320, 224, 9, 320) == SV_EXT_NO_SURFACE, "null surface");
  { SvExtendedSurface f = e; f.rgb24 = nullptr; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_NO_SURFACE, "null buffer"); }
  { SvExtendedSurface f = e; f.mode = SV_WIDESCREEN_ORIGINAL; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_MODE, "produced as Original"); }
  { SvExtendedSurface f = e; f.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_OK, "SEG-121: a surface produced as Level 3 is accepted"); }
  { SvExtendedSurface f = e; f.mode = static_cast<SvWidescreenMode>(SV_WIDESCREEN_COUNT); check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_MODE, "SEG-121: an unknown produced mode is refused"); }
  check(sv_extended_surface_check(&e, 320, 224, 8, 320) == SV_EXT_SEQUENCE, "stale sequence");
  check(sv_extended_surface_check(&e, 320, 224, 10, 320) == SV_EXT_SEQUENCE, "future sequence");
  { SvExtendedSurface f = e; f.width = 397; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_GEOMETRY, "width not symmetric"); }
  { SvExtendedSurface f = e; f.height = 223; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_GEOMETRY, "height mismatch"); }
  { SvExtendedSurface f = e; f.side = 0; f.width = 320; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_GEOMETRY, "no extension"); }
  { SvExtendedSurface f = e; f.side = SV_EXT_MAX_SIDE + 1; f.width = 320 + 2 * f.side; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_GEOMETRY, "side beyond bound"); }
  check(sv_extended_surface_check(&e, 321, 224, 9, 320) == SV_EXT_GEOMETRY, "native wider than the host accepts");
  check(sv_extended_surface_check(&e, 0, 224, 9, 320) == SV_EXT_GEOMETRY, "empty native");
  { SvExtendedSurface f = e; f.pitch = 398 * 3 - 1; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_PITCH, "insufficient pitch"); }
  { SvExtendedSurface f = e; f.pitch = -5; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_PITCH, "negative pitch"); }
  { SvExtendedSurface f = e; f.size = sizeof px - 1; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_SIZE, "one byte short"); }
  { SvExtendedSurface f = e; f.pitch = 2000000000; check(sv_extended_surface_check(&f, 320, 224, 9, 320) == SV_EXT_SIZE, "huge pitch cannot overflow into acceptance"); }
}

void crt_tests() {
  SvVideoSettings v;
  sv_video_defaults(&v);
  check(sv_crt_is_identity(&v), "Pixel Perfect is the identity (no overlay, warp or brightness change)");
  SvVideoSettings presets[4];
  for (int i = 0; i < 4; ++i) { sv_video_defaults(&presets[i]); sv_apply_crt_preset(&presets[i], static_cast<SvCrtPreset>(i)); }
  check(sv_crt_is_identity(&presets[0]) && !sv_crt_is_identity(&presets[1]) && !sv_crt_is_identity(&presets[2]) && !sv_crt_is_identity(&presets[3]), "only Pixel Perfect is the identity");
  // four distinct, increasingly strong presets: no label is backed by the same effect
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      check(presets[i].scanlines != presets[j].scanlines || presets[i].mask != presets[j].mask || presets[i].curvature != presets[j].curvature || presets[i].bloom != presets[j].bloom ||
                presets[i].brightness != presets[j].brightness,
            "presets " + std::to_string(i) + " and " + std::to_string(j) + " differ");
  check(presets[1].mask == 0 && presets[1].curvature == 0 && presets[2].mask > 0 && presets[2].curvature > 0 && presets[3].scanlines > presets[2].scanlines, "Soft is scanlines+glow only; Classic adds mask and curve; Arcade is strongest");
  // scanline profile: periodic, dark between lines, absent at identity strength, absent when unresolvable
  const float period = 3.0f;
  unsigned char dark = 0, bright = 255;
  for (int row = 0; row < 3; ++row) { const unsigned char a = sv_crt_scanline_alpha(row, period, 80); if (a > dark) dark = a; if (a < bright) bright = a; }
  check(dark > 100 && bright < 40, "scanlines alternate bright centres and dark gaps");
  check(sv_crt_scanline_alpha(7, period, 80) == sv_crt_scanline_alpha(7 + 3, period, 80), "scanline profile is periodic in the source line");
  check(sv_crt_scanline_alpha(4, period, 0) == 0 && sv_crt_scanline_alpha(4, 1.2f, 80) == 0, "no scanlines at strength 0 or below 1.5 rows per line");
  // mask
  check(sv_crt_mask_value(0, 0, 60) == 255 && sv_crt_mask_value(0, 1, 60) < 255 && sv_crt_mask_value(1, 1, 60) == 255 && sv_crt_mask_value(5, 0, 0) == 255, "aperture mask stripes");
  // warp
  float x, y, vig;
  sv_crt_warp(0.3f, -0.4f, 0, &x, &y, &vig);
  check(std::fabs(x - 0.3f) < 1e-6f && std::fabs(y + 0.4f) < 1e-6f && vig > 0.999f, "zero curvature is the identity");
  sv_crt_warp(1.0f, 1.0f, 100, &x, &y, &vig);
  check(std::fabs(x) < 0.9f && std::fabs(y) < 0.9f && vig < 1.0f, "warped corners are pulled inward and fall off");
  sv_crt_warp(1.0f, 0.0f, 100, &x, &y, &vig);
  check(std::fabs(x - 1.0f) < 1e-5f, "the middle of an edge stays on the rectangle (barrel, not pincushion)");
  sv_crt_warp(0.0f, 0.0f, 100, &x, &y, &vig);
  check(std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f, "the centre does not move");
  float mx, my;
  sv_crt_warp(0.5f, 0.0f, 100, &mx, &my, &vig);
  check(mx != 0.5f, "mid-edge points are displaced");
}

SvMenu make_menu() {
  SvMenu m;
  sv_menu_reset(&m);
  SvPage *p = sv_menu_push(&m, 1, "TEST");
  sv_page_add(p, SV_ITEM_LABEL, 1, "info");                                  // 0 label (never focusable)
  sv_page_add(p, SV_ITEM_BUTTON, 2, "button");                               // 1
  SvItem *t = sv_page_add(p, SV_ITEM_TOGGLE, 3, "toggle"); t->value = 0;     // 2
  SvItem *c = sv_page_add(p, SV_ITEM_CHOICE, 4, "choice"); c->min = 0; c->max = 2; c->value = 0;  // 3
  SvItem *s = sv_page_add(p, SV_ITEM_SLIDER, 5, "slider"); s->min = 0; s->max = 10; s->step = 5; s->value = 5;  // 4
  SvItem *d = sv_page_add(p, SV_ITEM_BUTTON, 6, "disabled"); sv_item_disable(d, "because");  // 5
  sv_page_focus_first(p);
  return m;
}

void menu_tests() {
  SvMenu m = make_menu();
  SvPage *p = sv_menu_top(&m);
  check(p->focus == 1, "focus starts on the first enabled, non-label item");
  check(sv_menu_navigate(&m, SV_NAV_DOWN).kind == SV_EVENT_NONE && p->focus == 2, "down moves focus");
  SvMenuEvent e = sv_menu_navigate(&m, SV_NAV_ACCEPT);
  check(e.kind == SV_EVENT_CHANGED && e.item_id == 3 && e.value == 1 && p->items[2].value == 1, "accept flips a toggle");
  e = sv_menu_navigate(&m, SV_NAV_LEFT);
  check(e.kind == SV_EVENT_CHANGED && e.value == 0, "left/right flip a toggle too");
  sv_menu_navigate(&m, SV_NAV_DOWN);
  e = sv_menu_navigate(&m, SV_NAV_RIGHT);
  check(e.kind == SV_EVENT_CHANGED && e.item_id == 4 && e.value == 1, "choice steps right");
  e = sv_menu_navigate(&m, SV_NAV_LEFT); e = sv_menu_navigate(&m, SV_NAV_LEFT);
  check(e.value == 2, "choice wraps");
  sv_menu_navigate(&m, SV_NAV_DOWN);
  e = sv_menu_navigate(&m, SV_NAV_RIGHT);
  check(e.kind == SV_EVENT_CHANGED && e.item_id == 5 && e.value == 10, "slider steps and reports the new value");
  e = sv_menu_navigate(&m, SV_NAV_RIGHT);
  check(e.kind == SV_EVENT_NONE && p->items[4].value == 10, "slider clamps at its maximum without an event");
  sv_menu_navigate(&m, SV_NAV_DOWN);  // wraps over the disabled item to the button
  check(p->focus == 1, "focus wraps and skips disabled items and labels");
  e = sv_menu_navigate(&m, SV_NAV_ACCEPT);
  check(e.kind == SV_EVENT_ACTIVATE && e.item_id == 2, "accept on a button activates");
  check(sv_menu_navigate(&m, SV_NAV_BACK).kind == SV_EVENT_BACK, "back is reported to the host");
  check(sv_menu_pick(&m, 5, 1).kind == SV_EVENT_NONE && p->focus == 1, "a disabled item cannot be picked by the mouse");
  check(sv_menu_pick(&m, 2, 1).kind == SV_EVENT_CHANGED && p->focus == 2, "picking activates the item under the pointer");
  // stack
  check(sv_menu_push(&m, 2, "SUB") != nullptr && m.depth == 2, "push");
  sv_menu_pop(&m);
  check(m.depth == 1, "pop");
  // scrolling keeps the focus visible
  SvMenu big; sv_menu_reset(&big); SvPage *bp = sv_menu_push(&big, 1, "BIG");
  for (int i = 0; i < 30; ++i) sv_page_add(bp, SV_ITEM_BUTTON, 100 + i, "row");
  sv_page_focus_first(bp);
  for (int i = 0; i < 20; ++i) sv_menu_navigate(&big, SV_NAV_DOWN);
  sv_menu_ensure_visible(bp, 8);
  check(bp->focus == 20 && bp->scroll <= 20 && bp->scroll + 8 > 20, "the focused row stays inside the visible window");
  // focus restore across a rebuild
  sv_page_restore_focus(bp, 105);
  check(bp->focus == 5, "focus is restored by item id after a rebuild");
  // a full page cannot overflow
  SvMenu full; sv_menu_reset(&full); SvPage *fp = sv_menu_push(&full, 1, "FULL");
  for (int i = 0; i < SV_MENU_MAX_ITEMS; ++i) check(sv_page_add(fp, SV_ITEM_BUTTON, i, "x") != nullptr, "capacity");
  check(sv_page_add(fp, SV_ITEM_BUTTON, 99, "x") == nullptr, "an overfull page refuses more items");
  // empty menu
  SvMenu closed; sv_menu_reset(&closed);
  check(sv_menu_navigate(&closed, SV_NAV_ACCEPT).kind == SV_EVENT_NONE, "a closed menu ignores input");
}


void font_tests() {
  // metrics come from the generated atlas: proportional heading face, 7-pixel body face, '?' for anything outside printable ASCII
  check(sv_font_glyph(SV_FONT_BODY, 'A')->advance > 0 && sv_font_glyph(SV_FONT_BODY, 0xE9)->advance == sv_font_glyph(SV_FONT_BODY, '?')->advance, "non-ASCII bytes draw as '?'");
  check(sv_font_glyph(SV_FONT_HEAD, 'i')->advance < sv_font_glyph(SV_FONT_HEAD, 'W')->advance, "the heading face is proportional (i is narrower than W)");
  check(sv_font_text_width(SV_FONT_BODY, "") == 0 && sv_font_text_width(SV_FONT_BODY, nullptr) == 0, "empty text has no width");
  check(sv_font_text_width(SV_FONT_BODY, "Resume Game") > sv_font_text_width(SV_FONT_BODY, "Resume"), "width grows with the text");
  check(sv_font_line_height(SV_FONT_BODY) >= 13, "the body line height is the readable 13 px class, not the 8 px debug cell");
  for (int ch = SV_FONT_FIRST; ch <= SV_FONT_LAST; ++ch)
    for (int face = 0; face < SV_FONT_COUNT; ++face) {
      const SvGlyph *g = sv_font_glyph(static_cast<SvFontId>(face), static_cast<unsigned char>(ch));
      check(g->x >= 0 && g->y >= 0 && g->x + g->w <= sv_font_atlas_w && g->y + g->h <= sv_font_atlas_h && g->advance > 0, "every glyph lies inside the atlas and advances");
    }
  // ellipsis never exceeds the budget, at any scale
  for (int scale = 1; scale <= 4; ++scale)
    for (int budget = 12; budget < 300; budget += 17) {
      char out[128];
      const int w = sv_font_ellipsize(SV_FONT_BODY, "A very long game title that cannot possibly fit", scale, budget, out, sizeof out);
      check(w <= budget || w <= sv_font_glyph(SV_FONT_BODY, '.')->advance * 3 * scale, "an ellipsized string fits its budget");
      check(sv_font_text_width(SV_FONT_BODY, out) * scale == w, "the reported width is the real width");
    }
  {
    char out[64];
    sv_font_ellipsize(SV_FONT_BODY, "Short", 1, 400, out, sizeof out);
    check(std::string(out) == "Short", "text that fits is untouched");
  }
  // wrapping: every line within the width (a single over-long word is hard-cut), words are not split, truncation is reported
  const char *text = "Esc or a gamepad's Back/Select (Nintendo -) opens this menu. Start is a game button, never the menu.";
  for (int width = 90; width <= 420; width += 33) {
    int start[8], len[8], truncated = 0;
    const int n = sv_font_wrap(SV_FONT_BODY, text, 1, width, 8, start, len, &truncated);
    check(n >= 1 && n <= 8, "wrap yields lines");
    for (int i = 0; i < n; ++i) {
      check(sv_font_text_width_n(SV_FONT_BODY, text + start[i], len[i]) <= width, "a wrapped line fits the width");
      if (i + 1 < n && text[start[i] + len[i]] != ' ' && text[start[i] + len[i] - 1] != ' ') check(false, "a break falls on a space");
    }
  }
  {
    int start[2], len[2], truncated = 0;
    const int n = sv_font_wrap(SV_FONT_BODY, text, 1, 120, 2, start, len, &truncated);
    check(n == 2 && truncated == 1, "a text that needs more lines than allowed reports truncation");
  }
}

void input_state_tests() {
  SvPadFacts f{};
  check(sv_pad_classify(&f) == SV_PAD_NO_JOYSTICK, "nothing enumerated: A");
  f.enumerated = 1;
  check(sv_pad_classify(&f) == SV_PAD_NOT_A_GAMEPAD, "a joystick without a mapping: B");
  f.is_gamepad = 1; f.open_attempted = 1; f.open_ok = 0;
  check(sv_pad_classify(&f) == SV_PAD_OPEN_FAILED, "a recognised pad that cannot be opened: C");
  f.open_ok = 1;
  check(sv_pad_classify(&f) == SV_PAD_NO_EVENTS, "opened but silent is not working: D");
  f.events = 3;
  check(sv_pad_classify(&f) == SV_PAD_LIVE, "events arrived: working");
  f.bindings_checked = 1; f.bindings_missing = 1;
  check(sv_pad_classify(&f) == SV_PAD_BINDINGS, "live input with a missing binding: E");
  f.bindings_missing = 0; f.bindings_conflicts = 1;
  check(sv_pad_classify(&f) == SV_PAD_BINDINGS, "live input with a conflicting binding: E");
  f.bindings_conflicts = 0; f.nav_checked = 1; f.nav_ok = 0;
  check(sv_pad_classify(&f) == SV_PAD_MENU_NAV, "SDL input the menu navigation never saw: F");
  f.nav_ok = 1;
  check(sv_pad_classify(&f) == SV_PAD_LIVE, "navigation reacted: working");
  check(std::string(sv_pad_state_code(SV_PAD_NOT_A_GAMEPAD)) == "B" && std::string(sv_pad_state_code(SV_PAD_LIVE)) == "OK", "state codes");
  for (int i = 0; i < SV_PAD_STATE_COUNT; ++i) check(std::string(sv_pad_state_advice(static_cast<SvPadState>(i))).size() > 20 && std::string(sv_pad_state_advice(static_cast<SvPadState>(i))).size() < 290, "every state has bounded advice text");
  // Switch 2: the first broken link of the native chain (pure; the SDL facts are gathered in sv_input_diag.c)
  {
    SvSwitch2Facts t{};
    char text[1536];
    check(sv_switch2_assess(&t) == SV_S2_NOT_SEEN && sv_switch2_message(&t, nullptr, text, sizeof text) == 0 && text[0] == '\0', "no Switch 2 seen: nothing to say");
    t.seen = 1; t.state = SV_PAD_NOT_A_GAMEPAD;
    check(sv_switch2_assess(&t) == SV_S2_DRIVER_ABSENT, "a raw Switch 2 on an SDL without the driver: driver absent");
    t.driver_declared = 1;
    check(sv_switch2_assess(&t) == SV_S2_LIBUSB_MISSING, "driver compiled, libusb runtime missing");
    t.libusb_loadable = 1;
    check(sv_switch2_assess(&t) == SV_S2_USB_NOT_CLAIMED, "driver and libusb present, still not a gamepad: USB interface not claimed");
    sv_switch2_message(&t, nullptr, text, sizeof text);
    check(std::string(text).find("Native USB support is available in this SDL build.") != std::string::npos && std::string(text).find("libusb runtime: available.") != std::string::npos &&
              std::string(text).find("WinUSB") != std::string::npos && std::string(text).find("Connection:") == std::string::npos,
          "the USB message names the available driver and libusb, the possible driver binding, and invents no connection type");
    std::snprintf(t.setup_trace, sizeof t.setup_trace, "begin iface=0 | stage=no_driver (switch2_driver_enabled=1)");
    check(sv_switch2_assess(&t) == SV_S2_SETUP_FAILED, "SDL's own trace of a missing driver selects the setup verdict");
    std::snprintf(t.setup_trace, sizeof t.setup_trace, "begin iface=0 | stage=open_path FAILED error=\"libusb_claim_interface(0) failed: libusb error -3\" | x: -12 | stage=init_device FAILED error=\"Couldn't claim interface 1: -12\"");
    sv_switch2_message(&t, nullptr, text, sizeof text);
    check(std::string(text).find("libusb error -3 (ACCESS)") != std::string::npos && std::string(text).find("interface 1: -12 (NOT_SUPPORTED)") != std::string::npos &&
              std::string(text).find("begin iface=0\nstage=open_path FAILED") != std::string::npos && std::string(text).find("x: -12 (NOT_SUPPORTED)") != std::string::npos,
          "the setup trace is shown one step per line with libusb error names");
    std::snprintf(t.setup_trace, sizeof t.setup_trace, "begin iface=0 class=3 | stage=open_path FAILED error=\"libusb_open failed for interface 0: libusb error -3\"");
    sv_switch2_message(&t, nullptr, text, sizeof text);
    check(std::string(text).find("(ACCESS)") != std::string::npos && std::string(text).find("Access denied while libusb opened the device") != std::string::npos && std::string(text).find("Steam") != std::string::npos,
          "the measured Windows failure (libusb_open -> ACCESS) is named with the possible cause, not asserted as the cause");
    {
      std::string whole(text);  // each line still fits one viewer note
      for (std::size_t at = 0; at < whole.size();) {
        const std::size_t nl = whole.find('\n', at);
        check((nl == std::string::npos ? whole.size() : nl) - at < 290, "ACCESS message lines fit one viewer note");
        if (nl == std::string::npos) break;
        at = nl + 1;
      }
    }
    std::snprintf(t.setup_trace, sizeof t.setup_trace, "begin iface=0 | stage=init_device ok driver=Switch2");
    check(sv_switch2_assess(&t) == SV_S2_USB_NOT_CLAIMED, "a trace without a failure keeps the generic verdict");
    t.setup_trace[0] = '\0';
    check(std::string(sv_libusb_error_name(-3)) == "ACCESS" && std::string(sv_libusb_error_name(-12)) == "NOT_SUPPORTED" && std::string(sv_libusb_error_name(7)).empty(), "libusb error names");
    t.transport = 2;
    check(sv_switch2_assess(&t) == SV_S2_BLUETOOTH, "Bluetooth: unsupported by the pinned driver, whatever else is present");
    sv_switch2_message(&t, nullptr, text, sizeof text);
    check(std::string(text).find("Connection: Bluetooth.") != std::string::npos && std::string(text).find("Steam Input") != std::string::npos, "the Bluetooth message states the connection and the alternatives");
    t.transport = 1; t.state = SV_PAD_OPEN_FAILED;
    check(sv_switch2_assess(&t) == SV_S2_OPEN_FAILED, "open failed");
    sv_switch2_message(&t, "libusb error -3", text, sizeof text);
    check(std::string(text).find("Gamepad initialization: failed.") != std::string::npos && std::string(text).find("Details: libusb error -3") != std::string::npos &&
              std::string(text).find("Connection: USB.") != std::string::npos,
          "the open-failure message carries SDL's own text");
    t.state = SV_PAD_NO_EVENTS;
    check(sv_switch2_assess(&t) == SV_S2_NO_EVENTS, "opened but silent");
    t.state = SV_PAD_BINDINGS;
    check(sv_switch2_assess(&t) == SV_S2_BINDINGS, "events with broken bindings");
    t.state = SV_PAD_MENU_NAV;
    check(sv_switch2_assess(&t) == SV_S2_NAV_PENDING, "events, launcher navigation not yet confirmed");
    t.state = SV_PAD_LIVE;
    check(sv_switch2_assess(&t) == SV_S2_WORKING, "events arriving: working");
    for (int v = SV_S2_BLUETOOTH; v < SV_S2_VERDICT_COUNT; ++v) {  // every line fits one viewer note (299 characters), for every verdict
      SvSwitch2Facts u{};
      u.seen = 1; u.driver_declared = 1; u.libusb_loadable = 1;
      u.state = v == SV_S2_OPEN_FAILED ? SV_PAD_OPEN_FAILED : v == SV_S2_NO_EVENTS ? SV_PAD_NO_EVENTS : v == SV_S2_BINDINGS ? SV_PAD_BINDINGS : v == SV_S2_NAV_PENDING ? SV_PAD_MENU_NAV : v == SV_S2_WORKING ? SV_PAD_LIVE : SV_PAD_NOT_A_GAMEPAD;
      u.transport = v == SV_S2_BLUETOOTH ? 2 : 0;
      if (v == SV_S2_SETUP_FAILED) std::snprintf(u.setup_trace, sizeof u.setup_trace, "begin iface=0 | stage=open_path ok backed_by_libusb=1 | usb: libusb loaded | stage=init_device FAILED driver=Switch2 error=\"Couldn't claim interface 1: -12\"");
      u.driver_declared = v != SV_S2_DRIVER_ABSENT; u.libusb_loadable = v != SV_S2_LIBUSB_MISSING;
      char big[1536];
      check(sv_switch2_assess(&u) == v, "the synthetic facts select the intended verdict");
      sv_switch2_message(&u, "details", big, sizeof big);
      std::string whole(big);
      check(!whole.empty() && whole.size() < 1535, "every verdict has bounded text");
      for (std::size_t at = 0; at < whole.size();) {
        const std::size_t nl = whole.find('\n', at);
        const std::size_t len = (nl == std::string::npos ? whole.size() : nl) - at;
        check(len < 280, "each message line fits one viewer note");
        if (nl == std::string::npos) break;
        at = nl + 1;
      }
    }
    char small[8];
    check(sv_switch2_message(&t, nullptr, small, sizeof small) < sizeof small, "the message respects a small buffer");
    check(sv_switch2_is_product(0x057e, 0x2069) && !sv_switch2_is_product(0x057e, 0x2009) && !sv_switch2_is_product(0x045e, 0x2069), "only Switch 2 products match");
  }
  // the binding audit follows the console's real actions
  SvSettings s = defaults();
  int missing = -1, conflicts = -1;
  sv_pad_binding_audit(&s, SV_SYSTEM_GENESIS, 0, &missing, &conflicts);
  check(missing == 0 && conflicts == 0, "default Genesis bindings are clean");
  sv_pad_binding_audit(&s, SV_SYSTEM_SMS, 0, &missing, &conflicts);
  check(missing == 0 && conflicts == 0, "default Master System bindings are clean (the Pause button counts)");
  s.bind[SV_SYSTEM_SMS].pad[0][SV_ACT_PAUSE] = -1;
  sv_pad_binding_audit(&s, SV_SYSTEM_SMS, 0, &missing, &conflicts);
  check(missing == 1, "an unbound Master System Pause button is reported");
  sv_pad_binding_audit(&s, SV_SYSTEM_SMS, 1, &missing, &conflicts);
  check(missing == 0, "Player 2 has no Pause button to be missing");
  s = defaults();
  s.bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON2] = s.bind[SV_SYSTEM_GENESIS].pad[0][SV_ACT_BUTTON1];
  sv_pad_binding_audit(&s, SV_SYSTEM_GENESIS, 0, &missing, &conflicts);
  check(conflicts == 1, "one button driving two actions is a conflict");
  // positional names and the printed Nintendo labels
  check(std::string(sv_pad_position_name(SV_PAD_SOUTH)) == "South" && std::string(sv_pad_nintendo_label(SV_PAD_SOUTH)) == "B" && std::string(sv_pad_nintendo_label(SV_PAD_EAST)) == "A" &&
            std::string(sv_pad_nintendo_label(SV_PAD_START)) == "+" && std::string(sv_pad_nintendo_label(SV_PAD_BACK)) == "-" && sv_pad_nintendo_label(SV_PAD_DPAD_UP) == nullptr,
        "SDL positions versus printed Nintendo labels");
}

} // namespace

// An out-of-range enumerator (what a corrupted in-memory value would look like) without an unspecified conversion.
SvWidescreenMode raw_mode(int v) { SvWidescreenMode m; static_assert(sizeof(m) == sizeof(int), "enum is int-sized"); std::memcpy(&m, &v, sizeof m); return m; }

void widescreen_tests() {
  // default and per-system behaviour
  SvSettings s = defaults();
  check(s.video.widescreen == SV_WIDESCREEN_ORIGINAL, "Widescreen Mode defaults to Original");
  // every mode round-trips through the text form with a stable name (not the enum number)
  for (int m = 0; m < SV_WIDESCREEN_COUNT; ++m) {
    SvSettings a = defaults();
    a.video.widescreen = static_cast<SvWidescreenMode>(m);
    char text[8192];
    const size_t n = sv_settings_format(&a, text, sizeof text);
    check(n > 0 && std::strstr(text, "video.widescreen=") != nullptr && std::strstr(text, "video.widescreen=0") == nullptr && std::strstr(text, "video.widescreen=1") == nullptr, "widescreen is persisted textually, not as a number");
    SvSettings b = defaults();
    b.video.widescreen = m == 0 ? SV_WIDESCREEN_ENHANCED_GAMEPLAY : SV_WIDESCREEN_ORIGINAL; // parse must overwrite either way
    sv_settings_parse(&b, text, n);
    check(b.video.widescreen == a.video.widescreen, "each Widescreen Mode round-trips");
  }
  { SvSettings a = defaults(); a.video.widescreen = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; char text[8192]; sv_settings_format(&a, text, sizeof text);
    check(std::strstr(text, "video.widescreen=extended_backgrounds\n") != nullptr, "stable key/value for Level 2");
    a.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY; sv_settings_format(&a, text, sizeof text);
    check(std::strstr(text, "video.widescreen=enhanced_gameplay\n") != nullptr, "stable key/value for Level 3"); }
  // an older file (written before the key existed) loads unchanged: Original, every other field intact
  { const char *old = "video.fullscreen=1\nvideo.scale=1\nvideo.aspect=1\nvideo.preset=2\nvideo.vsync=1\naudio.volume=35\ninput.deadzone=40\nbind.sms.p1.key.b1=20\n";
    SvSettings fresh = defaults();
    sv_settings_parse(&fresh, old, std::strlen(old));
    check(fresh.video.widescreen == SV_WIDESCREEN_ORIGINAL && fresh.video.fullscreen == 1 && fresh.video.scale == SV_SCALE_INTEGER && fresh.video.aspect == SV_ASPECT_4_3 && fresh.video.preset == SV_CRT_CLASSIC &&
              fresh.video.vsync == 1 && fresh.audio.volume == 35 && fresh.deadzone == 40 && fresh.bind[SV_SYSTEM_SMS].key[0][SV_ACT_BUTTON1] == 20,
          "an older settings file loads as Original and keeps every previous field"); }
  // invalid / hostile values become Original; the line is still recognised, nothing else changes
  { const char *vals[] = {"", "bogus", "2", "1", "EXTENDED_BACKGROUNDS", "enhanced_gameplay\x01", "extended backgrounds", "-1", "99999999999999999999"};
    for (const char *v : vals) {
      SvSettings t = defaults(); t.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY;
      const std::string line = std::string("audio.volume=12\nvideo.widescreen=") + v + "\n";
      sv_settings_parse(&t, line.c_str(), line.size());
      check(t.video.widescreen == SV_WIDESCREEN_ORIGINAL && t.audio.volume == 12, std::string("unknown widescreen value falls back to Original: '") + v + "'");
    } }
  { std::string big = "video.widescreen=" + std::string(200000, 'x') + "\naudio.volume=33\n";
    SvSettings t = defaults(); sv_settings_parse(&t, big.data(), big.size());
    check(t.video.widescreen == SV_WIDESCREEN_ORIGINAL, "an oversized widescreen value is bounded and ignored"); }
  { SvSettings t = defaults(); t.video.widescreen = raw_mode(77); sv_settings_sanitize(&t);
    check(t.video.widescreen == SV_WIDESCREEN_ORIGINAL, "sanitize clamps an out-of-range enum to Original");
    t.video.widescreen = raw_mode(-3); sv_settings_sanitize(&t);
    check(t.video.widescreen == SV_WIDESCREEN_ORIGINAL, "sanitize clamps a negative enum to Original"); }
  // Restore video defaults resets it, and an SvSystem makes no difference to the shared value
  { SvSettings t = defaults(); t.video.widescreen = SV_WIDESCREEN_EXTENDED_BACKGROUNDS; sv_video_defaults(&t.video);
    check(t.video.widescreen == SV_WIDESCREEN_ORIGINAL, "sv_video_defaults restores Original");
    for (int sys = 0; sys < SV_SYSTEM_COUNT; ++sys) { SvSettings u = defaults(); sv_bindings_defaults(&u, static_cast<SvSystem>(sys)); u.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY;
      char text[8192]; sv_settings_format(&u, text, sizeof text); SvSettings w = defaults(); sv_settings_parse(&w, text, std::strlen(text));
      check(w.video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "the value is system independent (one video block for Genesis, SMS and Sega CD)"); } }
  // effective-mode fallback Level 3 -> Level 2 -> Original; absence of a provider means Original
  { SvWidescreenCaps none = {SV_WS_UNAVAILABLE, SV_WS_UNAVAILABLE, nullptr, nullptr}, l2 = {SV_WS_AVAILABLE, SV_WS_UNAVAILABLE, nullptr, nullptr}, partial3 = {SV_WS_AVAILABLE, SV_WS_PARTIAL, nullptr, nullptr}, l3only = {SV_WS_UNAVAILABLE, SV_WS_AVAILABLE, nullptr, nullptr};
    check(sv_widescreen_effective(SV_WIDESCREEN_ENHANCED_GAMEPLAY, nullptr) == SV_WIDESCREEN_ORIGINAL && sv_widescreen_effective(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, nullptr) == SV_WIDESCREEN_ORIGINAL, "no capability provider: Original");
    check(sv_widescreen_effective(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &none) == SV_WIDESCREEN_ORIGINAL && sv_widescreen_effective(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &none) == SV_WIDESCREEN_ORIGINAL, "Unavailable levels stay Original");
    check(sv_widescreen_effective(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &l2) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "Level 3 falls back to Level 2");
    check(sv_widescreen_effective(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &l2) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "Level 2 is honoured when available");
    check(sv_widescreen_effective(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &partial3) == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "Partial counts as supported for the effective mode");
    check(sv_widescreen_effective(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &l3only) == SV_WIDESCREEN_ORIGINAL, "Level 2 never falls UP to Level 3");
    check(sv_widescreen_effective(SV_WIDESCREEN_ORIGINAL, &partial3) == SV_WIDESCREEN_ORIGINAL, "Original stays Original");
    check(sv_widescreen_effective(raw_mode(9), &partial3) == SV_WIDESCREEN_ORIGINAL, "an invalid request is Original");
    check(std::strcmp(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &none), "Not yet available for this game. Showing the original picture.") == 0 &&
              std::strcmp(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, nullptr), "Not yet available for this game. Showing the original picture.") == 0,
          "the unavailable message is the specified text");
    check(sv_widescreen_status(SV_WIDESCREEN_ORIGINAL, &none)[0] == '\0', "Original has no status text");
    check(std::strstr(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &l2), "Extended Backgrounds") != nullptr, "a Level 3 request served by Level 2 says so");
    check(std::strstr(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &partial3), "Partly") != nullptr, "Partial support is reported as partial");
    { // SEG-120: a platform-specific Partial sentence replaces the generic one for Level 2 only when it is the effective, Partial level
      static const char note[] = "Background repeats existing scenery; this does not reveal more of the game world.";
      SvWidescreenCaps wrapped = {SV_WS_PARTIAL, SV_WS_UNAVAILABLE, note, nullptr}, plain = {SV_WS_PARTIAL, SV_WS_UNAVAILABLE, nullptr, nullptr}, avail = {SV_WS_AVAILABLE, SV_WS_UNAVAILABLE, note, nullptr};
      check(std::strcmp(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &wrapped), note) == 0, "Partial with a platform note shows the note");
      check(std::strstr(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &plain), "Partly") != nullptr, "Partial without a note keeps the generic text");
      check(std::strcmp(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &avail), "Available for this game.") == 0, "an Available level never shows the Partial note");
      check(std::strstr(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &wrapped), "Level 3 is not available") != nullptr, "a Level 3 request served by a Partial Level 2 says so");
      // SEG-121: a Level 3 note is shown for an effective Partial Level 3 only; it falls back to the Level 2 note, and never replaces an Available level's text
      static const char sprites[] = "Only sprites crossing the screen edges can extend; this does not expand the game world.";
      SvWidescreenCaps l3 = {SV_WS_PARTIAL, SV_WS_PARTIAL, note, sprites}, l3_plain = {SV_WS_PARTIAL, SV_WS_PARTIAL, note, nullptr}, l3_avail = {SV_WS_PARTIAL, SV_WS_AVAILABLE, note, sprites};
      check(std::strcmp(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &l3), sprites) == 0, "Partial Level 3 shows its own note");
      check(std::strcmp(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &l3), note) == 0, "Level 2 keeps its note when Level 3 has one");
      check(std::strcmp(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &l3_plain), note) == 0, "Level 3 without its own note falls back to the platform note");
      check(std::strcmp(sv_widescreen_status(SV_WIDESCREEN_ENHANCED_GAMEPLAY, &l3_avail), "Available for this game.") == 0, "an Available Level 3 never shows a note");
      SvWidescreenCaps off = {SV_WS_UNAVAILABLE, SV_WS_UNAVAILABLE, note, nullptr};
      check(std::strstr(sv_widescreen_status(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, &off), "original picture") != nullptr, "Unavailable never shows the note");
    }
    for (int m = 0; m < SV_WIDESCREEN_COUNT; ++m) check(std::strlen(sv_widescreen_description(static_cast<SvWidescreenMode>(m))) > 20 && std::strlen(sv_widescreen_name(static_cast<SvWidescreenMode>(m))) < 40, "every mode has a label and description"); }
}

int main() {
  settings_tests();
  widescreen_tests();
  binding_tests();
  layout_tests();
  extension_layout_tests();
  extension_surface_check_tests();
  crt_tests();
  menu_tests();
  font_tests();
  input_state_tests();
  if (failures != 0) return 1;
  std::printf("sv_viewer_core_test: ok\n");
  return 0;
}
