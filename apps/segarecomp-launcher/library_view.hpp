#pragma once

// Pure, window-free pieces of the launcher's library view: the box-art frame proportion and fitting, the region column and the build queue.
// Header-only so a unit test can exercise them without ImGui.

#include <algorithm>
#include <string>
#include <vector>

namespace launcher {

// Standard box-art proportion (portrait 479x680), width / height.
inline constexpr float kCoverAspect = 479.0F / 680.0F;

struct FitRect { float x0, y0, x1, y1; };
// The largest centred rectangle of a `tex_w x tex_h` image inside a `frame_w x frame_h` frame that shows the WHOLE image (never cropped).
inline FitRect fit_inside(float frame_w, float frame_h, float tex_w, float tex_h) {
  if (frame_w <= 0 || frame_h <= 0 || tex_w <= 0 || tex_h <= 0) return {0, 0, frame_w, frame_h};
  const float scale = std::min(frame_w / tex_w, frame_h / tex_h), w = tex_w * scale, h = tex_h * scale;
  return {(frame_w - w) * 0.5F, (frame_h - h) * 0.5F, (frame_w + w) * 0.5F, (frame_h + h) * 0.5F};
}

// The library's region column: the Genesis run-time region of the game, the Master System baseline profile (the NTSC export console), else Unknown.
inline const char *region_label(const std::string &platform_id, const std::string &region) {
  if (platform_id == "master-system") return "Export";
  if (platform_id != "genesis") return "Unknown";
  if (region == "usa") return "USA";
  if (region == "japan") return "Japan";
  if (region == "europe") return "Europe";
  return "Unknown";
}

// FIFO of game ids waiting for the single build worker. A game appears at most once.
class BuildQueue {
public:
  bool push(const std::string &id) {
    if (contains(id)) return false;
    ids_.push_back(id);
    return true;
  }
  bool remove(const std::string &id) {
    const auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it == ids_.end()) return false;
    ids_.erase(it);
    return true;
  }
  [[nodiscard]] bool contains(const std::string &id) const { return std::find(ids_.begin(), ids_.end(), id) != ids_.end(); }
  [[nodiscard]] bool empty() const { return ids_.empty(); }
  [[nodiscard]] std::size_t size() const { return ids_.size(); }
  [[nodiscard]] std::string pop_front() {
    std::string id = ids_.front();
    ids_.erase(ids_.begin());
    return id;
  }
private:
  std::vector<std::string> ids_;
};

} // namespace launcher
