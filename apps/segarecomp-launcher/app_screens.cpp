// The launcher's screens. Every control here does something real: no placeholder buttons, no fabricated status or metrics.

#include "app.hpp"

#include "app_helpers.hpp"
#include "covers.hpp"
#include "pixel_assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;
using namespace ui;

namespace launcher {

using namespace uih;

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
        return ctx_.asset("ui/cover-placeholder.png");
      }
    }
  }
  return ctx_.asset("ui/cover-placeholder.png");
}

// ---------------------------------------------------------------- frame

void App::draw_background(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (SDL_Texture *bg = ctx_.asset("background.png")) {
    float bw = 0, bh = 0;
    SDL_GetTextureSize(bg, &bw, &bh);
    const float cover = std::max(w / bw, h / bh), dw = bw * cover, dh = bh * cover;
    dl->AddImage(reinterpret_cast<ImTextureID>(bg), ImVec2((w - dw) * 0.5F, (h - dh) * 0.5F), ImVec2((w - dw) * 0.5F + dw, (h - dh) * 0.5F + dh));
  } else {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), rgba(8, 11, 26));
  }
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), rgba(4, 8, 20, 96));  // readability scrim over the artwork
}

void App::draw() {
  int ww = 0, wh = 0;
  SDL_GetWindowSize(window_, &ww, &wh);
  win_w_ = static_cast<float>(ww);
  win_h_ = static_cast<float>(wh);
  {
    const float display_scale = SDL_GetWindowDisplayScale(window_), density = SDL_GetWindowPixelDensity(window_);
    float s = (display_scale > 0.0F && density > 0.0F) ? std::clamp(display_scale / density, 1.0F, 4.0F) : 1.0F;
    if (SDL_getenv("SEGARECOMP_SHOT_SIZE")) s = 1.0F;
    // Fit narrow windows without clipping: the layout is designed for 720 points and up.
    ctx_.S = settings_.compact ? s * 0.88F : s;
  }
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(win_w_, win_h_));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  draw_background(win_w_, win_h_);
  const float m = ctx_.u(10.0F);
  const float sidebar_w = win_w_ < ctx_.u(900.0F) ? ctx_.u(58.0F) : ctx_.u(190.0F);
  const float main_x = m + sidebar_w + m, main_w = win_w_ - main_x - m;
  const float top_h = ctx_.u(50.0F);
  draw_sidebar(m, m, sidebar_w, win_h_ - 2 * m);
  draw_topbar(main_x, m, main_w, top_h);
  const float cy = m + top_h + m, ch = win_h_ - cy - m;
  ImGui::SetCursorScreenPos(ImVec2(main_x, cy));
  panel(ctx_, ImGui::GetWindowDrawList(), ImVec2(main_x, cy), ImVec2(main_x + main_w, cy + ch));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ctx_.u(14.0F), ctx_.u(12.0F)));
  ImGui::SetCursorScreenPos(ImVec2(main_x + ctx_.u(2.0F), cy + ctx_.u(2.0F)));
  ImGui::BeginChild("##content", ImVec2(main_w - ctx_.u(4.0F), ch - ctx_.u(4.0F)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
  const float inner_w = main_w - ctx_.u(4.0F) - ctx_.u(28.0F);
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
  // Esc goes back one level (never while typing in a field or inside a dialog).
  if (modal_.kind == Modal::Kind::none && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (page_ == Page::details) page_ = previous_page_ == Page::details ? Page::library : previous_page_;
    else if (page_ != Page::library) page_ = Page::library;
  }
  if (!ImGui::GetIO().WantTextInput && modal_.kind == Modal::Kind::none && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { page_ = Page::library; focus_search_ = true; }
}

void App::draw_toasts(float w, float h) {
  const Uint64 now = SDL_GetTicks();
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast &t) { return now > t.until; }), toasts_.end());
  if (toasts_.empty()) return;
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  float y = h - ctx_.u(14.0F);
  const float px = ctx_.u(12.0F);
  for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
    const auto lines = wrap(ctx_.regular, px, std::min(w - ctx_.u(40.0F), ctx_.u(520.0F)), it->text);
    float tw = 0;
    for (const auto &l : lines) tw = std::max(tw, text_width(ctx_.regular, px, l.c_str()));
    const float bw = tw + ctx_.u(28.0F), bh = static_cast<float>(lines.size()) * (px + ctx_.u(3.0F)) + ctx_.u(18.0F);
    const ImVec2 a((w - bw) * 0.5F, y - bh);
    bevel(dl, a, ImVec2(a.x + bw, a.y + bh), std::max(1.0F, ctx_.u(2.0F)), col::border_dark, it->error ? col::danger : col::edge, it->error ? col::err : col::highlight, it->error ? col::danger_bottom : col::edge, col::panel_dark);
    float ly = a.y + ctx_.u(9.0F);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, px, it->error ? col::err : col::text, ImVec2(a.x + ctx_.u(14.0F), ly), l.c_str()); ly += px + ctx_.u(3.0F); }
    y = a.y - ctx_.u(6.0F);
  }
}

// ---------------------------------------------------------------- sidebar / topbar

void App::draw_sidebar(float x, float y, float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  panel(ctx_, dl, ImVec2(x, y), ImVec2(x + w, y + h));
  const bool wide = w > ctx_.u(100.0F);
  const float pad = ctx_.u(10.0F);
  float cy = y + pad;
  if (SDL_Texture *wm = ctx_.asset("wordmark.png")) {
    float tw = 0, th = 0;
    SDL_GetTextureSize(wm, &tw, &th);
    const float ww = wide ? w - 2 * pad : w - ctx_.u(12.0F), hh = ww * th / tw;
    dl->AddImage(reinterpret_cast<ImTextureID>(wm), ImVec2(x + (w - ww) * 0.5F, cy), ImVec2(x + (w - ww) * 0.5F + ww, cy + hh));
    cy += hh + pad;
  }
  struct Nav { Page page; const char *label; const char *icon; };
  const Nav items[] = {{Page::library, "Library", "library"}, {Page::import_page, "Import ROMs", "import"}, {Page::storage, "Storage", "storage"}, {Page::settings, "Settings", "settings"}};
  ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
  ImGui::SetCursorScreenPos(ImVec2(x + pad * 0.6F, cy));
  for (const auto &item : items) {
    const bool active = page_ == item.page || (item.page == Page::library && page_ == Page::details);
    ImGui::SetCursorScreenPos(ImVec2(x + pad * 0.6F, cy));
    const ImVec2 size(w - pad * 1.2F, ctx_.u(40.0F));
    ImGui::PushID(item.label);
    const bool pressed = ImGui::InvisibleButton("##nav", size);
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float step = std::max(1.0F, ctx_.u(2.0F));
    bevel(dl, a, b, step, col::border_dark, active ? col::btn_inner : col::edge, active ? col::btn_top : (hovered ? col::highlight : col::edge), active ? col::btn_bottom : col::edge, active ? col::btn : (hovered ? col::panel_hover : col::panel_solid));
    const float isz = ctx_.u(16.0F);
    icon_image(dl, ctx_.icon(item.icon), ImVec2(wide ? a.x + ctx_.u(12.0F) : (a.x + b.x) * 0.5F - isz * 0.5F, (a.y + b.y) * 0.5F - isz * 0.5F), isz);
    if (wide) draw_text(dl, ctx_.bold, ctx_.u(13.0F), active ? col::btn_text : col::text, ImVec2(a.x + ctx_.u(38.0F), (a.y + b.y) * 0.5F - ctx_.u(13.0F) * 0.58F), item.label);
    if (focused) focus_ring(ctx_, dl, a, b);
    if (pressed) {
      if (item.page == Page::import_page && page_ == Page::import_page) browse_import();
      go(item.page);
    }
    cy += ctx_.u(46.0F);
  }
  ImGui::PopStyleColor();
  // bottom: free space gauge, cache folder link, version
  float by = y + h - pad;
  const float px = ctx_.u(11.0F);
  if (wide) {
    by -= px + ctx_.u(2.0F);
    char version[64];
    std::snprintf(version, sizeof version, "v%s", SEGARECOMP_LAUNCHER_VERSION);
    draw_text(dl, ctx_.regular, px, col::dim, ImVec2(x + pad, by), version);
    by -= ctx_.u(34.0F);
    ImGui::SetCursorScreenPos(ImVec2(x + pad, by));
    if (button(ctx_, "open_cache", "Cache folder", ImVec2(w - 2 * pad, ctx_.u(28.0F)), Style::ghost, true, ctx_.icon("folder"))) SDL_OpenURL(("file://" + u8s(cache_dir_)).c_str());
    by -= ctx_.u(30.0F);
    std::error_code ec;
    const auto space = fs::space(fs::exists(cache_dir_, ec) ? cache_dir_ : cache_dir_.parent_path(), ec);
    if (!ec && space.capacity > 0) {
      const std::string free_text = format_bytes(space.available) + " free";
      draw_text(dl, ctx_.regular, px, col::text2, ImVec2(x + pad, by), free_text.c_str());
      const float gy = by + px + ctx_.u(4.0F), gw = w - 2 * pad;
      dl->AddRectFilled(ImVec2(x + pad, gy), ImVec2(x + pad + gw, gy + ctx_.u(6.0F)), col::border_dark);
      const double used = 1.0 - static_cast<double>(space.available) / static_cast<double>(space.capacity);
      dl->AddRectFilled(ImVec2(x + pad, gy), ImVec2(x + pad + gw * static_cast<float>(used), gy + ctx_.u(6.0F)), used > 0.92 ? col::err : col::btn_top);
    }
  }
}

void App::draw_topbar(float x, float y, float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  panel(ctx_, dl, ImVec2(x, y), ImVec2(x + w, y + h));
  const char *title = "LIBRARY";
  switch (page_) {
    case Page::library: title = "LIBRARY"; break;
    case Page::details: title = "GAME DETAILS"; break;
    case Page::import_page: title = "IMPORT ROMS"; break;
    case Page::storage: title = "STORAGE MANAGEMENT"; break;
    case Page::settings: title = "SETTINGS"; break;
  }
  const float pad = ctx_.u(14.0F);
  draw_text(dl, ctx_.bold, ctx_.u(17.0F), col::info, ImVec2(x + pad, y + h * 0.5F - ctx_.u(17.0F) * 0.58F), title);
  // right side: activity chips, then Import
  float rx = x + w - pad;
  const float bh = ctx_.u(30.0F);
  const float by = y + (h - bh) * 0.5F;
  const float bw = ctx_.u(138.0F);
  rx -= bw;
  ImGui::SetCursorScreenPos(ImVec2(rx, by));
  if (button(ctx_, "top_import", "Import ROM", ImVec2(bw, bh), Style::primary, !import_job_.valid(), ctx_.icon("import"))) browse_import();
  const float px = ctx_.u(11.0F);
  const auto chip_text = [&](const std::string &text, ImU32 color) {
    const float tw = text_width(ctx_.bold, px, text.c_str()) + ctx_.u(16.0F);
    rx -= tw + ctx_.u(8.0F);
    ImVec2 size;
    badge(ctx_, dl, ImVec2(rx, y + h * 0.5F - (px + ctx_.u(8.0F)) * 0.5F), text.c_str(), color, px, &size);
  };
  if (game_) {
    const LibraryEntry *e = catalog_.find_id(game_id_);
    chip_text("PLAYING " + ellipsize(ctx_.bold, px, ctx_.u(140.0F), e != nullptr ? e->title() : std::string("game")), col::ok);
  } else if (job_) {
    const LibraryEntry *e = catalog_.find_id(job_id_);
    chip_text("BUILDING " + ellipsize(ctx_.bold, px, ctx_.u(140.0F), e != nullptr ? e->title() : std::string("game")), col::warn);
  } else if (import_job_.valid()) {
    chip_text("IMPORTING " + std::to_string(import_done_.load()) + "/" + std::to_string(import_total_), col::info);
  } else if (maintenance_running_) {
    chip_text("CLEANING UP", col::warn);
  }
}

// ---------------------------------------------------------------- library

void App::draw_drop_zone(float w, float h, bool compact_box) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float bw = std::min(w, ctx_.u(620.0F)), bh = compact_box ? ctx_.u(210.0F) : std::min(h - ctx_.u(40.0F), ctx_.u(360.0F));
  const ImVec2 a(origin.x + (w - bw) * 0.5F, origin.y + (compact_box ? 0.0F : std::max(0.0F, (h - bh) * 0.35F)));
  const ImVec2 b(a.x + bw, a.y + bh);
  const bool hover = ImGui::IsMouseHoveringRect(a, b, false);
  const bool active = dragging_;
  bevel(dl, a, b, ctx_.u(3.0F), col::border_dark, col::edge, active ? col::dash_hover : col::highlight, col::edge, active ? rgba(10, 34, 58) : (hover ? col::panel_hover : col::panel_solid));
  dashed_rect(dl, ImVec2(a.x + ctx_.u(14.0F), a.y + ctx_.u(14.0F)), ImVec2(b.x - ctx_.u(14.0F), b.y - ctx_.u(14.0F)), active ? col::dash_hover : col::dash, std::max(1.0F, ctx_.u(2.0F)), ctx_.u(9.0F), ctx_.u(6.0F));
  const float cx = (a.x + b.x) * 0.5F;
  float cy = a.y + ctx_.u(30.0F);
  if (SDL_Texture *drop = ctx_.asset("icon-drop.png")) { const float isz = ctx_.u(compact_box ? 44.0F : 56.0F); icon_image(dl, drop, ImVec2(cx - isz * 0.5F, cy), isz, active ? rgba(150, 235, 255) : IM_COL32_WHITE); cy += isz + ctx_.u(12.0F); }
  const char *headline = active ? "Release to add the ROMs" : "Drop ROMs here";
  const float hpx = ctx_.u(compact_box ? 17.0F : 21.0F);
  draw_text(dl, ctx_.bold, hpx, col::text, ImVec2(cx - text_width(ctx_.bold, hpx, headline) * 0.5F, cy), headline);
  cy += hpx + ctx_.u(10.0F);
  const char *note = "Your ROMs are referenced where they are: never copied, moved or uploaded.";
  const float npx = ctx_.u(11.5F);
  draw_text(dl, ctx_.regular, npx, col::text2, ImVec2(cx - text_width(ctx_.regular, npx, note) * 0.5F, cy), note);
  cy += npx + ctx_.u(14.0F);
  ImGui::SetCursorScreenPos(ImVec2(cx - ctx_.u(100.0F), cy));
  if (button(ctx_, "dz_browse", "Browse...", ImVec2(ctx_.u(200.0F), ctx_.u(40.0F)), Style::primary, !import_job_.valid(), ctx_.icon("folder"))) browse_import();
  if (!compact_box) {
    cy += ctx_.u(52.0F);
    const char *foot = "Genesis / Mega Drive and Master System. Adding a game does not build it.";
    draw_text(dl, ctx_.regular, npx, col::dim, ImVec2(cx - text_width(ctx_.regular, npx, foot) * 0.5F, cy), foot);
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x, b.y + ctx_.u(10.0F)));
  ImGui::Dummy(ImVec2(w, ctx_.u(4.0F)));
}

void App::draw_card(const LibraryEntry &entry, float x, float y, float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const StatusInfo &info = status_of(entry.id());
  const bool selected = select_mode_ && selected_.count(entry.id()) != 0;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ImGui::PushID(entry.id().c_str());
  ImGui::SetNextItemAllowOverlap();
  const bool pressed = ImGui::InvisibleButton("##card", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  const ImVec2 a(x, y), b(x + w, y + h);
  const float step = std::max(1.0F, ctx_.u(2.0F));
  bevel(dl, a, b, step, col::border_dark, selected ? col::btn_inner : col::edge, selected ? col::btn_top : (hovered ? col::highlight : col::edge), col::edge, hovered ? col::panel_hover : col::panel_solid);
  const float pad = ctx_.u(8.0F);
  const float cw = w - 2 * pad, chh = cw * 4.0F / 3.0F;
  const ImVec2 ca(x + pad, y + pad), cb(x + pad + cw, y + pad + chh);
  if (SDL_Texture *cover = cover_texture(entry)) {
    float tw = 0, th = 0;
    SDL_GetTextureSize(cover, &tw, &th);
    // cover-crop to the 3:4 frame, never distort
    const float frame_ratio = cw / chh, tex_ratio = tw / th;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (tex_ratio > frame_ratio) { const float k = frame_ratio / tex_ratio; uv0.x = (1 - k) * 0.5F; uv1.x = uv0.x + k; }
    else { const float k = tex_ratio / frame_ratio; uv0.y = (1 - k) * 0.5F; uv1.y = uv0.y + k; }
    dl->AddImage(reinterpret_cast<ImTextureID>(cover), ca, cb, uv0, uv1, info.status == GameStatus::ready ? IM_COL32_WHITE : rgba(190, 200, 225));
  }
  dl->AddRect(ca, cb, col::border_dark);
  const float bpx = ctx_.u(10.0F);
  ImVec2 bsize;
  badge(ctx_, dl, ImVec2(ca.x + ctx_.u(5.0F), ca.y + ctx_.u(5.0F)), platform_short(entry.platform_id), col::info, bpx, &bsize);
  // favorite toggle (own hit box over the cover corner)
  {
    const float fs_ = ctx_.u(24.0F);
    ImGui::SetCursorScreenPos(ImVec2(cb.x - fs_ - ctx_.u(3.0F), ca.y + ctx_.u(3.0F)));
    if (icon_button(ctx_, "fav", ctx_.icon("star"), fs_, entry.favorite, true, entry.favorite ? "Remove from favorites" : "Add to favorites")) {
      if (LibraryEntry *e = catalog_.find_id(entry.id())) { e->favorite = !e->favorite; save_catalog(); }
    }
  }
  if (select_mode_) {
    bool on = selected;
    ImGui::SetCursorScreenPos(ImVec2(ca.x + ctx_.u(5.0F), cb.y - ctx_.u(26.0F)));
    if (checkbox(ctx_, "sel", "", &on)) { if (on) selected_.insert(entry.id()); else selected_.erase(entry.id()); }
  }
  // text block
  const float tpx = ctx_.u(12.0F), spx = ctx_.u(10.5F);
  float ty = cb.y + ctx_.u(8.0F);
  const auto lines = wrap(ctx_.bold, tpx, cw, entry.title());
  for (std::size_t i = 0; i < std::min<std::size_t>(lines.size(), 2); ++i) {
    std::string line = lines[i];
    if (i == 1 && lines.size() > 2) line = ellipsize(ctx_.bold, tpx, cw, line + " ...");
    draw_text(dl, ctx_.bold, tpx, col::text, ImVec2(ca.x, ty), line.c_str());
    ty += tpx + ctx_.u(3.0F);
  }
  ty = std::max(ty, cb.y + ctx_.u(8.0F) + 2 * (tpx + ctx_.u(3.0F))) + ctx_.u(2.0F);
  dl->AddCircleFilled(ImVec2(ca.x + ctx_.u(4.0F), ty + spx * 0.5F + ctx_.u(1.0F)), ctx_.u(3.5F), status_color(info));
  draw_text(dl, ctx_.regular, spx, status_color(info), ImVec2(ca.x + ctx_.u(12.0F), ty), ellipsize(ctx_.regular, spx, cw - ctx_.u(12.0F), status_label(info)).c_str());
  ty += spx + ctx_.u(4.0F);
  const std::string played = "Played " + fmt_relative(entry.last_played_at, std::time(nullptr));
  draw_text(dl, ctx_.regular, spx, col::dim, ImVec2(ca.x, ty), ellipsize(ctx_.regular, spx, cw, played).c_str());
  if (info.rom_missing && !entry.rom_path_unknown) { ty += spx + ctx_.u(3.0F); draw_text(dl, ctx_.regular, spx, col::warn, ImVec2(ca.x, ty), "ROM file missing"); }
  if (focused) focus_ring(ctx_, dl, a, b);
  // Play on ready games
  if (info.status == GameStatus::ready && !select_mode_) {
    const float ps = ctx_.u(26.0F);
    ImGui::SetCursorScreenPos(ImVec2(b.x - ps - pad, b.y - ps - pad));
    if (icon_button(ctx_, "play", ctx_.icon("play"), ps, true, !game_ && !job_, "Play")) { restarted_this_play_ = false; notices_.erase(entry.id()); launch(entry.id(), false); }
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
  const bool pressed = ImGui::InvisibleButton("##row", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
  const ImVec2 b(a.x + w, a.y + h);
  bevel(dl, a, b, std::max(1.0F, ctx_.u(2.0F)), col::border_dark, selected ? col::btn_inner : col::edge, selected ? col::btn_top : (hovered ? col::highlight : col::edge), col::edge, hovered ? col::panel_hover : col::panel_solid);
  const float pad = ctx_.u(8.0F), th = h - 2 * pad, tw = th * 0.75F;
  float x = a.x + pad;
  if (select_mode_) {
    bool on = selected;
    ImGui::SetCursorScreenPos(ImVec2(x, a.y + (h - ctx_.u(22.0F)) * 0.5F));
    if (checkbox(ctx_, "sel", "", &on)) { if (on) selected_.insert(entry.id()); else selected_.erase(entry.id()); }
    x += ctx_.u(30.0F);
  }
  if (SDL_Texture *cover = cover_texture(entry)) dl->AddImage(reinterpret_cast<ImTextureID>(cover), ImVec2(x, a.y + pad), ImVec2(x + tw, a.y + pad + th));
  x += tw + pad;
  const float px = ctx_.u(12.5F), spx = ctx_.u(11.0F);
  const float title_w = std::max(ctx_.u(120.0F), w * 0.34F);
  draw_text(dl, ctx_.bold, px, col::text, ImVec2(x, a.y + h * 0.5F - px * 0.9F), ellipsize(ctx_.bold, px, title_w, entry.title()).c_str());
  draw_text(dl, ctx_.regular, spx, col::dim, ImVec2(x, a.y + h * 0.5F + ctx_.u(2.0F)), platform_short(entry.platform_id));
  x += title_w + pad;
  dl->AddCircleFilled(ImVec2(x + ctx_.u(4.0F), a.y + h * 0.5F), ctx_.u(3.5F), status_color(info));
  draw_text(dl, ctx_.regular, spx, status_color(info), ImVec2(x + ctx_.u(12.0F), a.y + h * 0.5F - spx * 0.55F), status_label(info).c_str());
  x += ctx_.u(130.0F);
  if (w > ctx_.u(640.0F)) {
    draw_text(dl, ctx_.regular, spx, col::text2, ImVec2(x, a.y + h * 0.5F - spx * 0.55F), ("Played " + fmt_relative(entry.last_played_at, std::time(nullptr))).c_str());
    x += ctx_.u(130.0F);
  }
  if (report_valid_ && w > ctx_.u(520.0F))
    if (const EntryUsage *u = report_.find(entry.id())) draw_text(dl, ctx_.regular, spx, col::text2, ImVec2(x, a.y + h * 0.5F - spx * 0.55F), format_bytes(u->total.total()).c_str());
  if (entry.favorite) icon_image(dl, ctx_.icon("star"), ImVec2(b.x - ctx_.u(80.0F), a.y + h * 0.5F - ctx_.u(8.0F)), ctx_.u(16.0F), col::warn);
  if (focused) focus_ring(ctx_, dl, a, b);
  if (info.status == GameStatus::ready && !select_mode_) {
    const float ps = ctx_.u(28.0F);
    ImGui::SetCursorScreenPos(ImVec2(b.x - ps - pad, a.y + (h - ps) * 0.5F));
    if (icon_button(ctx_, "play", ctx_.icon("play"), ps, true, !game_ && !job_, "Play")) { restarted_this_play_ = false; notices_.erase(entry.id()); launch(entry.id(), false); }
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ctx_.u(4.0F)));
  if (pressed) {
    if (select_mode_) { if (selected) selected_.erase(entry.id()); else selected_.insert(entry.id()); }
    else open_details(entry.id());
  }
}

void App::draw_library(float w, float h) {
  const auto &all = catalog_.entries();
  if (all.empty()) {
    if (!catalog_notice_.empty() || catalog_.read_only()) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, catalog_.notice());
    draw_drop_zone(w, h - ctx_.u(30.0F), false);
    return;
  }
  // search + view + select
  {
    const float bh = ctx_.u(32.0F);
    if (focus_search_) { ImGui::SetKeyboardFocusHere(); focus_search_ = false; }
    const float tools_w = ctx_.u(32.0F) * 3 + ctx_.u(8.0F) * 3 + ctx_.u(100.0F);
    input_text(ctx_, "search", search_, sizeof search_, std::max(ctx_.u(140.0F), w - tools_w), "Search games (Ctrl+F)");
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (icon_button(ctx_, "view_grid", ctx_.icon("library"), bh, settings_.view == "grid", true, "Grid view")) { settings_.view = "grid"; save_settings(); }
    ImGui::SameLine(0.0F, ctx_.u(4.0F));
    if (icon_button(ctx_, "view_list", ctx_.icon("file"), bh, settings_.view == "list", true, "List view")) { settings_.view = "list"; save_settings(); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "select_mode", select_mode_ ? "Done" : "Select", ImVec2(ctx_.u(92.0F), bh), select_mode_ ? Style::primary : Style::secondary)) { select_mode_ = !select_mode_; if (!select_mode_) selected_.clear(); }
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
  // filters (wrapping chip rows)
  {
    const float right = ImGui::GetWindowPos().x + ImGui::GetCursorPos().x + w;
    const auto flow = [&](const char *label_text, const std::vector<std::string> &items, int *selected, const char *id) {
      ImGui::PushID(id);
      const float lpx = ctx_.u(11.0F);
      ImGui::AlignTextToFramePadding();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      draw_text(ImGui::GetWindowDrawList(), ctx_.regular, lpx, col::dim, ImVec2(p.x, p.y + ctx_.u(7.0F)), label_text);
      ImGui::Dummy(ImVec2(text_width(ctx_.regular, lpx, label_text) + ctx_.u(8.0F), ctx_.u(26.0F)));
      for (std::size_t i = 0; i < items.size(); ++i) {
        ImGui::SameLine(0.0F, ctx_.u(4.0F));
        const float need = text_width(ctx_.bold, ctx_.u(12.0F), items[i].c_str()) + ctx_.u(20.0F);
        if (ImGui::GetCursorScreenPos().x + need > right) ImGui::NewLine();
        if (chip(ctx_, items[i].c_str(), items[i].c_str(), static_cast<int>(i) == *selected)) *selected = static_cast<int>(i);
      }
      ImGui::PopID();
    };
    flow("Platform", {"All", "Genesis", "Master System"}, &platform_filter_, "pf");
    ImGui::SameLine(0.0F, ctx_.u(16.0F));
    flow("Status", {"All", "Built", "Needs build", "Failed", "Missing ROM"}, &status_filter_, "sf");
    const std::vector<std::string> sorts{"Last played", "Title", "Last built", "Added", "Size"};
    static const char *sort_ids[] = {"last_played", "title", "last_built", "added", "size"};
    int sort_index = 0;
    for (int i = 0; i < 5; ++i) if (settings_.sort == sort_ids[i]) sort_index = i;
    flow("Sort", sorts, &sort_index, "so");
    if (settings_.sort != sort_ids[sort_index]) { settings_.sort = sort_ids[sort_index]; save_settings(); }
  }
  if (soft_limit_exceeded_) {
    ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    ImGui::TextUnformatted("");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, "Builds use more than your soft limit of " + std::to_string(settings_.cleanup.soft_limit_mb) + " MiB. Nothing is deleted automatically.");
    if (button(ctx_, "soft_review", "Review in Storage", ImVec2(ctx_.u(170.0F), ctx_.u(28.0F)), Style::secondary)) go(Page::storage);
  }
  if (select_mode_) {
    ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    const auto vis = visible_entries();
    if (button(ctx_, "sel_all", "Select all", ImVec2(ctx_.u(112.0F), ctx_.u(28.0F)), Style::secondary)) for (const auto *e : vis) selected_.insert(e->id());
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "sel_none", "Clear", ImVec2(ctx_.u(80.0F), ctx_.u(28.0F)), Style::secondary)) selected_.clear();
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "sel_storage", ("Manage storage (" + std::to_string(selected_.size()) + ")").c_str(), ImVec2(ctx_.u(190.0F), ctx_.u(28.0F)), Style::secondary, !selected_.empty())) { storage_selected_ = selected_; go(Page::storage); }
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "sel_remove", "Remove from library", ImVec2(ctx_.u(190.0F), ctx_.u(28.0F)), Style::danger, !selected_.empty())) {
      modal_ = Modal{};
      modal_.kind = Modal::Kind::remove_entries;
      modal_.ids.assign(selected_.begin(), selected_.end());
      modal_.title = "Remove " + std::to_string(modal_.ids.size()) + " game(s) from the library?";
    }
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  const auto visible = visible_entries();
  if (visible.empty()) {
    paragraph(ctx_, ctx_.regular, ctx_.u(13.0F), col::text2, w, "No game matches the search and filters.");
    return;
  }
  // The grid/list area scrolls on its own so the controls above stay put.
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float avail_h = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - start.y - ctx_.u(26.0F);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
  ImGui::BeginChild("##games", ImVec2(w + ctx_.u(8.0F), std::max(avail_h, ctx_.u(120.0F))), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  if (settings_.view == "list") {
    const float row_h = ctx_.u(settings_.compact ? 40.0F : 52.0F);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()), row_h + ctx_.u(4.0F));
    while (clipper.Step())
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) draw_list_row(*visible[static_cast<std::size_t>(i)], w, row_h);
  } else {
    const float gap = ctx_.u(12.0F), card_w = ctx_.u(settings_.compact ? 128.0F : 158.0F);
    const float usable = w - ctx_.u(16.0F);  // leave room for the scroll bar
    const int columns = std::max(1, static_cast<int>((usable + gap) / (card_w + gap)));
    const float cw = card_w + (usable + gap - static_cast<float>(columns) * (card_w + gap)) / static_cast<float>(columns);  // spread the leftover width
    const float card_h = (cw - ctx_.u(16.0F)) * 4.0F / 3.0F + ctx_.u(88.0F);
    const int rows = (static_cast<int>(visible.size()) + columns - 1) / columns;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGuiListClipper clipper;
    clipper.Begin(rows, card_h + gap);
    while (clipper.Step())
      for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r)
        for (int c = 0; c < columns; ++c) {
          const std::size_t index = static_cast<std::size_t>(r * columns + c);
          if (index >= visible.size()) break;
          draw_card(*visible[index], origin.x + static_cast<float>(c) * (cw + gap), origin.y + static_cast<float>(r) * (card_h + gap), cw, card_h);
        }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(rows) * (card_h + gap)));
    ImGui::Dummy(ImVec2(w, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  // summary line
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    char text[128];
    std::snprintf(text, sizeof text, "%zu game(s)%s", visible.size(), report_valid_ ? (", " + format_bytes(report_.builds_total()) + " of builds").c_str() : "");
    draw_text(ImGui::GetWindowDrawList(), ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(p.x, p.y + ctx_.u(2.0F)), text);
  }
  (void)h;
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
  const float pw = std::min(w - ctx_.u(40.0F), ctx_.u(600.0F));
  // measure
  std::vector<std::string> lines = modal_.lines;
  if (modal_.kind == Modal::Kind::plan) {
    lines.clear();
    lines.push_back("Scope: " + std::to_string(modal_.plan.entry_ids.size()) + " game(s), " + std::to_string(modal_.plan.items.size()) + " item(s).");
    lines.push_back("Reclaims about " + format_bytes(modal_.plan.bytes) + ".");
    for (const auto &r : modal_.plan.remains) lines.push_back("Keeps: " + r);
    for (const auto &n : modal_.plan.notes) lines.push_back(n);
    if (!modal_.plan.excluded.empty()) { lines.push_back("Excluded:"); for (const auto &e : modal_.plan.excluded) lines.push_back("  - " + e); }
    lines.push_back("This cannot be undone. Original ROM files are never touched.");
  } else if (modal_.kind == Modal::Kind::remove_entries) {
    lines.clear();
    lines.push_back("The selected games are removed from the library (titles, history, settings, last played).");
    lines.push_back("Your ROM files are never deleted. Cover images stay in the covers folder.");
  } else if (modal_.kind == Modal::Kind::clear_library) {
    lines = {"Every game is removed from the library and the launcher forgets its history.", "Build folders in the cache are not deleted (use Storage for that); ROM files are never touched."};
  } else if (modal_.kind == Modal::Kind::restore_defaults) {
    lines = {"Launcher preferences (view, sort, compact mode, cleanup policy, region order) return to their defaults.", "The library, builds and viewer controls are not affected."};
  }
  const float px = ctx_.u(12.0F);
  float body_h = 0;
  std::vector<std::string> wrapped;
  for (const auto &l : lines) for (const auto &line : wrap(ctx_.regular, px, pw - ctx_.u(36.0F), l)) wrapped.push_back(line);
  body_h = static_cast<float>(wrapped.size()) * (px + ctx_.u(4.0F));
  const float extra = modal_.kind == Modal::Kind::remove_entries ? ctx_.u(34.0F) : (modal_.kind == Modal::Kind::plan && modal_.plan.kind == PlanKind::failed_partial ? ctx_.u(34.0F) : 0.0F);
  const float ph = ctx_.u(70.0F) + body_h + extra + ctx_.u(52.0F);
  const ImVec2 a((w - pw) * 0.5F, (h - ph) * 0.5F), b(a.x + pw, a.y + ph);
  bevel(dl, a, b, std::max(1.0F, ctx_.u(2.0F)), col::border_dark, col::edge, col::highlight, col::edge, col::panel_dark);
  draw_text(dl, ctx_.bold, ctx_.u(15.0F), modal_.kind == Modal::Kind::message ? col::text : col::warn, ImVec2(a.x + ctx_.u(18.0F), a.y + ctx_.u(16.0F)), ellipsize(ctx_.bold, ctx_.u(15.0F), pw - ctx_.u(36.0F), modal_.title).c_str());
  float y = a.y + ctx_.u(46.0F);
  for (const auto &line : wrapped) { draw_text(dl, ctx_.regular, px, col::text2, ImVec2(a.x + ctx_.u(18.0F), y), line.c_str()); y += px + ctx_.u(4.0F); }
  ImGui::SetCursorScreenPos(ImVec2(a.x + ctx_.u(18.0F), y + ctx_.u(2.0F)));
  if (modal_.kind == Modal::Kind::remove_entries) checkbox(ctx_, "mod_cache", "Also delete their build folders from the cache", &modal_.delete_cache);
  if (modal_.kind == Modal::Kind::plan && modal_.plan.kind == PlanKind::failed_partial) checkbox(ctx_, "mod_logs", "Keep build.log and status.json of failed builds", &modal_.keep_logs);
  const float by = b.y - ctx_.u(46.0F), bw = ctx_.u(150.0F), bh = ctx_.u(34.0F);
  const bool needs_confirm = modal_.kind != Modal::Kind::message;
  ImGui::SetCursorScreenPos(ImVec2(b.x - ctx_.u(18.0F) - bw, by));
  bool confirm = false, cancel = false;
  if (needs_confirm) {
    const char *confirm_label = modal_.kind == Modal::Kind::plan ? "Delete" : (modal_.kind == Modal::Kind::restore_defaults ? "Restore" : "Remove");
    confirm = button(ctx_, "mod_ok", confirm_label, ImVec2(bw, bh), modal_.kind == Modal::Kind::restore_defaults ? Style::primary : Style::danger, modal_.kind != Modal::Kind::plan || !modal_.plan.items.empty());
    ImGui::SetCursorScreenPos(ImVec2(b.x - ctx_.u(18.0F) - 2 * bw - ctx_.u(8.0F), by));
    cancel = button(ctx_, "mod_cancel", "Cancel", ImVec2(bw, bh), Style::secondary);
  } else {
    cancel = button(ctx_, "mod_ok", "OK", ImVec2(bw, bh), Style::primary);
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
