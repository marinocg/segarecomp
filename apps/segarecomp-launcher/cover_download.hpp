#pragma once

// Libretro box-art download (only on an explicit user action). Finds the thumbnail of a game by NAME, never by sending ROM bytes or hashes:
//   1. candidate names: the ROM file names (No-Intro sets are named exactly like the thumbnails), then the title in Title Case and as written,
//      with the region tags Libretro uses ("(USA, Europe)", "(USA)", "(World)", ...) in the user's preferred order;
//   2. when Libretro's directory listing of box arts can be read (cached for a week), the best entry whose base name matches is chosen
//      (exact candidate first, then preferred region, never a beta/prototype/pirate unless nothing else exists); otherwise the candidates are
//      tried one by one;
//   3. thumbnails.libretro.com first, then the libretro-thumbnails mirror on raw.githubusercontent.com; https only.
// Transfers use the operating system's curl (Windows 10+, macOS and Linux ship it); nothing is downloaded when it is missing.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace launcher {

// One HTTPS GET into `out`: 0 = saved (HTTP 200), 1 = not found (HTTP 404/410), 2 = unavailable (no curl, network, proxy, TLS, too large,
// timeout, any other status).
using HttpGet = std::function<int(const std::string &url, const std::filesystem::path &out)>;

struct CoverQuery {
  std::string platform_id;                     // genesis | master-system
  std::vector<std::string> titles;             // display title, detected (header) title
  std::vector<std::string> rom_names;          // tracked ROM paths; only their file names are used (extracted zip copies, named by digest, are skipped)
  std::vector<std::string> preferred_regions;  // e.g. USA, Europe, Japan
};

struct CoverFetch {
  bool ok = false;
  std::filesystem::path image;  // the downloaded file (in the work folder); validated by the caller with import_cover
  std::string matched_name;     // the Libretro thumbnail name used
  std::string source;           // host the image came from
  bool used_index = false;      // matched against the directory listing (else: candidate probing)
  std::string error;            // plain reason when !ok
};

// Candidate thumbnail names in priority order (unique, at most `limit`). Pure; unit tested.
[[nodiscard]] std::vector<std::string> libretro_cover_candidates(const CoverQuery &query, std::size_t limit = 24);
// The comparable base of a name: lower case letters and digits only, without "(...)" / "[...]" tags, "&" read as "and". Pure.
[[nodiscard]] std::string libretro_base_key(const std::string &name);
// Box-art names (without ".png") from a Libretro directory listing page (autoindex HTML). Pure.
[[nodiscard]] std::vector<std::string> parse_libretro_listing(const std::string &html);
// The best listed name for the query, or "" (exact candidate, then base-name match ranked by region preference and release status). Pure.
[[nodiscard]] std::string pick_libretro_name(const CoverQuery &query, const std::vector<std::string> &listing);
// Libretro's character substitution for thumbnail file names (&*/:`<>?\| become '_').
[[nodiscard]] std::string libretro_safe_name(const std::string &name);

// The whole lookup (blocking: call it from a worker). `work_dir` holds the listing cache and the downloaded file.
[[nodiscard]] CoverFetch fetch_libretro_cover(const CoverQuery &query, const std::filesystem::path &work_dir, const HttpGet &get);
// The system curl, or "" when this machine has none.
[[nodiscard]] std::filesystem::path system_curl();
// HttpGet through the system curl: fixed argument list (no shell), https only also across redirects, 2 MiB and 30 s limits.
[[nodiscard]] HttpGet curl_http_get();

} // namespace launcher
