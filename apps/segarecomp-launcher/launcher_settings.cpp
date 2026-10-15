#include "launcher_settings.hpp"

#include "json_lite.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace launcher {

LauncherSettings default_launcher_settings() { return {}; }

LauncherSettings load_launcher_settings(const fs::path &file) {
  LauncherSettings s;
  std::ifstream in(file, std::ios::binary);
  if (!in) return s;
  std::ostringstream text;
  text << in.rdbuf();
  const auto root = json::parse(text.str());
  if (!root || !root->is_object()) return s;  // damaged: defaults, the file is rewritten on the next change
  const std::string view = root->str("view");
  if (view == "list" || view == "grid") s.view = view;
  const std::string sort = root->str("sort");
  for (const char *known : {"last_played", "title", "last_built", "added", "size"})
    if (sort == known) s.sort = sort;
  s.compact = root->flag("compact");
  s.adopt_cache_on_start = root->flag("adopt_cache_on_start", true);
  s.keep_old_artifacts = root->flag("keep_old_artifacts", false);
  const std::string mode = root->str("cleanup_mode");
  for (const char *known : {"never", "intermediates", "keep_last_n", "soft_limit"})
    if (mode == known) s.cleanup.mode = mode;
  s.cleanup.keep_last_n = static_cast<int>(std::clamp(root->num("keep_last_n", 10), 1LL, 500LL));
  s.cleanup.soft_limit_mb = std::clamp(root->num("soft_limit_mb", 4096), 256LL, 1048576LL);
  if (const json::Value *regions = root->get("preferred_regions"); regions != nullptr && regions->is_array()) {
    std::vector<std::string> list;
    for (const auto &r : regions->items)
      if (r.kind == json::Value::Kind::string && !r.string.empty() && r.string.size() <= 24 && list.size() < 8 && std::find(list.begin(), list.end(), r.string) == list.end()) list.push_back(r.string);
    if (!list.empty()) s.preferred_regions = list;
  }
  return s;
}

bool save_launcher_settings(const LauncherSettings &s, const fs::path &file) {
  json::Value root = json::Value::make_object();
  root.set("schema", json::Value::make_int(1));
  root.set("view", json::Value::make_string(s.view));
  root.set("sort", json::Value::make_string(s.sort));
  root.set("compact", json::Value::make_bool(s.compact));
  root.set("adopt_cache_on_start", json::Value::make_bool(s.adopt_cache_on_start));
  root.set("keep_old_artifacts", json::Value::make_bool(s.keep_old_artifacts));
  root.set("cleanup_mode", json::Value::make_string(s.cleanup.mode));
  root.set("keep_last_n", json::Value::make_int(s.cleanup.keep_last_n));
  root.set("soft_limit_mb", json::Value::make_int(s.cleanup.soft_limit_mb));
  json::Value regions = json::Value::make_array();
  for (const auto &r : s.preferred_regions) regions.push(json::Value::make_string(r));
  root.set("preferred_regions", std::move(regions));
  const std::string text = json::dump(root);
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = fs::path(file).concat(".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) { fs::remove(tmp, ec); return false; }
  }
  fs::rename(tmp, file, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  return true;
}

} // namespace launcher
