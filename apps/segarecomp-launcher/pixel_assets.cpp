#include "pixel_assets.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG  // user-supplied covers may be JPEG; the built-in assets are PNG
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

SDL_Texture *load_file_texture(SDL_Renderer *renderer, const fs::path &path, bool nearest) {
  std::size_t size = 0;
  void *bytes = SDL_LoadFile(reinterpret_cast<const char *>(path.u8string().c_str()), &size);
  if (!bytes) return nullptr;
  int w = 0, h = 0, channels = 0;
  stbi_uc *pixels = size <= (8U << 20U) ? stbi_load_from_memory(static_cast<const stbi_uc *>(bytes), static_cast<int>(size), &w, &h, &channels, 4) : nullptr;
  SDL_free(bytes);
  if (!pixels) return nullptr;
  SDL_Texture *texture = nullptr;
  if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
    if (SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4)) {
      texture = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_DestroySurface(surface);
    }
  }
  stbi_image_free(pixels);
  if (texture) SDL_SetTextureScaleMode(texture, nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
  return texture;
}

ImageInfo probe_image(const fs::path &path, std::size_t max_bytes) {
  ImageInfo info;
  std::error_code ec;
  const auto size = fs::is_regular_file(path, ec) ? fs::file_size(path, ec) : 0U;
  if (ec || size == 0U) { info.error = "The file cannot be read."; return info; }
  if (size > max_bytes) { info.error = "The image is larger than " + std::to_string(max_bytes / 1024U) + " KiB."; return info; }
  std::size_t loaded = 0;
  void *bytes = SDL_LoadFile(reinterpret_cast<const char *>(path.u8string().c_str()), &loaded);
  if (!bytes) { info.error = "The file cannot be read."; return info; }
  int w = 0, h = 0, comp = 0;
  const bool ok = stbi_info_from_memory(static_cast<const stbi_uc *>(bytes), static_cast<int>(loaded), &w, &h, &comp) != 0;
  const auto *b = static_cast<const unsigned char *>(bytes);
  info.png = loaded >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
  info.jpeg = loaded >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
  if (ok && (info.png || info.jpeg)) {
    // A header can promise a small image and decode to something huge: decode once (bounded by the caps below) to prove it.
    if (w < 16 || h < 16 || w > 2048 || h > 2048) info.error = "The image must be between 16 and 2048 pixels on each side.";
    else {
      stbi_uc *pixels = stbi_load_from_memory(b, static_cast<int>(loaded), &w, &h, &comp, 4);
      if (!pixels) info.error = "The image could not be decoded.";
      else { stbi_image_free(pixels); info.ok = true; info.width = w; info.height = h; }
    }
  } else {
    info.error = "Only PNG and JPEG images can be used as covers.";
  }
  SDL_free(bytes);
  return info;
}

} // namespace launcher
