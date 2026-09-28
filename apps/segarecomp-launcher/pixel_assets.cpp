#include "pixel_assets.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <cstdio>

#ifndef SEGARECOMP_LAUNCHER_SOURCE_ASSETS_DIR
#define SEGARECOMP_LAUNCHER_SOURCE_ASSETS_DIR ""
#endif

namespace fs = std::filesystem;

namespace launcher {

fs::path asset_path(const std::string &relative) {
  const char *base = SDL_GetBasePath();
  const fs::path exe_dir = base ? fs::path(std::u8string(reinterpret_cast<const char8_t *>(base))) : fs::path{};
  for (const fs::path &root : {exe_dir.parent_path() / "Resources" / "assets", exe_dir / "assets"}) {
    std::error_code ec;
    if (fs::is_regular_file(root / relative, ec)) return root / relative;
  }
  // Developer fallback: an uninstalled build run straight from the build tree.
  return fs::path(SEGARECOMP_LAUNCHER_SOURCE_ASSETS_DIR) / relative;
}

SDL_Texture *load_pixel_texture(SDL_Renderer *renderer, const std::string &relative) {
  const fs::path path = asset_path(relative);
  std::size_t size = 0;
  void *bytes = SDL_LoadFile(reinterpret_cast<const char *>(path.u8string().c_str()), &size);
  if (!bytes) {
    std::fprintf(stderr, "launcher: cannot read asset %s: %s\n", path.string().c_str(), SDL_GetError());
    return nullptr;
  }
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(static_cast<const stbi_uc *>(bytes), static_cast<int>(size), &w, &h, &channels, 4);
  SDL_free(bytes);
  if (!pixels) {
    std::fprintf(stderr, "launcher: cannot decode asset %s: %s\n", path.string().c_str(), stbi_failure_reason());
    return nullptr;
  }
  SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
  SDL_Texture *texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
  if (surface) SDL_DestroySurface(surface);
  stbi_image_free(pixels);
  if (!texture) {
    std::fprintf(stderr, "launcher: cannot upload asset %s: %s\n", path.string().c_str(), SDL_GetError());
    return nullptr;
  }
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
  return texture;
}

} // namespace launcher
