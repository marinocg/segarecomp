#pragma once

// The launcher's own preferences (view, sort, cleanup policy, ...). Per-user, tiny, atomically written. The viewer's video/audio/control
// settings are a different file (libs/viewer sv_settings) that the Settings screen edits through the same shared code.

#include "storage.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

struct LauncherSettings {
  std::string view = "grid";            // grid | list
  std::string sort = "last_played";     // last_played | title | last_built | added | size
  bool compact = false;
  bool adopt_cache_on_start = true;     // read the product-owned cache folder once at start and adopt verifiable finished builds
  CleanupPolicy cleanup;
  std::vector<std::string> preferred_regions = {"USA", "Europe", "Japan"};
  bool provider_opt_in = false;         // reserved: no provider is packaged, the toggle is shown disabled
};

[[nodiscard]] LauncherSettings load_launcher_settings(const std::filesystem::path &file);
// Atomic (temporary file + rename). Returns false when the file cannot be written.
[[nodiscard]] bool save_launcher_settings(const LauncherSettings &settings, const std::filesystem::path &file);
[[nodiscard]] LauncherSettings default_launcher_settings();

} // namespace launcher
