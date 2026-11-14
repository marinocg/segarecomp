// SEG-140-T001/T003 (ADR 0303): the virtual-sprite command rules, the layer model against the native priority classes, clip rectangles, and the original per-line budget accounting.
// Every expectation below is hand-derived from the fixture's construction (which cell holds which colour, how many sprites a line holds), never from the code under test.
#include <memory>

#include "genesis_vsprite_fixture.hpp"

using namespace vsfx;

namespace {

constexpr unsigned kVirtTile = 200;   // 16 all-index-9 patterns (a 4x4 sprite) in the frame VRAM; kVirtTile + 20 holds 16 all-index-10 (white) patterns
constexpr unsigned kLowTile = 300, kHighTile = 301, kSpriteTile = 302;
constexpr uint8_t kVirtRgb[3] = {255, 0, 0};       // CRAM (3-bit 7,0,0) of entry 9
constexpr uint8_t kBackdropRgb[3] = {36, 36, 36};  // entry 3 = (1,1,1): 1 * 255 / 7 = 36

struct Layered {
  Scene scene;
  Rendered r;
  Layered() {
    scene.set_color(3, 1, 1, 1);
    scene.set_color(4, 0, 7, 0);    // plane B low   = green
    scene.set_color(5, 0, 0, 7);    // plane A high  = blue
    scene.set_color(6, 7, 7, 0);    // low hw sprite = yellow
    scene.set_color(7, 0, 7, 7);    // high hw sprite = cyan
    scene.set_color(9, 7, 0, 0);    // virtual art   = red
    for (unsigned t = 0; t < 16; ++t) scene.fill_tile(kVirtTile + t, 9);
    scene.fill_tile(kLowTile, 4);
    scene.fill_tile(kHighTile, 5);
    // band P (cell row 10 = y 80..87): cells 10,11 plane B low (x 80..95), cell 12 plane A high (x 96..103)
    scene.plane_cell(kPlaneB, 10, 10, kLowTile, 0, false);
    scene.plane_cell(kPlaneB, 11, 10, kLowTile, 0, false);
    scene.plane_cell(kPlaneA, 12, 10, kHighTile, 0, true);
    // band S (y 96..103): a low hardware sprite at x 72..79 and a high one at x 80..87
    scene.fill_tile(kSpriteTile, 6);
    scene.hw_sprite(72, 96, 1, 1, kSpriteTile, 0, false, false, false);
    scene.fill_tile(kSpriteTile + 1, 7);
    scene.hw_sprite(80, 96, 1, 1, kSpriteTile + 1, 0, false, false, true);
  }
  bool render() { return r.render(scene); }
  const uint8_t *native(unsigned x, unsigned y) const { return &r.rgb[((size_t)y * r.w + x) * 3]; }
};

bool same(const uint8_t *a, const uint8_t b[3]) { return std::memcmp(a, b, 3) == 0; }

// ---- decision 3: each priority case, per band and column range ----
void priority_cases() {
  const unsigned layers[] = {GENESIS_VSPRITE_LAYER_BEHIND_PLANES, GENESIS_VSPRITE_LAYER_BETWEEN, GENESIS_VSPRITE_LAYER_ABOVE_ALL};
  const uint8_t green[3] = {0, 255, 0}, blue[3] = {0, 0, 255}, yellow[3] = {255, 255, 0}, cyan[3] = {0, 255, 255};
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  for (unsigned layer : layers) {
    Layered L;
    check(L.render(), "layered scene renders");
    // the native classes themselves (hand-known): backdrop at (72,84), low plane at (84,84), high plane at (100,84), low sprite (75,100), high sprite (83,100)
    check(L.r.aux.layer[84 * L.r.w + 72] == GENESIS_VDP_LAYER_BACKDROP && L.r.aux.layer[84 * L.r.w + 84] == GENESIS_VDP_LAYER_LOW &&
              L.r.aux.layer[84 * L.r.w + 100] == GENESIS_VDP_LAYER_HIGH && L.r.aux.layer[100 * L.r.w + 75] == GENESIS_VDP_LAYER_LOW &&
              L.r.aux.layer[100 * L.r.w + 83] == GENESIS_VDP_LAYER_HIGH && L.r.aux.layer[92 * L.r.w + 90] == GENESIS_VDP_LAYER_BACKDROP,
          "native pixel classes are backdrop / low / high as constructed");
    check(same(L.native(84, 84), green) && same(L.native(100, 84), blue) && same(L.native(75, 100), yellow) && same(L.native(83, 100), cyan) && same(L.native(72, 84), kBackdropRgb), "native colours as constructed");
    zero_set(set.get());
    set->count = 1;
    set->sprites[0] = vsprite(72, 80, 4, 4, kVirtTile, 0, layer);
    std::vector<uint8_t> out(L.r.rgb.size());
    GenesisVirtualSpriteReport rep;
    check(genesis_virtual_sprites_compose(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, L.scene.vram, &L.r.frame, &L.r.aux, out.data(), out.size(), &rep) == 0 && rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "composed");
    auto px = [&](unsigned x, unsigned y) { return &out[((size_t)y * L.r.w + x) * 3]; };
    struct Probe { unsigned x, y; unsigned cls; };   // the native class at each probe
    const Probe probes[] = {{72, 84, 0}, {84, 84, 1}, {100, 84, 2}, {75, 100, 1}, {83, 100, 2}, {90, 92, 0}, {100, 108, 0}};
    for (const Probe &p : probes) {
      const bool visible = layer == GENESIS_VSPRITE_LAYER_ABOVE_ALL || (layer == GENESIS_VSPRITE_LAYER_BETWEEN && p.cls != 2) || (layer == GENESIS_VSPRITE_LAYER_BEHIND_PLANES && p.cls == 0);
      check(same(px(p.x, p.y), visible ? kVirtRgb : L.native(p.x, p.y)), visible ? "virtual sprite visible where its layer allows" : "native pixel kept where its layer is below it");
    }
    // nothing outside the sprite's rectangle changed
    check(same(px(71, 84), L.native(71, 84)) && same(px(104, 84), L.native(104, 84)) && same(px(80, 79), L.native(80, 79)) && same(px(80, 112), L.native(80, 112)), "pixels around the rectangle are untouched");
  }
}

// ---- order: ABOVE beats BETWEEN regardless of command order; the first command wins inside a layer ----
void ordering() {
  Layered L;
  check(L.render(), "render");
  L.scene.set_color(10, 7, 7, 7);   // white
  for (unsigned t = 0; t < 16; ++t) L.scene.fill_tile(kVirtTile + 20 + t, 10);
  check(L.render(), "render");
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  std::vector<uint8_t> out(L.r.rgb.size());
  GenesisVirtualSpriteReport rep;
  const uint8_t white[3] = {255, 255, 255};
  auto run = [&](unsigned l0, unsigned l1) {
    zero_set(set.get());
    set->count = 2;
    set->sprites[0] = vsprite(72, 80, 2, 2, kVirtTile, 0, l0);
    set->sprites[1] = vsprite(72, 80, 2, 2, kVirtTile + 20, 0, l1);
    return genesis_virtual_sprites_compose(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, L.scene.vram, &L.r.frame, &L.r.aux, out.data(), out.size(), &rep);
  };
  const unsigned A = GENESIS_VSPRITE_LAYER_ABOVE_ALL, B = GENESIS_VSPRITE_LAYER_BETWEEN;
  check(run(A, A) == 0 && same(&out[((size_t)84 * L.r.w + 72) * 3], kVirtRgb), "same layer: the first command wins");
  check(run(B, A) == 0 && same(&out[((size_t)84 * L.r.w + 72) * 3], white), "a higher layer wins even when it is the later command");
  check(run(A, B) == 0 && same(&out[((size_t)84 * L.r.w + 72) * 3], kVirtRgb), "a higher layer wins when it is the earlier command");
  // a sprite hidden by native priority does not block a lower one that is visible: (84,84) is low-priority native, so BETWEEN shows and BEHIND does not
  check(run(GENESIS_VSPRITE_LAYER_BEHIND_PLANES, B) == 0 && same(&out[((size_t)84 * L.r.w + 84) * 3], white), "a hidden BEHIND sprite lets a visible BETWEEN sprite through");
}

// ---- clip rectangles and picture edges ----
void clipping() {
  Layered L;
  check(L.render(), "render");
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  std::vector<uint8_t> out(L.r.rgb.size() + 64, 0xAB);   // canary after the picture
  GenesisVirtualSpriteReport rep;
  auto compose = [&]() { std::fill(out.begin(), out.end(), 0xAB); return genesis_virtual_sprites_compose(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, L.scene.vram, &L.r.frame, &L.r.aux, out.data(), L.r.rgb.size(), &rep); };
  auto drawn = [&](unsigned x, unsigned y) { return same(&out[((size_t)y * L.r.w + x) * 3], kVirtRgb); };
  // clip rectangle (80, 82) 16 x 4 inside a 4x4-cell sprite at (72, 80): exactly the pixels x 80..95, y 82..85 may change
  zero_set(set.get());
  set->count = 1;
  set->sprites[0] = vsprite(72, 80, 4, 4, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL);
  set->sprites[0].has_clip = 1; set->sprites[0].clip_x = 80; set->sprites[0].clip_y = 82; set->sprites[0].clip_w = 16; set->sprites[0].clip_h = 4;
  check(compose() == 0 && rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "clipped sprite composes");
  unsigned inside = 0, wrong = 0;
  for (unsigned y = 78; y < 90; ++y)
    for (unsigned x = 70; x < 106; ++x) {
      const bool expect = x >= 80 && x < 96 && y >= 82 && y < 86;
      if (expect) ++inside;
      if (drawn(x, y) != expect) ++wrong;
    }
  check(inside == 64 && wrong == 0, "the clip rectangle edges are exact (80..95 x 82..85, one pixel outside is untouched)");
  check(rep.pixels_drawn_verified == 64, "64 pixels drawn inside the clip");
  // picture edges: partly off the left/top and off the right/bottom; never a write outside the picture buffer
  const unsigned W = L.r.w, H = L.r.h;
  zero_set(set.get());
  set->count = 3;
  set->sprites[0] = vsprite(-4, -6, 2, 2, kVirtTile, 0);
  set->sprites[1] = vsprite((int)W - 3, 40, 2, 1, kVirtTile, 0);
  set->sprites[2] = vsprite(100, (int)H - 5, 1, 2, kVirtTile, 0);
  check(compose() == 0 && rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "edge sprites compose");
  check(drawn(0, 0) && drawn(11, 9) && !drawn(12, 0) && !drawn(0, 10), "left/top overhang: columns 0..11 and rows 0..9 are covered (sprite spans -4..11 x -6..9)");
  check(drawn(W - 1, 40) && drawn(W - 3, 47) && !drawn(W - 4, 40) && !drawn(W - 1, 48), "right overhang: exactly the 3 last columns, 8 rows");
  check(drawn(100, H - 1) && drawn(107, H - 5) && !drawn(100, H - 6), "bottom overhang: exactly the 5 last rows");
  bool canary = true;
  for (size_t i = L.r.rgb.size(); i < out.size(); ++i) canary = canary && out[i] == 0xAB;
  check(canary, "nothing was written past the picture buffer");
  // a sprite completely outside the picture draws nothing and is not an error
  zero_set(set.get());
  set->count = 1;
  set->sprites[0] = vsprite(900, 900, 1, 1, kVirtTile, 0);
  check(compose() == 0 && rep.pixels_drawn_verified == 0 && std::memcmp(out.data(), L.r.rgb.data(), L.r.rgb.size()) == 0, "a sprite far outside the picture changes nothing");
}

// ---- the rejection rules of ADR 0303 decision 4 ----
void rejection_rules() {
  Layered L;
  check(L.render(), "render");
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  std::vector<uint8_t> out(L.r.rgb.size());
  GenesisVirtualSpriteReport rep;
  struct Case { const char *name; unsigned reason; unsigned index; unsigned host; void (*mutate)(GenesisVirtualSpriteSet *); };
  const Case cases[] = {
      {"too many commands", GENESIS_VSPRITE_REJ_TOO_MANY, 65, 1, [](GenesisVirtualSpriteSet *s) { s->count = 65; }},
      {"no provenance", GENESIS_VSPRITE_REJ_PROVENANCE, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].provenance = 0; }},
      {"unknown provenance", GENESIS_VSPRITE_REJ_PROVENANCE, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].provenance = 3; }},
      {"verified art without a verified frame state", GENESIS_VSPRITE_REJ_UNVERIFIED_STATE, 0, 0, [](GenesisVirtualSpriteSet *) {}},
      {"zero width", GENESIS_VSPRITE_REJ_SIZE, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].width_cells = 0; }},
      {"width 5", GENESIS_VSPRITE_REJ_SIZE, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].width_cells = 5; }},
      {"height 5", GENESIS_VSPRITE_REJ_SIZE, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].height_cells = 5; }},
      {"patterns past the 64 KiB image", GENESIS_VSPRITE_REJ_TILE_RANGE, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].tile_index = 2047; s->sprites[0].width_cells = 2; }},
      {"tile index above 11 bits", GENESIS_VSPRITE_REJ_TILE_RANGE, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].tile_index = 4000; }},
      {"palette 4", GENESIS_VSPRITE_REJ_PALETTE, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].palette = 4; }},
      {"layer 3", GENESIS_VSPRITE_REJ_LAYER, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].layer = 3; }},
      {"x out of range", GENESIS_VSPRITE_REJ_POSITION, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].x = 1025; }},
      {"y out of range", GENESIS_VSPRITE_REJ_POSITION, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].y = -1025; }},
      {"empty clip", GENESIS_VSPRITE_REJ_CLIP, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].has_clip = 1; s->sprites[0].clip_w = 0; s->sprites[0].clip_h = 4; }},
      {"clip beyond the picture", GENESIS_VSPRITE_REJ_CLIP, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].has_clip = 1; s->sprites[1].clip_x = 300; s->sprites[1].clip_w = 21; s->sprites[1].clip_h = 1; }},
      {"negative clip origin", GENESIS_VSPRITE_REJ_CLIP, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].has_clip = 1; s->sprites[0].clip_x = -1; s->sprites[0].clip_w = 4; s->sprites[0].clip_h = 4; }},
      {"reserved byte", GENESIS_VSPRITE_REJ_RESERVED, 1, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[1].reserved = 1; }},
      {"flip not 0/1", GENESIS_VSPRITE_REJ_RESERVED, 0, 1, [](GenesisVirtualSpriteSet *s) { s->sprites[0].h_flip = 2; }},
      {"set reserved bytes", GENESIS_VSPRITE_REJ_RESERVED, 0, 1, [](GenesisVirtualSpriteSet *s) { s->reserved[1] = 1; }},
      {"unknown budget mode", GENESIS_VSPRITE_REJ_BUDGET_MODE, 0, 1, [](GenesisVirtualSpriteSet *s) { s->budget_mode = 2; }},
  };
  for (const Case &c : cases) {
    zero_set(set.get());
    set->count = 2;
    set->sprites[0] = vsprite(10, 10, 1, 1, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL, c.reason == GENESIS_VSPRITE_REJ_UNVERIFIED_STATE ? GENESIS_VSPRITE_GAME_STATE_VERIFIED : GENESIS_VSPRITE_MOD_AUTHORED);
    set->sprites[1] = vsprite(30, 10, 1, 1, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_MOD_AUTHORED);
    c.mutate(set.get());
    const int rc = genesis_virtual_sprites_compose(set.get(), c.host, L.scene.vram, &L.r.frame, &L.r.aux, out.data(), out.size(), &rep);
    char what[160];
    std::snprintf(what, sizeof what, "rejected with the stable reason and offending index: %s", c.name);
    check(rc == 0 && rep.status == GENESIS_VSPRITE_STATUS_REJECTED && rep.reason == c.reason && rep.reject_index == c.index, what);
    std::snprintf(what, sizeof what, "a rejected set draws nothing (fail closed): %s", c.name);
    check(out == L.r.rgb && rep.pixels_drawn_verified == 0 && rep.pixels_drawn_authored == 0, what);
  }
  // the same authored set is accepted without any host flag; verified art needs it
  zero_set(set.get());
  set->count = 1;
  set->sprites[0] = vsprite(10, 10, 1, 1, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_MOD_AUTHORED);
  check(genesis_virtual_sprites_compose(set.get(), 0, L.scene.vram, &L.r.frame, &L.r.aux, out.data(), out.size(), &rep) == 0 && rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "authored art needs no verified-state flag");
  // drawing never changes VRAM, the set or the native frame (read-only presentation; no object activation, no art residency)
  Layered L2;
  check(L2.render(), "render");
  Scene before = L2.scene;
  zero_set(set.get());
  set->count = 1;
  set->sprites[0] = vsprite(72, 80, 4, 4, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL);
  auto set_copy = std::make_unique<GenesisVirtualSpriteSet>(*set);
  GenesisFrameArtifact frame_copy = L2.r.frame;
  check(genesis_virtual_sprites_compose(set.get(), 1, L2.scene.vram, &L2.r.frame, &L2.r.aux, out.data(), out.size(), &rep) == 0, "compose");
  check(std::memcmp(&before, &L2.scene, sizeof before) == 0 && std::memcmp(set.get(), set_copy.get(), sizeof *set) == 0 && std::memcmp(&frame_copy, &L2.r.frame, sizeof frame_copy) == 0,
        "VRAM/CRAM, the command set and the native frame are byte-identical after composing");
}

// ---- budget accounting against a hand count ----
void budget() {
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  // H40: 20 sprites / 40 cells per line. Hardware: three 2-cell sprites on the band y 96..103 (3 sprites, 6 cells).
  Scene sc;
  sc.set_color(3, 1, 1, 1);
  sc.set_color(9, 7, 0, 0);
  for (unsigned t = 0; t < 16; ++t) sc.fill_tile(kVirtTile + t, 9);
  sc.fill_tile(kSpriteTile, 6);
  for (int i = 0; i < 3; ++i) sc.hw_sprite(20 + i * 16, 96, 2, 1, kSpriteTile, 0, false, false, false);
  Rendered r;
  check(r.render(sc), "H40 render");
  check(r.aux.sprite_limit == 20 && r.aux.cell_budget == 40 && r.aux.row_hw_sprites[100] == 3 && r.aux.row_hw_cells[100] == 6 && r.aux.row_hw_sprites[95] == 0 && r.aux.row_hw_sprites[104] == 0,
        "hardware accounting equals the hand count (3 sprites, 6 cells on rows 96..103, none beside)");
  std::vector<uint8_t> out(r.rgb.size());
  GenesisVirtualSpriteReport rep;
  // (a) ten 4-cell sprites on the band: cell demand 6 + 40 = 46 > 40, sprite demand 13 <= 20
  auto cells_scene = [&](unsigned mode) {
    zero_set(set.get());
    set->budget_mode = (uint8_t)mode;
    set->count = 10;
    for (unsigned i = 0; i < 10; ++i) set->sprites[i] = vsprite(100 + (int)i, 96, 4, 1, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_MOD_AUTHORED);
    std::memcpy(set->authored_vram, sc.vram, sizeof sc.vram);
    std::memcpy(set->authored_cram, sc.cram, sizeof sc.cram);
    return genesis_virtual_sprites_compose(set.get(), 0, sc.vram, &r.frame, &r.aux, out.data(), out.size(), &rep);
  };
  check(cells_scene(GENESIS_VSPRITE_BUDGET_LIMIT) == 0, "LIMIT compose");
  check(rep.row_sprites[100] == 13 && rep.row_cells[100] == 46 && rep.row_sprites[95] == 0 && rep.row_cells[104] == 0, "per-row demand = hardware + virtual (13 sprites, 46 cells), neighbours untouched");
  check(rep.rows_over_cell_budget == 8 && rep.rows_over_sprite_limit == 0, "8 rows over the cell budget, none over the sprite limit (hand count: the band is 8 rows)");
  // 34 cells left: eight sprites fit (32), the 9th gets 2 cells, the 10th none -> 2 truncated sprite-rows per row = 16, dropped 0
  check(rep.sprite_rows_dropped == 0 && rep.sprite_rows_truncated == 16, "LIMIT: 2 sprites truncated on each of 8 rows");
  // drawn width: x 100..: sprite 0..7 cover 100..138 (4 cells = 32px; later sprites start 1px later, union 100..139), the 9th starts at 108 and only 16 px are drawn
  check(rep.pixels_drawn_authored > 0, "pixels were drawn");
  const uint64_t limited = rep.pixels_drawn_authored;
  check(cells_scene(GENESIS_VSPRITE_BUDGET_EXTENDED) == 0, "EXTENDED compose");
  check(rep.sprite_rows_dropped == 0 && rep.sprite_rows_truncated == 0 && rep.rows_over_cell_budget == 8 && rep.row_cells[100] == 46, "EXTENDED draws everything but still reports the over-budget rows");
  check(rep.pixels_drawn_authored >= limited, "EXTENDED draws at least what LIMIT draws");
  // the union of 10 sprites starting at x = 100..109, each 32 px wide, is x 100..140 = 41 columns x 8 rows; LIMIT's union is x 100..(108+16+...) smaller
  check(rep.pixels_drawn_authored == 41u * 8u, "EXTENDED: the union of the ten 32-pixel sprites is 41 columns x 8 rows (hand count)");
  // (b) 25 one-cell sprites: sprite demand 3 + 25 = 28 > 20; LIMIT leaves 17 slots -> 8 dropped per row
  zero_set(set.get());
  set->count = 25;
  for (unsigned i = 0; i < 25; ++i) set->sprites[i] = vsprite(10 + (int)i * 9, 96, 1, 1, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_MOD_AUTHORED);
  std::memcpy(set->authored_vram, sc.vram, sizeof sc.vram);
  std::memcpy(set->authored_cram, sc.cram, sizeof sc.cram);
  check(genesis_virtual_sprites_compose(set.get(), 0, sc.vram, &r.frame, &r.aux, out.data(), out.size(), &rep) == 0, "slots compose");
  check(rep.row_sprites[96] == 28 && rep.row_cells[96] == 31 && rep.rows_over_sprite_limit == 8 && rep.rows_over_cell_budget == 0, "demand 28 sprites / 31 cells on 8 rows");
  check(rep.sprite_rows_dropped == 8u * 8u && rep.sprite_rows_truncated == 0, "LIMIT: 8 sprites dropped on each of the 8 rows");
  check(rep.pixels_drawn_authored == 17u * 8u * 8u, "17 one-cell sprites x 64 pixels are drawn (the 8 beyond the limit are not)");
  // (c) a hardware line already at the original limit (21 hardware sprites: the 21st is dropped by the VDP) leaves no slot
  Scene full;
  full.set_color(9, 7, 0, 0);
  for (unsigned t = 0; t < 16; ++t) full.fill_tile(kVirtTile + t, 9);
  full.fill_tile(kSpriteTile, 6);
  for (int i = 0; i < 21; ++i) full.hw_sprite(4 + i * 12, 96, 1, 1, kSpriteTile, 0, false, false, false);
  Rendered rf;
  check(rf.render(full) && rf.aux.row_hw_sprites[100] == 20 && rf.aux.row_hw_cells[100] == 20, "the VDP itself selects 20 of 21 hardware sprites (hand count)");
  zero_set(set.get());
  set->count = 1;
  set->sprites[0] = vsprite(250, 96, 1, 1, kVirtTile, 0, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_GAME_STATE_VERIFIED);
  std::vector<uint8_t> outf(rf.rgb.size());
  check(genesis_virtual_sprites_compose(set.get(), 1, full.vram, &rf.frame, &rf.aux, outf.data(), outf.size(), &rep) == 0 && rep.sprite_rows_dropped == 8 && rep.pixels_drawn_verified == 0 && outf == rf.rgb,
        "LIMIT: a line already full of hardware sprites takes no virtual sprite");
  set->budget_mode = GENESIS_VSPRITE_BUDGET_EXTENDED;
  check(genesis_virtual_sprites_compose(set.get(), 1, full.vram, &rf.frame, &rf.aux, outf.data(), outf.size(), &rep) == 0 && rep.sprite_rows_dropped == 0 && rep.pixels_drawn_verified == 64 && rep.rows_over_sprite_limit == 8,
        "EXTENDED (explicit opt-in): the same sprite is drawn and the 8 over-limit rows are reported");
  // H32 limits: 16 sprites / 32 cells
  Scene h32(false);
  Rendered r32;
  check(r32.render(h32) && r32.w == 256 && r32.aux.sprite_limit == 16 && r32.aux.cell_budget == 32, "H32 reports 256 wide, 16 sprites, 32 cells");
}

}  // namespace

int main() {
  priority_cases();
  ordering();
  clipping();
  rejection_rules();
  budget();
  if (failures != 0) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("genesis_vsprite_layers_tests: ok\n");
  return 0;
}
