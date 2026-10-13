#pragma once

// Library covers: user-chosen local images (validated, copied into the library's covers folder) and the conservative Libretro
// thumbnail naming used to point the user at a public image. The launcher downloads nothing (see docs/product/launcher-library.md):
// no HTTPS client is packaged for all three platforms, so the supported route is a local file, optionally found via the browser.

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

struct CoverImport {
  bool ok = false;
  std::string file_name;  // bare name inside the covers folder
  std::string error;
};

// Copies `source` into `covers_dir` as `<sha256>.<png|jpg>` after validating it (probe_image). The original is never moved or modified.
// An existing cover of the same game is replaced only after the new one is safely written.
[[nodiscard]] CoverImport import_cover(const std::filesystem::path &source, const std::filesystem::path &covers_dir, const std::string &sha256);
// Deletes the cover file named `file_name` inside `covers_dir` only if it is a plain file there (bare name, no separators). Returns whether
// something was removed.
bool remove_cover_file(const std::filesystem::path &covers_dir, const std::string &file_name);
// A cover already stored for this ROM digest (e.g. after the entry was removed and imported again), or "".
[[nodiscard]] std::string existing_cover_for(const std::filesystem::path &covers_dir, const std::string &sha256);

// ---- Libretro thumbnails (https://thumbnails.libretro.com/), conservative exact-name candidates ----
// Directory name of the system tree ("" for an unknown platform).
[[nodiscard]] std::string libretro_system_dir(const std::string &platform_id);
// Names follow the No-Intro style "Title (Region)"; a ROM header does not tell the exact set name, so these are CANDIDATES in the user's
// preferred region order, never a verified match. `title` is the display title; characters Libretro replaces (&*/:`<>?\|) become '_'.
[[nodiscard]] std::vector<std::string> libretro_candidate_names(const std::string &title, const std::vector<std::string> &preferred_regions);
// https://thumbnails.libretro.com/<system>/Named_Boxarts/<percent-encoded name>.png ("" when the platform or title is unusable).
[[nodiscard]] std::string libretro_candidate_url(const std::string &platform_id, const std::string &name);
[[nodiscard]] std::string percent_encode(const std::string &text);

} // namespace launcher
