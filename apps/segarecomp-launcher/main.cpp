// Segarecomp consumer launcher: pick or drop a ROM, recompile it with the packaged toolchain, play it.
// Thin UI over launcher_core; it owns no recompilation logic.

#include "launcher_core.hpp"
#include "pixel_assets.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <functional>
#include <future>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace launcher;

#ifndef SEGARECOMP_LAUNCHER_VERSION
#define SEGARECOMP_LAUNCHER_VERSION "dev"
#endif
#ifndef SEGARECOMP_ZIG_VERSION
#define SEGARECOMP_ZIG_VERSION "unknown"
#endif

namespace {

std::string u8s(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

fs::path from_u8(const char *text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text))); }

// ---- non-interactive mode (package smoke test): same core, no window ----

int headless(int argc, char **argv) {
  fs::path rom_path, report_path;
  std::string mapper;                  // Master System: explicit mapper declaration (never defaulted)
  std::string aot_policy = "compatibility";  // compatibility | optimized (Genesis); Compatibility is the default
  std::string aot_model = "v1";              // experimental opt-in of Optimized (v1 | gen2); forwarded verbatim to the product
  bool hint_discovery_arg = false;     // --hint-alias-discovery: advanced strict-H-interrupt preparation (default off)
  std::string region_arg;              // --region <usa|japan>: console region (run time, not part of the build identity)
  std::vector<std::string> game_args;  // forwarded to the launched game (automation passes a finite bound)
  bool run = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--build" && i + 1 < argc) rom_path = from_u8(argv[++i]);
    else if (arg == "--report" && i + 1 < argc) report_path = from_u8(argv[++i]);
    else if (arg == "--run") run = true;
    else if (arg == "--mapper" && i + 1 < argc) mapper = argv[++i];
    else if (arg == "--aot-policy" && i + 1 < argc && (std::string(argv[i + 1]) == "compatibility" || std::string(argv[i + 1]) == "optimized")) aot_policy = argv[++i];
    else if (arg == "--aot-model" && i + 1 < argc && (std::string(argv[i + 1]) == "v1" || std::string(argv[i + 1]) == "gen2")) aot_model = argv[++i];
    else if (arg == "--region" && i + 1 < argc && (std::string(argv[i + 1]) == "usa" || std::string(argv[i + 1]) == "japan" || std::string(argv[i + 1]) == "europe")) region_arg = argv[++i];
    else if (arg == "--hint-alias-discovery") hint_discovery_arg = true;
    else if (arg == "--game-arg" && i + 1 < argc) game_args.emplace_back(argv[++i]);
    else { std::fprintf(stderr, "usage: Segarecomp [--build <rom> [--mapper <sega|rom_only>] [--aot-policy <compatibility|optimized> [--aot-model <v1|gen2>]] [--hint-alias-discovery] [--run [--region <usa|japan|europe>] [--game-arg <arg>]...] [--report <file>]]\n"); return 2; }
  }
  if (aot_model != "v1" && aot_policy != "optimized") { std::fprintf(stderr, "--aot-model requires --aot-policy optimized\n"); return 2; }
  std::string report;
  const auto emit = [&](const std::string &line) { report += line + "\n"; std::puts(line.c_str()); };
  int rc = 0;
  const Layout layout = locate_layout();
  emit("layout_root=" + u8s(layout.root));
  emit("compiler=" + u8s(layout.cc));
  if (!layout.problem.empty()) { emit("result=failed problem=" + layout.problem); rc = 1; }
  else {
    RomView rom = inspect_rom_file(rom_path, layout);
    if (rom.platform_id == "master-system" && !mapper.empty()) rom.mapper = mapper;
    // The product CLI states whether the experimental Master System Optimized policy exists; a request for it is honoured only then.
    if (rom.platform_id == "master-system" && aot_model != "v1") { emit("result=failed problem=--aot-model is a Genesis option"); rc = 2; }
    if (rc == 0 && rom.platform_id == "master-system" && aot_policy == "optimized") rom.sms_optimized_available = sms_optimized_available_blocking(layout);
    if (rom.supports_optimized()) { rom.aot_policy = aot_policy; if (rom.platform_id == "genesis") rom.aot_model = aot_model; }
    else if (aot_policy == "optimized") emit("aot_policy_unavailable=Optimized is not available for this platform (building Compatibility)");
    if (!region_arg.empty() && rom.supports_region()) rom.region = region_arg;
    if (hint_discovery_arg && rom.supports_hint_alias_discovery()) rom.hint_alias_discovery = true;
    if (rc != 0) {}
    else if (!rom.error.empty()) { emit("result=failed problem=" + rom.error); rc = 1; }
    else {
      emit("rom_sha256=" + rom.sha256);
      emit("platform=" + (rom.platform_id.empty() ? std::string("unsupported") : rom.platform_id));
      if (rom.platform_id == "master-system")
        emit(std::string("mapper=") + (!rom.mapper.empty() ? rom.mapper : rom.mapper_manifest.empty() ? "undeclared" : "manifest"));
      // Build (or reuse) the entry of `view` and report its machine-readable policy; returns the entry or an empty path on failure.
      const auto prepare = [&](const RomView &view, const char *tag) -> fs::path {
        const fs::path entry = entry_dir(view, layout);
        emit(std::string(tag) + "aot_policy_requested=" + view.aot_policy);
        if (view.optimized() && view.platform_id == "genesis") emit(std::string(tag) + "aot_policy_requested_model=" + view.aot_model);
        const bool cached = entry_ready_for(entry, view, layout);
        emit(std::string(tag) + "cache=" + (cached ? "hit" : "miss"));
        if (view.optimized() && rom_policy_identity(layout, view).empty())
          emit(std::string(tag) + "cache_note=policy_identity_unavailable (cached Optimized builds are not reused; this build is fresh)");
        if (!cached) {
          BuildJob job(view, layout);
          job.wait();
          emit(std::string(tag) + "build=" + (job.succeeded() ? "ok" : "failed"));
          if (!job.succeeded()) {
            emit(std::string(tag) + "message=" + job.message());
            emit(std::string(tag) + "entry=" + u8s(entry));  // as before: the (absent) entry is still reported after a failed build
            return {};
          }
        }
        emit(std::string(tag) + "entry=" + u8s(entry));
        const PolicyReport policy = read_policy_report(entry);
        if (policy.present) {
          emit(std::string(tag) + "aot_policy_effective=" + policy.effective);
          emit(std::string(tag) + "aot_policy_fallback=" + (policy.fallback ? "yes" : "no"));
          emit(std::string(tag) + "aot_policy_reason=" + policy.reason);
          emit(std::string(tag) + "aot_policy_fallback_class=" + policy.fallback_class);
          emit(std::string(tag) + "aot_policy_model_family=" + policy.model_family);
          emit(std::string(tag) + "aot_policy_validator=" + policy.validator);
          emit(std::string(tag) + "aot_policy_selected_ppm=" + std::to_string(policy.selected_ppm));
          emit(std::string(tag) + "aot_policy_admitted_ppm=" + std::to_string(policy.admitted_ppm));
          emit(std::string(tag) + "aot_policy_policy_identity=" + policy.policy_identity);
          emit(std::string(tag) + "aot_policy_identity=" + policy.identity);
        }
        return entry;
      };
      fs::path entry = prepare(rom, "");
      if (entry.empty()) rc = 3;
      if (rom.supports_region()) emit("region=" + rom.region);
      if (rc == 0 && run && !launch_refusal(rom).empty()) { emit("run=refused reason=" + launch_refusal(rom)); rc = 5; }
      else if (rc == 0 && run) {
        // Run; a Master System Optimized program that stops on the typed unadmitted_pc outcome restarts ONCE, from reset, in Compatibility.
        RomView current = rom;
        bool restarted = false;
        for (;;) {
          GameRun game(current, layout, entry, game_args);
          if (!game.started()) { emit("run=failed_to_start"); rc = 4; break; }
          while (!game.poll()) SDL_Delay(50);
          emit("run=exited code=" + std::to_string(game.exit_code()));  // the historical line, unchanged
          const PolicyReport ran = read_policy_report(entry);
          emit("run_requested_mode=" + current.aot_policy);  // what was asked for
          emit("run_mode=" + effective_mode(ran));           // what actually ran (a build-time fallback runs Compatibility)
          const RestartDecision next = decide_after_exit(running_view(current, ran), game.exit_code(), restarted);
          if (!next.restart) { if (game.exit_code() != 0) rc = 4; break; }
          emit("run=unadmitted_pc");
          emit("fallback=compatibility_restart");
          emit("notice=" + next.notice);
          restarted = true;
          current = next.next;
          entry = prepare(current, "fallback_");
          if (entry.empty()) { rc = 3; break; }
        }
      }
    }
  }
  if (!report_path.empty()) { std::ofstream(report_path, std::ios::binary) << report; }
  return rc;
}

// ---- GUI ----

enum class State { NoRom, RomSelected, Building, Ready, Running, Failed };

struct Pending {
  std::mutex mutex;
  std::string path;   // set by the file dialog / drop, consumed on the UI thread
  std::string error;  // set when the dialog itself could not run (SDL_GetError() at the time); a plain
                      // cancel (files[0] == NULL, no error) leaves both empty and is silently ignored.
};

void dialog_callback(void *userdata, const char *const *files, int) {
  auto *pending = static_cast<Pending *>(userdata);
  std::lock_guard<std::mutex> lock(pending->mutex);
  if (!files) { pending->error = SDL_GetError(); return; }
  if (!files[0]) return;  // user cancelled; no feedback needed
  pending->path = files[0];
}

// ---- design-space layout (points at the reference window size; independent of DPI/resize) ----

constexpr float LW = 560.0F;   // logical design canvas: matches the pixel-art reference's proportions
constexpr float LH = 760.0F;

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// Palette, taken from the pixel-art reference (deliberately limited: one interactive accent family,
// the wordmark/background art carry the rest of the color).
const ImU32 panel_fill        = rgba(8, 20, 47);      // #08142F
const ImU32 panel_fill_hover  = rgba(13, 28, 62);
const ImU32 panel_border_dark = rgba(7, 16, 38);       // #071026
const ImU32 panel_edge        = rgba(35, 62, 120);     // #233E78
const ImU32 panel_highlight   = rgba(77, 103, 167);    // #4D67A7
const ImU32 dash_color        = rgba(94, 132, 209, 215);
const ImU32 dash_color_hover  = rgba(70, 220, 255, 255);
const ImU32 text_primary      = rgba(220, 232, 255);   // #DCE8FF
const ImU32 text_secondary    = rgba(169, 183, 221);   // #A9B7DD
const ImU32 text_dim          = rgba(120, 133, 170);
const ImU32 text_footer       = rgba(202, 211, 235, 255);   // brighter than text_dim: footer/legal text over busy background art
const ImU32 col_ok            = rgba(104, 224, 158);
const ImU32 col_err           = rgba(255, 129, 119);
const ImU32 link_normal       = rgba(62, 167, 255);    // #3EA7FF
const ImU32 link_hover        = rgba(121, 203, 255);   // #79CBFF
const ImU32 link_pressed      = rgba(37, 131, 217);    // #2583D9
const ImU32 btn_fill          = rgba(21, 87, 201);     // #1557C9
const ImU32 btn_fill_hover    = rgba(27, 111, 231);    // #1B6FE7
const ImU32 btn_top_highlight = rgba(64, 191, 245);    // #40BFF5
const ImU32 btn_inner_edge    = rgba(38, 141, 238);    // #268DEE
const ImU32 btn_bottom_shadow = rgba(8, 41, 110);      // #08296E
const ImU32 btn_outer_shadow  = rgba(3, 21, 56);       // #031538
const ImU32 btn_text          = rgba(244, 247, 255);   // #F4F7FF
const ImU32 focus_ring        = rgba(140, 224, 255);

// A design-space -> current-window-point-space transform. Fonts, drop shadows and dashed borders are all
// drawn through this so the whole composition scales uniformly (letterboxed, centered) as the window resizes,
// exactly as the DPI transform in `SDL_SetRenderScale` (applied at present time) then maps points -> pixels.
struct Layout2D {
  float scale, ox, oy;
  [[nodiscard]] ImVec2 at(float x, float y) const { return ImVec2(ox + x * scale, oy + y * scale); }
  [[nodiscard]] float len(float v) const { return v * scale; }
};

float text_width(ImFont *font, float px, const char *text) {
  // Measured at exactly `px` (what AddText draws), independent of any ImGui font scaling.
  return font->CalcTextSizeA(px, FLT_MAX, 0.0F, text).x;
}

// Greedy word wrap into at most two lines of `max_px` (device pixels); an overlong last line ends in "...", an overlong word is cut.
std::vector<std::string> wrap_two_lines(ImFont *font, float px, float max_px, const std::string &text) {
  const auto fits = [&](const std::string &candidate) { return text_width(font, px, candidate.c_str()) <= max_px; };
  const auto shorten = [&](std::string line, bool ellipsis) {
    if (fits(line + (ellipsis ? "..." : ""))) return ellipsis ? line + "..." : line;
    while (line.size() > 1 && !fits(line + (ellipsis ? "..." : ""))) line.pop_back();
    return ellipsis ? line + "..." : line;
  };
  std::vector<std::string> lines;
  std::string current, rest;
  std::istringstream words(text);
  std::string word;
  bool overflow = false;
  while (words >> word) {
    if (overflow) { rest += (rest.empty() ? "" : " ") + word; continue; }
    const std::string trial = current.empty() ? word : current + " " + word;
    if (fits(trial)) { current = trial; continue; }
    if (lines.empty()) {  // first line is full: continue on the second
      if (current.empty()) { current = shorten(word, false); rest = word.substr(std::min(word.size(), current.size())); }
      lines.push_back(current);
      current.clear();
      if (!rest.empty()) { current = rest; rest.clear(); if (!fits(current)) { overflow = true; rest = ""; } continue; }
      current = word;
    } else {
      overflow = true;
      rest = word;
    }
  }
  if (!current.empty() || lines.empty()) lines.push_back(current);
  if (overflow && lines.size() == 2) lines[1] = shorten(lines[1] + " " + rest, true);
  else if (lines.size() == 2 && !fits(lines[1])) lines[1] = shorten(lines[1], true);
  if (lines.size() > 2) lines.resize(2);
  return lines;
}

void draw_text(ImDrawList *dl, ImFont *font, float px, ImU32 color, ImVec2 pos, const char *text) {
  dl->AddText(font, px, pos, color, text);
}

void draw_text_centered(ImDrawList *dl, ImFont *font, float px, ImU32 color, float center_x, float top_y, const char *text) {
  draw_text(dl, font, px, color, ImVec2(center_x - text_width(font, px, text) * 0.5F, top_y), text);
}

// Chunky beveled pixel panel (stepped light top/left, dark bottom/right edges) -- the drop-target frame and
// the primary button both use this, just at different sizes. `rounding` stays tiny so corners read as a
// pixel step rather than a smooth modern radius.
void draw_bevel_frame(ImDrawList *dl, ImVec2 a, ImVec2 b, float step, ImU32 outer, ImU32 edge, ImU32 top_left,
                      ImU32 bottom_right, ImU32 fill, float rounding) {
  dl->AddRectFilled(a, b, outer, rounding);
  const ImVec2 i0(a.x + step, a.y + step), i1(b.x - step, b.y - step);
  dl->AddRectFilled(i0, i1, edge, rounding * 0.6F);
  dl->AddLine(ImVec2(i0.x, i0.y), ImVec2(i1.x, i0.y), top_left, step);
  dl->AddLine(ImVec2(i0.x, i0.y), ImVec2(i0.x, i1.y), top_left, step);
  dl->AddLine(ImVec2(i0.x, i1.y), ImVec2(i1.x, i1.y), bottom_right, step);
  dl->AddLine(ImVec2(i1.x, i0.y), ImVec2(i1.x, i1.y), bottom_right, step);
  const ImVec2 f0(i0.x + step, i0.y + step), f1(i1.x - step, i1.y - step);
  dl->AddRectFilled(f0, f1, fill, rounding * 0.4F);
}

// Short rectangular dashes, no antialiasing niceties -- the inner drop-target boundary.
void draw_dashed_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash, float gap) {
  for (float x = a.x; x < b.x; x += dash + gap) {
    const float x2 = std::min(x + dash, b.x);
    dl->AddLine(ImVec2(x, a.y), ImVec2(x2, a.y), color, thickness);
    dl->AddLine(ImVec2(x, b.y), ImVec2(x2, b.y), color, thickness);
  }
  for (float y = a.y; y < b.y; y += dash + gap) {
    const float y2 = std::min(y + dash, b.y);
    dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, y2), color, thickness);
    dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, y2), color, thickness);
  }
}

// A 16-bit-menu-style beveled push button. Owns its own hit box (ImGui::InvisibleButton), so Tab reaches it
// and Enter/Space activates it like any other ImGui widget (ImGuiConfigFlags_NavEnableKeyboard is enabled
// once in gui()); a keyboard focus ring is drawn outside the bevel, never color-only.
bool bevel_button(const Layout2D &L, const char *id, float x, float y, float w, float h, const char *label,
                  ImFont *font, float label_px) {
  ImGui::SetCursorPos(L.at(x, y));
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##b", ImVec2(L.len(w), L.len(h)));
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
  const float step = L.len(2.5F);
  const ImU32 fill = hovered || active ? btn_fill_hover : btn_fill;
  const ImU32 top_left = active ? btn_bottom_shadow : btn_top_highlight;
  const ImU32 bottom_right = active ? btn_top_highlight : btn_bottom_shadow;
  draw_bevel_frame(dl, p0, p1, step, btn_outer_shadow, btn_inner_edge, top_left, bottom_right, fill, L.len(3));
  if (!active) dl->AddLine(ImVec2(p0.x + step * 2, p0.y + step * 2), ImVec2(p1.x - step * 2, p0.y + step * 2),
                           btn_top_highlight, L.len(1));
  const float lw = text_width(font, L.len(label_px), label);
  const ImVec2 center((p0.x + p1.x) * 0.5F - lw * 0.5F, (p0.y + p1.y) * 0.5F - L.len(label_px) * 0.58F);
  const ImVec2 text_pos = active ? ImVec2(center.x + L.len(1.5F), center.y + L.len(1.5F)) : center;
  draw_text(dl, font, L.len(label_px), btn_text, text_pos, label);
  if (focused) dl->AddRect(ImVec2(p0.x - step, p0.y - step), ImVec2(p1.x + step, p1.y + step), focus_ring,
                           L.len(4), 0, L.len(1.5F));
  return pressed;
}

// Flatter secondary/outline action -- same hit-testing/focus contract as bevel_button, lower visual weight.
bool outline_button(const Layout2D &L, const char *id, float x, float y, float w, float h, const char *label,
                    ImFont *font, float label_px) {
  ImGui::SetCursorPos(L.at(x, y));
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##b", ImVec2(L.len(w), L.len(h)));
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
  dl->AddRectFilled(p0, p1, hovered ? panel_fill_hover : panel_fill, L.len(3));
  dl->AddRect(p0, p1, hovered ? panel_highlight : panel_edge, L.len(3), 0, L.len(1.5F));
  const float lw = text_width(font, L.len(label_px), label);
  draw_text(dl, font, L.len(label_px), text_primary, ImVec2((p0.x + p1.x) * 0.5F - lw * 0.5F, (p0.y + p1.y) * 0.5F - L.len(label_px) * 0.58F), label);
  if (focused) dl->AddRect(ImVec2(p0.x - L.len(2), p0.y - L.len(2)), ImVec2(p1.x + L.len(2), p1.y + L.len(2)),
                           focus_ring, L.len(4), 0, L.len(1.5F));
  return pressed;
}

// A desktop-chrome text link (the footer's "Open cache folder"): icon + text, underlines on hover, no button box.
bool text_link(const Layout2D &L, const char *id, float center_x, float y, SDL_Texture *icon, const char *label,
              ImFont *font, float px) {
  const float icon_size = icon ? px * 0.95F : 0.0F, gap = icon ? 6.0F : 0.0F;
  const float lw_design = text_width(font, L.len(px), label) / L.scale;
  const float total = icon_size + gap + lw_design;
  const float x = center_x - total * 0.5F;
  ImGui::SetCursorPos(L.at(x, y - 2.0F));
  ImGui::PushID(id);
  const bool pressed = ImGui::InvisibleButton("##link", ImVec2(L.len(total), L.len(px + 6.0F)));
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), focused = ImGui::IsItemFocused();
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 color = active ? link_pressed : (hovered ? link_hover : link_normal);
  const ImVec2 p0 = ImGui::GetItemRectMin();
  if (icon) dl->AddImage(reinterpret_cast<ImTextureID>(icon), ImVec2(p0.x, p0.y + L.len(1)),
                         ImVec2(p0.x + L.len(icon_size), p0.y + L.len(icon_size) + L.len(1)));
  const ImVec2 text_pos(p0.x + L.len(icon_size + gap), p0.y + L.len(3));
  draw_text(dl, font, L.len(px), color, text_pos, label);
  if (hovered) {
    const float tw = text_width(font, L.len(px), label);
    dl->AddLine(ImVec2(text_pos.x, text_pos.y + L.len(px) + L.len(1)), ImVec2(text_pos.x + tw, text_pos.y + L.len(px) + L.len(1)), color, L.len(1));
  }
  if (focused) dl->AddRect(ImVec2(p0.x - L.len(2), p0.y - L.len(2)),
                           ImVec2(p0.x + L.len(total) + L.len(2), p0.y + L.len(px + 6) + L.len(2)), focus_ring, L.len(3), 0, L.len(1.2F));
  return pressed;
}

// Build-stage row: check for done, spinning arc for running, hollow dot for pending. Left aligned.
void draw_stage_row(const Layout2D &L, ImDrawList *dl, float x, float y, const char *label, StageState state,
                    ImFont *font, float t) {
  const ImVec2 c = L.at(x + 7.0F, y + 9.0F);
  const float r = L.len(7.0F);
  if (state == StageState::done) {
    dl->PathLineTo(ImVec2(c.x - r * 0.7F, c.y)); dl->PathLineTo(ImVec2(c.x - r * 0.1F, c.y + r * 0.6F));
    dl->PathLineTo(ImVec2(c.x + r * 0.85F, c.y - r * 0.55F));
    dl->PathStroke(col_ok, 0, L.len(2));
  } else if (state == StageState::running) {
    dl->PathArcTo(c, r, t * 6.0F, t * 6.0F + 4.2F, 16);
    dl->PathStroke(link_hover, 0, L.len(2));
  } else {
    dl->AddCircle(c, r * 0.62F, text_dim, 16, L.len(1.3F));
  }
  draw_text(dl, font, L.len(15), state == StageState::pending ? text_dim : text_primary, L.at(x + 26.0F, y), label);
}

// Debug aid for reviewing the layout without a display: SEGARECOMP_SHOT=<file.bmp> saves a frame and exits.
void maybe_save_screenshot(SDL_Renderer *renderer, int frame) {
  const char *path = SDL_getenv("SEGARECOMP_SHOT");
  if (!path || frame < 12) return;
  if (SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr)) { SDL_SaveBMP(surface, path); SDL_DestroySurface(surface); }
  SDL_Event quit; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
}

int gui() {
  if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return 1; }
  float content_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (content_scale < 1.0F) content_scale = 1.0F;
  // Window size is in points (screen coordinates), never pre-multiplied by the display content scale --
  // SDL_WINDOW_HIGH_PIXEL_DENSITY alone gives a full-resolution backbuffer on a Retina/HiDPI display.
  SDL_Window *window = SDL_CreateWindow("Segarecomp", static_cast<int>(LW), static_cast<int>(LH),
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  // Initial size honours the display scale. SDL3 sizes windows in "points": on macOS a point is already a logical unit (pixel density 2),
  // but on a DPI-aware Windows/Linux window points are physical pixels (density 1) and a 4K display at 150-200% scaling would get a tiny
  // window. The needed factor is display scale / pixel density. The layout letterboxes to the window, so a larger window just scales it.
  float window_scale = 1.0F;
  if (window) {
    const float display_scale = SDL_GetWindowDisplayScale(window);
    const float density = SDL_GetWindowPixelDensity(window);
    if (display_scale > 0.0F && density > 0.0F) window_scale = std::clamp(display_scale / density, 1.0F, 4.0F);
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable) && usable.h > 0 && LH * window_scale > static_cast<float>(usable.h) * 0.92F)
      window_scale = std::max(1.0F, static_cast<float>(usable.h) * 0.92F / LH);  // never taller than the screen
    if (window_scale > 1.01F) {
      SDL_SetWindowSize(window, static_cast<int>(LW * window_scale), static_cast<int>(LH * window_scale));
      SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(LW * 0.55F), static_cast<int>(LH * 0.55F));
  }
  SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) { std::fprintf(stderr, "window failed: %s\n", SDL_GetError()); return 1; }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Tab/Enter/Space reach and activate the custom buttons
  ImGui::StyleColorsDark();
  // Every size in this UI is already in final device pixels (design units * the window-fit scale, and the window itself is sized by the
  // display scale), so ImGui must not multiply fonts again: with FontScaleDpi = display scale, ImGui::PushFont-based text (and its measured
  // width) came out display-scale times larger than the AddText-drawn text, which shifted "centered" text left on scaled Windows displays.
  ImGui::GetStyle().FontScaleDpi = 1.0F;
  (void)content_scale;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  // Fonts: Silkscreen (bold for primary actionable labels, regular for everything else). ImGui 1.92's
  // dynamic font system rasterizes each PushFont(font, size) on demand, so one loaded instance covers every
  // size this layout needs -- no fixed-size atlas baking. A missing font file falls back to ImGui's built-in
  // font rather than failing the launcher (assets are best-effort presentation, never load-bearing).
  ImFont *font_builtin = io.Fonts->AddFontDefault();  // guaranteed present: never leave the atlas fontless
  const auto load_font = [&](const char *relative) -> ImFont * {
    const fs::path path = asset_path(relative);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return nullptr;
    return io.Fonts->AddFontFromFileTTF(path.string().c_str(), 0.0F);
  };
  ImFont *font_bold = load_font("fonts/Silkscreen-Bold.ttf");
  ImFont *font_regular = load_font("fonts/Silkscreen-Regular.ttf");
  if (!font_bold) font_bold = font_builtin;
  if (!font_regular) font_regular = font_builtin;

  SDL_Texture *tex_background = load_pixel_texture(renderer, "background.png");
  SDL_Texture *tex_wordmark = load_pixel_texture(renderer, "wordmark.png");
  SDL_Texture *tex_drop = load_pixel_texture(renderer, "icon-drop.png");
  SDL_Texture *tex_info = load_pixel_texture(renderer, "icon-info.png");
  SDL_Texture *tex_folder = load_pixel_texture(renderer, "icon-folder.png");
  float bg_w = 0, bg_h = 0, wm_w_px = 0, wm_h_px = 0;
  if (tex_background) { float w, h; SDL_GetTextureSize(tex_background, &w, &h); bg_w = w; bg_h = h; }
  if (tex_wordmark) { float w, h; SDL_GetTextureSize(tex_wordmark, &w, &h); wm_w_px = w; wm_h_px = h; }

  const Layout layout = locate_layout();
  Pending pending;
  State state = State::NoRom;
  RomView rom;
  fs::path entry;
  std::string failure;
  bool show_diagnostics = false, dragging = false;
  std::string diagnostics;
  std::unique_ptr<BuildJob> job;
  std::unique_ptr<GameRun> game;
  // AOT policy chosen in this session (Compatibility is the default and the only value unless the user picks Optimized).
  std::string policy_choice = "compatibility";      // Genesis
  std::string sms_policy_choice = "compatibility";  // Master System (experimental Optimized): never inherits the Genesis choice
  const auto chosen_policy = [&]() -> std::string& { return rom.platform_id == "master-system" ? sms_policy_choice : policy_choice; };
  // SEG-057-T002: whether the product CLI offers the experimental Master System Optimized policy. Probed once on a worker at start-up; until
  // (and unless) the product says yes, the control is not shown. The window never decides availability itself.
  bool sms_optimized_available = false;
  std::future<bool> sms_probe;
  if (layout.problem.empty()) sms_probe = std::async(std::launch::async, [lay = layout] { return sms_optimized_available_blocking(lay); });
  // The Optimized -> Compatibility restart (one-shot per Play): the visible notice, and a pending automatic start of the Compatibility program.
  std::string restart_notice;
  bool restart_pending = false, restarted_this_play = false;
  // Review/automation aids (like SEGARECOMP_SHOT_*): SEGARECOMP_GUI_AUTOPLAY=1 presses Play once the selected game is Ready and quits when the run
  // (including a Compatibility restart) is over; SEGARECOMP_GUI_TRACE=<file> appends one `key=value` line per window state change, so the real
  // window state machine can be tested without a display server. Neither is a user feature.
  const bool gui_autoplay = SDL_getenv("SEGARECOMP_GUI_AUTOPLAY") != nullptr;
  bool autoplay_pending = gui_autoplay, autoplay_started = false;
  const char *gui_trace_path = SDL_getenv("SEGARECOMP_GUI_TRACE");
  const auto trace = [&](const std::string &line) {
    if (gui_trace_path) std::ofstream(gui_trace_path, std::ios::app) << line << '\n';
  };
  bool discovery_choice = false;    // advanced H-interrupt alias discovery (default off)
  std::string model_choice = "v1";  // experimental opt-in of Optimized ("gen2"); v1 is the default
  std::string observed_region;  // z80.observed_region of the Ready entry (build-time Z80 observation); see region_coverage_note
  PolicyReport policy_report;  // of the Ready entry (read from its status.json), for the requested/effective note
  const auto refresh_report = [&] {
    policy_report = state == State::Ready ? read_policy_report(entry) : PolicyReport{};
    observed_region = state == State::Ready ? read_observed_region(entry) : std::string();
  };
  const auto fallback_details = [&] {
    diagnostics = (policy_report.fallback ? "Optimized was requested but this game was built as Compatibility (the broad, always-correct program).\n" : std::string()) +
                  policy_details(policy_report) + "\nThe build log and status.json in the cache folder hold the sanitized details.";
  };

  // Entry identity and the "is a matching build already there" check. For Optimized they run the product's policy-identity tool (a process
  // that can take seconds, e.g. cold start or antivirus scan on Windows), so they run on a worker and the UI shows a spinner meanwhile.
  // Lifetime/staleness contract: the worker captures COPIES only (no reference into this frame), is joined before SDL_Quit, and its result
  // carries the key of the selection it was computed for; a result whose key no longer matches the current selection is discarded and
  // recomputed. While it runs the controls are hidden, a file picked/dropped meanwhile is deferred (not lost), and the per-frame code never
  // starts the tool (it only reads the memo), so a failing tool is retried once per user action, never once per frame.
  struct Resolved { fs::path entry; bool ready = false; std::string key; };
  std::future<Resolved> resolve_job;
  bool resolving = false;
  std::string deferred_path;  // a ROM chosen while a lookup was running
  const auto selection_key = [&] {
    return rom.sha256 + "|" + rom.aot_policy + "|" + rom.aot_model + "|" + (rom.hint_alias_discovery ? "d" : "-") + "|" + rom.mapper + "|" + rom.mapper_manifest_sha256;
  };
  std::function<void()> resolve_entry;
  const auto apply_resolved = [&](const Resolved &r) {
    if (!r.key.empty() && r.key != selection_key()) { resolve_entry(); return; }  // stale: computed for another selection
    entry = r.entry;
    state = r.ready ? State::Ready : State::RomSelected;
    refresh_report();
  };
  resolve_entry = [&] {
    if (!rom.optimized()) {
      const fs::path dir = entry_dir(rom, layout);
      apply_resolved({dir, entry_ready_for(dir, rom, layout), std::string()});
      return;
    }
    state = State::RomSelected;
    resolving = true;
    resolve_job = std::async(std::launch::async, [snapshot = rom, lay = layout, key = selection_key()] {
      Resolved r;
      r.entry = entry_dir(snapshot, lay);
      r.ready = entry_ready_for(r.entry, snapshot, lay);
      r.key = key;
      return r;
    });
  };

  const auto select_rom = [&](const std::string &path_text) {
    if (state == State::Building || state == State::Running || resolving) return;
    rom = inspect_rom_file(from_u8(path_text.c_str()), layout);
    show_diagnostics = false;
    if (!rom.error.empty()) { failure = "That file could not be read: " + rom.error; diagnostics = failure; state = State::Failed; return; }
    restart_notice.clear();
    restart_pending = false;
    restarted_this_play = false;
    rom.sms_optimized_available = sms_optimized_available;
    if (rom.supports_optimized()) { rom.aot_policy = chosen_policy(); if (rom.platform_id == "genesis") rom.aot_model = model_choice; }
    if (rom.supports_hint_alias_discovery()) rom.hint_alias_discovery = discovery_choice;
    resolve_entry();
  };
  const auto set_discovery = [&](bool on) {
    if (rom.hint_alias_discovery == on) return;
    discovery_choice = on;
    rom.hint_alias_discovery = on;
    show_diagnostics = false;
    resolve_entry();
  };
  const auto set_policy = [&](const char *choice) {
    if (rom.aot_policy == choice) return;
    chosen_policy() = choice;
    rom.aot_policy = choice;
    restart_notice.clear();
    if (std::string(choice) == "compatibility" && rom.platform_id == "genesis") { model_choice = "v1"; rom.aot_model = "v1"; }  // the experimental opt-in never survives leaving Optimized
    show_diagnostics = false;
    resolve_entry();
  };
  const auto set_model = [&](const char *choice) {
    if (rom.aot_model == choice) return;
    model_choice = choice;
    rom.aot_model = choice;
    show_diagnostics = false;
    resolve_entry();
  };
  const auto start_build = [&](bool force) {
    if (force) { std::error_code ec; fs::remove_all(entry, ec); }
    job = std::make_unique<BuildJob>(rom, layout);
    state = State::Building;
    trace(std::string("build=started mode=") + rom.aot_policy);
  };
  // Starts the native program of the Ready entry (a new process, so always from reset).
  const auto launch_game = [&] {
    if (const std::string refusal = launch_refusal(rom); !refusal.empty()) { failure = refusal; diagnostics = refusal; state = State::Failed; return; }
    game = std::make_unique<GameRun>(rom, layout, entry);
    if (game->started()) { state = State::Running; trace(std::string("run=started mode=") + effective_mode(policy_report) + " restarted=" + (restarted_this_play ? "1" : "0") + " requested=" + rom.aot_policy +
                                                   " label=" + running_label(policy_report, restarted_this_play)); }
    else { failure = "The game could not be started."; diagnostics = failure; game.reset(); state = State::Failed; }
  };
  const auto browse = [&] {
    static const SDL_DialogFileFilter filters[] = {{"Genesis / Mega Drive ROMs", "md;bin;gen;smd"}, {"Master System ROMs", "sms"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(dialog_callback, &pending, window, filters, 3, nullptr, false);
  };
  if (const char *shot_policy = SDL_getenv("SEGARECOMP_SHOT_POLICY"); shot_policy && std::string(shot_policy) == "optimized") policy_choice = sms_policy_choice = "optimized";  // review aid
  if (const char *shot_model = SDL_getenv("SEGARECOMP_SHOT_MODEL"); shot_model && std::string(shot_model) == "gen2") model_choice = "gen2";  // review aid
  if (const char *preselect = SDL_getenv("SEGARECOMP_SHOT_ROM")) {  // review aid, see maybe_save_screenshot
    if (sms_probe.valid()) sms_optimized_available = sms_probe.get();  // the availability probe must have answered before the ROM is inspected
    select_rom(preselect);
    if (SDL_getenv("SEGARECOMP_SHOT_BUILD") && state == State::RomSelected) start_build(false);
    if (SDL_getenv("SEGARECOMP_SHOT_READY") && state == State::RomSelected) state = State::Ready;  // review aid: the Ready layout without a build
    if (const char *shot_region = SDL_getenv("SEGARECOMP_SHOT_REGION"); shot_region && rom.supports_region()) rom.region = shot_region;  // review aid
  }

  bool running = true;
  int frame = 0;
  while (running) {
    ++frame;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) running = false;
      if (event.type == SDL_EVENT_DROP_BEGIN) dragging = true;
      if (event.type == SDL_EVENT_DROP_COMPLETE) dragging = false;
      if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
        if (resolving) deferred_path = event.drop.data; else select_rom(event.drop.data);
      }
    }
    {
      std::string chosen, dialog_error;
      if (!resolving) {  // a pick made while a lookup runs stays queued in `pending` until it finishes
        std::lock_guard<std::mutex> lock(pending.mutex);
        chosen.swap(pending.path);
        dialog_error.swap(pending.error);
      }
      if (!chosen.empty()) select_rom(chosen);
      else if (!dialog_error.empty() && state != State::Building && state != State::Running) {
        rom = RomView{};
        rom.error = "file picker unavailable";  // non-empty: suppresses the Failed state's "Try again" button
        failure = "The file picker could not be opened.";
        diagnostics = "SDL: " + dialog_error +
                      "\n\nOn Linux this usually means no file-chooser service (XDG Desktop Portal) is "
                      "available -- common under WSL/WSLg, which does not ship one by default. Drop the "
                      "ROM onto this window instead.";
        show_diagnostics = true;
        state = State::Failed;
      }
    }
    if (resolving && resolve_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      resolving = false;
      apply_resolved(resolve_job.get());
    }
    if (!resolving && !deferred_path.empty()) { const std::string next = std::move(deferred_path); deferred_path.clear(); select_rom(next); }
    if (sms_probe.valid() && sms_probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      sms_optimized_available = sms_probe.get();
      rom.sms_optimized_available = sms_optimized_available;
    }
    if (state == State::Building && job->finished()) {
      if (job->succeeded()) {
        state = State::Ready;
        refresh_report();
        if (restart_pending) { restart_pending = false; launch_game(); }  // the Compatibility restart: start it as soon as it is built
      } else {
        failure = job->message();
        diagnostics = (restart_pending ? restart_notice + "\nThe Compatibility build then failed:\n" : std::string()) +
                      read_tail(entry.parent_path() / (entry.filename().string() + ".failed") / "build.log", 24000);
        trace(std::string("build=failed restart_pending=") + (restart_pending ? "1" : "0") + " message=" + failure);
        restart_pending = false;  // never retried automatically: the real failure is shown
        state = State::Failed;
      }
      job->wait();
    }
    if (state == State::Running && game->poll()) {
      const int exit_code = game->exit_code();
      const RestartDecision next = decide_after_exit(running_view(rom, policy_report), exit_code, restarted_this_play);
      game.reset();
      trace("run=exited code=" + std::to_string(exit_code) + " mode=" + effective_mode(policy_report));
      if (exit_code == 0) state = State::Ready;
      else if (next.restart) {
        // The typed unadmitted_pc stop of an Optimized program: tell the user, switch to Compatibility, and restart from reset (once).
        restart_notice = next.notice;
        trace("restart=compatibility notice=" + next.notice);
        restarted_this_play = true;
        rom = next.next;
        chosen_policy() = "compatibility";
        show_diagnostics = false;
        restart_pending = true;
        resolve_entry();  // Compatibility is synchronous: Ready (reuse) or RomSelected (build)
        if (state == State::Ready) { restart_pending = false; launch_game(); }
        else if (state == State::RomSelected) start_build(false);
      } else {
        failure = "The game stopped unexpectedly.";
        diagnostics = read_tail(entry / "run.log", 24000);
        trace("state=failed reason=game_stopped_unexpectedly");
        state = State::Failed;
      }
    }
    // Automation: press Play once when the selected game is Ready; quit when the run is over.
    if (gui_autoplay && !resolving) {
      if (autoplay_pending && state == State::Ready) {
        autoplay_pending = false;
        autoplay_started = true;
        trace(std::string("autoplay=press mode=") + rom.aot_policy);
        launch_game();
      } else if (autoplay_pending && state == State::RomSelected && rom.error.empty() && !rom.needs_mapper() && rom.supported_platform) {
        trace(std::string("autoplay=build mode=") + rom.aot_policy);
        start_build(false);  // the "Recompile & Play" press for a game with no matching build
      } else if (autoplay_started && !restart_pending && (state == State::Ready || state == State::Failed)) {
        trace(std::string("final=") + (state == State::Ready ? "ready" : "failed") + " mode=" + rom.aot_policy + " restarted=" + (restarted_this_play ? "1" : "0") +
              " entry_requested=" + policy_report.requested + " effective=" + effective_mode(policy_report));
        autoplay_started = false;  // reported once
        SDL_Event quit; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
      }
    }

    const float t = static_cast<float>(SDL_GetTicks()) / 1000.0F;
    // win_w/win_h (points): the SAME value SDL itself uses for mouse-event coordinates, so every widget's
    // drawn position and its InvisibleButton hit-rect always agree pixel-for-pixel. Do not substitute a
    // separately-derived value here (a previous version divided the renderer's output pixels by the display
    // content scale instead, which on a live-resized/moved window can round to a value a point or two off
    // from SDL's own window-point tracking -- invisible in the drawing, but enough to offset every click
    // target from what's drawn under it).
    int win_w_pt = 0, win_h_pt = 0;
    SDL_GetWindowSize(window, &win_w_pt, &win_h_pt);
    const float win_w = static_cast<float>(win_w_pt);
    const float win_h = static_cast<float>(win_h_pt);
    // Uniform scale-to-fit ("letterbox") of the fixed design canvas within the current window size, centered;
    // this is the only concession to resizing -- the background instead covers the full window (below), never
    // letterboxed, so resizing crops scenery rather than shrinking the composition into empty bars.
    const Layout2D L{std::min(win_w / LW, win_h / LH), 0, 0};
    Layout2D centered = L;
    centered.ox = (win_w - LW * L.scale) * 0.5F;
    centered.oy = (win_h - LH * L.scale) * 0.5F;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // ---- background: cover-fill (crop, never distort) the whole window, independent of the letterbox ----
    if (tex_background && bg_w > 0 && bg_h > 0) {
      const float cover = std::max(win_w / bg_w, win_h / bg_h);
      const float dw = bg_w * cover, dh = bg_h * cover;
      const float dx = (win_w - dw) * 0.5F, dy = (win_h - dh) * 0.5F;
      dl->AddImage(reinterpret_cast<ImTextureID>(tex_background), ImVec2(dx, dy), ImVec2(dx + dw, dy + dh));
    } else {
      dl->AddRectFilled(ImVec2(0, 0), ImVec2(win_w, win_h), rgba(8, 11, 26));
    }

    // ---- header: pixel-art wordmark (or a bold-text fallback if the asset failed to load), compact subtitle ----
    const float wm_w = LW * 0.58F, wm_h = wm_w_px > 0 ? wm_w * (wm_h_px / wm_w_px) : wm_w * 0.2F;
    const float wm_top = 46.0F;
    if (tex_wordmark)
      dl->AddImage(reinterpret_cast<ImTextureID>(tex_wordmark), centered.at((LW - wm_w) * 0.5F, wm_top),
                  centered.at((LW - wm_w) * 0.5F + wm_w, wm_top + wm_h));
    else
      draw_text_centered(dl, font_bold, centered.len(40), text_primary, centered.at(LW * 0.5F, 0).x, centered.at(0, wm_top + wm_h * 0.3F).y, "SEGARECOMP");
    const float subtitle_y = wm_top + wm_h + 14.0F;
    draw_text_centered(dl, font_regular, centered.len(15), text_secondary, centered.at(LW * 0.5F, 0).x, centered.at(0, subtitle_y).y,
                       "Recompile Sega games to native code.");

    // ---- the ROM drop / status panel: same outer frame for every state, only the inner content changes ----
    const float panel_x0 = LW * 0.10F, panel_x1 = LW * 0.90F, panel_w = panel_x1 - panel_x0;
    const float panel_y0 = subtitle_y + 38.0F;
    const float content_x = panel_x0 + 22.0F;
    const float content_w = panel_w - 44.0F;
    const float center_x = (panel_x0 + panel_x1) * 0.5F;

    // Game title: cleaned in the core, wrapped to at most two lines. The header block (title, platform line) pushes everything below it.
    std::vector<std::string> title_lines;
    if (state != State::NoRom) title_lines = wrap_two_lines(font_bold, centered.len(18), centered.len(content_w), rom.title);
    const float title_pitch = 24.0F;
    const float header_h = 24.0F + title_pitch * static_cast<float>(std::max<std::size_t>(title_lines.size(), 1)) + 22.0F;
    const float row_y = panel_y0 + header_h;  // first free line below the title and platform line

    const bool settings_state = layout.problem.empty() && state != State::NoRom &&
                                ((state == State::RomSelected && rom.supported_platform && !rom.needs_mapper()) || state == State::Ready);
    // One flowing column for the ROM-selected and Ready states: status, one aligned action row, then labelled settings rows.
    // Run once with draw=false to measure the panel, once with draw=true to draw and handle input, so the frame always fits its content.
    const auto settings_column = [&](bool draw) -> float {
      float y = row_y;
      const float lx = content_x, bx = content_x + 62.0F;  // label column / button column
      if (resolving) {  // the Optimized lookup runs on a worker: animated feedback instead of a frozen window
        if (draw) {
          draw_stage_row(centered, dl, lx, y + 14.0F, rom.optimized_gen2() ? "Preparing the experimental model..." : "Preparing Optimized mode...", StageState::running, font_regular, t);
          draw_text(dl, font_regular, centered.len(11), text_dim, centered.at(lx, y + 44.0F), "Checking the build tool; this can take a few seconds.");
        }
        return y + 80.0F - panel_y0;
      }
      const auto separator = [&] {
        if (draw) dl->AddLine(centered.at(content_x, y), centered.at(content_x + content_w, y), rgba(70, 90, 140, 130), centered.len(1));
        y += 12.0F;
      };
      if (state == State::Ready) {
        if (draw) draw_text(dl, font_bold, centered.len(14), col_ok, centered.at(lx, y), "Native build ready");
        if (draw && policy_report.present && policy_report.requested == "optimized" &&
            outline_button(centered, "fb_details", content_x + content_w - 70.0F, y - 2.0F, 70.0F, 22.0F, policy_report.fallback ? "Why?" : "Details", font_regular, 11.0F)) {
          show_diagnostics = !show_diagnostics;
          fallback_details();
        }
        y += 22.0F;
        if (const std::string note = policy_summary(policy_report); !note.empty()) {
          if (draw) draw_text(dl, font_regular, centered.len(12), policy_report.fallback ? col_err : text_secondary, centered.at(lx, y), note.c_str());
          y += 18.0F;
        }
        if (!restart_notice.empty()) {  // the Optimized program stopped on omitted code: say so, and that Compatibility is what ran after it
          if (draw) {
            ImGui::SetCursorPos(centered.at(lx, y));
            ImGui::PushTextWrapPos(centered.at(content_x + content_w, 0).x);
            ImGui::PushStyleColor(ImGuiCol_Text, col_err);
            ImGui::PushFont(font_regular, centered.len(11));
            ImGui::TextUnformatted(restart_notice.c_str());
            ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
          }
          y += 42.0F;
        }
      }
      y += 6.0F;
      // Actions: one row, one height, same gaps.
      if (draw) {
        if (state == State::Ready) {
          if (bevel_button(centered, "play", lx, y, 126.0F, 42.0F, "Play", font_bold, 17.0F)) {
            restart_notice.clear();
            restarted_this_play = false;
            launch_game();
          }
          if (outline_button(centered, "recompile2", lx + 136.0F, y, 126.0F, 42.0F, "Recompile", font_regular, 14.0F)) start_build(true);
          if (outline_button(centered, "choose2", lx + 272.0F, y, 126.0F, 42.0F, "Choose...", font_regular, 14.0F)) browse();
        } else {
          if (bevel_button(centered, "recompile", lx, y, 200.0F, 42.0F, rom.compat_known ? "Recompile & Play" : "Try anyway", font_bold, 16.0F)) start_build(false);
          if (outline_button(centered, "choose1", lx + 210.0F, y, 190.0F, 42.0F, "Choose another...", font_regular, 14.0F)) browse();
        }
      }
      y += 42.0F + 14.0F;
      separator();
      if (rom.supports_optimized()) {
        if (draw) {
          draw_text(dl, font_regular, centered.len(12), text_secondary, centered.at(lx, y + 6.0F), "Mode");
          const bool opt = rom.optimized();
          if (opt ? outline_button(centered, "mode_compat", bx, y, 120.0F, 26.0F, "Compatibility", font_regular, 12.0F)
                  : bevel_button(centered, "mode_compat", bx, y, 120.0F, 26.0F, "Compatibility", font_bold, 12.0F))
            set_policy("compatibility");
          if (opt ? bevel_button(centered, "mode_opt", bx + 128.0F, y, 92.0F, 26.0F, "Optimized", font_bold, 12.0F)
                  : outline_button(centered, "mode_opt", bx + 128.0F, y, 92.0F, 26.0F, "Optimized", font_regular, 12.0F))
            set_policy("optimized");
          if (opt && rom.platform_id == "genesis") {  // advanced: the experimental model opt-in is only offered while Genesis Optimized is selected
            const bool g2 = rom.optimized_gen2();
            if (g2 ? bevel_button(centered, "model_g2", bx + 228.0F, y, 92.0F, 26.0F, "Experimental", font_bold, 11.0F)
                   : outline_button(centered, "model_g2", bx + 228.0F, y, 92.0F, 26.0F, "Experimental", font_regular, 11.0F))
              set_model(g2 ? "v1" : "gen2");
          }
        }
        y += 34.0F;
        if (rom.optimized() && rom.platform_id == "master-system") {  // the Master System mode is a single experimental choice, with its caveat
          if (draw) {
            draw_text(dl, font_regular, centered.len(10.5F), text_dim, centered.at(bx, y - 4.0F), "Experimental CNN compilation.");
            draw_text(dl, font_regular, centered.len(10.5F), text_dim, centered.at(bx, y + 9.0F), "May restart in Compatibility if omitted code is reached.");
          }
          y += 28.0F;
        } else if (rom.optimized()) {  // what the extra button is, in one quiet line
          if (draw) draw_text(dl, font_regular, centered.len(10.5F), text_dim, centered.at(bx, y - 4.0F),
                              rom.optimized_gen2() ? "Experimental model on (advanced); may fall back." : "Experimental: optional alternative model.");
          y += 14.0F;
        }
      }
      if (rom.supports_region()) {
        if (draw) {
          draw_text(dl, font_regular, centered.len(12), text_secondary, centered.at(lx, y + 6.0F), "Region");
          const auto option = [&](const char *id, const char *value, const char *label, float x, float w) {
            const bool on = rom.region == value;
            if (on ? bevel_button(centered, id, bx + x, y, w, 26.0F, label, font_bold, 12.0F)
                   : outline_button(centered, id, bx + x, y, w, 26.0F, label, font_regular, 12.0F))
              rom.region = value;
          };
          option("region_usa", "usa", "USA NTSC", 0.0F, 100.0F);
          option("region_jp", "japan", "Japan NTSC", 106.0F, 104.0F);
          option("region_eu", "europe", "Europe PAL", 216.0F, 104.0F);
        }
        y += 32.0F;
        std::vector<std::string> notes;
        if (rom.region == "europe") notes.push_back(region_note("europe"));
        else if (rom.region_ambiguous) notes.push_back("Header region is ambiguous; USA NTSC assumed.");
        if (state == State::Ready)
          if (const std::string coverage = region_coverage_note(observed_region, rom.region); !coverage.empty()) notes.push_back(coverage);
        for (const std::string &note : notes) {
          if (draw) draw_text(dl, font_regular, centered.len(10.5F), text_dim, centered.at(lx, y - 2.0F), note.c_str());
          y += 14.0F;
        }
        if (!notes.empty()) y += 2.0F;
      }
      if (rom.supports_hint_alias_discovery()) {
        if (draw) {  // advanced, default off
          if (rom.hint_alias_discovery ? bevel_button(centered, "hint_discovery", lx, y, content_w, 26.0F, "HInt alias discovery (strict build pass): ON", font_bold, 11.0F)
                                       : outline_button(centered, "hint_discovery", lx, y, content_w, 26.0F, "HInt alias discovery (strict build pass): off", font_regular, 11.0F))
            set_discovery(!rom.hint_alias_discovery);
        }
        y += 30.0F;
        // What the advanced toggle is for, so it is not switched on casually.
        if (draw) {
          draw_text(dl, font_regular, centered.len(10.5F), text_dim, centered.at(lx, y - 2.0F), "Advanced: for games whose raster/H-interrupt effects fail otherwise.");
          draw_text(dl, font_regular, centered.len(10.5F), text_dim, centered.at(lx, y + 11.0F), "Leave off unless the game does not work; builds may take longer.");
        }
        y += 32.0F;
      }
      if (rom.optimized() && rom_policy_identity_cached(layout, rom).empty()) {
        if (draw) {
          draw_text(dl, font_regular, centered.len(10.5F), col_err, centered.at(lx, y), "Optimized cannot verify the build tool identity:");
          draw_text(dl, font_regular, centered.len(10.5F), col_err, centered.at(lx, y + 14.0F), "saved Optimized builds are not reused.");
        }
        y += 32.0F;
      }
      if (state == State::Ready) {
        separator();
        if (draw) {
          draw_text(dl, font_bold, centered.len(12), text_secondary, centered.at(lx, y), "Controls");
          if (rom.platform_id == "master-system") {
            draw_text(dl, font_regular, centered.len(12), text_footer, centered.at(lx, y + 18.0F), "Arrows = D-Pad   Z = Button 1   X = Button 2");
            draw_text(dl, font_regular, centered.len(12), text_footer, centered.at(lx, y + 35.0F), "P = Pause   R = Reset");
          } else {
            draw_text(dl, font_regular, centered.len(12), text_footer, centered.at(lx, y + 18.0F), "Arrows = D-Pad   Z = A   X = B   C = C   Enter = Start");
          }
        }
        y += (rom.platform_id == "master-system" ? 56.0F : 40.0F);
      }
      return y - panel_y0 + 22.0F;  // panel height needed (clears the dashed inner frame)
    };
    const float panel_h = settings_state ? std::max(settings_column(false), 256.0F)
                                         : 256.0F + title_pitch * static_cast<float>(title_lines.size() > 1 ? 1 : 0);
    const float panel_y1 = panel_y0 + panel_h;
    const bool drop_hover = ImGui::IsMouseHoveringRect(centered.at(panel_x0, panel_y0), centered.at(panel_x1, panel_y1), false);
    const bool drop_active = state == State::NoRom && dragging;
    {
      const float step = centered.len(3);
      const ImU32 fill = drop_active ? rgba(10, 34, 58) : (state == State::NoRom && drop_hover ? panel_fill_hover : panel_fill);
      const ImU32 top_left = drop_active ? dash_color_hover : panel_highlight;
      draw_bevel_frame(dl, centered.at(panel_x0, panel_y0), centered.at(panel_x1, panel_y1), step, panel_border_dark,
                       panel_edge, top_left, panel_edge, fill, centered.len(6));
      draw_dashed_rect(dl, centered.at(panel_x0 + 16, panel_y0 + 16), centered.at(panel_x1 - 16, panel_y1 - 16),
                       drop_active ? dash_color_hover : dash_color, centered.len(2), centered.len(9), centered.len(6));
    }
    if (state == State::NoRom) {
      const float icon_size = 56.0F, icon_top = panel_y0 + 40.0F;
      if (tex_drop) {
        const ImU32 tint = drop_active ? rgba(150, 235, 255) : rgba(255, 255, 255);
        dl->AddImage(reinterpret_cast<ImTextureID>(tex_drop), centered.at(center_x - icon_size * 0.5F, icon_top),
                    centered.at(center_x + icon_size * 0.5F, icon_top + icon_size), ImVec2(0, 0), ImVec2(1, 1), tint);
      }
      const float label_y = icon_top + icon_size + 18.0F;
      draw_text_centered(dl, font_bold, centered.len(21), text_primary, centered.at(center_x, 0).x, centered.at(0, label_y).y,
                         drop_active ? "Release to load the ROM" : "Drop a ROM here");
      draw_text_centered(dl, font_regular, centered.len(14), text_secondary, centered.at(center_x, 0).x, centered.at(0, label_y + 32.0F).y, "or");
      if (bevel_button(centered, "browse", center_x - 90.0F, label_y + 58.0F, 180.0F, 46.0F, "Browse...", font_bold, 18.0F))
        browse();
    }

    if (state != State::NoRom) {
      for (std::size_t i = 0; i < title_lines.size(); ++i)
        draw_text(dl, font_bold, centered.len(18), text_primary, centered.at(content_x, panel_y0 + 22.0F + title_pitch * static_cast<float>(i)), title_lines[i].c_str());
      std::string meta = rom.platform;
      if (rom.supported_platform) meta += rom.compat_known ? "  -  Experimental compatibility" : "  -  Compatibility unknown";
      draw_text(dl, font_regular, centered.len(13), text_secondary,
                centered.at(content_x, panel_y0 + 22.0F + title_pitch * static_cast<float>(title_lines.size()) + 2.0F), meta.c_str());

      if (!layout.problem.empty()) {
        ImGui::SetCursorPos(centered.at(content_x, row_y));
        ImGui::PushTextWrapPos(centered.at(panel_x1 - 20.0F, 0).x);
        ImGui::PushStyleColor(ImGuiCol_Text, col_err);
        ImGui::PushFont(font_regular, centered.len(13));
        ImGui::TextUnformatted(layout.problem.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
      } else if (state == State::RomSelected) {
        if (!rom.supported_platform) {
          draw_text(dl, font_regular, centered.len(13), col_err, centered.at(content_x, row_y), "Not a supported Genesis or Master System ROM.");
          if (outline_button(centered, "choose_u", content_x, row_y + 30.0F, 190.0F, 42.0F, "Choose another...", font_regular, 14.0F)) browse();
        } else if (rom.needs_mapper()) {
          // The header never identifies the cartridge mapper and nothing is defaulted: the user declares it.
          draw_text(dl, font_regular, centered.len(13), text_secondary, centered.at(content_x, row_y - 2.0F),
                    "Declare the cartridge mapper (it cannot be detected):");
          const char *declared = nullptr;
          if (bevel_button(centered, "mapper_sega", content_x, row_y + 30.0F, 140.0F, 42.0F, "Sega mapper", font_bold, 16.0F)) declared = "sega";
          if (outline_button(centered, "mapper_rom", content_x + 148.0F, row_y + 30.0F, 150.0F, 42.0F, "ROM only (32 KiB)", font_regular, 13.0F)) declared = "rom_only";
          if (outline_button(centered, "choose_m", content_x + 306.0F, row_y + 30.0F, 90.0F, 42.0F, "Choose...", font_regular, 13.0F)) browse();
          if (declared != nullptr) {
            rom.mapper = declared;
            entry = entry_dir(rom, layout);
            if (entry_ready_for(entry, rom, layout)) state = State::Ready; else start_build(false);
          }
        } else {
          settings_column(true);
        }
      } else if (state == State::Building) {
        static const char *names[BuildJob::stage_count] = {"Analyzing ROM", "Generating native C", "Compiling", "Linking"};
        draw_text(dl, font_bold, centered.len(15), text_primary, centered.at(content_x, row_y - 6.0F),
                  restart_pending ? "Preparing Compatibility build..." : "Preparing game...");
        for (int i = 0; i < BuildJob::stage_count; ++i)
          draw_stage_row(centered, dl, content_x, row_y + 20.0F + 23.0F * static_cast<float>(i), names[i], job->stage(i), font_regular, t);
        draw_text(dl, font_regular, centered.len(12), text_dim, centered.at(content_x, panel_y0 + panel_h - 34.0F),
                 "Large games can take several minutes the first time.");
      } else if (state == State::Ready) {
        settings_column(true);
      } else if (state == State::Running) {
        draw_text(dl, font_bold, centered.len(16), text_primary, centered.at(content_x, row_y + 10.0F),
                  running_label(policy_report, restarted_this_play).c_str());  // the EFFECTIVE program, from the entry's status.json
        draw_text(dl, font_regular, centered.len(13), text_secondary, centered.at(content_x, row_y + 38.0F), "Close the game window to return.");
        if (restarted_this_play && !restart_notice.empty()) {
          ImGui::SetCursorPos(centered.at(content_x, row_y + 62.0F));
          ImGui::PushTextWrapPos(centered.at(panel_x1 - 20.0F, 0).x);
          ImGui::PushStyleColor(ImGuiCol_Text, col_err);
          ImGui::PushFont(font_regular, centered.len(11));
          ImGui::TextUnformatted(restart_notice.c_str());
          ImGui::PopFont();
          ImGui::PopStyleColor();
          ImGui::PopTextWrapPos();
        }
      } else if (state == State::Failed) {
        ImGui::SetCursorPos(centered.at(content_x, row_y - 6.0F));
        ImGui::PushTextWrapPos(centered.at(panel_x1 - 20.0F, 0).x);
        ImGui::PushStyleColor(ImGuiCol_Text, col_err);
        ImGui::PushFont(font_regular, centered.len(13));
        ImGui::TextUnformatted(failure.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        const float by = panel_y0 + panel_h - 64.0F;
        if (rom.error.empty() && bevel_button(centered, "retry", content_x, by, 110.0F, 40.0F, "Try again", font_bold, 15.0F))
          start_build(true);
        if (outline_button(centered, "diag", content_x + 120.0F, by, 150.0F, 40.0F, show_diagnostics ? "Hide diagnostics" : "View diagnostics",
                           font_regular, 13.0F))
          show_diagnostics = !show_diagnostics;
        if (outline_button(centered, "choose3", content_x + 280.0F, by, 110.0F, 40.0F, "Choose...", font_regular, 13.0F))
          browse();
      }
    }

    // ---- below the panel: diagnostics (when asked for) or the compact legal/info panel; then the footer ----
    const float below_y = panel_y1 + 16.0F;
    if (show_diagnostics && (state == State::Failed || (state == State::Ready && policy_report.present && policy_report.requested == "optimized"))) {
      const float dh = std::min(130.0F, (LH - 90.0F) - below_y);
      dl->AddRectFilled(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + dh), rgba(5, 9, 20, 235), centered.len(4));
      dl->AddRect(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + dh), panel_edge, centered.len(4));
      ImGui::SetCursorPos(centered.at(panel_x0 + 12.0F, below_y + 10.0F));
      ImGui::PushTextWrapPos(centered.at(panel_x1 - 12.0F, 0).x);
      ImGui::PushStyleColor(ImGuiCol_Text, text_secondary);
      ImGui::PushFont(font_regular, centered.len(12));
      ImGui::BeginChild("diag", ImVec2(centered.len(panel_w - 24.0F), centered.len(dh - 20.0F)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
      ImGui::TextUnformatted(diagnostics.c_str());
      ImGui::EndChild();
      ImGui::PopFont();
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
    } else if (const float avail = (LH - 90.0F) - below_y; avail < 82.0F) {
      // A tall panel (all options visible) leaves no room for the full notice: keep a compact one-line version rather than overlap the footer.
      const float legal_h = std::min(30.0F, avail);
      if (legal_h >= 22.0F) {
        dl->AddRectFilled(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + legal_h), panel_fill, centered.len(4));
        dl->AddRect(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + legal_h), panel_edge, centered.len(4), 0, centered.len(1.2F));
        draw_text_centered(dl, font_regular, centered.len(11), text_footer, centered.at(LW * 0.5F, 0).x, centered.at(0, below_y + (legal_h - 11.0F) * 0.5F - 1.0F).y,
                           "No ROMs included. Use only software you may legally analyze.");
      }
    } else {
      const float legal_h = 82.0F;
      dl->AddRectFilled(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + legal_h), panel_fill, centered.len(4));
      dl->AddRect(centered.at(panel_x0, below_y), centered.at(panel_x1, below_y + legal_h), panel_edge, centered.len(4), 0, centered.len(1.2F));
      if (tex_info) dl->AddImage(reinterpret_cast<ImTextureID>(tex_info), centered.at(panel_x0 + 14.0F, below_y + 14.0F),
                                centered.at(panel_x0 + 40.0F, below_y + 40.0F));
      const float tx = panel_x0 + (tex_info ? 52.0F : 16.0F);
      draw_text(dl, font_regular, centered.len(12.5F), text_footer, centered.at(tx, below_y + 12.0F), "No ROMs are included with Segarecomp. Use only");
      draw_text(dl, font_regular, centered.len(12.5F), text_footer, centered.at(tx, below_y + 30.0F), "software you are legally entitled to analyze.");
      draw_text(dl, font_regular, centered.len(12.5F), text_footer, centered.at(tx, below_y + 48.0F), "Compatibility is experimental.");
    }

    // ---- footer: quiet, centered status metadata, then the cache-folder link -- on its own scrim so it
    // stays legible over the busy background art regardless of what part of the scenery sits behind it ----
    {
      const float fx0 = LW * 0.5F - 220.0F, fx1 = LW * 0.5F + 220.0F, fy0 = LH - 86.0F, fy1 = LH - 16.0F;
      dl->AddRectFilled(centered.at(fx0, fy0), centered.at(fx1, fy1), rgba(6, 10, 22, 165), centered.len(6));
      dl->AddLine(centered.at(fx0 + 10.0F, fy0), centered.at(fx1 - 10.0F, fy0), rgba(60, 75, 120, 120), centered.len(1));
    }
    char info[160];
    std::snprintf(info, sizeof info, "v%s  \xC2\xB7  Zig %s  \xC2\xB7  SDL %d.%d.%d  \xC2\xB7  %s", SEGARECOMP_LAUNCHER_VERSION,
                  SEGARECOMP_ZIG_VERSION, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                  SDL_VERSIONNUM_MICRO(SDL_GetVersion()), host_description().c_str());
    draw_text_centered(dl, font_regular, centered.len(12.5F), text_footer, centered.at(LW * 0.5F, 0).x, centered.at(0, LH - 70.0F).y, info);
    if (text_link(centered, "cache", LW * 0.5F, LH - 42.0F, tex_folder, "Open cache folder", font_regular, 13.5F))
      SDL_OpenURL(("file://" + u8s(cache_root())).c_str());

    ImGui::End();

    ImGui::Render();
    // High-DPI: our draw calls above work in window points (win_w/win_h); the renderer target is pixels.
    // The points -> pixels ratio is computed fresh from the renderer's actual current output size every
    // frame, rather than trusted from io.DisplayFramebufferScale, which can lag a live DPI/monitor change
    // by a frame or more on some platforms and previously left part of the window unpainted.
    int output_w = win_w_pt, output_h = win_h_pt;
    SDL_GetCurrentRenderOutputSize(renderer, &output_w, &output_h);
    SDL_SetRenderScale(renderer, win_w_pt > 0 ? static_cast<float>(output_w) / win_w : 1.0F,
                       win_h_pt > 0 ? static_cast<float>(output_h) / win_h : 1.0F);
    SDL_SetRenderDrawColor(renderer, 8, 11, 26, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    maybe_save_screenshot(renderer, frame);
    SDL_RenderPresent(renderer);
  }
  game.reset();
  job.reset();
  if (tex_background) SDL_DestroyTexture(tex_background);
  if (tex_wordmark) SDL_DestroyTexture(tex_wordmark);
  if (tex_drop) SDL_DestroyTexture(tex_drop);
  if (tex_info) SDL_DestroyTexture(tex_info);
  if (tex_folder) SDL_DestroyTexture(tex_folder);
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  if (resolve_job.valid()) resolve_job.wait();  // the worker (bounded by the tool's own 5 s limit) must not touch SDL after SDL_Quit
  SDL_Quit();
  return 0;
}

} // namespace

#if defined(_WIN32)
namespace {
// Windows creates a window's decorated frame at the real per-monitor DPI regardless of the process's
// declared awareness, but only hands back correct point/pixel sizes -- and paints the whole client area --
// once the process has opted into per-monitor-v2 awareness. Without it, a scaled (e.g. 4K) display can leave
// part of the window unpainted (observed: small, correctly-proportioned content pinned to one corner, the
// rest of the frame showing whatever was behind the window). Resolved dynamically (GetProcAddress) rather
// than through a specific mingw-w64 SDK header vintage, with an older-Windows fallback chain; safe to call
// even when a manifest already declared awareness (the redundant call is simply ignored by Windows).
void enable_dpi_awareness() {
  if (HMODULE user32 = LoadLibraryA("user32.dll")) {
    using SetContextFn = BOOL(WINAPI *)(void *);
    void *per_monitor_v2 = reinterpret_cast<void *>(static_cast<std::intptr_t>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    auto *set_context = reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    const bool ok = set_context && set_context(per_monitor_v2);
    FreeLibrary(user32);
    if (ok) return;
  }
  if (HMODULE shcore = LoadLibraryA("shcore.dll")) {
    using SetAwarenessFn = HRESULT(WINAPI *)(int);
    if (auto *set_awareness = reinterpret_cast<SetAwarenessFn>(GetProcAddress(shcore, "SetProcessDpiAwareness")))
      set_awareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */);
    FreeLibrary(shcore);
    return;
  }
  SetProcessDPIAware();  // last-resort fallback, present since Windows Vista
}
} // namespace
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  enable_dpi_awareness();
#endif
  if (argc > 1) return headless(argc, argv);
  return gui();
}
