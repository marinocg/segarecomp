// The launcher's screens. Every control here does something real: no placeholder buttons, no fabricated status or metrics.

#include "app.hpp"

#include "app_helpers.hpp"
#include "imgui_internal.h"
#include "covers.hpp"
#include "pixel_assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace fs = std::filesystem;
using namespace ui;

namespace launcher {

using namespace uih;

namespace {
// "Genesis - USA": the platform with the game's region, as shown on library cards and rows.
std::string platform_region(const LibraryEntry &entry) { return std::string(platform_short(entry.platform_id)) + " - " + region_label(entry.platform_id, entry.region); }
} // namespace

// ---------------------------------------------------------------- data views

std::vector<const LibraryEntry *> App::visible_entries() const {
  std::vector<const LibraryEntry *> out;
  const std::string needle = lower(search_);
  for (const auto &e : catalog_.entries()) {
    if (platform_filter_ == 1 && e.platform_id != "genesis") continue;
    if (platform_filter_ == 2 && e.platform_id != "master-system") continue;
    const StatusInfo &info = status_of(e.id());
    if (status_filter_ == 1 && info.status != GameStatus::ready) continue;
    if (status_filter_ == 2 && info.status == GameStatus::ready) continue;
    if (status_filter_ == 3 && info.status != GameStatus::failed) continue;
    if (status_filter_ == 4 && !info.rom_missing) continue;
    if (!needle.empty() && lower(e.title()).find(needle) == std::string::npos && lower(platform_short(e.platform_id)).find(needle) == std::string::npos) continue;
    out.push_back(&e);
  }
  const auto size_of = [&](const LibraryEntry *e) { const EntryUsage *u = report_valid_ ? report_.find(e->id()) : nullptr; return u != nullptr ? u->total.total() : std::uintmax_t{0}; };
  std::stable_sort(out.begin(), out.end(), [&](const LibraryEntry *a, const LibraryEntry *b) {
    if (settings_.sort == "title") return lower(a->title()) < lower(b->title());
    if (settings_.sort == "last_built") return a->last_built_at > b->last_built_at;
    if (settings_.sort == "added") return a->added_at > b->added_at;
    if (settings_.sort == "size") return size_of(a) > size_of(b);
    if (a->last_played_at != b->last_played_at) return a->last_played_at > b->last_played_at;
    return lower(a->title()) < lower(b->title());
  });
  return out;
}

SDL_Texture *App::cover_texture(const LibraryEntry &entry) {
  if (!entry.cover.empty() && cover_missing_.count(entry.cover) == 0) {
    const auto found = cover_cache_.find(entry.cover);
    if (found != cover_cache_.end()) { if (found->second != nullptr) return found->second; }
    else {
      static int loaded_frame = -1;
      if (loaded_frame != frame_) {  // at most one decode per frame keeps scrolling smooth
        loaded_frame = frame_;
        SDL_Texture *texture = load_file_texture(renderer_, covers_dir_ / entry.cover, false);
        if (texture == nullptr) cover_missing_[entry.cover] = true;
        cover_cache_[entry.cover] = texture;
        if (texture != nullptr) return texture;
      } else {
        return nullptr;  // decoded on a later frame (the caller draws a plain tile meanwhile)
      }
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------- frame

namespace {

// Category label of one plan item for the confirmation dialog.
const char *plan_category(const PlanItem &item) {
  const std::string name = u8s(item.path.filename());
  if (name == "generated.c" || name == "generated" || name == "generated-z80") return "Generated C code";
  if (name == "obj") return "Object files";
  const std::string parent = u8s(item.path.parent_path().filename());
  if (parent == "zig-cache") return "Shared compiler cache";
  return item.directory && parse_dir_name(name).valid ? "Build folders (executable, logs, generated files)" : "Other build files";
}

} // namespace

void App::draw_topbar(float, float, float, float) {}

void App::draw() {
  int ww = 0, wh = 0;
  SDL_GetWindowSize(window_, &ww, &wh);
  win_w_ = static_cast<float>(ww);
  win_h_ = static_cast<float>(wh);
  {
    const float display_scale = SDL_GetWindowDisplayScale(window_), density = SDL_GetWindowPixelDensity(window_);
    float s = (display_scale > 0.0F && density > 0.0F) ? std::clamp(display_scale / density, 1.0F, 4.0F) : 1.0F;
    if (SDL_getenv("SEGARECOMP_SHOT_SIZE")) s = 1.0F;
    if (const char *forced = SDL_getenv("SEGARECOMP_SHOT_SCALE")) s = std::clamp(static_cast<float>(std::atof(forced)), 0.5F, 4.0F);  // review aid: a HiDPI display scale
    ctx_.S = settings_.compact ? s * 0.88F : s;
  }
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(win_w_, win_h_));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  draw_background(win_w_, win_h_);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float hero = hero_height(win_w_, win_h_);
  const float m = ctx_.u(12.0F);
  {
    float wm_h = 0.0F, wm_w = 0.0F;
    if (SDL_Texture *wm = ctx_.asset("wordmark.png")) {
      float tw = 0, th = 0;
      SDL_GetTextureSize(wm, &tw, &th);
      wm_h = std::min(hero * 0.62F, ctx_.u(112.0F));
      wm_w = wm_h * tw / th;
      const float top_y = std::max(ctx_.u(6.0F), hero * 0.5F - wm_h * 0.62F);
      dl->AddImage(reinterpret_cast<ImTextureID>(wm), ImVec2(m + ctx_.u(4.0F), top_y), ImVec2(m + ctx_.u(4.0F) + wm_w, top_y + wm_h));
      const char *tag = "RECOMPILE   *   PRESERVE   *   PLAY";
      const float px = std::clamp(wm_h * 0.115F, ctx_.u(8.0F), ctx_.u(11.5F));
      const float tx = m + ctx_.u(4.0F) + wm_w * 0.07F;
      const float ty = top_y + wm_h * 0.93F;
      if (hero > ctx_.u(118.0F)) {
        float cx = tx;
        for (const char *c = tag; *c != '\0'; ++c) {  // letter-spaced Silkscreen like the concept board
          const char one[2] = {*c, '\0'};
          draw_text(dl, ctx_.display, px, rgba(3, 8, 22, 230), ImVec2(cx + ctx_.u(1.0F), ty + ctx_.u(1.0F)), one);
          draw_text(dl, ctx_.display, px, rgba(214, 228, 255), ImVec2(cx, ty), one);
          cx += text_width(ctx_.display, px, one) + px * 0.28F;
        }
      }
    }
  }
  const float top = hero;
  const ImVec2 pa(m, top), pb(win_w_ - m, win_h_ - m);
  panel(ctx_, dl, pa, pb);
  const float sw = win_w_ < ctx_.u(900.0F) ? ctx_.u(62.0F) : ctx_.u(196.0F);
  draw_sidebar(pa.x, pa.y, sw, pb.y - pa.y);
  const float cx = pa.x + sw + ctx_.u(6.0F), cw = pb.x - cx - ctx_.u(4.0F), cy = pa.y + ctx_.u(4.0F), ch = pb.y - cy - ctx_.u(4.0F);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ctx_.u(18.0F), ctx_.u(14.0F)));
  ImGui::SetCursorScreenPos(ImVec2(cx, cy));
  ImGui::BeginChild("##content", ImVec2(cw, ch), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
  if (SDL_getenv("SEGARECOMP_SHOT_SCROLL") && page_ != Page::settings) ImGui::SetScrollY(ImGui::GetScrollMaxY());  // review aid: lower part of a long page
  const float inner_w = cw - ctx_.u(36.0F) - ctx_.u(8.0F);
  switch (page_) {
    case Page::library: draw_library(inner_w, ch); break;
    case Page::details: draw_details(inner_w, ch); break;
    case Page::import_page: draw_import(inner_w, ch); break;
    case Page::storage: draw_storage(inner_w, ch); break;
    case Page::settings: draw_settings(inner_w, ch); break;
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::End();
  draw_modal(win_w_, win_h_);
  draw_toasts(win_w_, win_h_);
  if (modal_.kind == Modal::Kind::none && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
    if (page_ == Page::details) close_details();
    else if (page_ != Page::library) go(Page::library);
  }
  if (!ImGui::GetIO().WantTextInput && modal_.kind == Modal::Kind::none && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { if (page_ == Page::details) close_details(); go(Page::library); focus_search_ = true; }
}

void App::draw_toasts(float w, float h) {
  const Uint64 now = SDL_GetTicks();
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast &t) { return now > t.until; }), toasts_.end());
  if (toasts_.empty()) return;
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  float y = h - ctx_.u(18.0F);
  const float px = ctx_.u(12.5F);
  for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
    const auto lines = wrap(ctx_.regular, px, std::min(w - ctx_.u(60.0F), ctx_.u(520.0F)), it->text);
    float tw = 0;
    for (const auto &l : lines) tw = std::max(tw, text_width(ctx_.regular, px, l.c_str()));
    const float bw = tw + ctx_.u(50.0F), bh = static_cast<float>(lines.size()) * (px + ctx_.u(3.0F)) + ctx_.u(18.0F);
    const ImVec2 a((w - bw) * 0.5F, y - bh);
    dl->AddRectFilled(a, ImVec2(a.x + bw, a.y + bh), col::panel_dark, ctx_.u(6.0F));
    dl->AddRect(a, ImVec2(a.x + bw, a.y + bh), it->error ? col::err : col::glow, ctx_.u(6.0F), 0, std::max(1.0F, ctx_.u(1.4F)));
    icon_image(dl, ctx_.icon(it->error ? "warning" : "info"), ImVec2(a.x + ctx_.u(12.0F), a.y + (bh - ctx_.u(16.0F)) * 0.5F), ctx_.u(16.0F), it->error ? col::err : col::info);
    float ly = a.y + ctx_.u(9.0F);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, px, col::text, ImVec2(a.x + ctx_.u(36.0F), ly), l.c_str()); ly += px + ctx_.u(3.0F); }
    y = a.y - ctx_.u(6.0F);
  }
}

// ---------------------------------------------------------------- sidebar

void App::draw_sidebar(float x, float y, float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool wide = w > ctx_.u(100.0F);
  const float pad = ctx_.u(10.0F);
  float cy = y + pad + ctx_.u(2.0F);
  // The primary navigation is exactly the four application areas. Game Details is contextual (a card, a row action or a context menu opens it)
  // and never a sidebar entry; while it is open, the area it was opened from stays highlighted.
  struct Nav { Page page; const char *label; const char *icon; };
  constexpr int kNav = 4;
  const Nav items[kNav] = {{Page::library, "Library", "library"}, {Page::import_page, "Import ROMs", "import"}, {Page::storage, "Storage", "storage"}, {Page::settings, "Settings", "settings"}};
  const Page highlighted = page_ == Page::details ? (previous_page_ == Page::details ? Page::library : previous_page_) : page_;
  ImGuiID nav_ids[kNav]{};
  ImRect nav_rects[kNav];
  int nav_target = -1;
  nav_items_.clear();
  for (int index = 0; index < kNav; ++index) {
    const auto &item = items[index];
    nav_items_ += (index == 0 ? "" : ",") + std::string(item.label);
    const bool active = highlighted == item.page;
    ImGui::SetCursorScreenPos(ImVec2(x + pad, cy));
    const ImVec2 size(w - 2 * pad, ctx_.u(46.0F));  // contiguous hit boxes: Down/Up stay inside the sidebar instead of jumping to the content beside it
    ImGui::PushID(item.label);
    const bool pressed = ui::nav_button("##nav", size);
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    nav_ids[index] = ImGui::GetItemID();
    nav_rects[index] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    // The footer and content children are spatial neighbours, not navigation siblings.
    // Keep vertical navigation in this fixed menu independent of hero/window height.
    if (focused) {
      int step = 0;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) step = 1;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) step = -1;
      if (step != 0) nav_target = std::clamp(index + step, 0, kNav - 1);
    }
    if (active && nav_reset_) ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());  // keyboard/gamepad focus starts on the current page's entry
    if (focused) focus_label_ = item.label;
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y + ctx_.u(38.0F));
    const bool current = page_ == item.page;  // on Details the origin area is marked, but not as the open page
    dl->AddRectFilled(a, b, current ? rgba(52, 128, 238) : (active ? rgba(20, 52, 112) : (hovered ? col::panel_hover : rgba(10, 24, 56))), ctx_.u(5.0F));
    dl->AddRect(a, b, current ? rgba(120, 190, 255) : (active ? rgba(64, 140, 230) : rgba(34, 70, 140)), ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
    if (current) dl->AddLine(ImVec2(a.x + ctx_.u(5.0F), a.y + ctx_.u(1.5F)), ImVec2(b.x - ctx_.u(5.0F), a.y + ctx_.u(1.5F)), rgba(170, 225, 255, 130), 1.0F);
    const float isz = ctx_.u(16.0F);
    icon_image(dl, ctx_.icon(item.icon), ImVec2(wide ? a.x + ctx_.u(12.0F) : (a.x + b.x) * 0.5F - isz * 0.5F, (a.y + b.y) * 0.5F - isz * 0.5F), isz);
    if (wide) draw_text(dl, ctx_.bold, ctx_.u(13.0F), current ? col::btn_text : col::text, ImVec2(a.x + ctx_.u(40.0F), (a.y + b.y) * 0.5F - ctx_.u(13.0F) * 0.55F), item.label);
    if (!wide && hovered) tooltip(ctx_, item.label);
    if (focused) focus_ring(ctx_, dl, a, b);
    if (pressed) go(item.page);
    cy += ctx_.u(46.0F);
  }
  if (nav_target >= 0) {
    ImGui::NavMoveRequestCancel();
    ImGui::SetFocusID(nav_ids[nav_target], ImGui::GetCurrentWindow());
    ImGuiContext &g = *ImGui::GetCurrentContext();
    g.NavCursorVisible = true;
    g.NavHighlightItemUnderNav = true;
    ImRect relative = nav_rects[nav_target];
    relative.Translate(ImVec2(-ImGui::GetWindowPos().x, -ImGui::GetWindowPos().y));
    ImGui::GetCurrentWindow()->NavRectRel[ImGuiNavLayer_Main] = relative;
  }
  nav_reset_ = false;
  const float px = ctx_.u(12.0F);
  float by = y + h - pad;
  if (wide) {
    std::error_code ec;
    const auto space = fs::space(fs::exists(cache_dir_, ec) ? cache_dir_ : cache_dir_.parent_path(), ec);
    by -= ctx_.u(34.0F);
    {
      // Build activity (running + queued) sits directly above the free-space bar; builds never lock the rest of the UI.
      const int running = job_ ? 1 : 0, queued = static_cast<int>(build_queue_.size());
      // One short line per fact so nothing is truncated: what is running, then how many wait.
      std::vector<std::string> lines;
      if (running > 0) lines.push_back("Building 1 game");
      if (queued > 0) lines.push_back(std::to_string(queued) + (queued == 1 ? " build queued" : " builds queued"));
      if (lines.empty()) lines.push_back("No builds running");
      const float bpx = ctx_.u(11.5F), lh = bpx + ctx_.u(4.0F);
      float ty = by - ctx_.u(10.0F) - lh * static_cast<float>(lines.size());
      for (const auto &line : lines) {
        const bool active = running + queued > 0;
        if (running > 0 && &line == &lines.front()) spinner(ctx_, dl, ImVec2(x + pad + ctx_.u(5.0F), ty + bpx * 0.55F), ctx_.u(5.0F), col::warn);
        draw_text(dl, ctx_.regular, bpx, active ? col::warn : col::dim, ImVec2(x + pad + (running > 0 && &line == &lines.front() ? ctx_.u(16.0F) : 0.0F), ty), ellipsize(ctx_.regular, bpx, w - 2 * pad - ctx_.u(16.0F), line).c_str());
        ty += lh;
      }
    }
    if (!ec && space.capacity > 0) {
      const bool low = space.available < (2ULL << 30U);
      const std::string free_text = format_bytes(space.available) + " free" + (low ? " - low" : "");
      draw_text(dl, ctx_.regular, px, low ? col::err : col::text2, ImVec2(x + pad, by), free_text.c_str());
      const double used = 1.0 - static_cast<double>(space.available) / static_cast<double>(space.capacity);
      progress_bar(ctx_, dl, ImVec2(x + pad, by + px + ctx_.u(6.0F)), ImVec2(x + pad + (w - 2 * pad - ctx_.u(34.0F)), by + px + ctx_.u(12.0F)), static_cast<float>(used), low ? col::err : col::btn_top);
    }
    ImGui::SetCursorScreenPos(ImVec2(x + w - pad - ctx_.u(28.0F), by - ctx_.u(2.0F)));
    if (icon_button(ctx_, "open_cache", ctx_.icon("folder"), ctx_.u(28.0F), false, true, "Open the cache folder")) SDL_OpenURL(("file://" + u8s(cache_dir_)).c_str());
    if (ImGui::IsItemFocused()) focus_label_ = "Cache folder";
    char version[64];
    std::snprintf(version, sizeof version, "v%s", SEGARECOMP_LAUNCHER_VERSION);
    draw_text(dl, ctx_.regular, ctx_.u(10.5F), col::dim, ImVec2(x + pad, by + ctx_.u(32.0F)), version);
  }
}

// ---------------------------------------------------------------- library

void App::draw_drop_zone(float w, float h, bool compact_box) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // once there are import results the drop zone shrinks so the results are visible without scrolling
  const float bw = w, bh = compact_box ? ctx_.u(!import_results_.empty() ? 150.0F : (win_h_ < ctx_.u(600.0F) ? 200.0F : 322.0F)) : std::max(ctx_.u(260.0F), h - ctx_.u(110.0F));
  const ImVec2 a(origin.x, origin.y), b(a.x + bw, a.y + bh);
  const bool hover = ImGui::IsMouseHoveringRect(a, b, false);
  const bool active = dragging_;
  dl->AddRectFilled(a, b, active ? rgba(10, 34, 70) : (hover ? col::panel_hover : rgba(8, 18, 42)), ctx_.u(8.0F));
  dashed_rect(dl, ImVec2(a.x + ctx_.u(6.0F), a.y + ctx_.u(6.0F)), ImVec2(b.x - ctx_.u(6.0F), b.y - ctx_.u(6.0F)), active ? col::dash_hover : col::dash, std::max(1.0F, std::round(ctx_.u(2.0F))), ctx_.u(10.0F), ctx_.u(7.0F));
  const float cx = (a.x + b.x) * 0.5F;
  const bool tiny = bh < ctx_.u(170.0F);
  float cy = a.y + (bh - (tiny ? ctx_.u(108.0F) : (compact_box ? ctx_.u(170.0F) : ctx_.u(210.0F)))) * 0.5F;
  if (SDL_Texture *drop = tiny ? nullptr : ctx_.asset("icon-drop.png")) { const float isz = ctx_.u(compact_box ? 52.0F : 64.0F); icon_image(dl, drop, ImVec2(cx - isz * 0.5F, cy), isz, active ? rgba(150, 235, 255) : IM_COL32_WHITE); cy += isz + ctx_.u(10.0F); }
  const char *headline = active ? "Release to add the ROMs" : "Drag & drop ROM files here";
  const float hpx = ctx_.u(compact_box ? 18.0F : 21.0F);
  draw_text(dl, ctx_.bold, hpx, col::text, ImVec2(cx - text_width(ctx_.bold, hpx, headline) * 0.5F, cy), headline);
  cy += hpx + ctx_.u(8.0F);
  const char *note = "Genesis / Mega Drive and Master System ROMs, or ZIP archives";
  const float npx = ctx_.u(12.0F);
  draw_text(dl, ctx_.regular, npx, col::text2, ImVec2(cx - text_width(ctx_.regular, npx, note) * 0.5F, cy), note);
  cy += npx + ctx_.u(16.0F);
  ImGui::SetCursorScreenPos(ImVec2(cx - ctx_.u(90.0F), cy));
  if (button(ctx_, "dz_browse", "Browse for ROMs...", ImVec2(ctx_.u(180.0F), ctx_.u(36.0F)), Style::primary, !import_job_.valid(), ctx_.icon("folder"))) browse_import();
  ImGui::SetCursorScreenPos(ImVec2(origin.x, b.y + ctx_.u(8.0F)));
  ImGui::Dummy(ImVec2(w, ctx_.u(2.0F)));
}

namespace {

// One short line of recent activity for a card or row: what the user did last with this game.
std::string activity_line(const LibraryEntry &entry, long long now) {
  if (entry.last_played_at > 0) return "Played " + fmt_relative(entry.last_played_at, now);
  if (entry.last_built_at > 0) return "Built " + fmt_relative(entry.last_built_at, now);
  if (entry.added_at > 0) return "Added " + fmt_relative(entry.added_at, now);
  return "Never played";
}

} // namespace

// Shared by cards and rows: focus bookkeeping, the restored focus after Details closes, and the context menu request.
static void card_focus_bookkeeping(const std::string &id, std::string &return_focus_id, std::string &focus_request) {
  if (return_focus_id == id || focus_request == id) {
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGuiContext &g = *ImGui::GetCurrentContext();
    g.NavCursorVisible = true;
    g.NavHighlightItemUnderNav = true;
    ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
    return_focus_id.clear();
    focus_request.clear();
  }
}

void App::draw_card(const LibraryEntry &entry, float x, float y, float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const StatusInfo &info = status_of(entry.id());
  const bool selected = select_mode_ && selected_.count(entry.id()) != 0;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ImGui::PushID(entry.id().c_str());
  ImGui::SetNextItemAllowOverlap();
  const bool pressed = ui::nav_button("##card", ImVec2(w, h));
  card_focus_bookkeeping(entry.id(), return_focus_id_, focus_card_request_);
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  if (ImGui::IsItemFocused()) focus_label_ = "card:" + entry.title();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { context_id_ = entry.id(); open_context_ = true; context_anchor_ = ImGui::GetMousePos(); }
  else if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Menu, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || std::exchange(menu_on_focus_, false))) { context_id_ = entry.id(); open_context_ = true; context_anchor_ = ImVec2(ImGui::GetItemRectMin().x + ctx_.u(24.0F), ImGui::GetItemRectMin().y + ctx_.u(24.0F)); }
  const ImVec2 a(x, y), b(x + w, y + h);
  // The card's own item loses hover to the Play / favorite buttons drawn over it, so their visibility follows the pointer being inside the
  // card's rectangle: keying it on the card item's hover made them flicker away under the pointer and the click fell through to Details.
  const bool pointer_over = ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  card_rects_[entry.id()] = ImRect(a, b);
  card(ctx_, dl, a, b, selected || focused, hovered || pointer_over);
  const long long now = std::time(nullptr);
  if (win_h_ < ctx_.u(600.0F)) {
    // Short windows retain a real grid, but use compact horizontal cards so title,
    // platform and build status are visible without first scrolling past box art.
    const float pad = ctx_.u(8.0F), art_w = ctx_.u(52.0F);
    draw_cover(dl, entry, ImVec2(x + pad, y + pad), ImVec2(x + pad + art_w, y + h - pad), ctx_.u(3.0F));
    const float tx = x + art_w + pad * 2, available = w - art_w - pad * 3;
    draw_text(dl, ctx_.bold, ctx_.u(13.5F), col::text, ImVec2(tx, y + pad), ellipsize(ctx_.bold, ctx_.u(13.5F), available - (entry.favorite ? ctx_.u(18.0F) : 0.0F), entry.title()).c_str());
    if (entry.favorite) icon_image(dl, ctx_.icon("star"), ImVec2(b.x - pad - ctx_.u(14.0F), y + pad), ctx_.u(14.0F), col::warn);
    draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::info, ImVec2(tx, y + ctx_.u(29.0F)), platform_region(entry).c_str());
    const Badge short_badge = badge_for(entry, info);
    if (short_badge.spinner) spinner(ctx_, dl, ImVec2(tx + ctx_.u(5.0F), y + ctx_.u(53.0F)), ctx_.u(4.5F), short_badge.color);
    else dl->AddCircleFilled(ImVec2(tx + ctx_.u(4.0F), y + ctx_.u(53.0F)), ctx_.u(4.0F), short_badge.color);
    draw_text(dl, ctx_.bold, ctx_.u(12.0F), short_badge.color, ImVec2(tx + ctx_.u(13.0F), y + ctx_.u(46.0F)), short_badge.label.c_str());
    draw_text(dl, ctx_.regular, ctx_.u(11.0F), info.rom_missing ? col::warn : col::dim, ImVec2(tx, y + ctx_.u(64.0F)), info.rom_missing ? "ROM file missing" : activity_line(entry, now).c_str());
    if (focused) focus_ring(ctx_, dl, a, b);
    ImGui::PopID();
    if (pressed) {
      if (select_mode_) { if (selected) selected_.erase(entry.id()); else selected_.insert(entry.id()); }
      else open_details(entry.id());
    }
    return;
  }
  const float pad = ctx_.u(8.0F);
  const float cw = w - 2 * pad, chh = cw / kCoverAspect;
  const ImVec2 ca(x + pad, y + pad), cb(x + pad + cw, y + pad + chh);
  draw_cover(dl, entry, ca, cb, ctx_.u(4.0F), info.status == GameStatus::ready ? IM_COL32_WHITE : rgba(196, 204, 228));
  if (entry.favorite || pointer_over || focused) {
    const float fs_ = ctx_.u(24.0F);
    ImGui::SetCursorScreenPos(ImVec2(cb.x - fs_ - ctx_.u(5.0F), ca.y + ctx_.u(5.0F)));
    fav_rects_[entry.id()] = ImRect(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + fs_, ImGui::GetCursorScreenPos().y + fs_));
    if (icon_button(ctx_, "fav", ctx_.icon("star"), fs_, entry.favorite, true, entry.favorite ? "Remove from favorites" : "Add to favorites")) {
      if (LibraryEntry *e = catalog_.find_id(entry.id())) { e->favorite = !e->favorite; save_catalog(); }
    }
  }
  if (select_mode_) {
    bool on = selected;
    ImGui::SetCursorScreenPos(ImVec2(ca.x + ctx_.u(5.0F), ca.y + ctx_.u(5.0F)));
    if (checkbox(ctx_, "sel", "", &on)) { if (on) selected_.insert(entry.id()); else selected_.erase(entry.id()); }
  }
  // the build state sits on the art's lower edge, where a box-art library shows it
  {
    const Badge badge = badge_for(entry, info);
    const std::string &label = badge.label;
    const ImU32 c = badge.color;
    const float px = ctx_.u(11.5F), bw = text_width(ctx_.bold, px, label.c_str()) + ctx_.u(26.0F), bh = px + ctx_.u(10.0F);
    const ImVec2 p0(ca.x + ctx_.u(6.0F), cb.y - bh - ctx_.u(6.0F));
    dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh), rgba(4, 10, 26, 225), ctx_.u(4.0F));
    dl->AddRect(p0, ImVec2(p0.x + bw, p0.y + bh), (c & 0x00FFFFFFU) | 0x90000000U, ctx_.u(4.0F));
    if (badge.spinner) spinner(ctx_, dl, ImVec2(p0.x + ctx_.u(10.0F), p0.y + bh * 0.5F), ctx_.u(4.0F), c);
    else dl->AddCircleFilled(ImVec2(p0.x + ctx_.u(10.0F), p0.y + bh * 0.5F), ctx_.u(3.5F), c);
    draw_text(dl, ctx_.bold, px, c, ImVec2(p0.x + ctx_.u(18.0F), p0.y + (bh - px) * 0.5F - ctx_.u(1.0F)), label.c_str());
  }
  const float tpx = ctx_.u(14.0F), spx = ctx_.u(11.5F);
  float ty = cb.y + ctx_.u(9.0F);
  const std::string shown_title = ellipsize(ctx_.bold, tpx, cw, entry.title());
  draw_text(dl, ctx_.bold, tpx, col::text, ImVec2(ca.x, ty), shown_title.c_str());
  if (shown_title != entry.title() && hovered) tooltip(ctx_, entry.title().c_str());
  ty += tpx + ctx_.u(8.0F);
  icon_image(dl, ctx_.icon("controller"), ImVec2(ca.x, ty - ctx_.u(1.0F)), ctx_.u(14.0F), col::info);
  // The platform and region take what they need (they identify the game); the "last played / added" text uses what remains.
  const std::string where = ellipsize(ctx_.regular, spx, cw - ctx_.u(19.0F), platform_region(entry));
  const float where_w = text_width(ctx_.regular, spx, where.c_str());
  const std::string act = activity_line(entry, now);
  const std::string act_shown = ellipsize(ctx_.regular, spx, std::max(0.0F, cw - ctx_.u(19.0F) - where_w - ctx_.u(8.0F)), act);
  draw_text(dl, ctx_.regular, spx, col::info, ImVec2(ca.x + ctx_.u(19.0F), ty), where.c_str());
  draw_text(dl, ctx_.regular, spx, col::dim, ImVec2(cb.x - text_width(ctx_.regular, spx, act_shown.c_str()), ty), act_shown.c_str());
  if (info.status == GameStatus::ready && !select_mode_ && (pointer_over || focused)) {
    const float ps = ctx_.u(40.0F);
    ImGui::SetCursorScreenPos(ImVec2(ca.x + (cw - ps) * 0.5F, ca.y + (chh - ps) * 0.5F));
    play_rects_[entry.id()] = ImRect(ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetCursorScreenPos().x + ps, ImGui::GetCursorScreenPos().y + ps));
    if (icon_button(ctx_, "play", ctx_.icon("play"), ps, true, can_play(info), "Play")) play(entry.id());
  }
  ImGui::PopID();
  if (pressed) {
    if (select_mode_) { if (selected) selected_.erase(entry.id()); else selected_.insert(entry.id()); }
    else open_details(entry.id());
  }
}

void App::draw_list_row(const LibraryEntry &entry, float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const StatusInfo &info = status_of(entry.id());
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool selected = select_mode_ && selected_.count(entry.id()) != 0;
  ImGui::PushID(entry.id().c_str());
  ImGui::SetNextItemAllowOverlap();
  const bool pressed = ui::nav_button("##row", ImVec2(w, h));
  card_focus_bookkeeping(entry.id(), return_focus_id_, focus_card_request_);
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  if (ImGui::IsItemFocused()) focus_label_ = "card:" + entry.title();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { context_id_ = entry.id(); open_context_ = true; context_anchor_ = ImGui::GetMousePos(); }
  else if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Menu, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || std::exchange(menu_on_focus_, false))) { context_id_ = entry.id(); open_context_ = true; context_anchor_ = ImVec2(ImGui::GetItemRectMin().x + ctx_.u(24.0F), ImGui::GetItemRectMin().y + ctx_.u(24.0F)); }
  const ImVec2 b(a.x + w, a.y + h);
  card(ctx_, dl, a, b, selected || focused, hovered);
  const float pad = ctx_.u(7.0F), th = h - 2 * pad, tw = th * kCoverAspect;
  float x = a.x + pad;
  if (select_mode_) {
    bool on = selected;
    ImGui::SetCursorScreenPos(ImVec2(x, a.y + (h - ctx_.u(22.0F)) * 0.5F));
    if (checkbox(ctx_, "sel", "", &on)) { if (on) selected_.insert(entry.id()); else selected_.erase(entry.id()); }
    x += ctx_.u(30.0F);
  }
  draw_cover(dl, entry, ImVec2(x, a.y + pad), ImVec2(x + tw, a.y + pad + th), ctx_.u(3.0F));
  x += tw + pad * 1.6F;
  const float px = ctx_.u(14.0F), spx = ctx_.u(11.5F);
  const float actions_w = ctx_.u(info.status == GameStatus::ready && !select_mode_ ? 74.0F : 40.0F);
  const bool wide_row = w > ctx_.u(640.0F);
  const float title_w = std::max(ctx_.u(120.0F), w * (wide_row ? 0.32F : 0.38F));
  draw_text(dl, ctx_.bold, px, col::text, ImVec2(x, a.y + h * 0.5F - px * 1.1F), ellipsize(ctx_.bold, px, title_w - (entry.favorite ? ctx_.u(20.0F) : 0.0F), entry.title()).c_str());
  if (entry.favorite) icon_image(dl, ctx_.icon("star"), ImVec2(x + std::min(title_w - ctx_.u(16.0F), text_width(ctx_.bold, px, entry.title().c_str()) + ctx_.u(6.0F)), a.y + h * 0.5F - px * 1.1F), ctx_.u(14.0F), col::warn);
  draw_text(dl, ctx_.regular, spx, col::info, ImVec2(x, a.y + h * 0.5F + ctx_.u(3.0F)), platform_region(entry).c_str());
  x += title_w + pad;
  const Badge row_badge = badge_for(entry, info);
  const std::string &label = row_badge.label;
  const ImU32 sc = row_badge.color;
  if (row_badge.spinner) spinner(ctx_, dl, ImVec2(x + ctx_.u(5.0F), a.y + h * 0.5F), ctx_.u(5.0F), sc);
  else dl->AddCircleFilled(ImVec2(x + ctx_.u(5.0F), a.y + h * 0.5F), ctx_.u(4.5F), sc);
  draw_text(dl, ctx_.bold, ctx_.u(12.5F), sc, ImVec2(x + ctx_.u(15.0F), a.y + h * 0.5F - ctx_.u(12.5F) * 0.55F), label.c_str());
  x += ctx_.u(150.0F);
  if (wide_row) {
    draw_text(dl, ctx_.regular, spx, col::text2, ImVec2(x, a.y + h * 0.5F - spx * 0.55F), activity_line(entry, std::time(nullptr)).c_str());
    x += ctx_.u(140.0F);
  }
  if (report_valid_ && x + ctx_.u(80.0F) < b.x - actions_w)
    if (const EntryUsage *u = report_.find(entry.id()); u != nullptr && u->total.total() > 0) draw_text(dl, ctx_.regular, spx, col::dim, ImVec2(x, a.y + h * 0.5F - spx * 0.55F), format_bytes(u->total.total()).c_str());
  const float ps2 = ctx_.u(30.0F);
  float bx = b.x - pad - ps2;
  if (!select_mode_) {
    ImGui::SetCursorScreenPos(ImVec2(bx, a.y + (h - ps2) * 0.5F));
    if (icon_button(ctx_, "details", ctx_.icon("info"), ps2, false, true, "Details")) open_details(entry.id());
    bx -= ps2 + ctx_.u(6.0F);
  }
  if (info.status == GameStatus::ready && !select_mode_) {
    ImGui::SetCursorScreenPos(ImVec2(bx, a.y + (h - ps2) * 0.5F));
    if (icon_button(ctx_, "play", ctx_.icon("play"), ps2, true, can_play(info), "Play")) play(entry.id());
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ctx_.u(6.0F)));
  ImGui::Dummy(ImVec2(w, 1));
  if (pressed) {
    if (select_mode_) { if (selected) selected_.erase(entry.id()); else selected_.insert(entry.id()); }
    else open_details(entry.id());
  }
}

void App::draw_library(float w, float h) {
  const auto &all = catalog_.entries();
  page_header(ctx_, w, "LIBRARY HOME", "Browse, search and manage your recompiled games.");
  if (all.empty()) {
    if (!catalog_notice_.empty() || catalog_.read_only()) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, catalog_.notice());
    draw_import_body(w, h);
    return;
  }
  const float bh = ctx_.u(34.0F);
  const bool narrow = w < ctx_.u(640.0F);
  const auto view_tools = [&] {
    if (icon_button(ctx_, "view_grid", ctx_.icon("library"), bh, settings_.view == "grid", true, "Grid view")) { settings_.view = "grid"; save_settings(); }
    ImGui::SameLine(0.0F, ctx_.u(4.0F));
    if (icon_button(ctx_, "view_list", ctx_.icon("file"), bh, settings_.view == "list", true, "List view")) { settings_.view = "list"; save_settings(); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "select_mode", select_mode_ ? "Done" : "Select", ImVec2(ctx_.u(80.0F), bh), select_mode_ ? Style::primary : Style::secondary)) { select_mode_ = !select_mode_; if (!select_mode_) selected_.clear(); }
  };
  const float tools = bh * 2 + ctx_.u(4.0F) + ctx_.u(8.0F) + ctx_.u(80.0F);
  {
    if (focus_search_) { ImGui::SetKeyboardFocusHere(); focus_search_ = false; }
    const float import_w = ctx_.u(narrow ? 120.0F : 132.0F);
    // a narrow window keeps the view switches on the search row, so the game list does not lose a whole toolbar row
    input_text(ctx_, "search", search_, sizeof search_, std::max(ctx_.u(140.0F), w - import_w - ctx_.u(12.0F) - (narrow ? tools + ctx_.u(10.0F) : 0.0F)), ImGui::GetIO().ConfigMacOSXBehaviors ? "Search games...  (Cmd+F)" : "Search games...  (Ctrl+F)");
    if (narrow) { ImGui::SameLine(0.0F, ctx_.u(10.0F)); view_tools(); }
    ImGui::SameLine(0.0F, ctx_.u(narrow ? 8.0F : 12.0F));
    if (button(ctx_, "lib_import", "Import ROMs", ImVec2(import_w, ctx_.u(36.0F)), Style::primary, !import_job_.valid(), ctx_.icon("import"))) browse_import();
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
  {
    int platform = platform_filter_, status = status_filter_;
    static const char *sort_ids[] = {"last_played", "title", "last_built", "added", "size"};
    int sort = 0;
    for (int i = 0; i < 5; ++i) if (settings_.sort == sort_ids[i]) sort = i;
    const float dw = narrow ? std::max(ctx_.u(104.0F), (w - ctx_.u(16.0F)) / 3.0F) : ctx_.u(160.0F);
    if (dropdown(ctx_, "pf", {"All Platforms", "Genesis", "Master System"}, &platform, dw, narrow ? nullptr : ctx_.icon("controller"))) platform_filter_ = platform;
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (dropdown(ctx_, "sf", {"All Statuses", "Ready", "Needs build", "Failed", "Missing ROM"}, &status, dw)) status_filter_ = status;
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (dropdown(ctx_, "so", {"Last Played", "Title", "Last Built", "Date Added", "Size"}, &sort, dw)) { settings_.sort = sort_ids[sort]; save_settings(); }
    if (!narrow) {
      ImGui::SameLine(std::max(ImGui::GetCursorPosX() + ctx_.u(8.0F), w - tools + ctx_.u(18.0F)), 0.0F);
      view_tools();
    }
  }
  if (soft_limit_exceeded_) {
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, "Builds use more than your soft limit of " + std::to_string(settings_.cleanup.soft_limit_mb) + " MiB. Nothing is deleted automatically.");
    if (button(ctx_, "soft_review", "Review in Storage", ImVec2(ctx_.u(170.0F), ctx_.u(28.0F)), Style::secondary)) go(Page::storage);
  }
  if (select_mode_) {
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    const auto vis = visible_entries();
    if (button(ctx_, "sel_all", "Select all", ImVec2(ctx_.u(110.0F), ctx_.u(28.0F)), Style::secondary)) for (const auto *e : vis) selected_.insert(e->id());
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "sel_none", "Clear", ImVec2(ctx_.u(80.0F), ctx_.u(28.0F)), Style::secondary)) selected_.clear();
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "sel_storage", ("Manage storage (" + std::to_string(selected_.size()) + ")").c_str(), ImVec2(ctx_.u(200.0F), ctx_.u(28.0F)), Style::secondary, !selected_.empty(), ctx_.icon("storage"))) { storage_selected_ = selected_; go(Page::storage); }
    if (w > ctx_.u(640.0F)) ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "sel_remove", "Remove from library", ImVec2(ctx_.u(190.0F), ctx_.u(28.0F)), Style::danger, !selected_.empty(), ctx_.icon("trash"))) {
      modal_ = Modal{};
      modal_.kind = Modal::Kind::remove_entries;
      modal_.ids.assign(selected_.begin(), selected_.end());
      modal_.title = "Remove " + std::to_string(modal_.ids.size()) + " game(s) from the library?";
    }
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  const auto visible = visible_entries();
  if (visible.empty()) { paragraph(ctx_, ctx_.regular, ctx_.u(13.0F), col::text2, w, "No game matches the search and filters."); return; }
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float avail_h = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - start.y - ctx_.u(30.0F);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
  ImGui::BeginChild("##games", ImVec2(w + ctx_.u(10.0F), std::max(avail_h, ctx_.u(120.0F))), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  if (restore_library_scroll_) { ImGui::SetScrollY(library_scroll_); restore_library_scroll_ = false; }
  else library_scroll_ = ImGui::GetScrollY();
  if (settings_.view == "list") {
    const float row_h = ctx_.u(settings_.compact ? 48.0F : 60.0F);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()), row_h + ctx_.u(6.0F));
    while (clipper.Step())
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) draw_list_row(*visible[static_cast<std::size_t>(i)], w, row_h);
  } else {
    const bool short_window = win_h_ < ctx_.u(600.0F);
    const float gap = ctx_.u(14.0F), card_w = ctx_.u(short_window ? 250.0F : (settings_.compact ? 140.0F : 178.0F));
    const float usable = w - ctx_.u(14.0F);
    const int columns = std::max(1, static_cast<int>((usable + gap) / (card_w + gap)));
    const float cw = card_w + (usable + gap - static_cast<float>(columns) * (card_w + gap)) / static_cast<float>(columns);
    const float card_h = short_window ? ctx_.u(88.0F) : (cw - ctx_.u(16.0F)) / kCoverAspect + ctx_.u(66.0F);
    const int rows = (static_cast<int>(visible.size()) + columns - 1) / columns;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGuiListClipper clipper;
    clipper.Begin(rows, card_h + gap);
    while (clipper.Step())
      for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        for (int c = 0; c < columns; ++c) {
          const std::size_t index = static_cast<std::size_t>(r * columns + c);
          if (index >= visible.size()) break;
          draw_card(*visible[index], origin.x + static_cast<float>(c) * (cw + gap) + ctx_.u(3.0F), origin.y + static_cast<float>(r) * (card_h + gap) + ctx_.u(3.0F), cw, card_h);
        }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(rows) * (card_h + gap)));
    ImGui::Dummy(ImVec2(w, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (open_context_) {
    ImGui::OpenPopup("##game_menu");
    open_context_ = false;
    // keep the menu inside the window (it is about 250 x 470 points)
    const ImVec2 at(std::min(context_anchor_.x, win_w_ - ctx_.u(270.0F)), std::clamp(context_anchor_.y, ctx_.u(8.0F), std::max(ctx_.u(8.0F), win_h_ - ctx_.u(480.0F))));
    ImGui::SetNextWindowPos(at, ImGuiCond_Always);
  }
  draw_game_menu("##game_menu", context_id_, true);
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    std::string text = std::to_string(visible.size()) + (visible.size() == 1 ? " game" : " games");
    if (report_valid_) text += "  -  " + format_bytes(report_.builds_total()) + " of builds";
    draw_text(ImGui::GetWindowDrawList(), ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(p.x, p.y + ctx_.u(2.0F)), text.c_str());
  }
}

// ---------------------------------------------------------------- modal

void App::draw_modal(float w, float h) {
  if (modal_.kind == Modal::Kind::none) return;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(w, h));
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##modal", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), col::scrim);
  const float pw = std::min(w - ctx_.u(40.0F), ctx_.u(620.0F)), inner = pw - ctx_.u(48.0F), px = ctx_.u(12.5F);
  const bool is_plan = modal_.kind == Modal::Kind::plan;
  std::vector<std::pair<std::string, std::uintmax_t>> cats;
  std::vector<std::string> body = modal_.lines, keeps, notes;
  std::string subtitle;
  if (is_plan) {
    for (const auto &item : modal_.plan.items) {
      const std::string c = plan_category(item);
      auto it = std::find_if(cats.begin(), cats.end(), [&](const auto &p) { return p.first == c; });
      if (it == cats.end()) cats.push_back({c, item.bytes}); else it->second += item.bytes;
    }
    subtitle = modal_.plan.entry_ids.empty() ? "This will permanently delete the selected files to free up disk space."
                                              : "This will permanently delete the selected files for " + std::to_string(modal_.plan.entry_ids.size()) + " game(s) to free up disk space.";
    keeps = modal_.plan.remains;
    notes = modal_.plan.notes;
    for (const auto &e : modal_.plan.excluded) notes.push_back("Excluded: " + e);
    notes.push_back("This cannot be undone. Original ROM files are never touched.");
  } else if (modal_.kind == Modal::Kind::remove_entries) {
    subtitle = "The selected games are removed from the library (titles, history, settings, last played).";
    keeps = {"Your ROM files (never deleted)", "ROM copies extracted from ZIP archives (kept in the library's roms folder)", "Cover images in the covers folder"};
  } else if (modal_.kind == Modal::Kind::clear_library) {
    subtitle = "Every game is removed from the library and the launcher forgets its history.";
    keeps = {"Build folders in the cache (use Storage to delete them)", "Your ROM files", "ROM copies extracted from ZIP archives"};
  } else if (modal_.kind == Modal::Kind::restore_defaults) {
    subtitle = "Launcher preferences (view, sort, compact mode, cleanup policy, region order) return to their defaults.";
    keeps = {"The library and builds", "Viewer controls and video settings"};
  }
  const std::vector<std::string> wrapped_sub = modal_.kind == Modal::Kind::message ? std::vector<std::string>{} : wrap(ctx_.regular, px, inner - ctx_.u(54.0F), subtitle);
  std::vector<std::string> msg_lines, note_lines;
  for (const auto &l : body) for (const auto &x : wrap(ctx_.regular, px, inner, l)) msg_lines.push_back(x);
  for (const auto &l : notes) for (const auto &x : wrap(ctx_.regular, ctx_.u(11.5F), inner, l)) note_lines.push_back(x);
  const float line_h = px + ctx_.u(5.0F);
  float ph = ctx_.u(30.0F) + ctx_.u(52.0F) + static_cast<float>(wrapped_sub.size()) * line_h + static_cast<float>(msg_lines.size()) * line_h;
  if (!cats.empty()) ph += static_cast<float>(cats.size()) * ctx_.u(26.0F) + ctx_.u(14.0F);
  if (!keeps.empty()) ph += ctx_.u(28.0F) + static_cast<float>(keeps.size()) * ctx_.u(21.0F);
  ph += static_cast<float>(note_lines.size()) * (ctx_.u(11.5F) + ctx_.u(4.0F)) + ctx_.u(8.0F);
  if (modal_.kind == Modal::Kind::remove_entries || (is_plan && modal_.plan.kind == PlanKind::failed_partial)) ph += ctx_.u(34.0F);
  if (is_plan) ph += ctx_.u(46.0F);
  ph += ctx_.u(70.0F);
  ph = std::min(ph, h - ctx_.u(24.0F));
  const ImVec2 a((w - pw) * 0.5F, std::max(ctx_.u(10.0F), (h - ph) * 0.5F)), b(a.x + pw, a.y + ph);
  dl->AddRectFilled(ImVec2(a.x - ctx_.u(2.0F), a.y - ctx_.u(2.0F)), ImVec2(b.x + ctx_.u(2.0F), b.y + ctx_.u(2.0F)), col::glow_soft, ctx_.u(10.0F));
  dl->AddRectFilled(a, b, rgba(9, 21, 48), ctx_.u(8.0F));
  dl->AddRect(a, b, col::glow, ctx_.u(8.0F), 0, std::max(1.0F, ctx_.u(1.5F)));
  const float lx = a.x + ctx_.u(24.0F);
  // Long scope/exclusion lists scroll inside the sheet; confirmation controls never leave the window.
  ImGui::SetCursorScreenPos(ImVec2(lx, a.y + ctx_.u(22.0F)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##confirmation_scope", ImVec2(inner, ph - ctx_.u(100.0F)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  dl = ImGui::GetWindowDrawList();
  float y = ImGui::GetCursorScreenPos().y;
  if (modal_.kind != Modal::Kind::message) {
    const bool destructive = is_plan || modal_.kind == Modal::Kind::remove_entries || modal_.kind == Modal::Kind::clear_library;
    const float isz = ctx_.u(40.0F);
    dl->AddRectFilled(ImVec2(lx, y), ImVec2(lx + isz, y + isz), destructive ? rgba(60, 20, 30) : rgba(14, 40, 92), ctx_.u(6.0F));
    icon_image(dl, ctx_.icon(destructive ? "trash" : "info"), ImVec2(lx + ctx_.u(8.0F), y + ctx_.u(8.0F)), ctx_.u(24.0F), destructive ? col::err : col::info);
    draw_text(dl, ctx_.bold, ctx_.u(17.0F), col::text, ImVec2(lx + isz + ctx_.u(14.0F), y), ellipsize(ctx_.bold, ctx_.u(17.0F), inner - isz - ctx_.u(40.0F), modal_.title).c_str());
    float sy = y + ctx_.u(24.0F);
    for (const auto &l : wrapped_sub) { draw_text(dl, ctx_.regular, px, col::text2, ImVec2(lx + isz + ctx_.u(14.0F), sy), l.c_str()); sy += line_h; }
    y = std::max(sy, y + isz) + ctx_.u(12.0F);
  } else {
    draw_text(dl, ctx_.bold, ctx_.u(16.0F), col::text, ImVec2(lx, y), ellipsize(ctx_.bold, ctx_.u(16.0F), inner, modal_.title).c_str());
    y += ctx_.u(30.0F);
    for (const auto &l : msg_lines) { draw_text(dl, ctx_.regular, px, col::text2, ImVec2(lx, y), l.c_str()); y += line_h; }
  }
  if (!cats.empty()) {
    for (const auto &c : cats) {
      const ImVec2 r0(lx, y), r1(lx + inner, y + ctx_.u(24.0F));
      dl->AddRectFilled(r0, r1, rgba(12, 28, 62), ctx_.u(4.0F));
      icon_image(dl, ctx_.icon("check"), ImVec2(r0.x + ctx_.u(8.0F), r0.y + ctx_.u(4.0F)), ctx_.u(16.0F), col::btn_top);
      draw_text(dl, ctx_.regular, px, col::text, ImVec2(r0.x + ctx_.u(32.0F), r0.y + ctx_.u(5.0F)), c.first.c_str());
      const std::string sz = format_bytes(c.second);
      draw_text(dl, ctx_.regular, px, col::text2, ImVec2(r1.x - ctx_.u(10.0F) - text_width(ctx_.regular, px, sz.c_str()), r0.y + ctx_.u(5.0F)), sz.c_str());
      y += ctx_.u(26.0F);
    }
    y += ctx_.u(14.0F);
  }
  if (!keeps.empty()) {
    draw_text(dl, ctx_.bold, px, col::text, ImVec2(lx, y), "Keep the following (not removed):");
    y += ctx_.u(26.0F);
    for (const auto &k : keeps) {
      icon_image(dl, ctx_.icon("check"), ImVec2(lx + ctx_.u(2.0F), y - ctx_.u(1.0F)), ctx_.u(15.0F), col::ok);
      draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(lx + ctx_.u(24.0F), y), k.c_str());
      y += ctx_.u(21.0F);
    }
  }
  for (const auto &l : note_lines) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::dim, ImVec2(lx, y), l.c_str()); y += ctx_.u(11.5F) + ctx_.u(4.0F); }
  y += ctx_.u(6.0F);
  if (modal_.kind == Modal::Kind::remove_entries) { ImGui::SetCursorScreenPos(ImVec2(lx, y)); checkbox(ctx_, "mod_cache", "Also delete their build folders from the cache", &modal_.delete_cache); y += ctx_.u(34.0F); }
  if (is_plan && modal_.plan.kind == PlanKind::failed_partial) { ImGui::SetCursorScreenPos(ImVec2(lx, y)); checkbox(ctx_, "mod_logs", "Keep build.log and status.json of failed builds", &modal_.keep_logs); y += ctx_.u(34.0F); }
  if (is_plan) {
    draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text2, ImVec2(lx, y + ctx_.u(10.0F)), "Total space to free:");
    const std::string total = format_bytes(modal_.plan.bytes);
    const float tpx = ctx_.u(26.0F);
    draw_text(dl, ctx_.bold, tpx, col::info, ImVec2(lx + inner - text_width(ctx_.bold, tpx, total.c_str()), y), total.c_str());
    y += ctx_.u(46.0F);
  }
  ImGui::SetCursorScreenPos(ImVec2(lx, y));
  ImGui::Dummy(ImVec2(inner, ctx_.u(8.0F)));
  ImGui::EndChild();
  ImGui::PopStyleVar();
  const float bw = (inner - ctx_.u(12.0F)) * 0.5F, bh = ctx_.u(38.0F), by = b.y - ctx_.u(24.0F) - bh;
  bool confirm = false, cancel = false;
  if (modal_.kind != Modal::Kind::message) {
    const char *confirm_label = is_plan ? "Remove Files" : (modal_.kind == Modal::Kind::restore_defaults ? "Restore" : "Remove");
    ImGui::SetCursorScreenPos(ImVec2(lx, by));
    cancel = button(ctx_, "mod_cancel", "Cancel", ImVec2(bw, bh), Style::secondary);
    ImGui::SetCursorScreenPos(ImVec2(lx + bw + ctx_.u(12.0F), by));
    confirm = button(ctx_, "mod_ok", confirm_label, ImVec2(bw, bh), modal_.kind == Modal::Kind::restore_defaults ? Style::primary : Style::danger, !is_plan || !modal_.plan.items.empty(), modal_.kind == Modal::Kind::restore_defaults ? nullptr : ctx_.icon("trash"));
  } else {
    ImGui::SetCursorScreenPos(ImVec2(b.x - ctx_.u(24.0F) - ctx_.u(150.0F), by));
    cancel = button(ctx_, "mod_ok", "OK", ImVec2(ctx_.u(150.0F), bh), Style::primary);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
  ImGui::End();
  if (confirm) {
    const Modal m = modal_;
    modal_ = Modal{};
    if (m.kind == Modal::Kind::plan) {
      Plan plan = m.plan;
      if (plan.kind == PlanKind::failed_partial) plan = plan_failed_partial(report_, m.keep_logs, busy_);
      run_plan(plan);
    } else if (m.kind == Modal::Kind::remove_entries) {
      if (m.delete_cache) run_plan(plan_remove_from_library(report_, m.ids, true, busy_));
      else {
        for (const auto &id : m.ids) { catalog_.remove(id); selected_.erase(id); storage_selected_.erase(id); if (detail_id_ == id) page_ = Page::library; }
        save_catalog();
        request_status_refresh();
        request_scan();
        toast("Removed " + std::to_string(m.ids.size()) + " game(s) from the library.");
      }
    } else if (m.kind == Modal::Kind::clear_library) {
      const std::size_t n = catalog_.entries().size();
      catalog_.entries().clear();
      selected_.clear(); storage_selected_.clear();
      save_catalog();
      request_status_refresh();
      request_scan();
      page_ = Page::library;
      toast("Library cleared (" + std::to_string(n) + " entries). Builds and ROMs were not touched.");
    } else if (m.kind == Modal::Kind::restore_defaults) {
      settings_ = default_launcher_settings();
      save_settings();
      toast("Launcher preferences restored.");
    }
  } else if (cancel) {
    modal_ = Modal{};
  }
}

} // namespace launcher
