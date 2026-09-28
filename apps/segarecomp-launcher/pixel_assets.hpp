#pragma once

// Minimal pixel-art asset loading: PNG -> nearest-sampled SDL_Texture, and TTF font discovery. No general
// asset-management framework; this is the small amount of glue the retro launcher UI needs.

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>

namespace launcher {

// The launcher's asset root: `<package>/assets` when packaged, else the source tree's
// `apps/segarecomp-launcher/assets` (a developer convenience, mirroring how the test suite locates
// fixtures via a compiled-in source path; never required for a packaged build).
[[nodiscard]] std::filesystem::path asset_path(const std::string &relative);

// Loads `assets/<relative>` as an SDL_Texture with nearest-neighbour sampling (pixel art must never be
// bilinear-filtered). Returns nullptr (logged to stderr) on a missing/corrupt file; callers must handle
// that by omitting the graphic, never by crashing -- a missing decorative asset is not a fatal error.
[[nodiscard]] SDL_Texture *load_pixel_texture(SDL_Renderer *renderer, const std::string &relative);

} // namespace launcher
