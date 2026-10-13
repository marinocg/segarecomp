#include "covers.hpp"

#include "pixel_assets.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;

namespace launcher {
namespace {

bool bare_name(const std::string &name) {
  return !name.empty() && name.size() <= 128 && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos;
}

} // namespace

CoverImport import_cover(const fs::path &source, const fs::path &covers_dir, const std::string &sha256) {
  CoverImport result;
  if (sha256.size() != 64U || !std::all_of(sha256.begin(), sha256.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) {
    result.error = "Internal error: invalid game identity.";
    return result;
  }
  const ImageInfo info = probe_image(source);
  if (!info.ok) { result.error = info.error; return result; }
  const std::string name = sha256 + (info.png ? ".png" : ".jpg");
  std::error_code ec;
  fs::create_directories(covers_dir, ec);
  if (ec) { result.error = "The covers folder cannot be created."; return result; }
  const fs::path tmp = covers_dir / (name + ".tmp");
  fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) { result.error = "The image could not be copied: " + ec.message(); fs::remove(tmp, ec); return result; }
  // Replace a previous cover of the other extension only once the new one is in place.
  fs::rename(tmp, covers_dir / name, ec);
  if (ec) { result.error = "The cover could not be stored: " + ec.message(); fs::remove(tmp, ec); return result; }
  const std::string other = sha256 + (info.png ? ".jpg" : ".png");
  fs::remove(covers_dir / other, ec);
  result.ok = true;
  result.file_name = name;
  return result;
}

bool remove_cover_file(const fs::path &covers_dir, const std::string &file_name) {
  if (!bare_name(file_name)) return false;
  std::error_code ec;
  const fs::path path = covers_dir / file_name;
  const auto status = fs::symlink_status(path, ec);
  if (ec || status.type() != fs::file_type::regular) return false;
  return fs::remove(path, ec);
}

std::string existing_cover_for(const fs::path &covers_dir, const std::string &sha256) {
  for (const char *ext : {".png", ".jpg"}) {
    std::error_code ec;
    const std::string name = sha256 + ext;
    if (fs::is_regular_file(covers_dir / name, ec) && !fs::is_symlink(covers_dir / name, ec)) return name;
  }
  return {};
}

std::string libretro_system_dir(const std::string &platform_id) {
  if (platform_id == "genesis") return "Sega - Mega Drive - Genesis";
  if (platform_id == "master-system") return "Sega - Master System - Mark III";
  return {};
}

std::vector<std::string> libretro_candidate_names(const std::string &title, const std::vector<std::string> &preferred_regions) {
  std::string clean;
  for (const char c : title) {
    if (std::string("&*/:`<>?\\|\"").find(c) != std::string::npos) clean.push_back('_');
    else clean.push_back(c);
  }
  // collapse whitespace, trim
  std::string collapsed;
  bool space = false;
  for (const char c : clean) {
    if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
    if (space && !collapsed.empty()) collapsed.push_back(' ');
    space = false;
    collapsed.push_back(c);
  }
  std::vector<std::string> names;
  if (collapsed.empty()) return names;
  for (const auto &region : preferred_regions) {
    if (region.empty() || region.size() > 24) continue;
    const std::string name = collapsed + " (" + region + ")";
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  }
  return names;
}

std::string percent_encode(const std::string &text) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(raw);
    else { out.push_back('%'); out.push_back(hex[c >> 4U]); out.push_back(hex[c & 15U]); }
  }
  return out;
}

std::string libretro_candidate_url(const std::string &platform_id, const std::string &name) {
  const std::string system = libretro_system_dir(platform_id);
  if (system.empty() || name.empty()) return {};
  return "https://thumbnails.libretro.com/" + percent_encode(system) + "/Named_Boxarts/" + percent_encode(name) + ".png";
}

} // namespace launcher
