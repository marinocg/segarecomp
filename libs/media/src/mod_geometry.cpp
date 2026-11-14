// Geometry declarations (SEG-139-T004, ADR 0302): the aspect range and per-scene component coverage a package claims. Pure declaration processing: no image bytes, no clock, no platform or title knowledge.
// The host (libs/viewer) compares the claim with what each frame delivered; this file only validates the claim and renders it in the compact form the host reads.
#include "segarecomp/mod.hpp"

#include <algorithm>

namespace segarecomp::mod {

namespace {
constexpr unsigned kMaxTerm = 4096;

// "N:M": canonical decimal terms 1..4096, 4:3 <= N/M <= 32:9 (exact integer arithmetic).
bool parse_ratio(const std::string &t, unsigned &n, unsigned &d) {
  const std::size_t colon = t.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= t.size()) return false;
  const auto term = [](const std::string &s, unsigned &out) {
    if (s.empty() || s.size() > 4 || (s.size() > 1 && s[0] == '0')) return false;
    unsigned v = 0;
    for (const char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + static_cast<unsigned>(c - '0'); }
    out = v;
    return v >= 1 && v <= kMaxTerm;
  };
  if (!term(t.substr(0, colon), n) || !term(t.substr(colon + 1), d)) return false;
  return static_cast<unsigned long long>(n) * 3 >= static_cast<unsigned long long>(d) * 4 && static_cast<unsigned long long>(n) * 9 <= static_cast<unsigned long long>(d) * 32;
}

bool id_token(const std::string &s) {
  if (s.empty() || s.size() > 32) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}
}  // namespace

std::optional<Rejection> check_geometry(const Manifest &m) {
  if (m.aspect_max.empty()) {
    if (!m.aspect_min.empty()) return Rejection{"E_GEOMETRY_ASPECT", "geometry aspect_min needs aspect_max"};
    if (!m.geometry_scenes.empty()) return Rejection{"E_GEOMETRY_ASPECT", "geometry scenes need an aspect_max: a coverage claim without a supported range claims nothing checkable"};
    return std::nullopt;
  }
  unsigned mn = 4, md = 3, xn = 0, xd = 0;
  if (!parse_ratio(m.aspect_max, xn, xd)) return Rejection{"E_GEOMETRY_ASPECT", "geometry aspect_max must be N:M inside 4:3 to 32:9"};
  if (!m.aspect_min.empty() && !parse_ratio(m.aspect_min, mn, md)) return Rejection{"E_GEOMETRY_ASPECT", "geometry aspect_min must be N:M inside 4:3 to 32:9"};
  if (static_cast<unsigned long long>(mn) * xd > static_cast<unsigned long long>(xn) * md) return Rejection{"E_GEOMETRY_ASPECT", "geometry aspect_min is above aspect_max"};
  if (m.geometry_scenes.size() > kMaxGeometryScenes) return Rejection{"E_GEOMETRY_SCENE", "a package declares at most 32 geometry scenes"};
  for (std::size_t i = 0; i < m.geometry_scenes.size(); ++i) {
    const GeometryScene &s = m.geometry_scenes[i];
    if (!id_token(s.id)) return Rejection{"E_GEOMETRY_SCENE", "a geometry scene id must be a token of [a-z0-9_-], at most 32 characters"};
    if (i > 0 && !(m.geometry_scenes[i - 1].id < s.id)) return Rejection{"E_GEOMETRY_SCENE", "geometry scenes must be sorted by id ascending without duplicates"};
    if (s.components.empty()) return Rejection{"E_GEOMETRY_SCENE", "a geometry scene declares at least one component"};
    for (std::size_t j = 0; j < s.components.size(); ++j) {
      if (std::none_of(std::begin(kGeometryComponents), std::end(kGeometryComponents), [&](std::string_view c) { return c == s.components[j]; }))
        return Rejection{"E_GEOMETRY_SCENE", "the geometry component \"" + s.components[j] + "\" is not one of actors, hud, scenery"};
      if (j > 0 && !(s.components[j - 1] < s.components[j])) return Rejection{"E_GEOMETRY_SCENE", "geometry components must be sorted ascending without duplicates"};
    }
  }
  return std::nullopt;
}

std::string geometry_declaration_text(const Manifest &m) {
  if (m.aspect_max.empty() || check_geometry(m)) return "";
  std::string t;
  if (!m.aspect_min.empty()) t += "min=" + m.aspect_min + ";";
  t += "max=" + m.aspect_max;
  for (const GeometryScene &s : m.geometry_scenes) {
    t += ";scene=" + s.id + ":";
    for (std::size_t i = 0; i < s.components.size(); ++i) t += (i ? "+" : "") + s.components[i];
  }
  return t;
}

}  // namespace segarecomp::mod
