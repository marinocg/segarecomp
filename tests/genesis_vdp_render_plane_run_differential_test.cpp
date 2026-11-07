// SEG-126 (ADR 0274): randomized differential test for the per-cell plane evaluation of the native row composer.
//
// genesis_vdp_render_row evaluates plane A/B per nametable cell run. This test checks every pixel of whole frames
// against a straightforward per-pixel oracle built ONLY from other public, independently implemented VDP primitives:
// genesis_vdp_resolve_plane_pixel (scroll registers, wrap, nametable address), genesis_vdp_decode_nametable_word, a
// test-local window rule and the documented six-layer priority rule. Sprites are disabled (their code is unchanged and
// covered by genesis_vdp_render_sprite_tests). All inputs are synthetic, generated from a fixed seed: no commercial data.
//
// Randomized per frame: H32/H40, interlace mode 2, plane sizes (up to 8 KiB nametables), plane bases, horizontal scroll
// mode (full / 8-line cell / line) and vertical scroll mode (full / 2-cell column), the window boundaries and mirror bits,
// left-column blanking, backdrop, flips, priorities, palettes, tile indices, VRAM/VSRAM/CRAM contents, and mid-frame
// register/VSRAM changes on a few field lines (the raster path).

#include "vdp_render.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

struct Rng {
  uint64_t state;
  uint32_t next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(state >> 33);
  }
  unsigned below(unsigned n) { return next() % n; }
};

void write_no_visible_sprite(uint8_t *vram, uint32_t sat_base) {
  // y and x far off screen, size 1x1, link 0: the SAT walk succeeds and no sprite ever covers a pixel.
  const uint16_t far = (uint16_t)(1000 + GENESIS_VDP_SPRITE_COORD_BIAS);
  vram[sat_base + 0] = (uint8_t)(far >> 8);
  vram[sat_base + 1] = (uint8_t)far;
  vram[sat_base + 2] = 0;
  vram[sat_base + 3] = 0;
  vram[sat_base + 4] = 0;
  vram[sat_base + 5] = 0;
  vram[sat_base + 6] = (uint8_t)(far >> 8);
  vram[sat_base + 7] = (uint8_t)far;
}

void randomize_line(Rng &rng, GenesisRasterLine &line, const GenesisRasterLine *previous, bool allow_geometry_change) {
  uint16_t *r = line.registers;
  if (previous == nullptr) {
    std::memset(r, 0, sizeof(line.registers));
    r[1] = 0x04;                                              // mode 5, 224 lines
    r[5] = 0x28;                                              // SAT at 0x5000
    r[12] = (uint16_t)(rng.below(2) ? 0x81 : 0x00);           // H40 / H32
    if (rng.below(5) == 0) r[12] |= 0x06;                     // interlace mode 2 (LSM1 = LSM0 = 1)
    static const unsigned sizes[][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {3, 0}};   // 32x32, 64x32, 32x64, 64x64, 128x32 (<= 8 KiB)
    const unsigned *size = sizes[rng.below(5)];
    r[16] = (uint16_t)(size[0] | (size[1] << 4));
  } else {
    std::memcpy(&line, previous, sizeof(line));
    if (!allow_geometry_change) {
      // keep r[1], r[5], r[12], r[16] (geometry and plane size are frame constants in this test)
    }
  }
  r[0] = (uint16_t)(rng.below(3) == 0 ? 0x20 : 0x00);                      // left-column blank
  r[2] = (uint16_t)((rng.below(8) << 3));                                  // plane A base (8 KiB steps)
  r[3] = (uint16_t)(rng.below(32) << 1);                                   // window base
  r[4] = (uint16_t)rng.below(8);                                           // plane B base
  r[7] = (uint16_t)rng.below(64);                                          // backdrop
  static const unsigned hmodes[] = {0, 2, 3};
  r[11] = (uint16_t)(hmodes[rng.below(3)] | (rng.below(2) ? 0x04 : 0x00)); // horizontal / vertical scroll modes
  r[13] = (uint16_t)rng.below(0x39);                                       // horizontal scroll table (kept inside VRAM)
  r[17] = (uint16_t)(rng.below(3) == 0 ? (rng.below(21) | (rng.below(2) ? 0x80 : 0x00)) : 0x00);   // window H boundary (cells of 2)
  r[18] = (uint16_t)(rng.below(3) == 0 ? (rng.below(29) | (rng.below(2) ? 0x80 : 0x00)) : 0x00);   // window V boundary
}

// The test's own window rule (vdp_render.h citations): inside the vertical OR the horizontal range, boundaries 2 cells / 1 cell.
bool window_covers(const uint16_t *r, unsigned cell_h, unsigned x, unsigned y) {
  const unsigned vboundary = (unsigned)(r[18] & 0x1F) * cell_h;
  const bool below = (r[18] & 0x80) != 0;
  const unsigned hboundary = (unsigned)(r[17] & 0x1F);
  const bool right = (r[17] & 0x80) != 0;
  const bool vertical = below ? (y >= vboundary) : (y < vboundary);
  const bool horizontal = right ? ((x >> 4) >= hboundary) : ((x >> 4) < hboundary);
  return vertical || horizontal;
}

struct Candidate {
  bool opaque;
  bool priority;
  unsigned palette;
  unsigned index;
};

Candidate pattern_pixel(const uint8_t *vram, bool im2, const GenesisVdpNametableEntry &e, unsigned tile_x, unsigned tile_y, unsigned cell_h) {
  if (e.h_flip) tile_x = 7U - tile_x;
  if (e.v_flip) tile_y = cell_h - 1U - tile_y;
  const unsigned tile_bytes = im2 ? 64U : 32U;
  const unsigned byte = vram[((unsigned)e.tile_index * tile_bytes + tile_y * 4U + tile_x / 2U) & 0xFFFFU];
  const unsigned pattern = (tile_x % 2U == 0U) ? (byte >> 4) : (byte & 0x0FU);
  return Candidate{pattern != 0U, e.priority != 0, e.palette, pattern};
}

bool oracle_pixel(const uint8_t *vram, const GenesisRasterLine &line, bool h40, bool im2, unsigned x, unsigned y, unsigned *index_out) {
  unsigned backdrop = 0;
  if (genesis_vdp_backdrop_cram_index(line.registers, &backdrop) != 0) return false;
  const unsigned cell_h = im2 ? 16U : 8U;
  if ((line.registers[0] & 0x20U) != 0U && x < 8U) {
    *index_out = backdrop;
    return true;
  }
  Candidate a{}, b{};
  for (int which = 0; which < 2; ++which) {
    GenesisVdpNametableEntry entry;
    unsigned tx, ty;
    const GenesisVdpPlaneSelector plane = which == 0 ? GENESIS_VDP_PLANE_A : GENESIS_VDP_PLANE_B;
    if (which == 0 && window_covers(line.registers, cell_h, x, y)) {
      const unsigned mask = h40 ? 0xF000U : 0xF800U;
      const unsigned base = ((unsigned)line.registers[3] << 10) & mask;
      const unsigned stride = h40 ? 64U : 32U;
      const unsigned addr = base + ((y / cell_h) * stride + x / 8U) * 2U;
      entry = genesis_vdp_decode_nametable_word((uint16_t)((vram[addr & 0xFFFFU] << 8) | vram[(addr + 1U) & 0xFFFFU]));
      tx = x % 8U;
      ty = y % cell_h;
    } else {
      GenesisVdpPlanePixelLocation loc;
      if (genesis_vdp_resolve_plane_pixel(vram, line.vsram, line.registers, plane, x, y, &loc) != 0) return false;
      entry = loc.entry;
      tx = loc.tile_x;
      ty = loc.tile_y;
    }
    (which == 0 ? a : b) = pattern_pixel(vram, im2, entry, tx, ty, cell_h);
  }
  // Six-layer order without sprites: A-high, B-high, A-low, B-low, backdrop.
  const Candidate *w = nullptr;
  if (a.opaque && a.priority) w = &a;
  else if (b.opaque && b.priority) w = &b;
  else if (a.opaque && !a.priority) w = &a;
  else if (b.opaque && !b.priority) w = &b;
  *index_out = w != nullptr ? genesis_vdp_cram_index(w->palette, w->index) : backdrop;
  return true;
}

void test_random_frames() {
  auto raster = std::make_unique<GenesisRasterFrameState>();
  auto frame = std::make_unique<GenesisFrameArtifact>();
  Rng rng{0x5E6126A11ULL};
  unsigned produced = 0, compared_pixels = 0, mismatches = 0, mid_frame = 0, windowed = 0, per_column = 0, line_scroll = 0, interlaced = 0, h32 = 0;
  for (unsigned iteration = 0; iteration < 48; ++iteration) {
    std::memset(raster.get(), 0, sizeof(*raster));
    for (unsigned i = 0; i < GENESIS_VDP_VRAM_BYTES; ++i) {
      const uint32_t v = rng.next();
      uint8_t b = (uint8_t)v;
      switch ((v >> 8) % 4U) {
        case 0: b = 0; break;                       // transparent pixels / nametable words with tile 0
        case 1: b = (uint8_t)(b & 0xF0U); break;    // one transparent nibble
        default: break;
      }
      raster->vram_start[i] = b;
    }
    uint8_t cram[GENESIS_VDP_CRAM_BYTES], vsram[GENESIS_VDP_VSRAM_BYTES];
    for (auto &c : cram) c = (uint8_t)rng.next();
    for (auto &c : vsram) c = (uint8_t)rng.next();
    write_no_visible_sprite(raster->vram_start, 0x5000);
    raster->has_vram_journal = 0;
    GenesisRasterLine base;
    randomize_line(rng, base, nullptr, true);
    std::memcpy(base.cram, cram, sizeof(base.cram));
    std::memcpy(base.vsram, vsram, sizeof(base.vsram));
    const bool h40 = (base.registers[12] & 0x01) != 0 && (base.registers[12] & 0x80) != 0;
    const bool im2 = (base.registers[12] & 0x06) == 0x06;
    unsigned change_at[3] = {rng.below(224), rng.below(224), rng.below(224)};
    for (unsigned l = 0; l < GENESIS_RASTER_MAX_LINES; ++l) {
      if (l == 0) {
        raster->lines[0] = base;
      } else {
        raster->lines[l] = raster->lines[l - 1];
        if (l == change_at[0] || l == change_at[1] || l == change_at[2]) {
          randomize_line(rng, raster->lines[l], &raster->lines[l - 1], false);
          for (auto &c : raster->lines[l].vsram) c = (uint8_t)rng.next();
          ++mid_frame;
        }
      }
    }
    // the shared geometry registers must stay frame constants; the previous-line copy kept r[1], r[5], r[12], r[16]
    if (genesis_vdp_produce_frame_raster(raster->vram_start, raster.get(), frame.get()) != 0) continue;   // an unrepresentable draw is skipped, never silently compared
    ++produced;
    const unsigned width = h40 ? 320U : 256U;
    const unsigned height = im2 ? 448U : 224U;
    check(frame->width == width && frame->height == height, "frame geometry follows the registers");
    for (unsigned y = 0; y < height; ++y) {
      const GenesisRasterLine &line = raster->lines[im2 ? y / 2U : y];
      if ((line.registers[17] | line.registers[18]) != 0U) ++windowed;
      if ((line.registers[11] & 0x04U) != 0U) ++per_column;
      if ((line.registers[11] & 0x03U) == 3U) ++line_scroll;
      for (unsigned x = 0; x < width; ++x) {
        unsigned expected = 0;
        if (!oracle_pixel(raster->vram_start, line, h40, im2, x, y, &expected)) {
          check(false, "oracle failed on a frame the renderer accepted");
          return;
        }
        ++compared_pixels;
        if (frame->pixels[(size_t)y * width + x] != (uint8_t)expected && ++mismatches <= 5)
          std::printf("MISMATCH iter %u x %u y %u: renderer %u oracle %u\n", iteration, x, y, (unsigned)frame->pixels[(size_t)y * width + x], expected);
      }
    }
    if (im2) ++interlaced;
    if (!h40) ++h32;
  }
  check(mismatches == 0, "the per-cell row composer is pixel-identical to the per-pixel oracle");
  check(produced >= 30, "enough random frames were representable");
  check(mid_frame > 20 && windowed > 1000 && per_column > 1000 && line_scroll > 1000 && interlaced >= 2 && h32 >= 5, "the random draw covered mid-frame changes, windows, column scroll, line scroll, interlace and H32");
  std::printf("plane-run differential: %u frames, %llu pixels, %u mismatches (mid-frame changes %u, interlaced frames %u, H32 frames %u)\n", produced,
              (unsigned long long)compared_pixels, mismatches, mid_frame, interlaced, h32);
}

}  // namespace

int main() {
  test_random_frames();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("genesis_vdp_render_plane_run_differential_tests: all checks passed\n");
  return 0;
}
