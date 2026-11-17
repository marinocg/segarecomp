// SEG-140-T009 (ADR 0307): the PRESENTATION coordinate space of virtual sprites on the presented widescreen surface, at the compositor level. Every expectation is hand-derived from the construction of the scene
// (which pixel is which colour, how many pixels a sprite covers on each side of the picture), never from the code under test. The viewer-level proof (real presenter, real side styles, F15 coverage) is
// tests/mod_vsprite_wide_test.py.
#include <chrono>
#include <memory>

#include "genesis_vsprite_fixture.hpp"

using namespace vsfx;

namespace {

constexpr unsigned kArtTile = 200;   // sixteen all-index-9 patterns: a 4x4-cell virtual sprite is a solid red 32 x 32 block
constexpr uint8_t kRed[3] = {255, 0, 0}, kMargin[3] = {10, 20, 30};

struct Rig {
  Scene scene;
  Rendered r;
  std::vector<uint8_t> surface;   // the produced extension: (w + 2 * side) x h RGB24, margins filled with kMargin, the centre the native picture
  unsigned side = 0, left = 0;   // `left` = the picture's column in the surface (ADR 0315; == side when centred)
  int shift = 0;
  Rig() {
    scene.set_color(3, 1, 1, 1);
    scene.set_color(9, 7, 0, 0);
    for (unsigned t = 0; t < 16; ++t) scene.fill_tile(kArtTile + t, 9);
    check(r.render(scene), "the scene renders");
  }
  void widen(unsigned s) { widen_at(s, s); }
  // ADR 0315: a surface of total width r.w + 2 * s whose picture starts at column `at_left` (0 .. 2 * s)
  void widen_at(unsigned s, unsigned at_left) {
    side = s;
    left = at_left;
    shift = (int)at_left - (int)s;
    surface.assign((size_t)(r.w + 2 * s) * r.h * 3, 0);
    for (unsigned y = 0; y < r.h; ++y)
      for (unsigned x = 0; x < r.w + 2 * s; ++x) {
        uint8_t *p = &surface[((size_t)y * (r.w + 2 * s) + x) * 3];
        if (x < at_left || x >= at_left + r.w) std::memcpy(p, kMargin, 3);
        else std::memcpy(p, &r.rgb[((size_t)y * r.w + (x - at_left)) * 3], 3);
      }
  }
  unsigned pitch() const { return r.w + 2 * side; }
};

const uint8_t *at(const std::vector<uint8_t> &out, const Rig &g, int picture_x, unsigned y) { return &out[((size_t)y * g.pitch() + (unsigned)(picture_x + (int)g.left)) * 3]; }
bool is(const uint8_t *p, const uint8_t c[3]) { return std::memcmp(p, c, 3) == 0; }

GenesisVirtualSpriteReport compose(Rig &g, const GenesisVirtualSpriteSet *set, unsigned visible, std::vector<uint8_t> &out, int *rc = nullptr) {
  GenesisVirtualSpriteSurface s = {g.surface.data(), g.side, visible, nullptr, g.shift};   // ADR 0312: no class data (the fail-closed policy cases below)
  GenesisVirtualSpriteReport rep;
  out.assign(g.surface.size(), 0);
  const int result = genesis_virtual_sprites_compose_wide(set, GENESIS_VSPRITE_HOST_STATE_VERIFIED, g.scene.vram, &g.r.frame, &g.r.aux, &s, out.data(), out.size(), &rep);
  if (rc != nullptr) *rc = result;
  return rep;
}

std::unique_ptr<GenesisVirtualSpriteSet> make_set(unsigned space = GENESIS_VSPRITE_SPACE_PRESENTATION) {
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  zero_set(set.get());
  set->space = (uint8_t)space;
  return set;
}

void coordinates_and_regions() {
  Rig g;
  g.widen(39);   // 16:9 of a 320 x 224 picture: 39 columns per side
  std::vector<uint8_t> out;
  auto set = make_set();
  set->count = 1;
  set->sprites[0] = vsprite(-16, 40, 4, 4, kArtTile, 0);   // 32 x 32 px: x -16..15 straddles the left boundary: 16 columns in the margin, 16 in the picture
  GenesisVirtualSpriteReport rep = compose(g, set.get(), 39, out);
  check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED && rep.pixels_drawn_verified == 1024 && rep.pixels_left_verified == 512 && rep.pixels_right_verified == 0 && rep.pixels_left_authored == 0,
        "a sprite straddling the left boundary: 1024 pixels, 512 of them in the left margin");
  check(is(at(out, g, -16, 40), kRed) && is(at(out, g, -1, 71), kRed) && is(at(out, g, 0, 40), kRed) && is(at(out, g, 15, 71), kRed) && is(at(out, g, -17, 40), kMargin), "the sprite covers x -16..15 and rows 40..71");
  check(is(at(out, g, -17, 40), kMargin) && is(at(out, g, -20, 55), kMargin) && std::memcmp(at(out, g, 16, 40), &g.surface[((size_t)40 * g.pitch() + 39 + 16) * 3], 3) == 0, "its neighbours keep the margin colour and the native picture");
  // the right margin
  set->sprites[0] = vsprite((int)g.r.w - 16, 100, 4, 4, kArtTile, 0);
  rep = compose(g, set.get(), 39, out);
  check(rep.pixels_drawn_verified == 1024 && rep.pixels_right_verified == 512 && rep.pixels_left_verified == 0 && is(at(out, g, (int)g.r.w + 15, 100), kRed) && is(at(out, g, (int)g.r.w + 16, 100), kMargin), "right boundary: 512 pixels in the right margin");
  // a sprite wholly in a margin
  set->sprites[0] = vsprite(-30, 100, 2, 2, kArtTile, 0);
  rep = compose(g, set.get(), 39, out);
  check(rep.pixels_drawn_verified == 256 && rep.pixels_left_verified == 256 && is(at(out, g, -30, 100), kRed) && is(at(out, g, -15, 115), kRed) && is(at(out, g, -14, 115), kMargin), "wholly in the left margin: 256 pixels");
  // the surface keeps its layout: untouched margins stay exactly as produced, the centre is the native picture
  rep = compose(g, set.get(), 39, out);
  bool untouched = true;
  for (unsigned y = 0; y < g.r.h && untouched; ++y)
    for (unsigned x = 0; x < g.pitch(); ++x) {
      const bool sprite = y >= 100 && y < 116 && x >= 39 - 30 && x < 39 - 30 + 16;
      if (!sprite && std::memcmp(&out[((size_t)y * g.pitch() + x) * 3], &g.surface[((size_t)y * g.pitch() + x) * 3], 3) != 0) { untouched = false; break; }
    }
  check(untouched, "every pixel outside the sprite equals the produced surface (margins and picture)");
}

void outer_boundary_and_visibility() {
  Rig g;
  g.widen(39);
  std::vector<uint8_t> out;
  auto set = make_set();
  set->count = 1;
  set->sprites[0] = vsprite(-39, 20, 1, 1, kArtTile, 0);   // flush with the outer boundary of the produced surface
  GenesisVirtualSpriteReport rep = compose(g, set.get(), 39, out);
  check(rep.pixels_drawn_verified == 64 && is(at(out, g, -39, 20), kRed) && is(at(out, g, -32, 27), kRed), "flush with the outer boundary: all 64 pixels");
  set->sprites[0] = vsprite(-43, 20, 1, 1, kArtTile, 0);   // 4 columns beyond the outer boundary: clipped there, never moved
  rep = compose(g, set.get(), 39, out);
  check(rep.pixels_drawn_verified == 32 && rep.pixels_left_verified == 32 && is(at(out, g, -39, 20), kRed) && is(at(out, g, -36, 20), kRed) && is(at(out, g, -35, 20), kMargin), "across the outer boundary: only the 4 presented columns are drawn");
  set->sprites[0] = vsprite(-60, 20, 1, 1, kArtTile, 0);   // wholly beyond: nothing, and not an error
  rep = compose(g, set.get(), 39, out);
  check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED && rep.pixels_drawn_verified == 0, "wholly beyond the surface: composed with no pixel");
  // only the VISIBLE columns are drawn: the host presents 8 of the 39 produced columns
  set->sprites[0] = vsprite(-16, 20, 2, 1, kArtTile, 0);
  rep = compose(g, set.get(), 8, out);
  check(rep.pixels_left_verified == 64 && is(at(out, g, -8, 20), kRed) && is(at(out, g, -9, 20), kMargin), "visible 8: columns -16..-9 are not presented and not drawn (64 of the 128 margin pixels)");
  rep = compose(g, set.get(), 0, out);
  check(rep.pixels_left_verified == 0 && rep.pixels_drawn_verified == 0 && is(at(out, g, -1, 20), kMargin), "no visible column (4:3, Original): the sprite lies wholly in the margin, so nothing is drawn and the margin is untouched");
  set->sprites[0] = vsprite(-8, 20, 2, 1, kArtTile, 0);
  rep = compose(g, set.get(), 0, out);
  check(rep.pixels_drawn_verified == 64 && rep.pixels_left_verified == 0 && is(at(out, g, -1, 20), kMargin) && is(at(out, g, 0, 20), kRed), "no visible column: a straddling sprite is drawn in the picture only");
}

void priority_policy() {
  Rig g;
  g.widen(39);
  std::vector<uint8_t> out;
  auto set = make_set();
  const unsigned layers[] = {GENESIS_VSPRITE_LAYER_BEHIND_PLANES, GENESIS_VSPRITE_LAYER_BETWEEN};
  for (unsigned layer : layers) {
    set->count = 2;
    set->sprites[0] = vsprite(100, 20, 1, 1, kArtTile, 0);                                // valid, above all
    set->sprites[1] = vsprite(-4, 20, 1, 1, kArtTile, 0, layer);                           // reaches the left margin
    GenesisVirtualSpriteReport rep = compose(g, set.get(), 39, out);
    check(rep.status == GENESIS_VSPRITE_STATUS_REJECTED && rep.reason == GENESIS_VSPRITE_REJ_SIDE_PRIORITY && rep.reject_index == 1 && rep.pixels_drawn_verified == 0 && out == g.surface,
          "a BEHIND/BETWEEN command that reaches a margin rejects the whole set (SIDE_PRIORITY, its index) and draws nothing, the valid sibling included");
    set->sprites[1] = vsprite(300, 20, 2, 1, kArtTile, 0, layer);                         // reaches the right margin (x 300..315 of 320: still inside) -> use 312
    set->sprites[1].x = 312;
    rep = compose(g, set.get(), 39, out);
    check(rep.reason == GENESIS_VSPRITE_REJ_SIDE_PRIORITY && rep.reject_index == 1, "...and so does reaching the right margin");
    set->sprites[1] = vsprite(300, 20, 1, 1, kArtTile, 0, layer);                          // wholly inside the picture
    rep = compose(g, set.get(), 39, out);
    check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "the same layer inside the picture is unchanged");
    set->sprites[1] = vsprite(-4, 20, 1, 1, kArtTile, 0, layer);
    set->sprites[1].has_clip = 1;                                                           // clipped back into the picture: it no longer reaches a margin
    set->sprites[1].clip_x = 0; set->sprites[1].clip_y = 20; set->sprites[1].clip_w = 4; set->sprites[1].clip_h = 8;
    rep = compose(g, set.get(), 39, out);
    check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "a clip that keeps the sprite inside the picture lifts the restriction");
    rep = compose(g, set.get(), 0, out);
    check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "with no visible column no margin exists to reach");
  }
  // the PICTURE space never draws in a margin and is the legacy behaviour
  auto picture = make_set(GENESIS_VSPRITE_SPACE_PICTURE);
  picture->count = 1;
  picture->sprites[0] = vsprite(-16, 40, 4, 4, kArtTile, 0);
  GenesisVirtualSpriteReport rep = compose(g, picture.get(), 39, out);
  check(rep.pixels_drawn_verified == 512 && rep.pixels_left_verified == 0 && is(at(out, g, -1, 40), kMargin) && is(at(out, g, 0, 40), kRed), "PICTURE space: clipped to the picture, 512 pixels, none in the margin");
  std::vector<uint8_t> legacy(g.r.rgb.size());
  GenesisVirtualSpriteReport rep_legacy;
  check(genesis_virtual_sprites_compose(picture.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, g.scene.vram, &g.r.frame, &g.r.aux, legacy.data(), legacy.size(), &rep_legacy) == 0 && rep_legacy.pixels_drawn_verified == 512, "the legacy entry point is unchanged");
  bool same_centre = true;
  for (unsigned y = 0; y < g.r.h && same_centre; ++y) same_centre = std::memcmp(&legacy[(size_t)y * g.r.w * 3], at(out, g, 0, y), (size_t)g.r.w * 3) == 0;
  check(same_centre, "the picture of the wide composition is byte-identical to the legacy picture-sized composition");
}

void validation() {
  Rig g;
  g.widen(39);
  std::vector<uint8_t> out;
  auto set = make_set();
  set->count = 1;
  set->sprites[0] = vsprite(0, 0, 1, 1, kArtTile, 0);
  set->space = 2;
  GenesisVirtualSpriteReport rep = compose(g, set.get(), 39, out);
  check(rep.status == GENESIS_VSPRITE_STATUS_REJECTED && rep.reason == GENESIS_VSPRITE_REJ_RESERVED, "an unknown space is rejected as a reserved value");
  set->space = GENESIS_VSPRITE_SPACE_PRESENTATION;
  set->reserved[0] = 1;
  rep = compose(g, set.get(), 39, out);
  check(rep.reason == GENESIS_VSPRITE_REJ_RESERVED, "non-zero reserved bytes are still rejected");
  set->reserved[0] = 0;
  set->sprites[0].has_clip = 1;
  set->sprites[0].clip_x = -20; set->sprites[0].clip_y = 0; set->sprites[0].clip_w = 30; set->sprites[0].clip_h = 8;
  rep = compose(g, set.get(), 39, out);
  check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "a PRESENTATION clip may start in the margin");
  set->space = GENESIS_VSPRITE_SPACE_PICTURE;
  rep = compose(g, set.get(), 39, out);
  check(rep.reason == GENESIS_VSPRITE_REJ_CLIP, "the same clip in the PICTURE space is still outside the picture");
  set->space = GENESIS_VSPRITE_SPACE_PRESENTATION;
  set->sprites[0].clip_y = 220; set->sprites[0].clip_h = 20;
  rep = compose(g, set.get(), 39, out);
  check(rep.reason == GENESIS_VSPRITE_REJ_CLIP, "a clip below the picture is rejected in both spaces");
}

void budget_in_margins() {
  Rig g;
  for (unsigned n = 0; n < 20; ++n) g.scene.hw_sprite((int)(8 * n), 96, 1, 1, kArtTile, 0, false, false, false);   // 20 hardware sprites on rows 96..103: the H40 per-line limit
  check(g.r.render(g.scene) && g.r.aux.sprite_limit == 20, "twenty hardware sprites on one line");
  g.widen(39);
  std::vector<uint8_t> out;
  auto set = make_set();
  set->count = 1;
  set->sprites[0] = vsprite(-20, 96, 1, 1, kArtTile, 0);
  GenesisVirtualSpriteReport rep = compose(g, set.get(), 39, out);
  check(rep.pixels_drawn_verified == 0 && rep.sprite_rows_dropped == 8, "LIMIT: a margin sprite finds no per-line slot on the saturated rows and is dropped on all 8");
  set->budget_mode = GENESIS_VSPRITE_BUDGET_EXTENDED;
  rep = compose(g, set.get(), 39, out);
  check(rep.pixels_drawn_verified == 64 && rep.pixels_left_verified == 64 && rep.rows_over_sprite_limit == 8 && rep.sprite_rows_dropped == 0, "EXTENDED: drawn, and the 8 rows over the original limit are reported");
}

void aspect_changes_leave_nothing_behind() {
  Rig g;
  std::vector<uint8_t> out;
  auto set = make_set();
  set->count = 2;
  set->sprites[0] = vsprite(-30, 60, 2, 2, kArtTile, 0);
  set->sprites[1] = vsprite(330, 60, 2, 2, kArtTile, 0);
  const unsigned sides[] = {0, 39, 101, 238, 39, 0, 101};
  for (unsigned s : sides) {   // the same buffers and the same set, frame after frame, under a changing aspect: each composition is exactly what that aspect shows, nothing of the previous one remains
    g.widen(s);
    GenesisVirtualSpriteReport rep = compose(g, set.get(), s, out);
    // hand count: sprite 0 spans columns -30..-15 (16 wide, rows 60..75): the visible columns -s..-1 hold max(0, min(s, 30) - 14) of them; sprite 1 spans 330..345, the visible right columns are 320..319+s
    const unsigned left_cols = s >= 30 ? 16U : (s >= 15 ? s - 14U : 0U), right_cols = s >= 26 ? 16U : (s >= 11 ? s - 10U : 0U);
    check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED && rep.pixels_left_verified == 16U * left_cols && rep.pixels_right_verified == 16U * right_cols && rep.pixels_drawn_verified == rep.pixels_left_verified + rep.pixels_right_verified,
          "side changes: the pixels drawn follow the columns visible in THIS frame");
    check(out.size() == (size_t)(g.r.w + 2 * s) * g.r.h * 3, "the output has this frame's layout");
    if (s == 0) check(rep.pixels_drawn_verified == 0 && out == g.surface, "side 0: nothing is drawn and the output is the native picture");
  }
  g.widen(39);
  GenesisVirtualSpriteReport a = compose(g, set.get(), 39, out);
  std::vector<uint8_t> first = out;
  g.widen(238);
  (void)compose(g, set.get(), 238, out);
  g.widen(39);
  GenesisVirtualSpriteReport b = compose(g, set.get(), 39, out);
  check(first == out && std::memcmp(&a, &b, sizeof a) == 0, "returning to an earlier aspect reproduces the earlier frame byte for byte (no stale state)");
}

void bounded_cost() {
  Rig g;
  auto set = make_set();
  set->count = GENESIS_VSPRITE_MAX;
  for (unsigned i = 0; i < GENESIS_VSPRITE_MAX; ++i) set->sprites[i] = vsprite((int)(i * 5) - 150, (int)((i * 7) % 180), 2, 2, kArtTile, 0);
  std::vector<uint8_t> out;
  auto run = [&](unsigned side) {
    g.widen(side);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) (void)compose(g, set.get(), side, out);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  const double narrow = run(0), wide = run(238);
  check(wide < narrow * 12.0 + 0.05, "64 commands on the widest surface (796 px) cost a bounded multiple of the picture-only composition (a linear row cost, no per-pixel search)");
}

}  // namespace

// ADR 0315: the compositor on an asymmetric surface. The reference is independent of the shift arithmetic: the same sprites composed on a SYMMETRIC surface big enough for either margin, cropped.
void asymmetric_surface() {
  constexpr unsigned kSide = 39, kBig = 2 * kSide;
  const int xs[] = {-60, -30, -16, 0, 100, (int)320 - 16, 330, 340};   // wholly beyond / in the left margin / straddling left / inside / straddling right / in the right margin / flush
  Rig ref;
  ref.widen(kBig);
  auto set = make_set();
  set->count = 0;
  for (int x : xs) set->sprites[set->count++] = vsprite(x, 20 + 14 * (int)set->count, 2, 2, kArtTile, 0);
  std::vector<uint8_t> ref_out;
  const GenesisVirtualSpriteReport ref_rep = compose(ref, set.get(), kBig, ref_out);
  check(ref_rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "asymmetric: the symmetric reference composes");
  for (unsigned left : {0u, 1u, 7u, 38u, 39u, 40u, 70u, 77u, 78u}) {
    Rig g;
    g.widen_at(kSide, left);
    std::vector<uint8_t> out;
    int rc = -1;
    const GenesisVirtualSpriteReport rep = compose(g, set.get(), kSide, out, &rc);   // visible == side: shown whole
    char what[200];
    std::snprintf(what, sizeof what, "asymmetric %u/%u: composed", left, 78 - left);
    check(rc == 0 && rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, what);
    bool equal = out.size() == g.surface.size();
    for (unsigned y = 0; y < g.r.h && equal; ++y)
      equal = std::memcmp(&out[(size_t)y * g.pitch() * 3], &ref_out[((size_t)y * ref.pitch() + (kBig - left)) * 3], (size_t)g.pitch() * 3) == 0;
    std::snprintf(what, sizeof what, "asymmetric %u/%u: equals the crop of the symmetric composition (sprites crossing either edge, clipped at the surface boundary)", left, 78 - left);
    check(equal, what);
    // the region accounting follows what is on this surface: pixels in the left columns count as left, in the right columns as right
    uint32_t want_left = 0, want_right = 0;
    for (unsigned y = 0; y < g.r.h; ++y)
      for (unsigned x = 0; x < g.pitch(); ++x) {
        const bool drawn = std::memcmp(&out[((size_t)y * g.pitch() + x) * 3], &g.surface[((size_t)y * g.pitch() + x) * 3], 3) != 0;
        if (drawn && x < left) ++want_left;
        if (drawn && x >= left + g.r.w) ++want_right;
      }
    std::snprintf(what, sizeof what, "asymmetric %u/%u: left/right pixel counts are the drawn pixels in each extension region", left, 78 - left);
    check(rep.pixels_left_verified == want_left && rep.pixels_right_verified == want_right, what);
  }
  {   // the exact arithmetic for one split: a 32 x 32 sprite at x -16 on a surface with 7 columns left of the picture: 7 columns in the margin, 16 in the picture
    Rig g;
    g.widen_at(kSide, 7);
    std::vector<uint8_t> out;
    auto one = make_set();
    one->count = 1;
    one->sprites[0] = vsprite(-16, 40, 4, 4, kArtTile, 0);
    const GenesisVirtualSpriteReport rep = compose(g, one.get(), kSide, out);
    check(rep.pixels_left_verified == 7 * 32 && rep.pixels_drawn_verified == 7 * 32 + 16 * 32 && is(at(out, g, -7, 40), kRed) && is(at(out, g, -1, 71), kRed), "split 7/71: seven margin columns drawn, the one beyond the surface clipped");
  }
  {   // an asymmetric surface is composed whole or not at all: a partial crop would reinterpret columns the producer did not make for that view
    Rig g;
    g.widen_at(kSide, 20);
    std::vector<uint8_t> out;
    int rc = 0;
    (void)compose(g, set.get(), 10, out, &rc);
    check(rc != 0, "an asymmetric surface with a partial visible width is refused");
    rc = 0;
    const GenesisVirtualSpriteReport none = compose(g, set.get(), 0, out, &rc);
    check(rc == 0 && none.pixels_left_verified == 0 && none.pixels_right_verified == 0, "no visible column: the extension is untouched, the picture is still composed at its own position");
    GenesisVirtualSpriteSurface bad = {g.surface.data(), g.side, kSide, nullptr, 40};
    GenesisVirtualSpriteReport rep;
    check(genesis_virtual_sprites_compose_wide(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, g.scene.vram, &g.r.frame, &g.r.aux, &bad, out.data(), out.size(), &rep) != 0, "a shift beyond the planned total is refused");
    bad.shift = -40;
    check(genesis_virtual_sprites_compose_wide(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, g.scene.vram, &g.r.frame, &g.r.aux, &bad, out.data(), out.size(), &rep) != 0, "a negative shift beyond the planned total is refused");
  }
}

int main() {
  coordinates_and_regions();
  asymmetric_surface();
  outer_boundary_and_visibility();
  priority_policy();
  validation();
  budget_in_margins();
  aspect_changes_leave_nothing_behind();
  bounded_cost();
  if (failures != 0) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("genesis_vsprite_wide_tests: ok\n");
  return 0;
}
