// SEG-140: shared synthetic fixture of the virtual-sprite tests. Project-authored VRAM/CRAM only (no commercial content); every expected value in the tests is derived
// independently of the code under test (the hardware path of the same VDP, a hand-built table, or a hand count).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "vdp_render.h"
#include "virtual_sprite.h"

namespace vsfx {

static int failures = 0;
inline void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

inline uint32_t xorshift(uint32_t &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

inline void put_word(uint8_t *buf, uint32_t addr, uint16_t v) {
  buf[addr] = (uint8_t)(v >> 8);
  buf[addr + 1] = (uint8_t)(v & 0xFF);
}

constexpr uint32_t kSat = 0x5000;      // register #5 = 0x28
constexpr uint32_t kPlaneA = 0xC000;   // register #2 = 0x30
constexpr uint32_t kPlaneB = 0x8000;   // register #4 = 0x04
constexpr unsigned kPlaneCells = 64;   // register #16 = 0x01 (64 x 32)

struct Scene {
  uint8_t vram[GENESIS_VDP_VRAM_BYTES];
  uint8_t vsram[GENESIS_VDP_VSRAM_BYTES];
  uint8_t cram[GENESIS_VDP_CRAM_BYTES];
  uint16_t regs[GENESIS_VDP_REGISTER_COUNT];
  unsigned next_sat = 0;

  explicit Scene(bool h40 = true) {
    std::memset(vram, 0, sizeof vram);
    std::memset(vsram, 0, sizeof vsram);
    std::memset(cram, 0, sizeof cram);
    std::memset(regs, 0, sizeof regs);
    regs[1] = 0x04;                  // Mode 5
    regs[2] = 0x30;
    regs[4] = 0x04;
    regs[5] = 0x28;
    regs[11] = 0x04;
    regs[12] = h40 ? 0x81 : 0x00;    // H40 / H32, non-interlaced
    regs[16] = 0x01;
    regs[7] = 0x03;                  // backdrop = CRAM entry 3
  }
  // Random patterns for tiles [first, first + count) (nibble 0 stays possible, so transparency is exercised) and a random CRAM (BBB0GGG0RRR0 words).
  void randomize(uint32_t seed, unsigned first_tile, unsigned count) {
    uint32_t s = seed;
    for (unsigned i = 0; i < count * GENESIS_VDP_TILE_BYTES; ++i) vram[first_tile * GENESIS_VDP_TILE_BYTES + i] = (uint8_t)xorshift(s);
    for (unsigned i = 0; i < GENESIS_VDP_CRAM_ENTRY_COUNT; ++i) put_word(cram, i * 2U, (uint16_t)(xorshift(s) & 0x0EEE));
  }
  // Writes the next hardware sprite (SAT entry) linked after the previous one.
  void hw_sprite(int x, int y, unsigned w, unsigned h, unsigned tile, unsigned pal, bool hf, bool vf, bool prio) {
    const uint32_t addr = kSat + next_sat * 8U;
    if (next_sat > 0) vram[kSat + (next_sat - 1) * 8U + 3] = (uint8_t)next_sat;
    put_word(vram, addr, (uint16_t)(y + GENESIS_VDP_SPRITE_COORD_BIAS));
    vram[addr + 2] = (uint8_t)(((w - 1) << 2) | (h - 1));
    vram[addr + 3] = 0;
    put_word(vram, addr + 4, (uint16_t)(((prio ? 1U : 0U) << 15) | (pal << 13) | ((vf ? 1U : 0U) << 12) | ((hf ? 1U : 0U) << 11) | (tile & 0x7FF)));
    put_word(vram, addr + 6, (uint16_t)(x + GENESIS_VDP_SPRITE_COORD_BIAS));
    ++next_sat;
  }
  // A Plane A / Plane B cell (cell coordinates, 64-cell wide planes).
  void plane_cell(uint32_t base, unsigned cx, unsigned cy, unsigned tile, unsigned pal, bool prio) {
    put_word(vram, base + (cy * kPlaneCells + cx) * 2U, (uint16_t)(((prio ? 1U : 0U) << 15) | (pal << 13) | (tile & 0x7FF)));
  }
  void fill_tile(unsigned tile, uint8_t nibble) {
    for (unsigned i = 0; i < GENESIS_VDP_TILE_BYTES; ++i) vram[tile * GENESIS_VDP_TILE_BYTES + i] = (uint8_t)((nibble << 4) | nibble);
  }
  void set_color(unsigned entry, unsigned r3, unsigned g3, unsigned b3) { put_word(cram, entry * 2U, (uint16_t)((b3 << 9) | (g3 << 5) | (r3 << 1))); }
};

struct Rendered {
  GenesisFrameArtifact frame;
  GenesisVdpFrameAux aux;
  std::vector<uint8_t> rgb;
  unsigned w = 0, h = 0;
  bool render(const Scene &s) {
    if (genesis_vdp_produce_frame_aux(s.vram, s.vsram, s.cram, s.regs, &frame, &aux) != 0) return false;
    w = frame.width;
    h = frame.height;
    rgb.assign((size_t)w * h * 3, 0);
    for (unsigned r = 0; r < h; ++r)
      if (genesis_vdp_frame_row_rgb888(&frame, r, &rgb[(size_t)r * w * 3]) != 0) return false;
    return true;
  }
};

inline GenesisVirtualSprite vsprite(int x, int y, unsigned w, unsigned h, unsigned tile, unsigned pal, unsigned layer = GENESIS_VSPRITE_LAYER_ABOVE_ALL,
                                    unsigned prov = GENESIS_VSPRITE_GAME_STATE_VERIFIED, bool hf = false, bool vf = false) {
  GenesisVirtualSprite v;
  std::memset(&v, 0, sizeof v);
  v.provenance = (uint8_t)prov;
  v.layer = (uint8_t)layer;
  v.width_cells = (uint8_t)w;
  v.height_cells = (uint8_t)h;
  v.palette = (uint8_t)pal;
  v.h_flip = hf;
  v.v_flip = vf;
  v.tile_index = (uint16_t)tile;
  v.x = (int16_t)x;
  v.y = (int16_t)y;
  return v;
}

inline void zero_set(GenesisVirtualSpriteSet *set) { std::memset(set, 0, sizeof *set); }

}  // namespace vsfx
