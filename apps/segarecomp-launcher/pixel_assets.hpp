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

// Loads `assets/<relative>` as an RGBA32 SDL_Surface (the window icon); nullptr (logged) when missing/undecodable. Caller destroys it.
[[nodiscard]] SDL_Surface *load_asset_surface(const std::string &relative);

// Loads any PNG/JPEG file (a cover) as a texture; nullptr on a missing/oversized/undecodable image. `nearest` for pixel art, linear otherwise.
[[nodiscard]] SDL_Texture *load_file_texture(SDL_Renderer *renderer, const std::filesystem::path &path, bool nearest);

struct ImageInfo {
  bool ok = false, png = false, jpeg = false;
  int width = 0, height = 0;
  std::string error;  // a plain-language reason when !ok
};
// Validates a candidate cover: PNG or JPEG by content (not extension), at most `max_bytes`, 16..2048 pixels per side, and fully decodable.
[[nodiscard]] ImageInfo probe_image(const std::filesystem::path &path, std::size_t max_bytes = 2U << 20U);

// Total bytes stb_image has requested from its allocator in this process (a test aid: the zip decoder must never allocate).
[[nodiscard]] std::size_t stb_requested_bytes_for_tests();

} // namespace launcher
