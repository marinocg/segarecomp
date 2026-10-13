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

void App::draw_background(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), rgba(3, 8, 22));
  // The original sunset artwork as the title band, fading into the page.
  const float band = ctx_.u(92.0F);
  if (SDL_Texture *bg = ctx_.asset("background.png")) {
    float bw = 0, bh = 0;
    SDL_GetTextureSize(bg, &bw, &bh);
    const float frac = (bw / (w / band)) / bh;  // the source rows that fill the band at full width
    const float v0 = 0.285F, v1 = std::min(1.0F, v0 + frac);
    dl->AddImage(reinterpret_cast<ImTextureID>(bg), ImVec2(0, 0), ImVec2(w, band), ImVec2(0, v0), ImVec2(1, v1));
  }
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, band), rgba(3, 8, 22, 40), rgba(3, 8, 22, 40), rgba(3, 8, 22, 255), rgba(3, 8, 22, 255));
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.55F, band), rgba(3, 8, 22, 170), rgba(3, 8, 22, 0), rgba(3, 8, 22, 200), rgba(3, 8, 22, 0));
}

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
  const float m = ctx_.u(12.0F), band = ctx_.u(92.0F);
  if (SDL_Texture *wm = ctx_.asset("wordmark.png")) {
    float tw = 0, th = 0;
    SDL_GetTextureSize(wm, &tw, &th);
    const float hh = ctx_.u(86.0F), ww2 = hh * tw / th;
    dl->AddImage(reinterpret_cast<ImTextureID>(wm), ImVec2(m, ctx_.u(2.0F)), ImVec2(m + ww2, ctx_.u(2.0F) + hh));
  }
  {
    const char *tag = "RECOMPILE   *   PRESERVE   *   PLAY";
    const float px = ctx_.u(11.0F);
    if (win_w_ > ctx_.u(820.0F)) draw_text(dl, ctx_.display, px, rgba(190, 215, 255), ImVec2(win_w_ - m - text_width(ctx_.display, px, tag), band * 0.5F - px * 0.5F), tag);
  }
  const float top = band - ctx_.u(8.0F);
  const ImVec2 pa(m, top), pb(win_w_ - m, win_h_ - m);
  panel(ctx_, dl, pa, pb);
  const float sw = win_w_ < ctx_.u(900.0F) ? ctx_.u(62.0F) : ctx_.u(196.0F);
  draw_sidebar(pa.x, pa.y, sw, pb.y - pa.y);
  const float cx = pa.x + sw + ctx_.u(6.0F), cw = pb.x - cx - ctx_.u(4.0F), cy = pa.y + ctx_.u(4.0F), ch = pb.y - cy - ctx_.u(4.0F);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ctx_.u(18.0F), ctx_.u(14.0F)));
  ImGui::SetCursorScreenPos(ImVec2(cx, cy));
  ImGui::BeginChild("##content", ImVec2(cw, ch), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
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
  struct Nav { Page page; const char *label; const char *icon; };
  const Nav items[] = {{Page::library, "Library", "library"}, {Page::import_page, "Import ROMs", "import"}, {Page::details, "Game Details", "info"}, {Page::storage, "Storage", "storage"}, {Page::settings, "Settings", "settings"}};
  for (const auto &item : items) {
    const bool is_details = item.page == Page::details;
    const bool active = page_ == item.page;
    const bool enabled = !is_details || !catalog_.entries().empty();
    ImGui::SetCursorScreenPos(ImVec2(x + pad, cy));
    const ImVec2 size(w - 2 * pad, ctx_.u(38.0F));
    ImGui::PushID(item.label);
    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton("##nav", size);
    if (!enabled) ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered() && enabled, focused = ImGui::IsItemFocused();
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, active ? rgba(52, 128, 238) : (hovered ? col::panel_hover : rgba(10, 24, 56)), ctx_.u(5.0F));
    dl->AddRect(a, b, active ? rgba(120, 190, 255) : rgba(34, 70, 140), ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
    const float isz = ctx_.u(16.0F);
    icon_image(dl, ctx_.icon(item.icon), ImVec2(wide ? a.x + ctx_.u(12.0F) : (a.x + b.x) * 0.5F - isz * 0.5F, (a.y + b.y) * 0.5F - isz * 0.5F), isz, enabled ? IM_COL32_WHITE : rgba(90, 105, 140));
    if (wide) draw_text(dl, ctx_.bold, ctx_.u(13.0F), active ? col::btn_text : (enabled ? col::text : col::dim), ImVec2(a.x + ctx_.u(40.0F), (a.y + b.y) * 0.5F - ctx_.u(13.0F) * 0.55F), item.label);
    if (focused) focus_ring(ctx_, dl, a, b);
    if (pressed) {
      if (is_details) {
        if (!detail_id_.empty() && catalog_.find_id(detail_id_) != nullptr) open_details(detail_id_);
        else if (const auto vis = visible_entries(); !vis.empty()) open_details(vis.front()->id());
      } else go(item.page);
    }
    cy += ctx_.u(46.0F);
  }
  const float px = ctx_.u(12.0F);
  float by = y + h - pad;
  if (wide) {
    std::error_code ec;
    const auto space = fs::space(fs::exists(cache_dir_, ec) ? cache_dir_ : cache_dir_.parent_path(), ec);
    by -= ctx_.u(34.0F);
    if (!ec && space.capacity > 0) {
      const bool low = space.available < (2ULL << 30U);
      const std::string free_text = format_bytes(space.available) + " free" + (low ? " - low" : "");
      draw_text(dl, ctx_.regular, px, low ? col::err : col::text2, ImVec2(x + pad, by), free_text.c_str());
      const double used = 1.0 - static_cast<double>(space.available) / static_cast<double>(space.capacity);
      progress_bar(ctx_, dl, ImVec2(x + pad, by + px + ctx_.u(6.0F)), ImVec2(x + pad + (w - 2 * pad - ctx_.u(34.0F)), by + px + ctx_.u(12.0F)), static_cast<float>(used), low ? col::err : col::btn_top);
    }
    ImGui::SetCursorScreenPos(ImVec2(x + w - pad - ctx_.u(28.0F), by - ctx_.u(2.0F)));
    if (icon_button(ctx_, "open_cache", ctx_.icon("folder"), ctx_.u(28.0F), false, true, "Open the cache folder")) SDL_OpenURL(("file://" + u8s(cache_dir_)).c_str());
    char version[64];
    std::snprintf(version, sizeof version, "v%s", SEGARECOMP_LAUNCHER_VERSION);
    draw_text(dl, ctx_.regular, ctx_.u(10.5F), col::dim, ImVec2(x + pad, by + ctx_.u(32.0F)), version);
  }
}

// ---------------------------------------------------------------- library

void App::draw_drop_zone(float w, float h, bool compact_box) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float bw = w, bh = compact_box ? ctx_.u(322.0F) : std::max(ctx_.u(260.0F), h - ctx_.u(110.0F));
  const ImVec2 a(origin.x, origin.y), b(a.x + bw, a.y + bh);
  const bool hover = ImGui::IsMouseHoveringRect(a, b, false);
  const bool active = dragging_;
  dl->AddRectFilled(a, b, active ? rgba(10, 34, 70) : (hover ? col::panel_hover : rgba(8, 18, 42)), ctx_.u(8.0F));
  dashed_rect(dl, ImVec2(a.x + ctx_.u(6.0F), a.y + ctx_.u(6.0F)), ImVec2(b.x - ctx_.u(6.0F), b.y - ctx_.u(6.0F)), active ? col::dash_hover : col::dash, std::max(1.0F, std::round(ctx_.u(2.0F))), ctx_.u(10.0F), ctx_.u(7.0F));
  const float cx = (a.x + b.x) * 0.5F;
  float cy = a.y + (bh - (compact_box ? ctx_.u(170.0F) : ctx_.u(210.0F))) * 0.5F;
  if (SDL_Texture *drop = ctx_.asset("icon-drop.png")) { const float isz = ctx_.u(compact_box ? 52.0F : 64.0F); icon_image(dl, drop, ImVec2(cx - isz * 0.5F, cy), isz, active ? rgba(150, 235, 255) : IM_COL32_WHITE); cy += isz + ctx_.u(10.0F); }
  const char *headline = active ? "Release to add the ROMs" : "Drag & drop ROM files here";
  const float hpx = ctx_.u(compact_box ? 18.0F : 21.0F);
  draw_text(dl, ctx_.bold, hpx, col::text, ImVec2(cx - text_width(ctx_.bold, hpx, headline) * 0.5F, cy), headline);
  cy += hpx + ctx_.u(8.0F);
  const char *note = "Supports Genesis / Mega Drive (.md .gen .bin .smd) and Master System (.sms)";
  const float npx = ctx_.u(12.0F);
  draw_text(dl, ctx_.regular, npx, col::text2, ImVec2(cx - text_width(ctx_.regular, npx, note) * 0.5F, cy), note);
  cy += npx + ctx_.u(16.0F);
  ImGui::SetCursorScreenPos(ImVec2(cx - ctx_.u(90.0F), cy));
  if (button(ctx_, "dz_browse", "Browse for ROMs...", ImVec2(ctx_.u(180.0F), ctx_.u(36.0F)), Style::primary, !import_job_.valid(), ctx_.icon("folder"))) browse_import();
  ImGui::SetCursorScreenPos(ImVec2(origin.x, b.y + ctx_.u(8.0F)));
  ImGui::Dummy(ImVec2(w, ctx_.u(2.0F)));
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
  card(ctx_, dl, a, b, selected || focused, hovered);
  const float pad = ctx_.u(8.0F);
  const float cw = w - 2 * pad, chh = cw * 0.92F;
  const ImVec2 ca(x + pad, y + pad), cb(x + pad + cw, y + pad + chh);
  if (SDL_Texture *cover = cover_texture(entry)) {
    float tw = 0, th = 0;
    SDL_GetTextureSize(cover, &tw, &th);
    const float frame_ratio = cw / chh, tex_ratio = tw / th;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (tex_ratio > frame_ratio) { const float k = frame_ratio / tex_ratio; uv0.x = (1 - k) * 0.5F; uv1.x = uv0.x + k; }
    else { const float k = tex_ratio / frame_ratio; uv0.y = (1 - k) * 0.5F; uv1.y = uv0.y + k; }
    dl->AddImageRounded(reinterpret_cast<ImTextureID>(cover), ca, cb, uv0, uv1, info.status == GameStatus::ready ? IM_COL32_WHITE : rgba(200, 208, 230), ctx_.u(4.0F));
  }
  {
    const float fs_ = ctx_.u(22.0F);
    ImGui::SetCursorScreenPos(ImVec2(cb.x - fs_ - ctx_.u(4.0F), ca.y + ctx_.u(4.0F)));
    if (icon_button(ctx_, "fav", ctx_.icon("star"), fs_, entry.favorite, true, entry.favorite ? "Remove from favorites" : "Add to favorites")) {
      if (LibraryEntry *e = catalog_.find_id(entry.id())) { e->favorite = !e->favorite; save_catalog(); }
    }
  }
  if (select_mode_) {
    bool on = selected;
    ImGui::SetCursorScreenPos(ImVec2(ca.x + ctx_.u(5.0F), ca.y + ctx_.u(5.0F)));
    if (checkbox(ctx_, "sel", "", &on)) { if (on) selected_.insert(entry.id()); else selected_.erase(entry.id()); }
  }
  const float tpx = ctx_.u(13.5F), spx = ctx_.u(11.0F);
  float ty = cb.y + ctx_.u(9.0F);
  draw_text(dl, ctx_.bold, tpx, col::text, ImVec2(ca.x, ty), ellipsize(ctx_.bold, tpx, cw, entry.title()).c_str());
  ty += tpx + ctx_.u(7.0F);
  ImVec2 psize;
  pill(ctx_, dl, ImVec2(ca.x, ty), platform_short(entry.platform_id), col::info, ctx_.icon("controller"), ctx_.u(11.0F), &psize);
  ty += psize.y + ctx_.u(7.0F);
  const auto kv = [&](const char *k, const std::string &v) {
    draw_text(dl, ctx_.regular, spx, col::dim, ImVec2(ca.x, ty), k);
    draw_text(dl, ctx_.regular, spx, col::text2, ImVec2(ca.x + ctx_.u(80.0F), ty), ellipsize(ctx_.regular, spx, cw - ctx_.u(80.0F), v).c_str());
    ty += spx + ctx_.u(4.0F);
  };
  kv("Last played", fmt_relative(entry.last_played_at, std::time(nullptr)));
  kv("Compiled", entry.last_built_at > 0 ? fmt_date(entry.last_built_at).substr(0, 10) : "-");
  ty += ctx_.u(3.0F);
  dl->AddCircleFilled(ImVec2(ca.x + ctx_.u(5.0F), ty + spx * 0.55F), ctx_.u(4.5F), status_color(info));
  draw_text(dl, ctx_.bold, spx, status_color(info), ImVec2(ca.x + ctx_.u(15.0F), ty), ellipsize(ctx_.bold, spx, cw * 0.55F, status_label(info)).c_str());
  if (report_valid_)
    if (const EntryUsage *u = report_.find(entry.id()); u != nullptr && u->total.total() > 0) {
      const std::string size = format_bytes(u->total.total());
      const float sw = text_width(ctx_.bold, ctx_.u(10.5F), size.c_str()) + ctx_.u(34.0F);
      pill(ctx_, dl, ImVec2(cb.x - sw, ty - ctx_.u(3.0F)), size.c_str(), col::text2, ctx_.icon("storage"), ctx_.u(10.5F));
    }
  if (info.rom_missing && !entry.rom_path_unknown) draw_text(dl, ctx_.regular, spx, col::warn, ImVec2(ca.x, ty + spx + ctx_.u(6.0F)), "ROM file missing");
  if (info.status == GameStatus::ready && !select_mode_ && hovered) {
    const float ps = ctx_.u(30.0F);
    ImGui::SetCursorScreenPos(ImVec2(ca.x + (cw - ps) * 0.5F, ca.y + (chh - ps) * 0.5F));
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
  card(ctx_, dl, a, b, selected || focused, hovered);
  const float pad = ctx_.u(7.0F), th = h - 2 * pad, tw = th * 0.9F;
  float x = a.x + pad;
  if (select_mode_) {
    bool on = selected;
    ImGui::SetCursorScreenPos(ImVec2(x, a.y + (h - ctx_.u(22.0F)) * 0.5F));
    if (checkbox(ctx_, "sel", "", &on)) { if (on) selected_.insert(entry.id()); else selected_.erase(entry.id()); }
    x += ctx_.u(30.0F);
  }
  if (SDL_Texture *cover = cover_texture(entry)) dl->AddImageRounded(reinterpret_cast<ImTextureID>(cover), ImVec2(x, a.y + pad), ImVec2(x + tw, a.y + pad + th), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, ctx_.u(3.0F));
  x += tw + pad * 1.5F;
  const float px = ctx_.u(13.5F), spx = ctx_.u(11.5F);
  const float title_w = std::max(ctx_.u(120.0F), w * 0.30F);
  draw_text(dl, ctx_.bold, px, col::text, ImVec2(x, a.y + h * 0.5F - px * 1.05F), ellipsize(ctx_.bold, px, title_w, entry.title()).c_str());
  ImVec2 ps;
  pill(ctx_, dl, ImVec2(x, a.y + h * 0.5F + ctx_.u(1.0F)), platform_short(entry.platform_id), col::info, nullptr, ctx_.u(10.0F), &ps);
  x += title_w + pad;
  dl->AddCircleFilled(ImVec2(x + ctx_.u(5.0F), a.y + h * 0.5F), ctx_.u(4.5F), status_color(info));
  draw_text(dl, ctx_.bold, spx, status_color(info), ImVec2(x + ctx_.u(15.0F), a.y + h * 0.5F - spx * 0.55F), status_label(info).c_str());
  x += ctx_.u(140.0F);
  if (w > ctx_.u(640.0F)) {
    draw_text(dl, ctx_.regular, spx, col::text2, ImVec2(x, a.y + h * 0.5F - spx * 0.55F), ("Played " + fmt_relative(entry.last_played_at, std::time(nullptr))).c_str());
    x += ctx_.u(130.0F);
  }
  if (report_valid_ && w > ctx_.u(520.0F))
    if (const EntryUsage *u = report_.find(entry.id())) pill(ctx_, dl, ImVec2(x, a.y + (h - ctx_.u(21.0F)) * 0.5F), format_bytes(u->total.total()).c_str(), col::text2, ctx_.icon("storage"), ctx_.u(10.5F));
  if (entry.favorite) icon_image(dl, ctx_.icon("star"), ImVec2(b.x - ctx_.u(86.0F), a.y + h * 0.5F - ctx_.u(8.0F)), ctx_.u(16.0F), col::warn);
  if (info.status == GameStatus::ready && !select_mode_) {
    const float ps2 = ctx_.u(28.0F);
    ImGui::SetCursorScreenPos(ImVec2(b.x - ps2 - pad, a.y + (h - ps2) * 0.5F));
    if (icon_button(ctx_, "play", ctx_.icon("play"), ps2, true, !game_ && !job_, "Play")) { restarted_this_play_ = false; notices_.erase(entry.id()); launch(entry.id(), false); }
  }
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ctx_.u(5.0F)));
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
    draw_drop_zone(w, h - ctx_.u(60.0F), false);
    return;
  }
  const float bh = ctx_.u(34.0F);
  {
    if (focus_search_) { ImGui::SetKeyboardFocusHere(); focus_search_ = false; }
    const float import_w = ctx_.u(132.0F);
    input_text(ctx_, "search", search_, sizeof search_, std::max(ctx_.u(140.0F), w - import_w - ctx_.u(12.0F)), "Search games...  (Ctrl+F)");
    ImGui::SameLine(0.0F, ctx_.u(12.0F));
    if (button(ctx_, "lib_import", "Import ROMs", ImVec2(import_w, ctx_.u(36.0F)), Style::primary, !import_job_.valid(), ctx_.icon("import"))) browse_import();
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
  {
    int platform = platform_filter_, status = status_filter_;
    static const char *sort_ids[] = {"last_played", "title", "last_built", "added", "size"};
    int sort = 0;
    for (int i = 0; i < 5; ++i) if (settings_.sort == sort_ids[i]) sort = i;
    const float dw = ctx_.u(160.0F);
    if (dropdown(ctx_, "pf", {"All Platforms", "Genesis", "Master System"}, &platform, dw, ctx_.icon("controller"))) platform_filter_ = platform;
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (dropdown(ctx_, "sf", {"All Statuses", "Built", "Needs build", "Failed", "Missing ROM"}, &status, dw)) status_filter_ = status;
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (dropdown(ctx_, "so", {"Last Played", "Title", "Last Built", "Date Added", "Size"}, &sort, dw)) { settings_.sort = sort_ids[sort]; save_settings(); }
    const float tools = ctx_.u(34.0F) * 2 + ctx_.u(8.0F) + ctx_.u(88.0F) + ctx_.u(4.0F);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + ctx_.u(8.0F), w - tools + ctx_.u(18.0F)), 0.0F);
    if (icon_button(ctx_, "view_grid", ctx_.icon("library"), bh, settings_.view == "grid", true, "Grid view")) { settings_.view = "grid"; save_settings(); }
    ImGui::SameLine(0.0F, ctx_.u(4.0F));
    if (icon_button(ctx_, "view_list", ctx_.icon("file"), bh, settings_.view == "list", true, "List view")) { settings_.view = "list"; save_settings(); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "select_mode", select_mode_ ? "Done" : "Select", ImVec2(ctx_.u(88.0F), bh), select_mode_ ? Style::primary : Style::secondary)) { select_mode_ = !select_mode_; if (!select_mode_) selected_.clear(); }
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
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
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
  if (settings_.view == "list") {
    const float row_h = ctx_.u(settings_.compact ? 44.0F : 56.0F);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()), row_h + ctx_.u(5.0F));
    while (clipper.Step())
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) draw_list_row(*visible[static_cast<std::size_t>(i)], w, row_h);
  } else {
    const float gap = ctx_.u(14.0F), card_w = ctx_.u(settings_.compact ? 140.0F : 178.0F);
    const float usable = w - ctx_.u(14.0F);
    const int columns = std::max(1, static_cast<int>((usable + gap) / (card_w + gap)));
    const float cw = card_w + (usable + gap - static_cast<float>(columns) * (card_w + gap)) / static_cast<float>(columns);
    const float card_h = (cw - ctx_.u(16.0F)) * 0.92F + ctx_.u(132.0F);
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
    keeps = {"Your ROM files (never deleted)", "Cover images in the covers folder"};
  } else if (modal_.kind == Modal::Kind::clear_library) {
    subtitle = "Every game is removed from the library and the launcher forgets its history.";
    keeps = {"Build folders in the cache (use Storage to delete them)", "Your ROM files"};
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
  const ImVec2 a((w - pw) * 0.5F, std::max(ctx_.u(10.0F), (h - ph) * 0.5F)), b(a.x + pw, a.y + ph);
  dl->AddRectFilled(ImVec2(a.x - ctx_.u(2.0F), a.y - ctx_.u(2.0F)), ImVec2(b.x + ctx_.u(2.0F), b.y + ctx_.u(2.0F)), col::glow_soft, ctx_.u(10.0F));
  dl->AddRectFilled(a, b, rgba(9, 21, 48), ctx_.u(8.0F));
  dl->AddRect(a, b, col::glow, ctx_.u(8.0F), 0, std::max(1.0F, ctx_.u(1.5F)));
  float y = a.y + ctx_.u(22.0F);
  const float lx = a.x + ctx_.u(24.0F);
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
  }
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
