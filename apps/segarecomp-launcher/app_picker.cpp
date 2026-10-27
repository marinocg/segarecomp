// SEG-069-T004: the built-in file picker and the gamepad hint bar. See file_picker.hpp for the safety model; this file is only the screen.

#include "app.hpp"

#include "app_helpers.hpp"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;
using namespace ui;

namespace launcher {

using namespace uih;

namespace {
std::string path_text(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
fs::path path_from_text(const std::string &text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str()))); }
}  // namespace

bool App::gamepad_is_last_input() const {
  const ImGuiContext *g = ImGui::GetCurrentContext();
  return g != nullptr && g->NavInputSource == ImGuiInputSource_Gamepad;
}

bool App::use_builtin_picker() const {
  if (const char *mode = SDL_getenv("SEGARECOMP_FILE_PICKER")) {
    if (std::string(mode) == "builtin") return true;
    if (std::string(mode) == "native") return false;
  }
  return gamepad_is_last_input();  // a controller user cannot be assumed to operate the OS dialog; the mouse and keyboard keep the native one
}

void App::open_picker(const std::string &purpose, const std::string &key) {
  picker_ = PickerState{};
  picker_.open = true;
  picker_.purpose = purpose;
  picker_.key = key;
  if (purpose == "import") { picker_.title = "Choose ROMs to import"; picker_.multi = true; picker_.extensions = {"md", "bin", "gen", "smd", "sms", "zip"}; }
  else if (purpose == "locate") { picker_.title = "Locate the ROM file"; picker_.extensions = {"md", "bin", "gen", "smd", "sms", "zip"}; }
  else if (purpose == "scd_bios" || purpose == "scd_locate_bios") { picker_.title = "Choose the Sega CD BIOS image"; picker_.extensions = {"bin", "md", "rom"}; }
  else if (purpose == "scd_cue" || purpose == "scd_locate_cue") { picker_.title = "Choose the disc's CUE file"; picker_.extensions = {"cue"}; }
  else if (purpose == "scd_program" || purpose == "scd_attach") { picker_.title = "Choose the verified viewer program"; picker_.extensions = {"", "exe"}; }
  else { picker_.title = "Choose a cover image"; picker_.extensions = {"png", "jpg", "jpeg"}; }
  const char *extra = SDL_getenv("SEGARECOMP_PICKER_ROOT");
  picker_.roots = picker_roots(extra != nullptr ? path_from_text(extra) : fs::path{});
  picker_.focus_first = true;
  modal_focus_done_ = false;
  trace("picker=opened purpose=" + purpose);
}

void App::picker_set_dir(const fs::path &root, const fs::path &dir) {
  if (root != picker_.root) picker_.chosen.clear();  // a tick belongs to the location it was made in
  picker_.root = root;
  picker_.dir = dir;
  picker_.page = 0;
  picker_.message.clear();
  picker_.listing = root.empty() ? PickerListing{} : picker_list(root, dir, picker_.extensions);
  if (!picker_.listing.error.empty()) picker_.message = picker_.listing.error;
  picker_.focus_first = true;
}

void App::picker_finish(bool accept) {
  if (accept && !picker_.chosen.empty()) {
    DialogEvent event;
    event.purpose = picker_.purpose;
    event.key = picker_.key;
    for (const auto &text : picker_.chosen) {
      std::string why;
      if (picker_accept(picker_.root, path_from_text(text), picker_.extensions, &why)) event.paths.push_back(text);
      else toast(why, true);
    }
    if (!event.paths.empty()) {
      trace("picker=chosen count=" + std::to_string(event.paths.size()));
      std::lock_guard<std::mutex> lock(dialog_mutex_);
      dialog_events_.push_back(std::move(event));
    }
  } else {
    trace("picker=cancelled");
  }
  picker_ = PickerState{};
  modal_focus_done_ = false;
}

// A one-line legend of the controller buttons at the bottom of the panel; shown only while a controller is the last input device, so mouse and keyboard
// users never see it. Text only (no vendor glyphs): the SDL "south/east/north" positions are named by the labels most pads print.
void App::draw_gamepad_hints(float w, float h) {
  if (!gamepad_is_last_input() || modal_.kind != Modal::Kind::none || picker_.open) return;
  std::string text = "A: select     B: back     Y: menu";
  if (page_ == Page::library) text = "A: open     Y: menu     B: sidebar     D-pad: move";
  else if (page_ == Page::details) text = "A: select     B: back to library     D-pad: move";
  else text = "A: select     B: back     D-pad: move     Left: sidebar";
  const float px = ctx_.u(11.0F), tw = text_width(ctx_.regular, px, text.c_str());
  ImDrawList *dl = ImGui::GetWindowDrawList();
  draw_text(dl, ctx_.regular, px, col::dim, ImVec2(w - ctx_.u(30.0F) - tw, h - ctx_.u(34.0F)), text.c_str());
}

void App::draw_file_picker(float w, float h) {
  if (!picker_.open) return;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(w, h));
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##picker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), col::scrim);
  const float pw = std::min(w - ctx_.u(32.0F), ctx_.u(720.0F)), ph = h - ctx_.u(32.0F);
  const ImVec2 a((w - pw) * 0.5F, ctx_.u(16.0F)), b(a.x + pw, a.y + ph);
  dl->AddRectFilled(ImVec2(a.x - ctx_.u(2.0F), a.y - ctx_.u(2.0F)), ImVec2(b.x + ctx_.u(2.0F), b.y + ctx_.u(2.0F)), col::glow_soft, ctx_.u(10.0F));
  dl->AddRectFilled(a, b, rgba(9, 21, 48), ctx_.u(8.0F));
  dl->AddRect(a, b, col::glow, ctx_.u(8.0F), 0, std::max(1.0F, ctx_.u(1.5F)));
  const float lx = a.x + ctx_.u(20.0F), inner = pw - ctx_.u(40.0F);
  float y = a.y + ctx_.u(16.0F);
  draw_text(dl, ctx_.bold, ctx_.u(16.0F), col::text, ImVec2(lx, y), picker_.title.c_str());
  y += ctx_.u(26.0F);
  const bool at_roots = picker_.root.empty();
  std::string where = "Choose a location";
  if (!at_roots) {
    where = path_text(picker_.dir);
    for (const auto &r : picker_.roots) if (r.path == picker_.root) { where = r.label + (picker_.dir == picker_.root ? std::string() : "  /  " + path_text(picker_.dir.lexically_relative(picker_.root))); break; }
  }
  draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::info, ImVec2(lx, y), ellipsize(ctx_.regular, ctx_.u(12.0F), inner, where).c_str());
  y += ctx_.u(24.0F);

  // ---- rows: [Up] + entries (or roots), one page at a time so every row and button stays in one navigable window ----
  struct Row { std::string label, detail; bool directory = false, up = false, file = false; fs::path path; fs::path root; };
  std::vector<Row> rows;
  if (at_roots) {
    for (const auto &r : picker_.roots) rows.push_back({r.label, path_text(r.path), true, false, false, r.path, r.path});
  } else {
    const fs::path parent = picker_parent(picker_.root, picker_.dir);
    rows.push_back({parent.empty() ? "Back to locations" : "Up one folder", "", true, true, false, parent, picker_.root});
    for (const auto &e : picker_.listing.entries) rows.push_back({e.name, e.directory ? std::string() : format_bytes(e.size), e.directory, false, !e.directory, e.path, picker_.root});
  }
  const float row_h = ctx_.u(34.0F), footer_h = ctx_.u(104.0F);
  const int per_page = std::max(3, static_cast<int>((b.y - y - footer_h) / (row_h + ctx_.u(3.0F))));
  const int pages = std::max(1, (static_cast<int>(rows.size()) + per_page - 1) / per_page);
  picker_.page = std::clamp(picker_.page, 0, pages - 1);
  const int first = picker_.page * per_page, last = std::min(static_cast<int>(rows.size()), first + per_page);
  bool leave_level = false;
  fs::path next_dir, next_root;
  bool enter = false;
  for (int i = first; i < last; ++i) {
    const Row &row = rows[static_cast<std::size_t>(i)];
    ImGui::SetCursorScreenPos(ImVec2(lx, y));
    ImGui::PushID(i);
    const bool pressed = ui::nav_button("##pkrow", ImVec2(inner, row_h));
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    if (ImGui::IsItemFocused()) ui::focus_id = "pkrow#" + std::to_string(i);
    if (picker_.focus_first && i == first) { ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow()); ImGui::SetNavCursorVisible(true); picker_.focus_first = false; }
    ImGui::PopID();
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    const bool ticked = row.file && picker_.chosen.count(path_text(row.path)) != 0;
    dl->AddRectFilled(r0, r1, ticked ? rgba(22, 74, 170) : (hovered ? col::panel_hover : rgba(11, 26, 60)), ctx_.u(5.0F));
    dl->AddRect(r0, r1, ticked ? col::btn_top : rgba(46, 96, 190), ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
    const float mid = (r0.y + r1.y) * 0.5F;
    icon_image(dl, ctx_.icon(row.up ? "reset" : (row.directory ? "folder" : (ticked ? "check" : "file"))), ImVec2(r0.x + ctx_.u(10.0F), mid - ctx_.u(8.0F)), ctx_.u(16.0F), ticked ? col::ok : IM_COL32_WHITE);
    const float detail_w = row.detail.empty() ? 0.0F : text_width(ctx_.regular, ctx_.u(11.5F), row.detail.c_str()) + ctx_.u(12.0F);
    draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(r0.x + ctx_.u(36.0F), mid - ctx_.u(13.0F) * 0.55F), ellipsize(ctx_.bold, ctx_.u(13.0F), inner - ctx_.u(52.0F) - detail_w, row.label).c_str());
    if (!row.detail.empty()) draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::dim, ImVec2(r1.x - detail_w + ctx_.u(2.0F), mid - ctx_.u(11.5F) * 0.55F), row.detail.c_str());
    if (focused) focus_ring(ctx_, dl, r0, r1);
    if (pressed) {
      if (row.up) { if (row.path.empty()) leave_level = true; else { enter = true; next_root = row.root; next_dir = row.path; } }
      else if (row.directory) { enter = true; next_root = row.root; next_dir = row.path; }
      else if (picker_.multi) { const std::string text = path_text(row.path); if (!picker_.chosen.erase(text)) picker_.chosen.insert(text); }
      else { picker_.chosen = {path_text(row.path)}; picker_.root = row.root; picker_finish(true); ImGui::End(); return; }
    }
    y += row_h + ctx_.u(3.0F);
  }
  if (rows.empty() || (!at_roots && rows.size() == 1U)) {
    draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(lx, y + ctx_.u(4.0F)), at_roots ? "No locations are available." : "Nothing here that can be used. Go up to another folder.");
  }
  if (!picker_.message.empty()) draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::warn, ImVec2(lx, b.y - footer_h + ctx_.u(2.0F)), picker_.message.c_str());
  else if (picker_.listing.truncated) draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::warn, ImVec2(lx, b.y - footer_h + ctx_.u(2.0F)), "This folder has more files than are shown.");

  // ---- bottom bar ----
  const float bh = ctx_.u(34.0F), by = b.y - ctx_.u(50.0F);
  const float bw = (inner - ctx_.u(18.0F)) / 4.0F;
  ImGui::SetCursorScreenPos(ImVec2(lx, by));
  if (button(ctx_, "pk_prev", "Previous page", ImVec2(bw, bh), Style::secondary, picker_.page > 0)) { --picker_.page; picker_.focus_first = true; }
  ImGui::SetCursorScreenPos(ImVec2(lx + (bw + ctx_.u(6.0F)), by));
  if (button(ctx_, "pk_next", ("Next page " + std::to_string(picker_.page + 1) + "/" + std::to_string(pages)).c_str(), ImVec2(bw, bh), Style::secondary, picker_.page + 1 < pages)) { ++picker_.page; picker_.focus_first = true; }
  ImGui::SetCursorScreenPos(ImVec2(lx + 2.0F * (bw + ctx_.u(6.0F)), by));
  const bool cancel_button = button(ctx_, "pk_cancel", "Cancel", ImVec2(bw, bh), Style::secondary);
  ImGui::SetCursorScreenPos(ImVec2(lx + 3.0F * (bw + ctx_.u(6.0F)), by));
  const std::string ok_label = picker_.multi ? "Import (" + std::to_string(picker_.chosen.size()) + ")" : "Select";
  const bool ok_button = button(ctx_, "pk_ok", ok_label.c_str(), ImVec2(bw, bh), Style::primary, !picker_.chosen.empty());
  draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(lx, b.y - ctx_.u(8.0F) - ctx_.u(11.0F)), "A: open / tick     B: up / back     LB / RB: page");
  const bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) && picker_.page > 0) { --picker_.page; picker_.focus_first = true; }
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) && picker_.page + 1 < pages) { ++picker_.page; picker_.focus_first = true; }
  ImGui::End();
  if (cancel_button) { picker_finish(false); return; }
  if (ok_button) { picker_finish(true); return; }
  if (enter) { picker_set_dir(next_root, next_dir); return; }
  if (leave_level || back) {
    const fs::path parent = at_roots ? fs::path{} : picker_parent(picker_.root, picker_.dir);
    if (at_roots) picker_finish(false);                       // B at the list of locations closes the picker
    else if (parent.empty()) picker_set_dir(fs::path{}, fs::path{});  // B at a location's top returns to the list of locations
    else picker_set_dir(picker_.root, parent);
  }
}

}  // namespace launcher
