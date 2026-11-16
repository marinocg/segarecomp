// ADR 0312: virtual-sprite priority in the extension columns, end to end: the real extension producer (which records the winner class of every extension pixel) feeding the real compositor through the
// adapter callback, on a hand-built scene. Every expectation comes from a small independent model of the documented VDP composition rules (six layers: sprite-high, plane A-high, plane B-high, sprite-low,
// plane A-low, plane B-low, backdrop; transparent pixels expose what lies below) and of the documented virtual-sprite layer rules (ABOVE_ALL always draws; BETWEEN is hidden only where a high-priority pixel
// won; BEHIND_PLANES draws only on the backdrop; the first command that may draw a pixel claims it, higher layers first). Nothing is taken from the code under test; project-authored data only.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "frame_extension.h"
#include "genesis_vsprite_fixture.hpp"

using namespace vsfx;

namespace {

constexpr int kW = 320, kH = 224, kSide = 64;
constexpr unsigned kPitchPx = kW + 2 * kSide;
constexpr unsigned kTileGreen = 1, kTileChecker = 2, kTileBlue = 3, kTileHwLow = 4, kTileHwHigh = 8, kArtRed = 200, kArtCyan = 216;
// CRAM entries (palette line 0, pattern nibble == entry)
constexpr unsigned kEntGreen = 1, kEntYellow = 2, kEntBackdrop = 3, kEntBlue = 4, kEntMagenta = 6, kEntWhite = 7, kEntRed = 9, kEntCyan = 10;

struct Hw { int x, y; bool prio; };   // all hardware sprites are 2 x 2 cells (16 x 16 px), solid

struct Cmd { int x, y; unsigned layer; unsigned entry; };   // all virtual sprites are 4 x 4 cells (32 x 32 px), solid

void checker_tile(Scene &s, unsigned tile) {
  for (unsigned ty = 0; ty < 8; ++ty)
    for (unsigned tx = 0; tx < 8; tx += 2) {
      const unsigned n0 = ((tx + ty) & 1U) != 0U ? kEntYellow : 0U, n1 = ((tx + 1 + ty) & 1U) != 0U ? kEntYellow : 0U;
      s.vram[tile * GENESIS_VDP_TILE_BYTES + ty * 4 + tx / 2] = static_cast<uint8_t>((n0 << 4) | n1);
    }
}

// The scene: rows 0..7 of Plane A hold a low-priority solid tile, rows 8..15 a HIGH-priority checker tile (every other pixel transparent), Plane B a low-priority solid tile on rows 0..15, nothing below y = 128 (backdrop).
// Every one of the 64 plane columns is filled, so the extension columns (plane x 448..511 and 320..383) carry the same content as the picture.
struct World {
  Scene sc;
  std::vector<Hw> hw;
  explicit World(const std::vector<Hw> &sprites, bool checker_is_high = true) : hw(sprites) {
    sc.set_color(kEntGreen, 0, 6, 0);
    sc.set_color(kEntYellow, 7, 7, 0);
    sc.set_color(kEntBackdrop, 2, 2, 2);
    sc.set_color(kEntBlue, 0, 0, 7);
    sc.set_color(kEntMagenta, 7, 0, 7);
    sc.set_color(kEntWhite, 7, 7, 7);
    sc.set_color(kEntRed, 7, 0, 0);
    sc.set_color(kEntCyan, 0, 7, 7);
    sc.fill_tile(kTileGreen, kEntGreen);
    checker_tile(sc, kTileChecker);
    sc.fill_tile(kTileBlue, kEntBlue);
    for (unsigned t = 0; t < 4; ++t) { sc.fill_tile(kTileHwLow + t, kEntMagenta); sc.fill_tile(kTileHwHigh + t, kEntWhite); }
    for (unsigned t = 0; t < 16; ++t) { sc.fill_tile(kArtRed + t, kEntRed); sc.fill_tile(kArtCyan + t, kEntCyan); }
    for (unsigned cx = 0; cx < kPlaneCells; ++cx) {
      for (unsigned cy = 0; cy < 8; ++cy) sc.plane_cell(kPlaneA, cx, cy, kTileGreen, 0, false);
      for (unsigned cy = 8; cy < 16; ++cy) sc.plane_cell(kPlaneA, cx, cy, kTileChecker, 0, checker_is_high);
      for (unsigned cy = 0; cy < 16; ++cy) sc.plane_cell(kPlaneB, cx, cy, kTileBlue, 0, false);
    }
    for (const Hw &h : hw) sc.hw_sprite(h.x, h.y, 2, 2, h.prio ? kTileHwHigh : kTileHwLow, 0, false, false, h.prio);
    high = checker_is_high;
  }
  bool high = true;

  // ---- the independent model ----
  struct Px { unsigned cls; unsigned entry; };   // cls: 0 backdrop, 1 low, 2 high
  Px native(int x, int y, bool sprites_in_margin) const {
    const bool in_picture = x >= 0 && x < kW;
    bool a_opaque = false, a_prio = false;
    unsigned a_entry = 0;
    if (y < 64) { a_opaque = true; a_entry = kEntGreen; }
    else if (y < 128 && ((x + y) & 1) != 0) { a_opaque = true; a_prio = high; a_entry = kEntYellow; }
    const bool b_opaque = y < 128;
    bool s_opaque = false, s_prio = false;
    unsigned s_entry = 0;
    if (in_picture || sprites_in_margin)
      for (const Hw &h : hw)
        if (x >= h.x && x < h.x + 16 && y >= h.y && y < h.y + 16) { s_opaque = true; s_prio = h.prio; s_entry = h.prio ? kEntWhite : kEntMagenta; break; }
    if (s_opaque && s_prio) return {2, s_entry};
    if (a_opaque && a_prio) return {2, a_entry};
    // Plane B is never high priority in this scene
    if (s_opaque) return {1, s_entry};
    if (a_opaque) return {1, a_entry};
    if (b_opaque) return {1, kEntBlue};
    return {0, kEntBackdrop};
  }
  unsigned expected_entry(int x, int y, const std::vector<Cmd> &set, bool sprites_in_margin, int visible) const {
    const Px n = native(x, y, sprites_in_margin);
    const bool shown = x >= -visible && x < kW + visible;   // only the presented columns are ever drawn
    if (shown)
      for (int layer = 2; layer >= 0; --layer)
        for (const Cmd &c : set) {
          if (static_cast<int>(c.layer) != layer || x < c.x || x >= c.x + 32 || y < c.y || y >= c.y + 32) continue;
          if (layer == 0 && n.cls != 0) continue;
          if (layer == 1 && n.cls == 2) continue;
          return c.entry;
        }
    return n.entry;
  }
};

void rgb_of(const uint8_t *cram, unsigned entry, uint8_t out[3]) {
  const unsigned w = (static_cast<unsigned>(cram[entry * 2]) << 8) | cram[entry * 2 + 1];
  out[0] = static_cast<uint8_t>(((w >> 1) & 7U) * 255U / 7U);
  out[1] = static_cast<uint8_t>(((w >> 5) & 7U) * 255U / 7U);
  out[2] = static_cast<uint8_t>(((w >> 9) & 7U) * 255U / 7U);
}

// ---- the system under test, wired like the viewer: producer -> compat provider (submits the set) -> adapter compositor callback ----
struct Wire {
  GenesisVirtualSpriteSet set;
  unsigned visible = kSide;
  bool called = false, classes_passed = false;
  std::vector<uint8_t> composed;      // the compositor's output surface
  GenesisVirtualSpriteReport rep;
  void (*tamper)(uint8_t *classes) = nullptr;
};
Wire *g_wire = nullptr;

void prep(void *, const GenesisCompatFrame *f, GenesisCompatDecision *d) { d->accepted = f->requested & GENESIS_COMPAT_HUD; }
int hud(void *, GenesisCompatOverlay *o) { if (o->vsprites != nullptr) *o->vsprites = g_wire->set; return 1; }
const GenesisNativeCompat kProvider = {GENESIS_NATIVE_COMPAT_ABI, "priority-test", 1, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SPRITES, prep, nullptr, hud};

int compose_cb(void *, const GenesisVirtualSpriteSet *set, unsigned host_flags, const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster, const GenesisFrameArtifact *native,
               const uint8_t *surface, const uint8_t *classes, unsigned side, GenesisVirtualSpriteOutcome *out) {
  Wire &w = *g_wire;
  w.called = true;
  w.classes_passed = classes != nullptr;
  if (w.tamper != nullptr && classes != nullptr) w.tamper(const_cast<uint8_t *>(classes));
  auto aux = std::make_unique<GenesisVdpFrameAux>();
  auto scratch = std::make_unique<GenesisFrameArtifact>();
  if (genesis_vdp_produce_frame_raster_aux(vram, raster, scratch.get(), aux.get()) != 0) return -1;
  GenesisVirtualSpriteSurface t = {surface, side, w.visible, classes};
  w.composed.assign(static_cast<size_t>(kPitchPx) * kH * 3, 0);
  if (genesis_virtual_sprites_compose_wide(set, host_flags, vram, native, aux.get(), &t, w.composed.data(), w.composed.size(), &w.rep) != 0) return -1;
  out->status = w.rep.status;
  out->reason = w.rep.reason;
  out->index = w.rep.reject_index;
  out->commands = w.rep.commands;
  return 0;
}

int plan(void *, unsigned, unsigned, unsigned *side) { *side = kSide; return 0; }
int no_plan(void *, unsigned, unsigned, unsigned *) { return -1; }

struct Out {
  int rc = -1;
  GenesisExtensionResult res;
  std::vector<uint8_t> surface, classes, composed;
  bool called = false, classes_passed = false;
  GenesisVirtualSpriteReport rep;
};

std::vector<uint8_t> g_ram(GENESIS_COMPAT_RAM_BYTES), g_overlay(GENESIS_COMPAT_OVERLAY_BYTES);

Out run(const World &wd, const std::vector<Cmd> &cmds, unsigned level, bool give_classes = true, size_t classes_capacity = SIZE_MAX, unsigned visible = kSide, void (*tamper)(uint8_t *) = nullptr,
        bool planned = true, std::vector<uint8_t> *classes_inout = nullptr) {
  Wire wire;
  std::memset(&wire.set, 0, sizeof wire.set);
  wire.set.space = static_cast<uint8_t>(GENESIS_VSPRITE_SPACE_PRESENTATION);
  wire.set.budget_mode = static_cast<uint8_t>(GENESIS_VSPRITE_BUDGET_EXTENDED);
  wire.set.count = static_cast<uint32_t>(cmds.size());
  for (size_t i = 0; i < cmds.size(); ++i) wire.set.sprites[i] = vsprite(cmds[i].x, cmds[i].y, 4, 4, cmds[i].entry == kEntRed ? kArtRed : kArtCyan, 0, cmds[i].layer);
  wire.visible = visible;
  wire.tamper = tamper;
  g_wire = &wire;

  auto raster = std::make_unique<GenesisRasterFrameState>();
  std::memset(raster.get(), 0, sizeof *raster);
  for (auto &line : raster->lines) {
    std::memcpy(line.registers, wd.sc.regs, sizeof line.registers);
    std::memcpy(line.cram, wd.sc.cram, sizeof line.cram);
    std::memcpy(line.vsram, wd.sc.vsram, sizeof line.vsram);
  }
  std::memcpy(raster->vram_start, wd.sc.vram, GENESIS_VDP_VRAM_BYTES);
  auto native = std::make_unique<GenesisFrameArtifact>();
  std::memset(native.get(), 0, sizeof *native);
  Out o;
  if (genesis_vdp_produce_frame_raster(wd.sc.vram, raster.get(), native.get()) != 0) { check(false, "the native frame renders"); return o; }

  std::vector<uint8_t> surface(GENESIS_EXT_SURFACE_BYTES, 0xA5);
  std::vector<uint8_t> own_classes(GENESIS_EXT_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT, 0xEE);
  std::vector<uint8_t> &classes = classes_inout != nullptr ? *classes_inout : own_classes;
  GenesisFrameExtension ext{};
  ext.request = static_cast<uint8_t>(level);
  ext.plan = planned ? plan : no_plan;
  ext.pixels = surface.data();
  ext.capacity = surface.size();
  if (give_classes) { ext.classes = classes.data(); ext.classes_capacity = classes_capacity == SIZE_MAX ? classes.size() : classes_capacity; }
  ext.compat = &kProvider;
  ext.compat_components = GENESIS_COMPAT_HUD | GENESIS_COMPAT_SPRITES;
  ext.compat_caps = GENESIS_CAP_FRAME_WORK_RAM | GENESIS_CAP_OVERLAY_RGBA;
  ext.compat_ram = g_ram.data();
  ext.compat_ram_valid = 1;
  ext.overlay_rgba = g_overlay.data();
  auto host_set = std::make_unique<GenesisVirtualSpriteSet>();   // host-owned: the producer zeroes it before asking the provider, which fills it from `wire.set`
  ext.vsprite_set = host_set.get();
  ext.vsprite_compose = compose_cb;
  o.rc = genesis_vdp_produce_extension(wd.sc.vram, raster.get(), native.get(), &ext, &o.res);
  o.surface = surface;
  o.classes = classes;
  o.composed = wire.composed;
  o.called = wire.called;
  o.classes_passed = wire.classes_passed;
  o.rep = wire.rep;
  return o;
}

// Compares every pixel of the composed surface and every recorded extension class with the model. Returns the number of mismatches.
unsigned compare(const World &wd, const std::vector<Cmd> &cmds, const Out &o, bool sprites_in_margin, int visible, const char *what, bool check_classes = true) {
  unsigned bad_px = 0, bad_cls = 0;
  if (o.composed.size() != static_cast<size_t>(kPitchPx) * kH * 3) { check(false, what); return 1; }
  for (int y = 0; y < kH; ++y)
    for (int x = -kSide; x < kW + kSide; ++x) {
      uint8_t want[3];
      rgb_of(wd.sc.cram, wd.expected_entry(x, y, cmds, sprites_in_margin, visible), want);
      const uint8_t *got = &o.composed[(static_cast<size_t>(y) * kPitchPx + static_cast<size_t>(x + kSide)) * 3];
      if (std::memcmp(want, got, 3) != 0) {
        if (bad_px == 0) std::printf("  %s: first pixel mismatch at x=%d y=%d want %u,%u,%u got %u,%u,%u\n", what, x, y, want[0], want[1], want[2], got[0], got[1], got[2]);
        ++bad_px;
      }
      if (check_classes && (x < 0 || x >= kW)) {
        const unsigned want_cls = wd.native(x, y, sprites_in_margin).cls;
        const unsigned got_cls = o.classes[static_cast<size_t>(y) * kPitchPx + static_cast<size_t>(x + kSide)];
        if (want_cls != got_cls) {
          if (bad_cls == 0) std::printf("  %s: first class mismatch at x=%d y=%d want %u got %u\n", what, x, y, want_cls, got_cls);
          ++bad_cls;
        }
      }
    }
  check(bad_px == 0, what);
  check(bad_cls == 0, what);
  return bad_px + bad_cls;
}

bool rgb_eq(const Out &o, const World &wd, int x, int y, unsigned entry) {
  uint8_t want[3];
  rgb_of(wd.sc.cram, entry, want);
  return std::memcmp(want, &o.composed[(static_cast<size_t>(y) * kPitchPx + static_cast<size_t>(x + kSide)) * 3], 3) == 0;
}

// Left and right margin, the picture boundaries, all three layers, mixed in one set.
const std::vector<Cmd> kMixed = {
    {-56, 72, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed},       // left margin over the high-priority checker: red only in the transparent holes
    {-56, 8, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed},        // left margin over low-priority content: solid red
    {330, 72, GENESIS_VSPRITE_LAYER_ABOVE_ALL, kEntRed},     // right margin, above everything: solid red even over the checker
    {330, 112, GENESIS_VSPRITE_LAYER_BEHIND_PLANES, kEntCyan},   // right margin: visible only on the backdrop rows (y >= 128)
    {-16, 72, GENESIS_VSPRITE_LAYER_BETWEEN, kEntCyan},      // straddles the left picture boundary (x = 0)
    {304, 72, GENESIS_VSPRITE_LAYER_BETWEEN, kEntCyan},      // straddles the right picture boundary (x = 320); overlaps the ABOVE_ALL sprite at x 330..335 (which claims first)
    {-56, 72, GENESIS_VSPRITE_LAYER_BETWEEN, kEntCyan},      // a later command on the same pixels as the first: the first claims them
};

void priority_in_extension_columns() {
  const World wd({});
  const Out o = run(wd, kMixed, GENESIS_EXT_MODE_BACKGROUNDS);
  check(o.rc == 0 && o.called && o.classes_passed && o.rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "a mixed BETWEEN / ABOVE_ALL / BEHIND set composes in the extension columns (it was rejected before ADR 0312)");
  compare(wd, kMixed, o, false, kSide, "mixed set against the model");
  // hand-checked pixels (independent of the model above)
  check(rgb_eq(o, wd, -56, 72, kEntRed), "BETWEEN over a transparent hole of the high-priority plane: the sprite shows (x + y even)");
  check(rgb_eq(o, wd, -55, 72, kEntYellow), "BETWEEN over an opaque high-priority pixel: the plane stays in front (x + y odd)");
  check(rgb_eq(o, wd, -56, 8, kEntRed) && rgb_eq(o, wd, -25, 39, kEntRed), "BETWEEN over low-priority planes: solid");
  check(rgb_eq(o, wd, 331, 73, kEntRed) && rgb_eq(o, wd, 330, 73, kEntRed), "ABOVE_ALL: opaque and transparent plane pixels alike");
  check(rgb_eq(o, wd, 335, 115, kEntYellow) || rgb_eq(o, wd, 335, 115, kEntBlue), "BEHIND_PLANES: hidden where a plane pixel exists (rows 112..127)");
  check(rgb_eq(o, wd, 340, 130, kEntCyan) && rgb_eq(o, wd, 361, 143, kEntCyan), "BEHIND_PLANES: drawn on the backdrop (rows 128..143)");
  check(rgb_eq(o, wd, -56, 72, kEntRed), "the first command claims a pixel; a later one on the same pixels does not repaint it");
}

void boundary_continuity() {
  const World wd({});
  const std::vector<Cmd> set = {{-16, 72, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed}, {304, 96, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed}};
  const Out o = run(wd, set, GENESIS_EXT_MODE_BACKGROUNDS);
  check(o.rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "sprites straddling the native/extension boundaries compose");
  compare(wd, set, o, false, kSide, "boundary sprites against the model");
  // the same rule on both sides of x = 0 and x = 320: red exactly on the transparent holes, the plane colour on the opaque pixels
  unsigned seam_bad = 0;
  for (int y = 72; y < 104; ++y)
    for (int x = -16; x < 16; ++x) {
      const bool hole = ((x + y) & 1) == 0 && y >= 64 && y < 128;
      if (!rgb_eq(o, wd, x, y, hole ? kEntRed : kEntYellow)) ++seam_bad;
    }
  check(seam_bad == 0, "no discontinuity at x = 0: the extension half and the picture half obey the same rule");
  unsigned seam_bad_r = 0;
  for (int y = 96; y < 128; ++y)
    for (int x = 304; x < 336; ++x) {
      const bool hole = ((x + y) & 1) == 0;
      if (!rgb_eq(o, wd, x, y, hole ? kEntRed : kEntYellow)) ++seam_bad_r;
    }
  check(seam_bad_r == 0, "no discontinuity at x = 320");
}

// Level 3: the extension also holds hardware sprites from the frame's own SAT; virtual BETWEEN sprites are ordered against them exactly like against native ones.
void hardware_sprite_candidates() {
  const std::vector<Hw> hw = {{-50, 80, false}, {-30, 96, true}, {100, 70, false}, {200, 100, true}, {340, 90, true}, {300, 150, false}};
  const World wd(hw);
  const std::vector<Cmd> set = {
      {-56, 72, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed},    // over a low hardware sprite (drawn over it), a high one (hidden by it) and the checker
      {96, 66, GENESIS_VSPRITE_LAYER_BETWEEN, kEntCyan},    // in the picture over a low hardware sprite
      {196, 96, GENESIS_VSPRITE_LAYER_BETWEEN, kEntCyan},   // in the picture, hidden by a high hardware sprite
      {330, 80, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed},    // right margin over a high hardware sprite
      {296, 140, GENESIS_VSPRITE_LAYER_BEHIND_PLANES, kEntCyan},   // over a low hardware sprite on the backdrop rows: hidden (a sprite pixel is not the backdrop)
      {-40, 120, GENESIS_VSPRITE_LAYER_ABOVE_ALL, kEntCyan},
  };
  const Out o = run(wd, set, GENESIS_EXT_MODE_SPRITES);
  check(o.rc == 0 && o.rep.status == GENESIS_VSPRITE_STATUS_COMPOSED && o.res.mode == GENESIS_EXT_MODE_SPRITES, "Level 3 produced the hardware sprites of the extension and the set composed");
  compare(wd, set, o, true, kSide, "hardware sprite candidates and ordering against the model");
  check(rgb_eq(o, wd, -45, 85, kEntRed), "a BETWEEN sprite is drawn over a low-priority hardware sprite in the margin");
  check(rgb_eq(o, wd, -28, 98, kEntWhite), "a high-priority hardware sprite in the margin hides a BETWEEN sprite");
  check(rgb_eq(o, wd, 105, 75, kEntCyan), "a BETWEEN sprite is drawn over a low-priority hardware sprite in the picture");
  check(rgb_eq(o, wd, 205, 105, kEntWhite), "a high-priority hardware sprite hides a BETWEEN sprite in the picture");
}

// The previous behaviour is untouched: ABOVE_ALL output and the unrelated surface are byte-identical with and without class data.
void legacy_behaviour_unchanged() {
  const World wd({});
  const std::vector<Cmd> above = {{-56, 72, GENESIS_VSPRITE_LAYER_ABOVE_ALL, kEntRed}, {330, 100, GENESIS_VSPRITE_LAYER_ABOVE_ALL, kEntCyan}, {-10, 10, GENESIS_VSPRITE_LAYER_ABOVE_ALL, kEntRed}};
  const Out with = run(wd, above, GENESIS_EXT_MODE_BACKGROUNDS, true);
  const Out without = run(wd, above, GENESIS_EXT_MODE_BACKGROUNDS, false);
  check(with.rep.status == GENESIS_VSPRITE_STATUS_COMPOSED && without.rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "ABOVE_ALL composes with and without class data");
  check(with.composed == without.composed && !without.classes_passed && with.classes_passed, "ABOVE_ALL output is byte-identical with and without class data");
  // the produced surface itself (colours) is the same whether or not classes are recorded
  const Out plain = run(wd, {}, GENESIS_EXT_MODE_BACKGROUNDS, false);
  const Out rec = run(wd, {}, GENESIS_EXT_MODE_BACKGROUNDS, true);
  check(plain.surface == rec.surface && plain.res.status == rec.res.status && plain.res.flags == rec.res.flags, "recording classes does not change the produced surface or its status");
  // a native 4:3 picture: a BETWEEN command entirely inside the picture does not need class data
  const std::vector<Cmd> inside = {{100, 72, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed}};
  const Out in = run(wd, inside, GENESIS_EXT_MODE_BACKGROUNDS, false);
  check(in.rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "a BETWEEN command wholly inside the picture composes without class data (native rules)");
  compare(wd, inside, in, false, kSide, "picture-only BETWEEN against the model", false);
}

void fail_closed() {
  const World wd({});
  const std::vector<Cmd> margin = {{-56, 72, GENESIS_VSPRITE_LAYER_ABOVE_ALL, kEntRed}, {-56, 8, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed}};
  auto rejected = [&](const Out &o, const char *what) {
    check(o.called && o.rep.status == GENESIS_VSPRITE_STATUS_REJECTED && o.rep.reason == GENESIS_VSPRITE_REJ_SIDE_PRIORITY && o.rep.reject_index == 1, what);
    check(o.composed.size() == static_cast<size_t>(kPitchPx) * kH * 3, what);
    // nothing of the set (the valid ABOVE_ALL sibling included) is drawn: every extension pixel equals the produced surface
    unsigned drew = 0;
    for (int y = 0; y < kH; ++y)
      for (int x = -kSide; x < 0; ++x)
        if (std::memcmp(&o.composed[(static_cast<size_t>(y) * kPitchPx + static_cast<size_t>(x + kSide)) * 3], &o.surface[(static_cast<size_t>(y) * kPitchPx + static_cast<size_t>(x + kSide)) * 3], 3) != 0) ++drew;
    check(drew == 0, what);
  };
  rejected(run(wd, margin, GENESIS_EXT_MODE_BACKGROUNDS, false), "no class buffer: a BETWEEN command that reaches an extension column rejects the whole set");
  rejected(run(wd, margin, GENESIS_EXT_MODE_BACKGROUNDS, true, 1000), "a class buffer too small for the surface is no class data: the set is rejected");
  rejected(run(wd, margin, GENESIS_EXT_MODE_BACKGROUNDS, true, SIZE_MAX, kSide, [](uint8_t *c) { c[static_cast<size_t>(30) * kPitchPx + 10] = 7; }), "an inconsistent class byte on a presented extension pixel rejects the set");
  rejected(run(wd, margin, GENESIS_EXT_MODE_BACKGROUNDS, true, SIZE_MAX, kSide, [](uint8_t *c) { c[static_cast<size_t>(200) * kPitchPx + kSide + kW + 63] = 3; }), "...on the right margin as well");
  // an inconsistent byte in a column the host does not present (visible 16 of 64) is not consulted
  const std::vector<Cmd> near = {{-16, 8, GENESIS_VSPRITE_LAYER_BETWEEN, kEntRed}};
  const Out far_bad = run(wd, near, GENESIS_EXT_MODE_BACKGROUNDS, true, SIZE_MAX, 16, [](uint8_t *c) { c[static_cast<size_t>(30) * kPitchPx + 2] = 9; });
  check(far_bad.rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "a bad byte beyond the presented columns does not reject a set that only uses presented ones");
  // a failed production never reaches the compositor
  const Out none = run(wd, kMixed, GENESIS_EXT_MODE_BACKGROUNDS, true, SIZE_MAX, kSide, nullptr, false);
  check(none.rc != 0 && !none.called, "an unavailable extension never calls the compositor (no stale class data is ever used)");
}

// Replay / save-restore: the classes are a pure function of the frame's own state. The same state reproduces the same bytes, a restored copy likewise, and a stale buffer is fully overwritten.
void replay_and_restore() {
  const World wd({});
  const Out first = run(wd, kMixed, GENESIS_EXT_MODE_BACKGROUNDS);
  const Out replay = run(wd, kMixed, GENESIS_EXT_MODE_BACKGROUNDS);
  check(first.composed == replay.composed && first.classes == replay.classes && first.surface == replay.surface, "replaying the same frame state reproduces the classes and the composed surface byte for byte");
  const World saved = wd;           // a save state: the whole VDP image copied
  const Out restored = run(saved, kMixed, GENESIS_EXT_MODE_BACKGROUNDS);
  check(first.composed == restored.composed && first.classes == restored.classes, "a restored copy of the state produces the same frame");
  // a different frame run through the SAME (stale) buffer: nothing of the earlier classes survives in the extension columns
  std::vector<uint8_t> shared(GENESIS_EXT_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT, 0xEE);
  (void)run(wd, kMixed, GENESIS_EXT_MODE_BACKGROUNDS, true, SIZE_MAX, kSide, nullptr, true, &shared);   // frame N fills the buffer
  const World low(std::vector<Hw>{}, false);   // frame N + 1: the checker is low priority now
  const Out next = run(low, kMixed, GENESIS_EXT_MODE_BACKGROUNDS, true, SIZE_MAX, kSide, nullptr, true, &shared);
  compare(low, kMixed, next, false, kSide, "the next frame's classes and composition follow the next frame's own state (nothing stale)");
  check(rgb_eq(next, low, -55, 72, kEntRed), "with a low-priority checker the BETWEEN sprite is solid over it (the earlier frame's HIGH classes are gone)");
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  priority_in_extension_columns();
  boundary_continuity();
  hardware_sprite_candidates();
  legacy_behaviour_unchanged();
  fail_closed();
  replay_and_restore();
  if (failures != 0) { std::printf("genesis_vsprite_priority_test: %d FAILED\n", failures); return 1; }
  std::printf("genesis_vsprite_priority_test: OK\n");
  return 0;
}
