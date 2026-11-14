// SEG-140-T002 (ADR 0303): virtual sprites are decoded by the VDP's own routines. Differential evidence against the hardware path of the same VDP on synthetic VRAM, no duplicated
// decode, and zero change to a normal render.
#include <memory>

#include "genesis_vsprite_fixture.hpp"

using namespace vsfx;

namespace {

// A virtual sprite composed over a frame WITHOUT the hardware sprite must equal the frame WITH the hardware sprite, pixel for pixel (planes blank, so priority is moot).
void differential_all_shapes() {
  const unsigned shapes[][2] = {{1, 1}, {2, 3}, {4, 4}, {3, 2}, {1, 4}, {4, 1}};
  unsigned cases = 0, mismatches = 0, opaque_total = 0;
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  auto hw = std::make_unique<Rendered>();
  auto blank = std::make_unique<Rendered>();
  std::vector<uint8_t> out;
  for (const auto &shape : shapes)
    for (unsigned pal = 0; pal < 4; ++pal)
      for (unsigned flips = 0; flips < 4; ++flips) {
        const bool hf = (flips & 1U) != 0, vf = (flips & 2U) != 0;
        const int x = 40 + (int)(cases * 7U % 120U), y = 30 + (int)(cases * 5U % 100U);
        const unsigned tile = 64 + (cases * 13U) % 300U;
        Scene with_hw, without;
        with_hw.randomize(0x5EED0000U + cases, 0, 600);
        without = with_hw;
        with_hw.hw_sprite(x, y, shape[0], shape[1], tile, pal, hf, vf, false);
        check(hw->render(with_hw) && blank->render(without), "both frames render");
        zero_set(set.get());
        set->count = 1;
        set->sprites[0] = vsprite(x, y, shape[0], shape[1], tile, pal, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_GAME_STATE_VERIFIED, hf, vf);
        GenesisVirtualSpriteReport rep;
        out.assign((size_t)blank->w * blank->h * 3, 0);
        check(genesis_virtual_sprites_compose(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, without.vram, &blank->frame, &blank->aux, out.data(), out.size(), &rep) == 0, "compose ok");
        check(rep.status == GENESIS_VSPRITE_STATUS_COMPOSED, "composed");
        if (out != hw->rgb) ++mismatches;
        // The standalone decode helper agrees with the hardware frame too: opaque pixels equal the hardware's, transparent ones leave the blank frame's.
        GenesisVdpSpriteEntry e;
        std::memset(&e, 0, sizeof e);
        e.width_cells = shape[0]; e.height_cells = shape[1]; e.tile_index = (uint16_t)tile; e.palette = (uint8_t)pal; e.h_flip = hf; e.v_flip = vf;
        std::vector<uint8_t> rgba((size_t)shape[0] * 8 * shape[1] * 8 * 4);
        check(genesis_vdp_decode_sprite_rgba(&e, without.vram, without.cram, rgba.data(), rgba.size()) == 0, "decode helper ok");
        for (unsigned sy = 0; sy < shape[1] * 8; ++sy)
          for (unsigned sx = 0; sx < shape[0] * 8; ++sx) {
            const uint8_t *p = &rgba[((size_t)sy * shape[0] * 8 + sx) * 4];
            const size_t o = ((size_t)(y + (int)sy) * hw->w + (size_t)(x + (int)sx)) * 3;
            if (p[3] != 0) { ++opaque_total; if (std::memcmp(p, &hw->rgb[o], 3) != 0) ++mismatches; }
            else if (std::memcmp(&blank->rgb[o], &hw->rgb[o], 3) != 0) ++mismatches;
          }
        // MOD_AUTHORED: the same art placed in the authored pool decodes identically (the pool is a private VRAM/CRAM image, the same decoder).
        zero_set(set.get());
        set->count = 1;
        set->sprites[0] = vsprite(x, y, shape[0], shape[1], tile, pal, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_MOD_AUTHORED, hf, vf);
        std::memcpy(set->authored_vram, without.vram, sizeof without.vram);
        std::memcpy(set->authored_cram, without.cram, sizeof without.cram);
        std::vector<uint8_t> blank_vram_out((size_t)blank->w * blank->h * 3);
        Scene wiped = without;   // the frame's own VRAM must not matter to an authored sprite
        std::memset(wiped.vram, 0, sizeof wiped.vram);
        check(genesis_virtual_sprites_compose(set.get(), 0U, wiped.vram, &blank->frame, &blank->aux, blank_vram_out.data(), blank_vram_out.size(), &rep) == 0, "authored compose ok");
        // the authored palette equals the frame's CRAM for this fixture, so the pixels are the hardware's as well
        if (blank_vram_out != hw->rgb) ++mismatches;
        ++cases;
      }
  {   // negative control: the comparison can fail (a flipped sprite is NOT the hardware's picture)
    Scene a; a.randomize(0x77, 0, 600);
    Scene b = a;
    a.hw_sprite(60, 50, 2, 2, 100, 1, true, false, false);
    Rendered ra, rb;
    check(ra.render(a) && rb.render(b), "control renders");
    zero_set(set.get()); set->count = 1;
    set->sprites[0] = vsprite(60, 50, 2, 2, 100, 1, GENESIS_VSPRITE_LAYER_ABOVE_ALL, GENESIS_VSPRITE_GAME_STATE_VERIFIED, false, false);
    out.assign(rb.rgb.size(), 0);
    GenesisVirtualSpriteReport rep;
    check(genesis_virtual_sprites_compose(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, b.vram, &rb.frame, &rb.aux, out.data(), out.size(), &rep) == 0 && out != ra.rgb, "a wrongly flipped virtual sprite differs from the hardware picture");
  }
  check(cases == 6 * 4 * 4, "all 96 shape/palette/flip cases ran");
  check(mismatches == 0, "virtual sprites decode to exactly the VDP's own pixels (verified and authored)");
  check(opaque_total > 1000, "the fixtures exercised many opaque pixels");
}

void normal_output_unchanged() {
  Scene s;
  s.randomize(0xABCDEF01U, 0, 400);
  s.regs[0] = 0;
  s.plane_cell(kPlaneA, 3, 2, 10, 1, false);
  s.plane_cell(kPlaneB, 5, 4, 20, 2, true);
  s.hw_sprite(50, 40, 2, 2, 30, 3, true, false, true);
  GenesisFrameArtifact plain, with_aux;
  GenesisVdpFrameAux aux;
  check(genesis_vdp_produce_frame(s.vram, s.vsram, s.cram, s.regs, &plain) == 0, "plain render");
  check(genesis_vdp_produce_frame_aux(s.vram, s.vsram, s.cram, s.regs, &with_aux, &aux) == 0, "aux render");
  check(std::memcmp(plain.pixels, with_aux.pixels, sizeof plain.pixels) == 0 && std::memcmp(plain.row_palette, with_aux.row_palette, sizeof plain.row_palette) == 0 &&
            std::memcmp(plain.palette_snapshot, with_aux.palette_snapshot, sizeof plain.palette_snapshot) == 0 &&
            std::memcmp(plain.frame_digest, with_aux.frame_digest, sizeof plain.frame_digest) == 0 && plain.width == with_aux.width && plain.height == with_aux.height,
        "the aux render is byte-identical to the normal render (pixels, palettes, geometry, digest)");
  // golden: pinned first 8 digest bytes, recorded from the render BEFORE this delivery changed vdp_render.c (identical on origin/main), so a change of normal VDP output is a test failure
  {
    char hex[17];
    for (int i = 0; i < 8; ++i) std::snprintf(hex + 2 * i, 3, "%02x", plain.frame_digest[i]);
    check(std::strcmp(hex, "811bc4487b760945") == 0, "normal VDP output of the pinned fixture matches its golden digest");
  }
  // composing NOTHING yields the native picture exactly (also for an absent, empty and a rejected set)
  Rendered r;
  check(r.render(s), "render");
  std::vector<uint8_t> out(r.rgb.size());
  GenesisVirtualSpriteReport rep;
  check(genesis_virtual_sprites_compose(nullptr, 0, s.vram, &r.frame, &r.aux, out.data(), out.size(), &rep) == 0 && out == r.rgb && rep.status == GENESIS_VSPRITE_STATUS_NONE, "no set = native picture");
  auto set = std::make_unique<GenesisVirtualSpriteSet>();
  zero_set(set.get());
  check(genesis_virtual_sprites_compose(set.get(), 0, s.vram, &r.frame, &r.aux, out.data(), out.size(), &rep) == 0 && out == r.rgb && rep.status == GENESIS_VSPRITE_STATUS_NONE, "empty set = native picture");
  set->count = 1;
  set->sprites[0] = vsprite(10, 10, 1, 1, 5, 0);
  set->sprites[0].provenance = 0;
  check(genesis_virtual_sprites_compose(set.get(), GENESIS_VSPRITE_HOST_STATE_VERIFIED, s.vram, &r.frame, &r.aux, out.data(), out.size(), &rep) == 0 && out == r.rgb &&
            rep.status == GENESIS_VSPRITE_STATUS_REJECTED && rep.reason == GENESIS_VSPRITE_REJ_PROVENANCE && rep.reject_index == 0,
        "a rejected set = native picture, with a diagnostic");
}

void helper_arguments() {
  Scene s;
  GenesisVdpSpriteEntry e;
  std::memset(&e, 0, sizeof e);
  e.width_cells = 1; e.height_cells = 1;
  uint8_t buf[8 * 8 * 4];
  check(genesis_vdp_decode_sprite_rgba(nullptr, s.vram, s.cram, buf, sizeof buf) != 0, "NULL sprite refused");
  check(genesis_vdp_decode_sprite_rgba(&e, s.vram, s.cram, buf, sizeof buf - 1) != 0, "short buffer refused");
  e.width_cells = 5;
  check(genesis_vdp_decode_sprite_rgba(&e, s.vram, s.cram, buf, sizeof buf) != 0, "oversized sprite refused");
  e.width_cells = 1; e.palette = 4;
  check(genesis_vdp_decode_sprite_rgba(&e, s.vram, s.cram, buf, sizeof buf) != 0, "bad palette refused");
}

}  // namespace

int main() {
  differential_all_shapes();
  normal_output_unchanged();
  helper_arguments();
  if (failures != 0) { std::printf("%d failure(s)\n", failures); return 1; }
  std::printf("genesis_vsprite_decode_tests: ok\n");
  return 0;
}
