#pragma once
// SEG-138-T010: the ONE translation from a Luau render result (rasterized fixed HUD overlay and declared layers) into the viewer's existing compatibility seam (`GenesisCompatOverlay`, ADR 0267 and
// the SEG-140-T004 layer transport). Shared by the ABI 1 HUD provider (luau_provider.cpp) and the scheduled-mod presenter (mod_capture.cpp) so no second copy of the packing exists.
#include "segarecomp/luau_api.hpp"

#include "segarecomp_genesis_patch.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace segarecomp::luau {

using RasterizedLayers = std::vector<std::pair<OverlayLayerDecl, OverlayImage>>;

// Returns 1 when something is presented (a fixed overlay and/or at least one layer), else 0.
inline int pack_hud(bool have_image, bool fixed, const OverlayImage &image, const RasterizedLayers &layers, GenesisCompatOverlay *out) {
  if (!have_image || out->capacity < image.rgba.size()) return 0;
  if (fixed) {   // only a request that drew into the fixed overlay makes the legacy path show anything
    std::memcpy(out->rgba, image.rgba.data(), image.rgba.size());
    out->width = static_cast<std::uint16_t>(image.width);
    out->height = static_cast<std::uint16_t>(image.height);
  }
  out->fallback_x = 0;
  out->fallback_y = 0;
  if (out->layer_rgba != nullptr) {   // SEG-140-T004: the declared layers, packed one after another into the host's buffer
    std::size_t offset = 0;
    out->layer_count = 0;
    for (const auto &entry : layers) {
      const std::size_t bytes = entry.second.rgba.size();
      if (out->layer_count >= GENESIS_COMPAT_LAYERS_MAX || offset + bytes > out->layer_capacity) break;
      std::memcpy(out->layer_rgba + offset, entry.second.rgba.data(), bytes);
      GenesisCompatLayer &l = out->layers[out->layer_count++];
      std::memset(&l, 0, sizeof l);
      l.width = static_cast<std::uint16_t>(entry.first.w);
      l.height = static_cast<std::uint16_t>(entry.first.h);
      l.anchor = static_cast<std::uint8_t>(entry.first.anchor);
      l.reference = static_cast<std::uint8_t>(entry.first.reference);
      l.z = static_cast<std::uint8_t>(entry.first.z);
      l.dx = static_cast<std::int16_t>(entry.first.dx);
      l.dy = static_cast<std::int16_t>(entry.first.dy);
      l.offset = static_cast<std::uint32_t>(offset);
      offset += bytes;
    }
  }
  return fixed || out->layer_count > 0 ? 1 : 0;
}

// SEG-140-T008 (ADR 0306): maps one callback's sega.sprite submission onto the shared GenesisVirtualSpriteSet transport (the host zeroed `*out`). Returns true when the script submitted any sprite command; the
// host's whole-set validation then decides everything else. `count` is the number of commands the script ADDED, so a set larger than the transport holds is rejected as TOO_MANY, never truncated.
inline bool pack_sprites(const SpriteSubmission &s, GenesisVirtualSpriteSet *out) {
  if (out == nullptr || s.total == 0) return false;
  out->budget_mode = static_cast<std::uint8_t>(s.budget_mode);
  out->count = s.total;
  for (std::size_t i = 0; i < s.commands.size() && i < GENESIS_VSPRITE_MAX; ++i) {
    const SpriteCommand &c = s.commands[i];
    GenesisVirtualSprite &d = out->sprites[i];
    d.provenance = static_cast<std::uint8_t>(c.provenance);
    d.layer = static_cast<std::uint8_t>(c.layer);
    d.width_cells = static_cast<std::uint8_t>(c.w_cells);
    d.height_cells = static_cast<std::uint8_t>(c.h_cells);
    d.palette = static_cast<std::uint8_t>(c.palette);
    d.h_flip = static_cast<std::uint8_t>(c.h_flip);
    d.v_flip = static_cast<std::uint8_t>(c.v_flip);
    d.has_clip = c.has_clip ? 1U : 0U;
    d.tile_index = static_cast<std::uint16_t>(c.tile);
    d.x = static_cast<std::int16_t>(c.x);
    d.y = static_cast<std::int16_t>(c.y);
    d.clip_x = static_cast<std::int16_t>(c.clip_x);
    d.clip_y = static_cast<std::int16_t>(c.clip_y);
    d.clip_w = static_cast<std::uint16_t>(c.clip_w);
    d.clip_h = static_cast<std::uint16_t>(c.clip_h);
  }
  if (!s.vram.empty()) std::memcpy(out->authored_vram, s.vram.data(), s.vram.size() < sizeof out->authored_vram ? s.vram.size() : sizeof out->authored_vram);
  if (!s.cram.empty()) std::memcpy(out->authored_cram, s.cram.data(), s.cram.size() < sizeof out->authored_cram ? s.cram.size() : sizeof out->authored_cram);
  return true;
}

}  // namespace segarecomp::luau
