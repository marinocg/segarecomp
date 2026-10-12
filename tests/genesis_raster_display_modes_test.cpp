// SEG-055: synthetic, hermetic fixtures for the generic Genesis video/raster/region behaviors:
//   scanline-aware CRAM/VSRAM/register state, horizontal-scroll modes 0/2/3 and per-column vertical scroll
//   (wrapping, negative offsets, mixed modes), H32 geometry and sprite limits, interlace mode 2 (8x16 patterns,
//   field-line scroll tables, 11-bit vertical scroll, doubled sprite Y system), the Window plane, the left-column mask,
//   display-mode transitions, the H-interrupt counter and level-4 admission, console-region version register values and
//   independent Player 1 / Player 2 controller ports.
//
// Every expectation is derived by hand from the documented hardware rule (see vdp_render.h's SEG-055 block) using a
// tiny encoding trick: pattern/tile identity encodes position, so the expected color of any pixel is plain arithmetic in
// this file and does not call the renderer's own helpers. No ROM, no commercial data, no host clock.
#include "runtime.h"
#include "vdp_render.h"
#include "frame_export.h"
#include "../libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// ---- synthetic VDP scene ---------------------------------------------------------------------------------------
struct Scene {
  std::vector<uint8_t> vram = std::vector<uint8_t>(GENESIS_VDP_VRAM_BYTES, 0);
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES] = {};
  uint8_t cram[GENESIS_VDP_CRAM_BYTES] = {};
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT] = {};
};

void put16(Scene &s, uint32_t addr, unsigned value) {
  s.vram[addr & 0xFFFFU] = (uint8_t)(value >> 8);
  s.vram[(addr + 1U) & 0xFFFFU] = (uint8_t)value;
}

// Palette variants: variant 0 maps CRAM index i (0..63) to R = i & 7, G = (i >> 3) & 7, B = (i ^ (i >> 3)) & 7, so every
// index has a unique RGB. Variant 1 swaps R/G and pins B, a visibly different palette.
void fill_cram(uint8_t cram[GENESIS_VDP_CRAM_BYTES], int variant) {
  for (unsigned i = 0; i < 64U; ++i) {
    const unsigned r = variant == 0 ? (i & 7U) : ((i >> 3) & 7U);
    const unsigned g = variant == 0 ? ((i >> 3) & 7U) : (i & 7U);
    const unsigned b = variant == 0 ? ((i ^ (i >> 3)) & 7U) : 7U;
    const unsigned word = (b << 9) | (g << 5) | (r << 1);
    cram[i * 2U] = (uint8_t)(word >> 8);
    cram[i * 2U + 1U] = (uint8_t)word;
  }
}

struct Rgb { unsigned r, g, b; };
Rgb expected_rgb(unsigned i, int variant) {
  const unsigned r = variant == 0 ? (i & 7U) : ((i >> 3) & 7U);
  const unsigned g = variant == 0 ? ((i >> 3) & 7U) : (i & 7U);
  const unsigned b = variant == 0 ? ((i ^ (i >> 3)) & 7U) : 7U;
  return {r * 255U / 7U, g * 255U / 7U, b * 255U / 7U};
}

// A solid pattern (every pixel = `color`); `bytes` is 32 for an 8x8 pattern, 64 for an 8x16 one.
void solid_tile(Scene &s, unsigned tile, unsigned color, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) s.vram[(tile * bytes + i) & 0xFFFFU] = (uint8_t)((color << 4) | color);
}
// An 8x16 pattern whose rows 0-7 are `top` and rows 8-15 are `bottom`.
void split_tile_im2(Scene &s, unsigned tile, unsigned top, unsigned bottom) {
  for (unsigned row = 0; row < 16U; ++row)
    for (unsigned b = 0; b < 4U; ++b)
      s.vram[(tile * 64U + row * 4U + b) & 0xFFFFU] = (uint8_t)((((row < 8U) ? top : bottom) << 4) | ((row < 8U) ? top : bottom));
}
unsigned nt_word(unsigned tile, unsigned pal = 0, unsigned prio = 0, unsigned hf = 0, unsigned vf = 0) {
  return (prio << 15) | (pal << 13) | (vf << 12) | (hf << 11) | (tile & 0x7FFU);
}

// Standard layout: Plane A 0xC000, window 0xD000, Plane B 0xE000, SAT 0xF800, H-scroll table 0xFC00, planes 64x32.
void base_scene(Scene &s, unsigned reg12) {
  fill_cram(s.cram, 0);
  s.regs[0] = 0;
  s.regs[1] = 0x04;
  s.regs[2] = 0x30;
  s.regs[3] = 0x34;
  s.regs[4] = 0x07;
  s.regs[5] = 0x7C;
  s.regs[7] = 0;
  s.regs[11] = 0;
  s.regs[12] = (uint16_t)reg12;
  s.regs[13] = 0x3F;
  s.regs[16] = 0x01;
}

std::unique_ptr<GenesisFrameArtifact> make_frame() {
  auto frame = std::make_unique<GenesisFrameArtifact>();
  std::memset(frame.get(), 0, sizeof(*frame));
  return frame;
}

// Renders a uniform scene through BOTH producers (single snapshot and per-scanline) and requires them to agree.
bool render(const Scene &s, GenesisFrameArtifact *frame) {
  auto raster = std::make_unique<GenesisRasterFrameState>();
  for (auto &line : raster->lines) {
    std::memcpy(line.registers, s.regs, sizeof(line.registers));
    std::memcpy(line.cram, s.cram, sizeof(line.cram));
    std::memcpy(line.vsram, s.vsram, sizeof(line.vsram));
  }
  auto legacy = make_frame();
  const int rc_raster = genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), frame);
  const int rc_legacy = genesis_vdp_produce_frame(s.vram.data(), s.vsram, s.cram, s.regs, legacy.get());
  check(rc_raster == rc_legacy, "legacy and scanline producers agree on success/failure");
  if (rc_raster == 0 && rc_legacy == 0)
    check(std::memcmp(frame, legacy.get(), sizeof(*frame)) == 0, "uniform scanline state == single-snapshot render (byte-identical artifact)");
  return rc_raster == 0;
}

// Independent artifact -> RGB decode (CRAM word layout ----BBB-GGG-RRR-, 3-bit channel value * 255 / 7) so the expectations never
// depend on the renderer's own conversion helper (that helper is cross-checked against this in test_export_and_presentation_agree).
Rgb pixel(const GenesisFrameArtifact &frame, unsigned x, unsigned row) {
  const unsigned width = GENESIS_FRAME_ARTIFACT_WIDTH(&frame);
  if (row >= GENESIS_FRAME_ARTIFACT_HEIGHT(&frame) || x >= width) return {999, 999, 999};
  const unsigned index = frame.pixels[(size_t)row * width + x];
  const unsigned slot = frame.row_palette[row];
  if (index >= 64U || slot >= GENESIS_FRAME_MAX_PALETTES || (slot != 0U && slot >= frame.palette_count)) return {999, 999, 999};
  const uint8_t *palette = slot == 0U ? frame.palette_snapshot : frame.extra_palettes[slot - 1U];
  const unsigned word = ((unsigned)palette[index * 2U] << 8) | palette[index * 2U + 1U];
  return {((word >> 1) & 7U) * 255U / 7U, ((word >> 5) & 7U) * 255U / 7U, ((word >> 9) & 7U) * 255U / 7U};
}
bool is_color(const GenesisFrameArtifact &frame, unsigned x, unsigned row, unsigned cram_index, int variant = 0) {
  const Rgb p = pixel(frame, x, row);
  const Rgb e = expected_rgb(cram_index, variant);
  return p.r == e.r && p.g == e.g && p.b == e.b;
}

int mod(int value, int m) {
  const int r = value % m;
  return r < 0 ? r + m : r;
}

// ---- scroll fixtures -------------------------------------------------------------------------------------------
// Plane B: 64x32 cells, cell (c, r) uses solid tile 1 + (c % 14) => color index 1 + (c % 14). Plane A is empty.
void fill_plane_b_columns(Scene &s) {
  for (unsigned t = 1; t <= 14U; ++t) solid_tile(s, t, t, 32U);
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(s, 0xE000U + (r * 64U + c) * 2U, nt_word(1U + (c % 14U)));
}
// Plane A: cell (c, r) uses solid tile 1 + (r % 14). Plane B is empty.
void fill_plane_a_rows(Scene &s) {
  for (unsigned t = 1; t <= 14U; ++t) solid_tile(s, t, t, 32U);
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(s, 0xC000U + (r * 64U + c) * 2U, nt_word(1U + (r % 14U)));
}
int road_hs(unsigned y) { return (int)((y * 7U) % 600U) - 300; }  // -300..299: negative, positive, wraps the 512px plane
void write_hscroll_table(Scene &s) {
  for (unsigned y = 0; y < 224U; ++y) {
    put16(s, 0xFC00U + y * 4U, 0);  // Plane A word (unused here)
    put16(s, 0xFC00U + y * 4U + 2U, (unsigned)road_hs(y) & 0x3FFU);
  }
}

int expected_b_color(int hs, unsigned x) { return 1 + mod((mod((int)x - hs, 512)) / 8, 14); }

void test_hscroll_modes_and_road_like_scene() {
  Scene s;
  base_scene(s, 0x81);
  fill_plane_b_columns(s);
  write_hscroll_table(s);
  auto frame = make_frame();
  unsigned bad;

  // Mode 3 (per line): every row uses its own entry; negative offsets and 512-pixel wrapping included.
  s.regs[11] = 0x03;
  check(render(s, frame.get()), "line-scroll road scene renders");
  bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 320U; ++x)
      if (!is_color(*frame, x, y, (unsigned)expected_b_color(road_hs(y), x))) ++bad;
  check(bad == 0, "mode 3: per-line scroll with negative offsets and wrapping matches the derived expectation");

  // Mode 2 (per 8-line cell row): rows share the entry of their group's first line.
  s.regs[11] = 0x02;
  check(render(s, frame.get()), "cell-scroll scene renders (OutRun-class mode, previously rejected)");
  bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 320U; ++x)
      if (!is_color(*frame, x, y, (unsigned)expected_b_color(road_hs(y & ~7U), x))) ++bad;
  check(bad == 0, "mode 2: one scroll entry per 8-line group matches the derived expectation");

  // Mode 0 (full screen): entry 0 for every row.
  s.regs[11] = 0x00;
  check(render(s, frame.get()), "full-screen scroll scene renders");
  bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 320U; ++x)
      if (!is_color(*frame, x, y, (unsigned)expected_b_color(road_hs(0), x))) ++bad;
  check(bad == 0, "mode 0: the first entry applies to every row");

  // Mode 1 is prohibited: fail closed, artifact untouched.
  s.regs[11] = 0x01;
  auto sentinel = make_frame();
  std::memset(sentinel.get(), 0xAB, sizeof(*sentinel));
  auto before = make_frame();
  std::memcpy(before.get(), sentinel.get(), sizeof(*sentinel));
  auto raster = std::make_unique<GenesisRasterFrameState>();
  for (auto &line : raster->lines) {
    std::memcpy(line.registers, s.regs, sizeof(line.registers));
    std::memcpy(line.cram, s.cram, sizeof(line.cram));
    std::memcpy(line.vsram, s.vsram, sizeof(line.vsram));
  }
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), sentinel.get()) != 0 &&
            std::memcmp(sentinel.get(), before.get(), sizeof(*sentinel)) == 0,
        "prohibited scroll mode 01 fails closed and leaves the artifact unmodified");
}

void test_hscroll_extremes() {
  Scene s;
  base_scene(s, 0x81);
  fill_plane_b_columns(s);
  auto frame = make_frame();
  const int values[] = {-512, -1, 0, 1, 8, 511};
  for (int hs : values) {
    put16(s, 0xFC00U + 2U, (unsigned)hs & 0x3FFU);
    check(render(s, frame.get()), "extreme scroll renders");
    unsigned bad = 0;
    for (unsigned x = 0; x < 320U; ++x)
      if (!is_color(*frame, x, 0, (unsigned)expected_b_color(hs, x))) ++bad;
    check(bad == 0, "scroll boundary value matches (-512..511, direction: positive moves the plane right)");
  }
  // Direction check from first principles: hscroll +8 puts plane cell 0 at screen x=8 and cell 63 (color 1+63%14=8) at x=0.
  put16(s, 0xFC00U + 2U, 8);
  check(render(s, frame.get()) && is_color(*frame, 8, 0, 1) && is_color(*frame, 0, 0, 8), "positive hscroll shifts the picture right with wrap-around");
}

void test_vertical_scroll_modes() {
  Scene s;
  base_scene(s, 0x81);
  fill_plane_a_rows(s);
  auto frame = make_frame();
  unsigned bad;

  // Full-plane vertical scroll, negative value (-37) wrapping the 32-cell (256-row) plane.
  put16(s, 0, 0);
  s.vsram[0] = (uint8_t)(((unsigned)-37 & 0x3FFU) >> 8);
  s.vsram[1] = (uint8_t)((unsigned)-37 & 0xFFU);
  s.regs[11] = 0x00;
  check(render(s, frame.get()), "full-plane vertical scroll renders");
  bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 320U; ++x)
      if (!is_color(*frame, x, y, 1U + (unsigned)mod(mod((int)y - 37, 256) / 8, 14))) ++bad;
  check(bad == 0, "mode full-plane VSRAM: negative offset wraps modulo the plane height");

  // Per-2-cell-column vertical scroll: group g (screen x / 16) reads its own VSRAM word for Plane A.
  s.regs[11] = 0x04;
  std::memset(s.vsram, 0, sizeof(s.vsram));
  auto group_vs = [](unsigned g) { return (int)((g * 13U) % 400U) - 150; };
  for (unsigned g = 0; g < 20U; ++g) {
    const unsigned raw = (unsigned)group_vs(g) & 0x3FFU;
    s.vsram[g * 4U] = (uint8_t)(raw >> 8);
    s.vsram[g * 4U + 1U] = (uint8_t)raw;
  }
  check(render(s, frame.get()), "per-column vertical scroll renders");
  bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 320U; ++x)
      if (!is_color(*frame, x, y, 1U + (unsigned)mod(mod((int)y + group_vs(x / 16U), 256) / 8, 14))) ++bad;
  check(bad == 0, "mode 2-cell VSRAM: each 16-pixel column group scrolls independently (mixed with line mode below)");

  // Mixed: per-column vertical scroll together with per-line horizontal scroll on the same plane.
  s.regs[11] = 0x07;
  for (unsigned y = 0; y < 224U; ++y) put16(s, 0xFC00U + y * 4U, (unsigned)((int)(y % 5U) * 8 - 16) & 0x3FFU);
  // Plane A cell (c, r) = tile 1 + (r % 14): horizontal scroll alone changes nothing visible, vertical does.
  check(render(s, frame.get()), "mixed vertical+horizontal modes render");
  bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 320U; ++x)
      if (!is_color(*frame, x, y, 1U + (unsigned)mod(mod((int)y + group_vs(x / 16U), 256) / 8, 14))) ++bad;
  check(bad == 0, "mixed scroll modes compose without interfering");
}

// ---- display-mode geometry -------------------------------------------------------------------------------------
void test_h32_geometry_and_fail_closed_modes() {
  Scene s;
  base_scene(s, 0x00);  // RS0 = RS1 = 0: H32
  fill_plane_b_columns(s);
  auto frame = make_frame();
  check(render(s, frame.get()), "H32 renders");
  check(frame->width == 256U && frame->height == 224U && !genesis_vdp_frame_is_legacy_shape(frame.get()),
        "H32 artifact is 256x224 and not legacy-shaped");
  unsigned bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 256U; ++x)
      if (!is_color(*frame, x, y, (unsigned)expected_b_color(0, x))) ++bad;
  check(bad == 0, "H32 shows exactly the first 32 plane cells (256 pixels)");

  // H40 stays 320x224 and legacy-shaped (digest compatibility).
  base_scene(s, 0x81);
  fill_plane_b_columns(s);
  check(render(s, frame.get()) && frame->width == 320U && frame->height == 224U && genesis_vdp_frame_is_legacy_shape(frame.get()),
        "H40 remains the legacy 320x224 shape");

  // Reserved / unsupported combinations stay fail-closed.
  const unsigned reserved[] = {0x80, 0x01, 0x82 /* H32+LSM0: interlace mode 1 */, 0x84 /* reserved LSM */};
  for (unsigned reg12 : reserved) {
    Scene r;
    base_scene(r, reg12);
    auto f = make_frame();
    check(!render(r, f.get()), "reserved/unsupported display configuration fails closed");
  }
  Scene m4;
  base_scene(m4, 0x81);
  m4.regs[1] = 0x00;  // Mode 4
  auto f4 = make_frame();
  check(!render(m4, f4.get()), "Mode 4 fails closed");
  Scene v30;
  base_scene(v30, 0x81);
  v30.regs[1] = 0x0C;  // 30-cell height: not representable on the 224-line NTSC schedule
  auto f30 = make_frame();
  check(!render(v30, f30.get()), "30-cell (240-line) display fails closed");
}

// Sprite attribute helper: raw Y/X include the +128 bias (+256 for Y in interlace mode 2).
void put_sprite(Scene &s, uint32_t sat, unsigned index, unsigned raw_y, unsigned width_cells, unsigned height_cells,
                unsigned link, unsigned tile, unsigned raw_x, unsigned pal = 0, unsigned prio = 0, unsigned hf = 0,
                unsigned vf = 0) {
  const uint32_t a = sat + index * 8U;
  put16(s, a, raw_y);
  s.vram[a + 2U] = (uint8_t)(((width_cells - 1U) << 2) | (height_cells - 1U));
  s.vram[a + 3U] = (uint8_t)link;
  put16(s, a + 4U, nt_word(tile, pal, prio, hf, vf));
  put16(s, a + 6U, raw_x);
}

void test_h32_sprite_table_and_limits() {
  Scene s;
  base_scene(s, 0x00);  // H32
  s.regs[5] = 0x7D;     // H32 SAT base keeps bit 0: 0x7D << 9 = 0xFA00 (512-byte granularity); H40 would use 0xF800
  solid_tile(s, 1, 2, 32U);
  // 17 one-cell sprites on screen row 50, 8 px apart, linked in order. H32 draws 16 per line.
  for (unsigned i = 0; i < 17U; ++i)
    put_sprite(s, 0xFA00U, i, 128U + 50U, 1, 1, i == 16U ? 0U : i + 1U, 1, 128U + i * 8U);
  auto frame = make_frame();
  check(render(s, frame.get()), "H32 sprite scene renders");
  bool first16 = true;
  for (unsigned i = 0; i < 16U; ++i) first16 = first16 && is_color(*frame, i * 8U + 3U, 50, 2);
  check(first16, "H32 draws the first 16 sprites on a line");
  check(!is_color(*frame, 16U * 8U + 3U, 50, 2), "H32 drops the 17th sprite on a line (16-sprite limit)");

  // The SAT base differs per mode: in H40 the same register selects 0xF800 (empty table) so nothing is drawn.
  s.regs[12] = 0x81;
  check(render(s, frame.get()) && !is_color(*frame, 3, 50, 2), "H40 reads its SAT at the 1 KiB-aligned base, not the H32 one");

  // 32-cell budget: nine 4-cell sprites on one line draw eight fully; H40's budget is 40 cells (ten).
  Scene b;
  base_scene(b, 0x00);
  b.regs[5] = 0x7C;
  solid_tile(b, 1, 3, 32U);
  for (unsigned t = 1; t <= 16U; ++t) solid_tile(b, t, 3, 32U);
  for (unsigned i = 0; i < 9U; ++i) put_sprite(b, 0xF800U, i, 128U + 20U, 4, 1, i == 8U ? 0U : i + 1U, 1, 128U + i * 32U);
  check(render(b, frame.get()), "H32 cell-budget scene renders");
  bool eight = true;
  for (unsigned i = 0; i < 8U; ++i) eight = eight && is_color(*frame, i * 32U + 16U, 20, 3) && (i * 32U + 16U < 256U);
  check(eight, "H32 draws 32 sprite cells (eight 4-cell sprites) per line");
  // Sprite 8 would start at x=256 (outside H32 anyway); use a dedicated overflow check with partially visible cells.
  Scene c;
  base_scene(c, 0x00);
  c.regs[5] = 0x7C;
  for (unsigned t = 1; t <= 16U; ++t) solid_tile(c, t, 4, 32U);
  // Eight 4-cell sprites stacked at x=0 (all overlapping) consume 32 cells; the ninth (a different color at x=0) is out of budget.
  for (unsigned t = 20; t <= 24U; ++t) solid_tile(c, t, 5, 32U);
  for (unsigned i = 0; i < 8U; ++i) put_sprite(c, 0xF800U, i, 128U + 10U, 4, 1, i + 1U, 1, 128U);
  put_sprite(c, 0xF800U, 8, 128U + 10U, 4, 1, 0, 20, 128U + 40U);
  check(render(c, frame.get()) && !is_color(*frame, 44, 10, 5), "H32 cell budget (32 cells) exhausted by earlier sprites hides the next sprite");
  c.regs[12] = 0x81;
  check(render(c, frame.get()) && is_color(*frame, 44, 10, 5), "H40 budget (40 cells) still draws the ninth sprite");

  // Sprite clipping at the right edge of H32: a sprite at x=252 shows 4 pixels; one at x=260 is invisible in H32.
  Scene e;
  base_scene(e, 0x00);
  solid_tile(e, 1, 6, 32U);
  put_sprite(e, 0xF800U, 0, 128U + 30U, 1, 1, 1, 1, 128U + 252U);
  put_sprite(e, 0xF800U, 1, 128U + 30U, 1, 1, 0, 1, 128U + 260U);
  check(render(e, frame.get()) && is_color(*frame, 252, 30, 6) && is_color(*frame, 255, 30, 6) && frame->width == 256U,
        "H32 sprite at the right edge is clipped to the 256-pixel display");
  e.regs[12] = 0x81;
  check(render(e, frame.get()) && is_color(*frame, 260, 30, 6), "the same sprite is visible once H40 widens the display");

  // Link beyond the 64-entry H32 table terminates the list.
  Scene l;
  base_scene(l, 0x00);
  solid_tile(l, 1, 7, 32U);
  put_sprite(l, 0xF800U, 0, 128U + 5U, 1, 1, 64, 1, 128U);
  put_sprite(l, 0xF800U, 1, 128U + 5U, 1, 1, 0, 1, 128U + 16U);  // would be reached only by a (non-existent) index 64 link
  check(render(l, frame.get()) && is_color(*frame, 3, 5, 7) && !is_color(*frame, 19, 5, 7),
        "a link >= 64 ends the H32 sprite list");
}

// ---- interlace mode 2 ------------------------------------------------------------------------------------------
void test_interlace_mode2() {
  Scene s;
  base_scene(s, 0x87);  // H40 + LSM1:LSM0 = 11
  // Plane A cell row r: tile 1 + r, an 8x16 pattern whose top eight rows are color (r % 7) + 1 and bottom rows (r % 7) + 8.
  for (unsigned r = 0; r < 32U; ++r) split_tile_im2(s, 1U + r, 1U + (r % 7U), 8U + (r % 7U));
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(s, 0xC000U + (r * 64U + c) * 2U, nt_word(1U + r));
  auto frame = make_frame();
  check(render(s, frame.get()), "interlace mode 2 renders");
  check(frame->width == 320U && frame->height == 448U, "interlace mode 2 artifact is 320x448");
  // 8x16 pattern addressing: tile n lives at n * 64 bytes. Cell row r covers output rows 16r..16r+15.
  unsigned bad = 0;
  for (unsigned y = 0; y < 448U; ++y) {
    const unsigned cell_row = y / 16U;
    const unsigned color = ((y % 16U) < 8U) ? 1U + (cell_row % 7U) : 8U + (cell_row % 7U);
    for (unsigned x = 0; x < 320U; x += 7U)
      if (!is_color(*frame, x, y, color)) ++bad;
  }
  check(bad == 0, "8x16 patterns: cell row = output row / 16, tile address = tile * 64, both halves distinct");

  // Vertical flip flips the whole 16-row cell.
  for (unsigned c = 0; c < 64U; ++c) put16(s, 0xC000U + c * 2U, nt_word(1U, 0, 0, 0, 1));
  check(render(s, frame.get()) && is_color(*frame, 0, 0, 8U) && is_color(*frame, 0, 8, 1U) && is_color(*frame, 0, 15, 1U),
        "interlace mode 2 vertical flip mirrors all 16 pattern rows");
  for (unsigned c = 0; c < 64U; ++c) put16(s, 0xC000U + c * 2U, nt_word(1U));

  // 11-bit vertical scroll: 0x7FF is -1 and wraps the 32-cell (512-row) plane; +21 shifts by 21 output rows.
  s.vsram[0] = 0x07;
  s.vsram[1] = 0xFF;
  check(render(s, frame.get()), "interlace mode 2 vertical scroll renders");
  bad = 0;
  for (unsigned y = 0; y < 448U; ++y) {
    const int py = mod((int)y - 1, 512);
    const unsigned cell_row = (unsigned)py / 16U;
    const unsigned color = ((unsigned)py % 16U < 8U) ? 1U + (cell_row % 7U) : 8U + (cell_row % 7U);
    if (!is_color(*frame, 5, y, color)) ++bad;
  }
  check(bad == 0, "interlace mode 2: 11-bit VSRAM value 0x7FF scrolls by -1 output row (wrapping 512 rows)");
  s.vsram[0] = 0;
  s.vsram[1] = 21;
  check(render(s, frame.get()), "interlace mode 2 vertical scroll +21 renders");
  bad = 0;
  for (unsigned y = 0; y < 448U; ++y) {
    const unsigned py = (y + 21U) % 512U;
    const unsigned cell_row = py / 16U;
    const unsigned color = (py % 16U < 8U) ? 1U + (cell_row % 7U) : 8U + (cell_row % 7U);
    if (!is_color(*frame, 5, y, color)) ++bad;
  }
  check(bad == 0, "interlace mode 2: vertical scroll is measured in output rows");
  s.vsram[1] = 0;

  // Horizontal scroll tables are per FIELD line: output rows 2L and 2L+1 share entry L (field parity does not matter).
  fill_plane_b_columns(s);  // Plane B columns use 8x8-colored tiles: re-tile as 8x16 solids
  for (unsigned t = 1; t <= 14U; ++t) solid_tile(s, t, t, 64U);
  std::memset(&s.vram[0], 0, 64U);  // tile 0 (8x16, 64 bytes) must be transparent: the 8x8 fills above overlap it
  for (unsigned c = 0; c < 64U; ++c)
    for (unsigned r = 0; r < 32U; ++r) put16(s, 0xC000U + (r * 64U + c) * 2U, 0);  // Plane A transparent
  s.regs[11] = 0x03;
  for (unsigned line = 0; line < 224U; ++line) put16(s, 0xFC00U + line * 4U + 2U, (line * 3U) % 200U);
  check(render(s, frame.get()), "interlace mode 2 line scroll renders");
  bad = 0;
  for (unsigned y = 0; y < 448U; ++y)
    for (unsigned x = 0; x < 320U; x += 5U)
      if (!is_color(*frame, x, y, (unsigned)expected_b_color((int)(((y / 2U) * 3U) % 200U), x))) ++bad;
  if (bad != 0) {
    for (unsigned y = 0; y < 448U && bad != 0; ++y)
      for (unsigned x = 0; x < 320U; x += 5U)
        if (!is_color(*frame, x, y, (unsigned)expected_b_color((int)(((y / 2U) * 3U) % 200U), x))) {
          std::printf("  first mismatch x=%u y=%u got=%u,%u,%u want_idx=%d\n", x, y, pixel(*frame, x, y).r, pixel(*frame, x, y).g, pixel(*frame, x, y).b, expected_b_color((int)(((y / 2U) * 3U) % 200U), x));
          bad = 0;
          break;
        }
    bad = 1;
  }
  check(bad == 0, "interlace mode 2: horizontal scroll rows index by field line (output row / 2)");

  // Sprites: Y has 10 bits and a +256 bias; a sprite cell is 16 rows; column-major tile order uses 8x16 patterns.
  Scene t;
  base_scene(t, 0x87);
  solid_tile(t, 1, 3, 64U);
  solid_tile(t, 2, 4, 64U);
  put_sprite(t, 0xF800U, 0, 256U + 100U, 1, 2, 0, 1, 128U + 40U);
  check(render(t, frame.get()), "interlace mode 2 sprite renders");
  check(is_color(*frame, 44, 100, 3) && is_color(*frame, 44, 115, 3) && is_color(*frame, 44, 116, 4) && is_color(*frame, 44, 131, 4) &&
            !is_color(*frame, 44, 99, 3) && !is_color(*frame, 44, 132, 4),
        "interlace mode 2 sprite: Y bias 256, two 16-row cells (tile T then T+1), 32 rows tall");
  put_sprite(t, 0xF800U, 0, 256U + 100U, 1, 2, 0, 1, 128U + 40U, 0, 0, 0, 1);  // vertical flip of the whole 32-row sprite
  check(render(t, frame.get()) && is_color(*frame, 44, 100, 4) && is_color(*frame, 44, 131, 3),
        "interlace mode 2 sprite vertical flip mirrors the whole bounding box");
  // A tall (10-bit) Y beyond the 9-bit range: raw Y = 256 + 400 = 656 is row 400.
  put_sprite(t, 0xF800U, 0, 256U + 400U, 1, 1, 0, 1, 128U + 40U);
  check(render(t, frame.get()) && is_color(*frame, 44, 400, 3) && is_color(*frame, 44, 415, 3) && !is_color(*frame, 44, 416, 3),
        "interlace mode 2 sprite Y uses 10 bits (row 400 reachable)");

  // Normal (non-interlaced) rendering of the same register file minus LSM is unchanged: 8x8 cells.
  Scene n;
  base_scene(n, 0x81);
  for (unsigned r = 0; r < 32U; ++r) solid_tile(n, 1U + r, 1U + (r % 14U), 32U);
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(n, 0xC000U + (r * 64U + c) * 2U, nt_word(1U + r));
  check(render(n, frame.get()) && frame->height == 224U && is_color(*frame, 3, 7, 1) && is_color(*frame, 3, 8, 2) &&
            is_color(*frame, 3, 223, 1U + (27U % 14U)),
        "non-interlaced frames keep 8x8 cells and 224 rows");
}

// ---- window plane ----------------------------------------------------------------------------------------------
// Plane A: solid color 1 everywhere. Window nametable: window cell (c, r) uses tile 2 + ((r * 3 + c) % 10) (color 2..11).
void window_scene(Scene &s, unsigned reg12) {
  base_scene(s, reg12);
  solid_tile(s, 1, 1, 32U);
  for (unsigned t = 2; t <= 11U; ++t) solid_tile(s, t, t, 32U);
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(s, 0xC000U + (r * 64U + c) * 2U, nt_word(1));
  const bool h40 = (reg12 & 1U) != 0U;
  const unsigned stride = h40 ? 64U : 32U;
  const uint32_t base = h40 ? 0xD000U : 0xD000U;
  for (unsigned r = 0; r < 28U; ++r)
    for (unsigned c = 0; c < stride; ++c) put16(s, base + (r * stride + c) * 2U, nt_word(2U + ((r * 3U + c) % 10U)));
}
unsigned window_color(unsigned x, unsigned y) { return 2U + (((y / 8U) * 3U + (x / 8U)) % 10U); }

bool window_matches(const GenesisFrameArtifact &f, unsigned width, unsigned height, bool (*in_window)(unsigned, unsigned)) {
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      const unsigned want = in_window(x, y) ? window_color(x, y) : 1U;
      if (!is_color(f, x, y, want)) return false;
    }
  return true;
}

void test_window_plane() {
  auto frame = make_frame();
  Scene s;
  // (a) window left of 5 * 16 = 80 px, vertical range empty.
  window_scene(s, 0x81);
  s.regs[17] = 0x05;
  s.regs[18] = 0x00;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned x, unsigned) { return x < 80U; }),
        "window: horizontal boundary 5 (x < 80), window on the left");
  // (b) window right of 80 px.
  s.regs[17] = 0x85;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned x, unsigned) { return x >= 80U; }),
        "window: horizontal boundary 5, window on the right");
  // (c) vertical: window above row 24 (3 cells) OR right of 32 px.
  s.regs[17] = 0x82;
  s.regs[18] = 0x03;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned x, unsigned y) { return y < 24U || x >= 32U; }),
        "window: vertical range (above 3 cells) unions with the horizontal range");
  // (d) window below row 24 (reg18 bit 7), horizontal left of 16 px.
  s.regs[17] = 0x01;
  s.regs[18] = 0x83;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned x, unsigned y) { return y >= 24U || x < 16U; }),
        "window: vertical range below row 24 unions with the horizontal range");
  // (e) degenerate boundaries: left of 496 px (> 320) is the whole screen; right of it is empty; boundary 0 mirrors.
  s.regs[18] = 0;
  s.regs[17] = 0x1F;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned, unsigned) { return true; }), "window: boundary beyond the screen, left side = full screen");
  s.regs[17] = 0x9F;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned, unsigned) { return false; }), "window: boundary beyond the screen, right side = empty");
  s.regs[17] = 0x00;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned, unsigned) { return false; }), "window: boundary 0, left side = empty");
  s.regs[17] = 0x80;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned, unsigned) { return true; }), "window: boundary 0, right side = full screen");

  // (f) the window does not scroll: scroll Plane A a lot, the window must not move; Plane A (color 1) would not show it anyway.
  s.regs[17] = 0x05;
  s.regs[11] = 0x07;
  for (unsigned y = 0; y < 224U; ++y) put16(s, 0xFC00U + y * 4U, 13U + y);
  std::memset(s.vsram, 0, sizeof(s.vsram));
  s.vsram[0] = 0x01; s.vsram[1] = 0x23;
  check(render(s, frame.get()) && window_matches(*frame, 320, 224, [](unsigned x, unsigned) { return x < 80U; }),
        "window ignores horizontal and vertical scroll");
  s.regs[11] = 0;

  // (g) H40 ignores register #3 bit 1 (4 KiB alignment); H32 keeps it (2 KiB alignment).
  window_scene(s, 0x81);
  s.regs[17] = 0x80;  // whole screen
  s.regs[3] = 0x36;   // 0x36 << 10 = 0xD800; H40 masks to 0xD000
  check(render(s, frame.get()) && is_color(*frame, 0, 0, window_color(0, 0)) && is_color(*frame, 100, 100, window_color(100, 100)),
        "H40 window base ignores register #3 bit 1");
  Scene h32;
  window_scene(h32, 0x00);
  h32.regs[17] = 0x80;
  check(render(h32, frame.get()) && frame->width == 256U && is_color(*frame, 200, 40, window_color(200, 40)) && is_color(*frame, 255, 223, window_color(255, 223)),
        "H32 window uses a 32-cell row stride");
  // H32 base keeps bit 1: with 0x36 the table would be at 0xD800 (unpopulated): the window then reads backdrop-transparent cells.
  h32.regs[3] = 0x36;
  check(render(h32, frame.get()) && is_color(*frame, 0, 0, 0U), "H32 window base honors register #3 bit 1 (the unpopulated table shows the backdrop)");

  // (h) priority: window pixels compete as Plane A (window priority bit, sprites, Plane B).
  Scene p;
  window_scene(p, 0x81);
  p.regs[17] = 0x80;
  for (unsigned t = 12; t <= 13U; ++t) solid_tile(p, t, t, 32U);
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(p, 0xE000U + (r * 64U + c) * 2U, nt_word(12));  // Plane B color 12 everywhere
  check(render(p, frame.get()) && is_color(*frame, 4, 4, window_color(4, 4)), "low-priority window pixel beats low-priority Plane B (A/window over B)");
  for (unsigned r = 0; r < 28U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(p, 0xD000U + (r * 64U + c) * 2U, nt_word(2U + ((r * 3U + c) % 10U), 0, 0));
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(p, 0xE000U + (r * 64U + c) * 2U, nt_word(12, 0, 1));  // Plane B high priority
  check(render(p, frame.get()) && is_color(*frame, 4, 4, 12U), "high-priority Plane B beats a low-priority window");
  for (unsigned r = 0; r < 28U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(p, 0xD000U + (r * 64U + c) * 2U, nt_word(2U + ((r * 3U + c) % 10U), 0, 1));
  check(render(p, frame.get()) && is_color(*frame, 4, 4, window_color(4, 4)), "high-priority window beats high-priority Plane B");
  put_sprite(p, 0xF800U, 0, 128U, 1, 1, 0, 13, 128U, 0, 0);  // low-priority sprite over a high-priority window: window wins
  check(render(p, frame.get()) && is_color(*frame, 2, 2, window_color(2, 2)), "high-priority window beats a low-priority sprite");
  put_sprite(p, 0xF800U, 0, 128U, 1, 1, 0, 13, 128U, 0, 1);  // high-priority sprite beats the window
  check(render(p, frame.get()) && is_color(*frame, 2, 2, 13U), "a high-priority sprite beats the window");

  // (i) interlace mode 2: vertical boundary units are 16 output rows.
  Scene q;
  base_scene(q, 0x87);
  for (unsigned t = 1; t <= 3U; ++t) solid_tile(q, t, t, 64U);
  for (unsigned r = 0; r < 32U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(q, 0xC000U + (r * 64U + c) * 2U, nt_word(1));
  for (unsigned r = 0; r < 28U; ++r)
    for (unsigned c = 0; c < 64U; ++c) put16(q, 0xD000U + (r * 64U + c) * 2U, nt_word(2));
  q.regs[17] = 0x00;
  q.regs[18] = 0x03;  // window above 3 cells = 48 output rows
  check(render(q, frame.get()) && is_color(*frame, 10, 47, 2U) && is_color(*frame, 10, 48, 1U) && frame->height == 448U,
        "interlace mode 2 window: vertical boundary is measured in 16-row cells");

  // (j) left-column mask (register #0 bit 5): the left 8 pixels show the backdrop.
  Scene m;
  window_scene(m, 0x81);
  m.regs[0] = 0x20;
  m.regs[7] = 0x05;  // backdrop color index 5
  check(render(m, frame.get()) && is_color(*frame, 0, 0, 5U) && is_color(*frame, 7, 100, 5U) && is_color(*frame, 8, 0, 1U),
        "register #0 bit 5 masks the leftmost 8 pixels to the backdrop color");
}

// ---- scanline-aware palette ------------------------------------------------------------------------------------
void test_scanline_palette_switch() {
  Scene s;
  base_scene(s, 0x81);
  s.regs[7] = 0x01;  // backdrop color index 1 everywhere (empty planes): the visible color is exactly CRAM[1] of the line
  auto raster = std::make_unique<GenesisRasterFrameState>();
  const unsigned switch_line = 100;
  for (unsigned l = 0; l < 224U; ++l) {
    std::memcpy(raster->lines[l].registers, s.regs, sizeof(s.regs));
    std::memcpy(raster->lines[l].vsram, s.vsram, sizeof(s.vsram));
    fill_cram(raster->lines[l].cram, l < switch_line ? 0 : 1);
  }
  auto frame = make_frame();
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), frame.get()) == 0, "two-palette frame renders");
  check(frame->palette_count == 2U && !genesis_vdp_frame_is_legacy_shape(frame.get()), "a mid-frame palette change yields two palette slots");
  check(is_color(*frame, 10, 0, 1U, 0) && is_color(*frame, 10, switch_line - 1U, 1U, 0), "rows above the transition use the first palette (underwater-style split: above)");
  check(is_color(*frame, 10, switch_line, 1U, 1) && is_color(*frame, 10, 223, 1U, 1), "rows from the transition on use the second palette (below)");
  check(!is_color(*frame, 10, switch_line, 1U, 0) && !is_color(*frame, 10, 0, 1U, 1), "the two palettes are distinguishable at the transition");

  // Scanline boundaries: a change at line 0 (whole frame), at the last line (one row) and the digest consequences.
  for (unsigned l = 0; l < 224U; ++l) fill_cram(raster->lines[l].cram, l >= 223U ? 1 : 0);
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), frame.get()) == 0 && is_color(*frame, 0, 222, 1U, 0) &&
            is_color(*frame, 0, 223, 1U, 1) && frame->palette_count == 2U,
        "a palette change on the final active line affects exactly that row");
  for (unsigned l = 0; l < 224U; ++l) fill_cram(raster->lines[l].cram, 1);
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), frame.get()) == 0 && is_color(*frame, 0, 0, 1U, 1) &&
            frame->palette_count == 1U && genesis_vdp_frame_is_legacy_shape(frame.get()),
        "a frame-start palette change (all lines identical) is an ordinary single-palette frame");
  // The legacy digest rule: uniform frames hash exactly pixels||palette_snapshot (SHA-256 recomputed independently below).
  GenesisSha256 sha;
  uint8_t expect[32];
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, frame->pixels, 320U * 224U);
  genesis_sha256_update(&sha, frame->palette_snapshot, 128U);
  genesis_sha256_final(&sha, expect);
  check(std::memcmp(expect, frame->frame_digest, 32U) == 0, "legacy-shaped frames keep the pre-existing digest rule");
  // Distinct digests for distinct raster states with identical pixel indices.
  auto frame2 = make_frame();
  for (unsigned l = 0; l < 224U; ++l) fill_cram(raster->lines[l].cram, l < 50U ? 0 : 1);
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), frame2.get()) == 0 &&
            std::memcmp(frame2->frame_digest, frame->frame_digest, 32U) != 0 && frame2->palette_count == 2U,
        "the digest covers per-row palettes (same indices, different raster palette => different digest)");

  // Per-line register changes: a scroll register change between lines affects only later rows (VSRAM/registers captured per line).
  Scene t;
  base_scene(t, 0x81);
  fill_plane_a_rows(t);
  for (unsigned l = 0; l < 224U; ++l) {
    std::memcpy(raster->lines[l].registers, t.regs, sizeof(t.regs));
    std::memcpy(raster->lines[l].cram, t.cram, sizeof(t.cram));
    std::memset(raster->lines[l].vsram, 0, sizeof(raster->lines[l].vsram));
    if (l >= 112U) raster->lines[l].vsram[1] = 16;  // Plane A full-plane vscroll = +16 from line 112 (second "screen")
  }
  check(genesis_vdp_produce_frame_raster(t.vram.data(), raster.get(), frame.get()) == 0, "per-line VSRAM frame renders");
  check(is_color(*frame, 0, 0, 1U) && is_color(*frame, 0, 111, 1U + (111U / 8U) % 14U) &&
            is_color(*frame, 0, 112, 1U + ((112U + 16U) / 8U) % 14U) && is_color(*frame, 0, 223, 1U + ((223U + 16U) / 8U) % 14U),
        "VSRAM changes between lines take effect on later rows only (split-screen scroll)");

  // Mixed scroll modes across lines (register #11 changes mid-frame): lines < 60 mode 3, the rest mode 2.
  Scene u;
  base_scene(u, 0x81);
  fill_plane_b_columns(u);
  write_hscroll_table(u);
  for (unsigned l = 0; l < 224U; ++l) {
    std::memcpy(raster->lines[l].registers, u.regs, sizeof(u.regs));
    raster->lines[l].registers[11] = l < 60U ? 0x03 : 0x02;
    std::memcpy(raster->lines[l].cram, u.cram, sizeof(u.cram));
    std::memset(raster->lines[l].vsram, 0, sizeof(raster->lines[l].vsram));
  }
  check(genesis_vdp_produce_frame_raster(u.vram.data(), raster.get(), frame.get()) == 0, "mixed-mode raster frame renders");
  unsigned bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    for (unsigned x = 0; x < 320U; x += 3U)
      if (!is_color(*frame, x, y, (unsigned)expected_b_color(road_hs(y < 60U ? y : (y & ~7U)), x))) ++bad;
  check(bad == 0, "mixed horizontal-scroll modes within one frame use each line's own register state");

  // A display-mode change inside the frame cannot be one rectangular image: fail closed, nothing published.
  for (unsigned l = 0; l < 224U; ++l) raster->lines[l].registers[12] = l < 100U ? 0x81 : 0x00;
  auto untouched = make_frame();
  std::memset(untouched.get(), 0x5A, sizeof(*untouched));
  auto copy = make_frame();
  std::memcpy(copy.get(), untouched.get(), sizeof(*untouched));
  check(genesis_vdp_produce_frame_raster(u.vram.data(), raster.get(), untouched.get()) != 0 &&
            std::memcmp(untouched.get(), copy.get(), sizeof(*untouched)) == 0,
        "an H32/H40 switch inside the active display fails closed without touching the artifact");
}

// ---- headless export agrees with the shared RGB conversion (the SDL viewer uses the same function) ------------------
void test_export_and_presentation_agree() {
  Scene s;
  base_scene(s, 0x00);  // H32
  s.regs[7] = 0x02;
  auto raster = std::make_unique<GenesisRasterFrameState>();
  for (unsigned l = 0; l < 224U; ++l) {
    std::memcpy(raster->lines[l].registers, s.regs, sizeof(s.regs));
    std::memcpy(raster->lines[l].vsram, s.vsram, sizeof(s.vsram));
    fill_cram(raster->lines[l].cram, l < 80U ? 0 : 1);
  }
  auto frame = make_frame();
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), frame.get()) == 0, "export fixture renders");
  FILE *tmp = std::tmpfile();
  check(tmp != nullptr, "tmpfile");
  if (tmp == nullptr) return;
  check(genesis_frame_export_ppm_to_stream(frame.get(), tmp) == GENESIS_FRAME_EXPORT_STATUS_OK, "PPM export of an H32 two-palette frame");
  std::rewind(tmp);
  unsigned w = 0, h = 0, maxv = 0;
  check(std::fscanf(tmp, "P6 %u %u %u", &w, &h, &maxv) == 3 && w == 256U && h == 224U && maxv == 255U, "PPM header carries the artifact geometry");
  (void)std::fgetc(tmp);  // single whitespace after maxval
  std::vector<uint8_t> body((size_t)w * h * 3U);
  check(std::fread(body.data(), 1, body.size(), tmp) == body.size(), "PPM body length is width*height*3");
  std::fclose(tmp);
  unsigned bad = 0;
  for (unsigned row = 0; row < 224U; ++row) {
    uint8_t rgb[GENESIS_FRAME_MAX_WIDTH * 3U];
    if (genesis_vdp_frame_row_rgb888(frame.get(), row, rgb) != 0 || std::memcmp(rgb, &body[(size_t)row * 256U * 3U], 256U * 3U) != 0) ++bad;
  }
  check(bad == 0, "the headless PPM and the shared presentation conversion (used by the SDL viewer) are byte-identical for every row");
  check(is_color(*frame, 5, 0, 2U, 0) && is_color(*frame, 5, 200, 2U, 1), "export reflects the per-row palette");
}

// ---- runtime: scanline capture, H-interrupt, STOP -------------------------------------------------------------------
constexpr uint32_t kRamBegin = 0x00FF0000u;

struct Observed { GenesisFrameArtifact frame; GenesisLiveFrameObserver observer; GenesisRasterCapture capture; };
void attach_raster(Observed &o, GenesisRuntime &r) {
  std::memset(&o, 0, sizeof(o));
  o.observer.producer = genesis_vdp_produce_frame;
  o.observer.latest = &o.frame;
  o.capture.producer = genesis_vdp_produce_frame_raster;
  r.live_frame_observer = &o.observer;
  r.live_raster = &o.capture;
}

void setup_runtime(GenesisRuntime &r) {
  r.pc = 0x200u;
  r.sr = 0x2000u;
  r.a[7] = kRamBegin + 0x8000u;
  Scene s;
  base_scene(s, 0x81);
  s.regs[7] = 0x01;
  std::memcpy(r.devices.vdp.registers, s.regs, sizeof(s.regs));
  std::memcpy(r.devices.vdp.cram, s.cram, sizeof(s.cram));
}

void test_runtime_captures_scanline_palette_and_virtual_frames() {
  auto r = std::make_unique<GenesisRuntime>();
  std::memset(r.get(), 0, sizeof(*r));
  auto o = std::make_unique<Observed>();
  setup_runtime(*r);
  attach_raster(*o, *r);
  // The "guest" writes CRAM[1] on a scanline boundary (as an H-interrupt handler would): a write made after line L began
  // is first visible on line L + 1.
  bool changed = false;
  uint32_t pc = 0x200u;
  while (r->virtual_frame_count == 0U) {
    const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc);
    pc = t.next_pc;
    if (!changed && r->scheduler.master_ticks >= 100ull * GENESIS_NTSC_MASTER_TICKS_PER_LINE) {
      fill_cram(r->devices.vdp.cram, 1);
      changed = true;
    }
  }
  check(o->observer.sequence == 1u && r->virtual_frame_count == 1u, "the first frame boundary publishes one frame and counts one virtual frame");
  check(o->frame.palette_count == 2U, "the mid-frame CRAM write produced a second palette");
  check(is_color(o->frame, 0, 0, 1U, 0) && is_color(o->frame, 0, 100, 1U, 0), "rows up to the line during which the write happened keep the old palette");
  check(is_color(o->frame, 0, 101, 1U, 1) && is_color(o->frame, 0, 223, 1U, 1), "later rows use the new palette");

  // Next frame: the new CRAM is in force from line 0 on => an ordinary single-palette frame.
  const uint64_t seq = o->observer.sequence;
  while (r->virtual_frame_count == 1U) {
    const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc);
    pc = t.next_pc;
  }
  check(o->observer.sequence == seq + 1U && o->frame.palette_count == 1U && is_color(o->frame, 0, 0, 1U, 1),
        "the following frame starts with the switched palette on line 0");

  // A rejected render (reserved display mode) publishes nothing but the virtual frame counter still advances.
  r->devices.vdp.registers[12] = 0x80;
  const uint64_t vf = r->virtual_frame_count;
  const uint64_t seq2 = o->observer.sequence;
  while (r->virtual_frame_count == vf) {
    const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc);
    pc = t.next_pc;
  }
  check(o->observer.sequence == seq2 && o->capture.render_rejections >= 1U && r->virtual_frame_count == vf + 1U,
        "an unsupported frame is rejected (not published) while virtual frame time keeps advancing");

  // Mode switch between frames is clean: H32 next frame renders with its own geometry; a switch mid-frame is rejected.
  r->devices.vdp.registers[12] = 0x00;
  const uint64_t vf2 = r->virtual_frame_count;
  while (r->virtual_frame_count == vf2) {
    const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc);
    pc = t.next_pc;
  }
  check(o->frame.width == 256U || o->observer.sequence == seq2 + 1U, "the first frame after the switch is either H32 or (mid-frame switch) rejected");
  const uint64_t vf3 = r->virtual_frame_count;
  const uint64_t seq3 = o->observer.sequence;
  while (r->virtual_frame_count == vf3) {
    const GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc);
    pc = t.next_pc;
  }
  check(o->observer.sequence == seq3 + 1U && o->frame.width == 256U && o->frame.height == 224U, "a stable H32 frame is published with 256x224 geometry");
}


// ---- VRAM changes during the active display (split-screen sprite tables) --------------------------------------------
void vdp_control(GenesisRuntime &r, unsigned word) {
  uint32_t v = word;
  GenesisRuntimeStop stop{};
  check(genesis_route_access(&r, 0xC00004, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK, "VDP control word accepted");
}
void vdp_data(GenesisRuntime &r, unsigned word) {
  uint32_t v = word;
  GenesisRuntimeStop stop{};
  check(genesis_route_access(&r, 0xC00000, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK, "VDP data word accepted");
}
// VRAM write command for `address` (CD1:CD0 = 01): first word 0x4000 | A13..A0, second word A15..A14.
void vdp_vram_address(GenesisRuntime &r, unsigned address) {
  vdp_control(r, 0x4000U | (address & 0x3FFFU));
  vdp_control(r, (address >> 14) & 3U);
}
void put_sprite_words(GenesisRuntime &r, unsigned sat, unsigned raw_y, unsigned tile, unsigned raw_x) {
  vdp_vram_address(r, sat);
  vdp_data(r, raw_y);
  vdp_data(r, 0x0000);  // 1x1 cell, link 0
  vdp_data(r, nt_word(tile));
  vdp_data(r, raw_x);
}

void test_vram_written_during_active_display_is_scanline_resolved() {
  auto r = std::make_unique<GenesisRuntime>();
  std::memset(r.get(), 0, sizeof(*r));
  auto o = std::make_unique<Observed>();
  Scene s;
  base_scene(s, 0x81);
  s.regs[7] = 0;
  r->pc = 0x200u;
  r->sr = 0x2000u;
  r->a[7] = 0x00FF8000u;
  std::memcpy(r->devices.vdp.registers, s.regs, sizeof(s.regs));
  std::memcpy(r->devices.vdp.cram, s.cram, sizeof(s.cram));
  r->devices.vdp.auto_increment_value = 2;
  // Sprite A (tile 1, color 2) at screen (8, 10) in the table at 0xF800; sprite B's tile 2 is color 3.
  Scene t;
  solid_tile(t, 1, 2, 32U);
  solid_tile(t, 2, 3, 32U);
  std::memcpy(r->devices.vdp.vram, t.vram.data(), 0x100);
  {
    const unsigned a = 0xF800U;
    r->devices.vdp.vram[a] = (uint8_t)((128U + 10U) >> 8);
    r->devices.vdp.vram[a + 1U] = (uint8_t)(128U + 10U);
    r->devices.vdp.vram[a + 4U] = (uint8_t)(nt_word(1) >> 8);
    r->devices.vdp.vram[a + 5U] = (uint8_t)nt_word(1);
    r->devices.vdp.vram[a + 6U] = (uint8_t)((128U + 8U) >> 8);
    r->devices.vdp.vram[a + 7U] = (uint8_t)(128U + 8U);
  }
  attach_raster(*o, *r);
  uint32_t pc = 0x200u;
  bool swapped = false;
  while (r->virtual_frame_count == 0U) {
    pc = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc).next_pc;
    if (!swapped && r->scheduler.master_ticks >= 100ull * GENESIS_NTSC_MASTER_TICKS_PER_LINE) {
      put_sprite_words(*r, 0xF800U, 128U + 150U, 2, 128U + 8U);  // the lower screen's sprite table, written by a handler
      swapped = true;
    }
  }
  check(swapped && o->observer.sequence == 1u && o->capture.frame.has_vram_journal == 0u,
        "the frame published; its journal is closed again for VBlank");
  check(is_color(o->frame, 9, 12, 2U) && is_color(o->frame, 9, 16, 2U), "upper screen shows the sprite table that was in VRAM when its lines were drawn");
  check(!is_color(o->frame, 9, 152, 2U) && is_color(o->frame, 9, 152, 3U) && is_color(o->frame, 9, 156, 3U),
        "lower screen shows the table written mid-frame (split-screen sprite tables)");
  check(!is_color(o->frame, 9, 12, 3U), "the table written mid-frame does not leak into the lines already drawn");
  check(o->capture.vram_journal_overflows == 0u, "no journal overflow for a table-sized write");

  // A journal that overflows is abandoned for that frame (documented fallback: the frame-end VRAM image for every line).
  // Sprite C (above the write line, so a working journal could never show it) is written first, then enough data to overflow.
  const uint64_t frame_before = r->virtual_frame_count;
  bool spammed = false;
  while (r->virtual_frame_count == frame_before) {
    pc = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc).next_pc;
    if (!spammed && (r->scheduler.master_ticks % GENESIS_NTSC_MASTER_TICKS_PER_FRAME) >= 110ull * GENESIS_NTSC_MASTER_TICKS_PER_LINE &&
        (r->scheduler.master_ticks % GENESIS_NTSC_MASTER_TICKS_PER_FRAME) < 140ull * GENESIS_NTSC_MASTER_TICKS_PER_LINE) {
      put_sprite_words(*r, 0xF800U, 128U + 30U, 1, 128U + 8U);
      vdp_vram_address(*r, 0x4000U);
      for (unsigned i = 0; i < 5000U; ++i) vdp_data(*r, 0x0000);  // 10000 bytes > the journal capacity
      spammed = true;
    }
  }
  check(spammed && o->capture.vram_journal_overflows == 1u, "an overflowing journal is counted and abandoned for the frame");
  check(is_color(o->frame, 9, 32, 2U), "after an overflow the frame-end VRAM image is used for every line (the late sprite C shows above its write line)");
}

// ---- SEG-055 adversarial regression coverage: exact hardware boundaries --------------------------------------------
// H40 sprite limits: 20 sprites and 40 cells per line, counted in link-list order; the first excluded entry is the 21st / the 41st cell.
void test_h40_sprite_limit_boundaries() {
  auto frame = make_frame();
  const unsigned row = 50U;
  // 20 versus 21 sprites on one line. Sprites 0..18 are spaced 12 px apart, #19 straddles the right edge (x = 316), #20 sits at x = 240.
  Scene a;
  base_scene(a, 0x81);
  for (unsigned t = 1; t <= 14U; ++t) solid_tile(a, t, t, 32U);
  const unsigned count = 21U;
  for (unsigned i = 0; i < count; ++i) {
    const unsigned x = i < 19U ? i * 12U : (i == 19U ? 316U : 240U);
    put_sprite(a, 0xF800U, i, 128U + row, 1, 1, i + 1U == count ? 0U : i + 1U, 1U + (i % 14U), 128U + x, 0, i == 20U ? 1U : 0U);
  }
  check(render(a, frame.get()), "H40 21-sprite line renders");
  bool first_twenty = true;
  for (unsigned i = 0; i < 19U; ++i) first_twenty = first_twenty && is_color(*frame, i * 12U + 3U, row, 1U + (i % 14U));
  check(first_twenty, "H40 draws sprites 1..19 of the list");
  check(is_color(*frame, 316, row, 1U + (19U % 14U)) && is_color(*frame, 319, row, 1U + (19U % 14U)),
        "the 20th sprite (last accepted) is drawn, clipped at the right edge of the 320-pixel display");
  check(!is_color(*frame, 243, row, 1U + (20U % 14U)) && is_color(*frame, 243, row, 0U),
        "the 21st sprite (first excluded) is not drawn although it has the highest priority bit and a free position: the cut is list order");
  // The same 21st sprite on a line that holds only 19 other sprites is accepted: the limit is per line, not per frame.
  put_sprite(a, 0xF800U, 20, 128U + row + 8U, 1, 1, 0, 1U + (20U % 14U), 128U + 240U, 0, 1);
  check(render(a, frame.get()) && is_color(*frame, 243, row + 8U, 1U + (20U % 14U)) && is_color(*frame, 316, row, 1U + (19U % 14U)),
        "moving the 21st sprite to another line makes it visible and leaves the 20 on the first line intact");

  // 40 versus 41 cells. Off-screen (x = -127) cells still consume the budget.
  const unsigned y = 72U;
  const auto run_cells = [&](unsigned four_cell, unsigned three_cell, bool *visible_a, bool *visible_b) {
    Scene c;
    base_scene(c, 0x81);
    solid_tile(c, 1, 5, 32U);
    solid_tile(c, 2, 6, 32U);
    unsigned n = 0;
    std::vector<unsigned> widths;
    for (unsigned i = 0; i < four_cell; ++i) widths.push_back(4U);
    for (unsigned i = 0; i < three_cell; ++i) widths.push_back(3U);
    widths.push_back(1U);  // visible probe A (cells 40 or 41 of the line)
    widths.push_back(1U);  // visible probe B (the next cell)
    for (unsigned i = 0; i < widths.size(); ++i, ++n) {
      const bool probe = i + 2U >= widths.size();
      put_sprite(c, 0xF800U, i, 128U + y, widths[i], 1, i + 1U == widths.size() ? 0U : i + 1U, probe ? (i + 2U == widths.size() ? 1U : 2U) : 3U,
                 probe ? 128U + 100U + (i + 2U == widths.size() ? 0U : 20U) : 1U);
    }
    check(render(c, frame.get()), "H40 cell-budget scene renders");
    *visible_a = is_color(*frame, 103, y + 2U, 5U);
    *visible_b = is_color(*frame, 123, y + 2U, 6U);
  };
  bool a40 = false, b41 = false;
  run_cells(9, 1, &a40, &b41);  // 36 + 3 = 39 cells before probe A: probe A is the 40th cell, probe B the 41st
  check(a40 && !b41, "H40: the 40th cell is drawn, the 41st is excluded");
  run_cells(10, 0, &a40, &b41);  // 40 cells already consumed: probe A is the 41st cell
  check(!a40 && !b41, "H40: with 40 cells consumed, the next sprite (41st cell) is excluded");
  run_cells(8, 1, &a40, &b41);  // 35 cells before the probes: both fit
  check(a40 && b41, "H40: well under budget both probes are drawn");

  // Priority and clipping at the 20-sprite boundary do not leak between lines (rows 50 and 58 above) and the H32 limit stays 16.
  Scene h32;
  base_scene(h32, 0x00);
  solid_tile(h32, 1, 2, 32U);
  for (unsigned i = 0; i < 20U; ++i) put_sprite(h32, 0xFA00U, i, 128U + 50U, 1, 1, i == 19U ? 0U : i + 1U, 1, 128U + (i % 16U) * 8U);
  h32.regs[5] = 0x7D;
  check(render(h32, frame.get()) && is_color(*frame, 15 * 8 + 3, 50, 2), "H32 still accepts 16 sprites after the H40 boundary tests");
}

// Interlace mode 2: the vertical scroll value is 11 bits. A 128-cell plane (reg #16 = 0x30) is 2048 interlaced rows tall, so every one of the 11 bits is
// observable (a smaller plane would hide bit 10 modulo its height).
void test_interlace_vscroll_full_range() {
  auto frame = make_frame();
  Scene s;
  base_scene(s, 0x87);
  s.regs[16] = 0x30;  // 32 cells wide, 128 cells tall
  for (unsigned r = 0; r < 128U; ++r)
    for (unsigned c = 0; c < 32U; ++c) put16(s, 0xC000U + (r * 32U + c) * 2U, nt_word(1U + (r % 7U)));
  for (unsigned t = 0; t < 7U; ++t) split_tile_im2(s, 1U + t, 1U + t, 8U + t);
  const auto expect_for = [&](unsigned vscroll) {
    unsigned bad = 0;
    for (unsigned y = 0; y < 448U; ++y) {
      const unsigned py = (y + vscroll) % 2048U;  // 11-bit scroll over a 2048-row plane (independent of the renderer's masks)
      const unsigned cell_row = py / 16U;
      const unsigned color = (py % 16U < 8U) ? 1U + (cell_row % 7U) : 8U + (cell_row % 7U);
      if (!is_color(*frame, 5, y, color)) ++bad;
    }
    return bad;
  };
  const unsigned values[] = {0x000, 0x001, 0x3FF, 0x400, 0x401, 0x5A5, 0x7FE, 0x7FF};
  for (const unsigned v : values) {
    s.vsram[0] = (uint8_t)(v >> 8);
    s.vsram[1] = (uint8_t)v;
    char message[160];
    std::snprintf(message, sizeof(message), "interlace mode 2 renders with 11-bit vertical scroll 0x%03X", v);
    check(render(s, frame.get()), message);
    std::snprintf(message, sizeof(message), "interlace mode 2: vertical scroll 0x%03X shifts by exactly that many output rows (bit 10 included)", v);
    check(expect_for(v) == 0U, message);
  }
  // Bits 15..11 of the VSRAM word are not part of the scroll value.
  s.vsram[0] = 0xF8;
  s.vsram[1] = 0x05;
  check(render(s, frame.get()) && expect_for(5U) == 0U, "interlace mode 2: VSRAM bits above bit 10 are ignored");
  // Non-interlaced: the same register file without LSM keeps the 10-bit, 8-row-cell behaviour (128 cells = 1024 rows).
  Scene n;
  base_scene(n, 0x81);
  n.regs[16] = 0x30;
  for (unsigned r = 0; r < 128U; ++r)
    for (unsigned c = 0; c < 32U; ++c) put16(n, 0xC000U + (r * 32U + c) * 2U, nt_word(1U + (r % 7U)));
  for (unsigned t = 0; t < 7U; ++t) solid_tile(n, 1U + t, 1U + t, 32U);
  n.vsram[0] = 0x03;
  n.vsram[1] = 0xFF;
  check(render(n, frame.get()), "non-interlaced 128-cell plane renders");
  unsigned bad = 0;
  for (unsigned y = 0; y < 224U; ++y)
    if (!is_color(*frame, 3, y, 1U + (((y + 1023U) % 1024U) / 8U) % 7U)) ++bad;
  check(bad == 0U && frame->height == 224U, "non-interlaced: scroll 0x3FF shifts by 1023 rows of a 1024-row plane (unchanged by interlace support)");
}

// An independent SHA-256 (FIPS 180-4), so the frame-digest expectations never call the production hash or its digest rule.
struct RefSha256 {
  static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32U - n)); }
  static void digest(const std::vector<uint8_t> &message, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m = message;
    const uint64_t bits = (uint64_t)message.size() * 8U;
    m.push_back(0x80);
    while (m.size() % 64U != 56U) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back((uint8_t)(bits >> (i * 8)));
    for (size_t block = 0; block < m.size(); block += 64U) {
      uint32_t w[64];
      for (unsigned i = 0; i < 16U; ++i)
        w[i] = ((uint32_t)m[block + i * 4U] << 24) | ((uint32_t)m[block + i * 4U + 1U] << 16) | ((uint32_t)m[block + i * 4U + 2U] << 8) |
               m[block + i * 4U + 3U];
      for (unsigned i = 16; i < 64U; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t v[8];
      for (unsigned i = 0; i < 8U; ++i) v[i] = h[i];
      for (unsigned i = 0; i < 64U; ++i) {
        const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
        const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
        const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
        const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const uint32_t t2 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
      }
      for (unsigned i = 0; i < 8U; ++i) h[i] += v[i];
    }
    for (unsigned i = 0; i < 8U; ++i) {
      out[i * 4U] = (uint8_t)(h[i] >> 24); out[i * 4U + 1U] = (uint8_t)(h[i] >> 16); out[i * 4U + 2U] = (uint8_t)(h[i] >> 8); out[i * 4U + 3U] = (uint8_t)h[i];
    }
  }
};

// The documented digest rule, rebuilt from the artifact's fields with the independent hash above.
void reference_digest(const GenesisFrameArtifact &f, uint8_t out[32]) {
  const unsigned w = GENESIS_FRAME_ARTIFACT_WIDTH(&f), h = GENESIS_FRAME_ARTIFACT_HEIGHT(&f);
  std::vector<uint8_t> bytes(f.pixels, f.pixels + (size_t)w * h);
  bytes.insert(bytes.end(), f.palette_snapshot, f.palette_snapshot + GENESIS_VDP_CRAM_BYTES);
  if (!genesis_vdp_frame_is_legacy_shape(&f)) {
    const uint8_t header[6] = {(uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(f.palette_count >> 8), (uint8_t)f.palette_count};
    bytes.insert(bytes.end(), header, header + 6);
    bytes.insert(bytes.end(), f.row_palette, f.row_palette + h);
    for (unsigned slot = 1; slot < f.palette_count; ++slot) bytes.insert(bytes.end(), f.extra_palettes[slot - 1U], f.extra_palettes[slot - 1U] + GENESIS_VDP_CRAM_BYTES);
  }
  RefSha256::digest(bytes, out);
}

void test_frame_digest_covers_row_palette_selection() {
  Scene s;
  base_scene(s, 0x81);
  fill_plane_a_rows(s);
  auto raster = std::make_unique<GenesisRasterFrameState>();
  const auto fill_lines = [&](unsigned switch_line) {
    for (unsigned l = 0; l < 224U; ++l) {
      std::memcpy(raster->lines[l].registers, s.regs, sizeof(s.regs));
      std::memset(raster->lines[l].vsram, 0, sizeof(raster->lines[l].vsram));
      fill_cram(raster->lines[l].cram, l < switch_line ? 0 : 1);
    }
  };
  auto a = make_frame(), b = make_frame();
  fill_lines(100U);
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), a.get()) == 0, "digest scene A renders");
  fill_lines(150U);
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), b.get()) == 0, "digest scene B renders");
  check(std::memcmp(a->pixels, b->pixels, 320U * 224U) == 0 && a->palette_count == 2U && b->palette_count == 2U,
        "the two frames are pixel-identical and use the same two palettes");
  check(std::memcmp(a->frame_digest, b->frame_digest, 32U) != 0, "pixel-identical frames that display different palettes on rows 100-149 have distinct digests");
  uint8_t expect[32];
  reference_digest(*a, expect);
  check(std::memcmp(expect, a->frame_digest, 32U) == 0, "non-legacy digest equals the independently recomputed rule (pixels, palette, geometry, per-row slots, extra palettes)");
  // Mutate exactly one field of the digested data at a time; each change must move the digest.
  uint8_t d0[32], d1[32];
  genesis_vdp_frame_digest(a.get(), d0);
  auto m = make_frame();
  std::memcpy(m.get(), a.get(), sizeof(*a));
  m->row_palette[120] = (uint8_t)(m->row_palette[120] ^ 1U);  // only the slot selection of one row
  genesis_vdp_frame_digest(m.get(), d1);
  check(std::memcmp(d0, d1, 32U) != 0, "changing only one row's palette slot changes the digest");
  reference_digest(*m, expect);
  check(std::memcmp(expect, d1, 32U) == 0, "and the independent rule agrees on the mutated artifact");
  std::memcpy(m.get(), a.get(), sizeof(*a));
  m->extra_palettes[0][2] ^= 0x02U;  // only an extra-palette byte
  genesis_vdp_frame_digest(m.get(), d1);
  check(std::memcmp(d0, d1, 32U) != 0, "changing only an extra palette byte changes the digest");
  std::memcpy(m.get(), a.get(), sizeof(*a));
  m->palette_snapshot[2] ^= 0x02U;
  genesis_vdp_frame_digest(m.get(), d1);
  check(std::memcmp(d0, d1, 32U) != 0, "changing only the base palette changes the digest");
  std::memcpy(m.get(), a.get(), sizeof(*a));
  m->pixels[5] ^= 1U;
  genesis_vdp_frame_digest(m.get(), d1);
  check(std::memcmp(d0, d1, 32U) != 0, "changing only a pixel changes the digest");
  // A legacy-shaped (single palette, 320x224) frame keeps the pre-SEG-055 rule: SHA-256(pixels || palette), nothing else.
  fill_lines(0U);
  auto legacy = make_frame();
  check(genesis_vdp_produce_frame_raster(s.vram.data(), raster.get(), legacy.get()) == 0 && genesis_vdp_frame_is_legacy_shape(legacy.get()),
        "a uniform-palette frame is legacy-shaped");
  std::vector<uint8_t> legacy_bytes(legacy->pixels, legacy->pixels + 320U * 224U);
  legacy_bytes.insert(legacy_bytes.end(), legacy->palette_snapshot, legacy->palette_snapshot + 128U);
  RefSha256::digest(legacy_bytes, expect);
  check(std::memcmp(expect, legacy->frame_digest, 32U) == 0, "the legacy 320x224 single-palette digest is unchanged (independent SHA-256 of pixels || palette)");
  // The independent hash itself: the FIPS 180-4 "abc" vector.
  const std::vector<uint8_t> abc = {'a', 'b', 'c'};
  RefSha256::digest(abc, expect);
  check(expect[0] == 0xba && expect[1] == 0x78 && expect[2] == 0x16 && expect[3] == 0xbf && expect[31] == 0xad, "the reference SHA-256 matches the published test vector");
}

// ---- DMA, fill and CPU VRAM writes while the display is active: ordering and the following-scanline visibility rule ----
struct JournalRig {
  std::unique_ptr<GenesisRuntime> r = std::make_unique<GenesisRuntime>();
  std::unique_ptr<Observed> o = std::make_unique<Observed>();
  uint32_t pc = 0x200u;
  JournalRig() {
    std::memset(r.get(), 0, sizeof(*r));
    Scene s;
    base_scene(s, 0x81);
    s.regs[7] = 0;
    s.regs[1] = 0x14;  // DMA enable
    r->pc = 0x200u;
    r->sr = 0x2000u;
    r->a[7] = 0x00FF8000u;
    std::memcpy(r->devices.vdp.registers, s.regs, sizeof(s.regs));
    std::memcpy(r->devices.vdp.cram, s.cram, sizeof(s.cram));
    r->devices.vdp.auto_increment_value = 2;
    r->devices.vdp.registers[15] = 2;
    Scene t;
    solid_tile(t, 1, 2, 32U);
    solid_tile(t, 2, 3, 32U);
    std::memcpy(r->devices.vdp.vram + 0x20, t.vram.data() + 0x20, 0x40);
    attach_raster(*o, *r);
  }
  void advance_to(uint64_t ticks) {
    while (r->scheduler.master_ticks < ticks) pc = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc).next_pc;
  }
  void run_until_frame(uint64_t frames) {
    while (r->virtual_frame_count < frames) pc = genesis_runtime_retire_m68k_instruction(r.get(), 4u, pc).next_pc;
  }
  static uint64_t at_line(uint64_t frame, unsigned line) { return frame * GENESIS_NTSC_MASTER_TICKS_PER_FRAME + (uint64_t)line * GENESIS_NTSC_MASTER_TICKS_PER_LINE; }
  void set_sprite0(unsigned raw_y, unsigned tile, unsigned raw_x) {
    unsigned a = 0xF800U;
    r->devices.vdp.vram[a] = (uint8_t)(raw_y >> 8); r->devices.vdp.vram[a + 1U] = (uint8_t)raw_y;
    r->devices.vdp.vram[a + 2U] = 0; r->devices.vdp.vram[a + 3U] = 0;
    r->devices.vdp.vram[a + 4U] = (uint8_t)(nt_word(tile) >> 8); r->devices.vdp.vram[a + 5U] = (uint8_t)nt_word(tile);
    r->devices.vdp.vram[a + 6U] = (uint8_t)(raw_x >> 8); r->devices.vdp.vram[a + 7U] = (uint8_t)raw_x;
  }
  void dma_regs(unsigned length_words, uint32_t source, unsigned mode_bits) {
    const uint32_t word = source >> 1;
    vdp_control(*r, 0x8000U | (19U << 8) | (length_words & 0xFFU));
    vdp_control(*r, 0x8000U | (20U << 8) | ((length_words >> 8) & 0xFFU));
    vdp_control(*r, 0x8000U | (21U << 8) | (word & 0xFFU));
    vdp_control(*r, 0x8000U | (22U << 8) | ((word >> 8) & 0xFFU));
    vdp_control(*r, 0x8000U | (23U << 8) | (((word >> 16) & 0x7FU) | mode_bits));
  }
  void dma_command(unsigned vram_address) {
    vdp_control(*r, 0x4000U | (vram_address & 0x3FFFU));
    vdp_control(*r, 0x0080U | ((vram_address >> 14) & 3U));
  }
};

void test_dma_fill_and_cpu_writes_follow_the_scanline_visibility_rule() {
  const unsigned L = 100U;  // the write happens while line L is being drawn: first visible on line L + 1

  // (a) Memory-to-VRAM DMA replaces the sprite table mid-frame (the table itself, 4 words).
  {
    JournalRig g;
    g.set_sprite0(128U + 96U, 1, 128U + 8U);  // old sprite: rows 96..103, color 2
    uint8_t *ram = g.r->work_ram + 0x0100;
    const unsigned entry[4] = {128U + 160U, 0x0000U, nt_word(2), 128U + 8U};  // new sprite: rows 160..167, color 3
    for (unsigned i = 0; i < 4U; ++i) { ram[i * 2U] = (uint8_t)(entry[i] >> 8); ram[i * 2U + 1U] = (uint8_t)entry[i]; }
    g.advance_to(JournalRig::at_line(0, L));
    g.dma_regs(4, 0x00FF0100u, 0x00);
    g.dma_command(0xF800U);
    g.run_until_frame(1);
    check(g.o->observer.sequence == 1u && g.o->capture.vram_journal_overflows == 0u, "DMA scene publishes one frame without overflow");
    check(is_color(g.o->frame, 9, 96, 2U) && is_color(g.o->frame, 9, L, 2U), "memory-to-VRAM DMA: rows up to and including the DMA line still draw the old table");
    check(!is_color(g.o->frame, 9, L + 1U, 2U) && !is_color(g.o->frame, 9, 103, 2U), "memory-to-VRAM DMA: the new table is first visible on the following line");
    check(is_color(g.o->frame, 9, 160, 3U) && is_color(g.o->frame, 9, 167, 3U) && !is_color(g.o->frame, 9, 168, 3U), "memory-to-VRAM DMA: the DMA'd entry draws below the DMA line");
  }
  // (b) A wrapped DMA fill: the fill starts on the last two bytes of VRAM and wraps onto tile 0 (pattern 0x11 -> palette index 1).
  {
    JournalRig g;
    g.advance_to(JournalRig::at_line(0, L));
    g.r->devices.vdp.registers[15] = 1;
    g.r->devices.vdp.auto_increment_value = 1;
    g.dma_regs(33, 0, 0x80);
    g.dma_command(0xFFFEU);
    vdp_data(*g.r, 0x1111U);
    g.run_until_frame(1);
    check(g.o->observer.sequence == 1u && g.o->capture.vram_journal_overflows == 0u, "wrapped fill scene publishes one frame");
    check(is_color(g.o->frame, 200, 0, 0U) && is_color(g.o->frame, 200, L, 0U), "DMA fill: rows up to and including the fill line still show the old pattern (backdrop)");
    check(is_color(g.o->frame, 200, L + 1U, 1U) && is_color(g.o->frame, 200, 223, 1U), "DMA fill: the wrapped destination (tile 0) is visible from the following line");
  }
  // (b2) A fill's own data word is written (low byte at P, high byte at P + 1) before the fill bytes: it must be journaled too.
  {
    JournalRig g;
    g.advance_to(JournalRig::at_line(0, L));
    g.r->devices.vdp.registers[15] = 1;
    g.r->devices.vdp.auto_increment_value = 1;
    g.dma_regs(31, 0, 0x80);
    g.dma_command(0x0000U);
    vdp_data(*g.r, 0x1122U);  // byte 0 = 0x22 (only the data word writes it), bytes 1..31 = 0x11
    g.run_until_frame(1);
    check(is_color(g.o->frame, 0, 104, 2U) && is_color(g.o->frame, 1, 104, 2U) && is_color(g.o->frame, 2, 104, 1U) && is_color(g.o->frame, 0, 100, 0U),
          "DMA fill: the data word's own low byte is journaled and replayed from the following line");
  }
  // (b3) An odd-address VRAM data write (A0 = 1): the byte pair is exchanged at the even base; both bytes are journaled.
  {
    JournalRig g;
    g.advance_to(JournalRig::at_line(0, L));
    vdp_vram_address(*g.r, 0x0001U);
    vdp_data(*g.r, 0x1122U);  // byte 0 = 0x22, byte 1 = 0x11 (exchanged)
    g.run_until_frame(1);
    check(g.o->capture.vram_journal_overflows == 0u && is_color(g.o->frame, 0, 104, 2U) && is_color(g.o->frame, 2, 104, 1U) && is_color(g.o->frame, 0, 100, 0U),
          "an odd-address VRAM write is journaled with its exchanged bytes and replayed from the following line");
  }
  // (c) Ordinary CPU data-port writes on the first and last active lines, and during VBlank.
  const auto cpu_fill_tile0 = [](JournalRig &g) {
    vdp_vram_address(*g.r, 0x0000U);
    for (unsigned i = 0; i < 16U; ++i) vdp_data(*g.r, 0x1111U);
  };
  {
    JournalRig g;  // first active line
    g.advance_to(JournalRig::at_line(0, 0) + 28U);
    cpu_fill_tile0(g);
    g.run_until_frame(1);
    check(is_color(g.o->frame, 200, 0, 0U) && is_color(g.o->frame, 200, 1, 1U) && is_color(g.o->frame, 200, 223, 1U),
          "a CPU write during the first active line is visible from line 1, not on line 0");
  }
  {
    JournalRig g;  // last active line: no later row exists in this frame; the next frame's line-0 snapshot contains it
    g.advance_to(JournalRig::at_line(0, 223));
    cpu_fill_tile0(g);
    g.run_until_frame(1);
    check(is_color(g.o->frame, 200, 222, 0U) && is_color(g.o->frame, 200, 223, 0U) && g.o->capture.frame.vram_write_count == 32u,
          "a CPU write during the last active line is journaled (32 bytes) but changes no row of that frame");
    g.run_until_frame(2);
    check(is_color(g.o->frame, 200, 0, 1U) && is_color(g.o->frame, 200, 223, 1U), "the next frame starts from the updated image");
  }
  {
    JournalRig g;  // VBlank
    g.run_until_frame(1);
    g.advance_to(JournalRig::at_line(0, 240));
    cpu_fill_tile0(g);
    g.run_until_frame(2);
    check(g.o->capture.frame.vram_write_count == 0u && is_color(g.o->frame, 200, 0, 1U),
          "a VBlank write is not journaled (zero entries) and is already in the next frame's line-0 snapshot");
  }
  // (d) Ordering: two writes to the same byte on different lines replay in order (the later one wins only on later lines).
  {
    JournalRig g;
    g.advance_to(JournalRig::at_line(0, 60));
    vdp_vram_address(*g.r, 0x0000U);
    for (unsigned i = 0; i < 16U; ++i) vdp_data(*g.r, 0x1111U);
    g.advance_to(JournalRig::at_line(0, 140));
    vdp_vram_address(*g.r, 0x0000U);
    for (unsigned i = 0; i < 16U; ++i) vdp_data(*g.r, 0x2222U);
    g.run_until_frame(1);
    check(is_color(g.o->frame, 200, 60, 0U) && is_color(g.o->frame, 200, 61, 1U) && is_color(g.o->frame, 200, 140, 1U) &&
              is_color(g.o->frame, 200, 141, 2U) && is_color(g.o->frame, 200, 223, 2U),
          "two writes to the same bytes on different lines replay in order, each from the following line");
  }
  // (e) Journal capacity boundary: exactly the capacity is replayed; one byte pair more abandons the journal for the frame.
  {
    JournalRig g;
    g.advance_to(JournalRig::at_line(0, 60));
    vdp_vram_address(*g.r, 0x4000U);  // harmless region (unused VRAM)
    for (unsigned i = 0; i < GENESIS_RASTER_VRAM_WRITE_CAPACITY / 2U - 16U; ++i) vdp_data(*g.r, 0x0000U);
    vdp_vram_address(*g.r, 0x0000U);
    for (unsigned i = 0; i < 16U; ++i) vdp_data(*g.r, 0x1111U);  // 32 more bytes: the journal is exactly full
    g.run_until_frame(1);
    check(g.o->capture.vram_journal_overflows == 0u && g.o->capture.frame.vram_write_count == GENESIS_RASTER_VRAM_WRITE_CAPACITY &&
              is_color(g.o->frame, 200, 60, 0U) && is_color(g.o->frame, 200, 61, 1U),
          "a journal filled to exactly its capacity is replayed (no overflow, the write is line-resolved)");
  }
  {
    JournalRig g;
    g.advance_to(JournalRig::at_line(0, 60));
    vdp_vram_address(*g.r, 0x4000U);
    for (unsigned i = 0; i < GENESIS_RASTER_VRAM_WRITE_CAPACITY / 2U - 16U; ++i) vdp_data(*g.r, 0x0000U);
    vdp_vram_address(*g.r, 0x0000U);
    for (unsigned i = 0; i < 17U; ++i) vdp_data(*g.r, 0x1111U);  // one word beyond the capacity (tile 1 starts at byte 0x20: its first bytes change too)
    g.run_until_frame(1);
    check(g.o->capture.vram_journal_overflows == 1u && g.o->capture.frame.vram_write_count == GENESIS_RASTER_VRAM_WRITE_CAPACITY &&
              is_color(g.o->frame, 200, 0, 1U),
          "one word beyond the capacity abandons the journal: the frame-end image is used for every line (fallback, counted)");
  }
}

// Tile upload through LONG data-port writes (the staging-buffer loop `MOVE.L (A1)+,$C00000` a game uses to load font tiles): the four bytes land
// in VRAM in big-endian order, and the renderer decodes the resulting pattern pixel for pixel (a VDP-side check that a tile whose contents look
// wrong on screen was wrong in the guest's staging buffer, not altered by the write path or the decoder).
void test_long_data_port_tile_upload_round_trips_to_pixels() {
  JournalRig g;
  const uint32_t pattern[8] = {0x77377377u, 0x73777373u, 0x77773377u, 0x73373777u, 0x77773373u, 0x77737377u, 0x77377777u, 0x77777777u};
  vdp_vram_address(*g.r, 0x3000U);  // tile 0x180
  for (const uint32_t word : pattern) {
    uint32_t v = word;
    GenesisRuntimeStop stop{};
    check(genesis_route_access(g.r.get(), 0xC00000, GENESIS_ACCESS_LONG, GENESIS_ACCESS_WRITE, &v, &stop) == GENESIS_ACCESS_OK, "LONG data-port write accepted");
  }
  bool bytes = true;
  for (unsigned i = 0; i < 8U; ++i)
    bytes = bytes && g.r->devices.vdp.vram[0x3000U + i * 4U] == (uint8_t)(pattern[i] >> 24) && g.r->devices.vdp.vram[0x3001U + i * 4U] == (uint8_t)(pattern[i] >> 16) &&
            g.r->devices.vdp.vram[0x3002U + i * 4U] == (uint8_t)(pattern[i] >> 8) && g.r->devices.vdp.vram[0x3003U + i * 4U] == (uint8_t)pattern[i];
  check(bytes, "a LONG data-port write stores its four bytes in big-endian order and the address advances by four");
  // Plane A cell (0, 0) uses tile 0x180; every pattern nibble becomes that pixel's palette index (7 or 3 here).
  Scene s;
  base_scene(s, 0x81);
  std::memcpy(s.vram.data() + 0x3000, g.r->devices.vdp.vram + 0x3000, 32);
  put16(s, 0xC000U, nt_word(0x180));
  auto frame = make_frame();
  check(render(s, frame.get()), "the uploaded tile renders");
  unsigned bad = 0;
  for (unsigned row = 0; row < 8U; ++row)
    for (unsigned x = 0; x < 8U; ++x)
      if (!is_color(*frame, x, row, (pattern[row] >> (28U - 4U * x)) & 0xFU)) ++bad;
  check(bad == 0U, "every pixel of the uploaded tile decodes to its pattern nibble (high nibble = left pixel)");
}
}  // namespace

int main() {
  test_hscroll_modes_and_road_like_scene();
  test_hscroll_extremes();
  test_vertical_scroll_modes();
  test_h32_geometry_and_fail_closed_modes();
  test_h32_sprite_table_and_limits();
  test_interlace_mode2();
  test_window_plane();
  test_scanline_palette_switch();
  test_export_and_presentation_agree();
  test_runtime_captures_scanline_palette_and_virtual_frames();
  test_vram_written_during_active_display_is_scanline_resolved();
  test_h40_sprite_limit_boundaries();
  test_interlace_vscroll_full_range();
  test_frame_digest_covers_row_palette_selection();
  test_dma_fill_and_cpu_writes_follow_the_scanline_visibility_rule();
  test_long_data_port_tile_upload_round_trips_to_pixels();
  if (failures != 0) std::printf("%d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
