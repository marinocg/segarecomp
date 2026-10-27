#pragma once

// Small drawing/formatting helpers shared by the launcher's screens (inline: no unused-function noise in any translation unit).

#include "ui_kit.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {
namespace uih {

using namespace ui;
namespace fs = std::filesystem;

inline std::string u8s(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

inline std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

inline std::string fmt_date(long long secs) {
  if (secs <= 0) return "unknown";
  SDL_DateTime dt;
  if (!SDL_TimeToDateTime(static_cast<SDL_Time>(secs) * 1000000000LL, &dt, true)) return "unknown";
  char buf[48];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
  return buf;
}

inline std::string fmt_relative(long long secs, long long now) {
  if (secs <= 0) return "never";
  const long long d = std::max(0LL, now - secs);
  if (d < 90) return "just now";
  if (d < 3600) return std::to_string(d / 60) + " min ago";
  if (d < 86400) return std::to_string(d / 3600) + " h ago";
  if (d < 86400 * 60) return std::to_string(d / 86400) + " d ago";
  return std::to_string(d / (86400 * 30)) + " mo ago";
}

inline std::vector<std::string> wrap(ImFont *font, float px, float max_w, const std::string &text) {
  max_w = std::max(1.0F, max_w);
  std::vector<std::string> lines;
  std::string line, word;
  const auto flush_word = [&] {
    if (word.empty()) return;
    // Digests and paths have no spaces. Split at UTF-8 boundaries rather than letting them
    // paint over adjacent columns (or trimming a multibyte character in half).
    if (text_width(font, px, word.c_str()) > max_w) {
      if (!line.empty()) { lines.push_back(line); line.clear(); }
      while (text_width(font, px, word.c_str()) > max_w) {
        std::size_t end = 0, fit = 0;
        while (end < word.size()) {
          ++end;
          while (end < word.size() && (static_cast<unsigned char>(word[end]) & 0xC0U) == 0x80U) ++end;
          if (fit != 0 && text_width(font, px, word.substr(0, end).c_str()) > max_w) break;
          fit = end;
        }
        lines.push_back(word.substr(0, fit));
        word.erase(0, fit);
      }
    }
    const std::string trial = line.empty() ? word : line + " " + word;
    if (text_width(font, px, trial.c_str()) <= max_w || line.empty()) line = trial;
    else { lines.push_back(line); line = word; }
    word.clear();
  };
  for (const char c : text) {
    if (c == '\n') { flush_word(); lines.push_back(line); line.clear(); }
    else if (c == ' ') flush_word();
    else word.push_back(c);
  }
  flush_word();
  if (!line.empty() || lines.empty()) lines.push_back(line);
  return lines;
}

// Draws wrapped text at the cursor and advances the ImGui cursor by its height.
inline float paragraph(Ctx &ctx, ImFont *font, float px, ImU32 color, float max_w, const std::string &text, float line_gap = 3.0F) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  float y = 0;
  for (const auto &line : wrap(font, px, max_w, text)) {
    draw_text(dl, font, px, color, ImVec2(origin.x, origin.y + y), line.c_str());
    y += px + ctx.u(line_gap);
  }
  ImGui::Dummy(ImVec2(max_w, std::max(y, 1.0F)));
  return y;
}

inline void label_value(Ctx &ctx, float label_w, float value_w, const char *label, const std::string &value, ImU32 color = col::text) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float px = ctx.u(12.0F);
  draw_text(dl, ctx.regular, px, col::dim, origin, label);
  float y = 0;
  for (const auto &line : wrap(ctx.regular, px, value_w, value)) { draw_text(dl, ctx.regular, px, color, ImVec2(origin.x + label_w, origin.y + y), line.c_str()); y += px + ctx.u(3.0F); }
  ImGui::Dummy(ImVec2(label_w + value_w, std::max(y, px) + ctx.u(4.0F)));
}

inline void heading(Ctx &ctx, const char *text, ImU32 color = col::text) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float px = ctx.u(12.5F);
  draw_text(ImGui::GetWindowDrawList(), ctx.display, px, color == col::text ? rgba(120, 205, 255) : color, origin, text);
  ImGui::Dummy(ImVec2(10.0F, px + ctx.u(12.0F)));
}

inline void page_header(Ctx &ctx, float w, const char *title, const char *subtitle) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  draw_text(dl, ctx.display, ctx.u(15.0F), rgba(120, 205, 255), p, title);
  // Keep the header height stable so page-specific actions can share its first row.
  // A long descriptive subtitle belongs in the page body, not behind the Back / sort controls.
  (void)subtitle;
  ImGui::Dummy(ImVec2(w, ctx.u(26.0F)));
}


inline void rule(Ctx &ctx, float w) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // A filled rectangle on whole pixels: a 1-px AddLine at a fractional y rasterizes with gaps on the software renderer.
  const float y = std::floor(origin.y + ctx.u(4.0F)), thick = std::max(1.0F, std::round(ctx.u(1.0F)));
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(std::floor(origin.x), y), ImVec2(std::floor(origin.x + w), y + thick), IM_COL32(70, 90, 140, 130));
  ImGui::Dummy(ImVec2(w, ctx.u(10.0F)));
}

inline void spinner(Ctx &ctx, ImDrawList *dl, ImVec2 c, float r, ImU32 color) {
  const float t = static_cast<float>(SDL_GetTicks()) / 1000.0F;
  dl->PathArcTo(c, r, t * 6.0F, t * 6.0F + 4.2F, 16);
  dl->PathStroke(color, 0, std::max(1.0F, ctx.u(2.0F)));
}

inline const char *platform_short(const std::string &platform_id) { return platform_id == "master-system" ? "Master System" : platform_id == "sega-cd" ? "Sega CD" : "Genesis"; }


} // namespace uih
} // namespace launcher
