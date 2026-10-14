// The contextual Game Details page (opened from a card, a list row or a game's context menu; never a sidebar entry).
//
// Hierarchy, top to bottom, all inside the first 1080x740 viewport except the provenance section:
//   A. identity: cover, editable title, platform / build state / ROM health, Play and Build as the two dominant actions, everything else in "More".
//   B. Build Configuration: Build Mode (Compatibility | Optimized) and the Genesis console region or the Master System mapper as large option tiles,
//      the state of the executable versus the selection, and the advanced options behind an expander.
//   C. Build provenance (digest, ROM link health, dates, toolchain, identity, diagnostics) below, calmer and secondary.
//   D. a storage rail with the measured footprint of this game's build files.
// Every value shown is read from the library, the build folder or the product: no invented compatibility, progress or sizes.

#include "app.hpp"

#include "app_helpers.hpp"
#include "covers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;
using namespace ui;
using namespace launcher::uih;

namespace launcher {

namespace {

const char *run_label(const std::string &run) {
  if (run == "clean") return "ended normally";
  if (run == "failed") return "stopped with an error";
  if (run == "unadmitted_pc") return "stopped on omitted code (restarted in Compatibility)";
  return "no result recorded";
}

void stage_chip(Ctx &ctx, ImDrawList *dl, ImVec2 p, const char *label, StageState state, float *width_out) {
  const float px = ctx.u(12.0F), w = text_width(ctx.bold, px, label) + ctx.u(34.0F), h = ctx.u(26.0F);
  const ImU32 edge = state == StageState::done ? col::ok : (state == StageState::running ? col::info : rgba(40, 70, 130));
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), state == StageState::running ? rgba(14, 40, 88) : rgba(8, 18, 42), ctx.u(5.0F));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), edge, ctx.u(5.0F), 0, std::max(1.0F, ctx.u(1.2F)));
  const ImVec2 c(p.x + ctx.u(13.0F), p.y + h * 0.5F);
  const float r = ctx.u(5.5F);
  if (state == StageState::done) {
    dl->PathLineTo(ImVec2(c.x - r * 0.75F, c.y)); dl->PathLineTo(ImVec2(c.x - r * 0.1F, c.y + r * 0.6F)); dl->PathLineTo(ImVec2(c.x + r * 0.85F, c.y - r * 0.6F));
    dl->PathStroke(col::ok, 0, ctx.u(2.0F));
  } else if (state == StageState::running) spinner(ctx, dl, c, r, col::info);
  else dl->AddCircle(c, r * 0.6F, col::dim, 16, ctx.u(1.3F));
  draw_text(dl, ctx.bold, px, state == StageState::pending ? col::dim : col::text, ImVec2(p.x + ctx.u(25.0F), p.y + (h - px) * 0.5F - ctx.u(1.0F)), label);
  *width_out = w;
}

// A section card with a Silkscreen heading; returns the y where its content starts.
float section_card(Ctx &ctx, ImDrawList *dl, ImVec2 a, ImVec2 b, const char *title, ImU32 fill = 0) {
  card(ctx, dl, a, b, false, false, fill != 0 ? fill : rgba(8, 19, 46, 240));
  draw_text(dl, ctx.display, ctx.u(12.5F), rgba(120, 205, 255), ImVec2(a.x + ctx.u(16.0F), a.y + ctx.u(13.0F)), title);
  return a.y + ctx.u(36.0F);
}

// The state line of a card header: a dot and short text.
void state_dot_text(Ctx &ctx, ImDrawList *dl, ImVec2 right_top, const std::string &text, ImU32 color, float max_w) {
  const float px = ctx.u(12.0F);
  const std::string shown = ellipsize(ctx.bold, px, max_w - ctx.u(16.0F), text);
  const float w = text_width(ctx.bold, px, shown.c_str());
  dl->AddCircleFilled(ImVec2(right_top.x - w - ctx.u(10.0F), right_top.y + px * 0.55F), ctx.u(4.0F), color);
  draw_text(dl, ctx.bold, px, color, ImVec2(right_top.x - w, right_top.y), shown.c_str());
}

} // namespace

// ---------------------------------------------------------------- shared game actions

std::string App::build_action_label(const StatusInfo &info) const {
  switch (info.status) {
    case GameStatus::ready: return "Rebuild";
    case GameStatus::metadata_only: return "Rebuild";
    case GameStatus::failed: return "Try again";
    case GameStatus::stale: return "Recompile";
    case GameStatus::not_built: return "Build";
  }
  return "Build";
}

bool App::can_build(const LibraryEntry &entry, const StatusInfo &info) const {
  const bool mapper_needed = entry.platform_id == "master-system" && entry.mapper.empty() && entry.mapper_manifest_sha256.empty();
  return !job_ && !game_ && !maintenance_running_ && !info.rom_missing && !mapper_needed;
}

bool App::can_play(const StatusInfo &info) const { return info.status == GameStatus::ready && !game_ && !job_ && !maintenance_running_; }

void App::play(const std::string &id) {
  restarted_this_play_ = false;
  notices_.erase(id);
  show_log_ = false;
  launch(id, false);
}

void App::ask_remove(const LibraryEntry &entry) {
  modal_ = Modal{};
  modal_.kind = Modal::Kind::remove_entries;
  modal_.ids = {entry.id()};
  modal_.title = "Remove \"" + ellipsize(ctx_.bold, ctx_.u(17.0F), ctx_.u(300.0F), entry.title()) + "\" from the library?";
}

// The overflow / context menu of one game. Rare and destructive actions live here, never beside Play and Build.
void App::draw_game_menu(const std::string &popup_id, const std::string &id, bool include_details) {
  if (!begin_menu(ctx_, popup_id.c_str())) return;
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr) { ImGui::CloseCurrentPopup(); end_menu(); return; }
  const StatusInfo info = status_of(id);
  const float mw = ctx_.u(250.0F);
  std::error_code ec;
  if (include_details) {
    if (menu_item(ctx_, "m_details", "Details", nullptr, ctx_.icon("info"), mw)) open_details(id);
    if (menu_item(ctx_, "m_play", "Play", nullptr, ctx_.icon("play"), mw, can_play(info))) play(id);
    if (menu_item(ctx_, "m_build", build_action_label(info).c_str(), nullptr, ctx_.icon("reset"), mw, can_build(*entry, info))) { notices_.erase(id); start_build(id); }
    if (menu_item(ctx_, "m_fav", entry->favorite ? "Remove from favorites" : "Add to favorites", nullptr, ctx_.icon("star"), mw)) { entry->favorite = !entry->favorite; save_catalog(); }
    menu_separator(ctx_, mw);
  }
  if (menu_item(ctx_, "m_logs", show_log_ && !include_details ? "Hide build log" : "View build log", nullptr, ctx_.icon("file"), mw)) { if (include_details) open_details(id); show_log_ = include_details ? true : !show_log_; }
  if (menu_item(ctx_, "m_folder", "Open build folder", nullptr, ctx_.icon("folder"), mw, fs::is_directory(info.entry, ec))) SDL_OpenURL(("file://" + u8s(info.entry)).c_str());
  if (menu_item(ctx_, "m_storage", "Manage storage...", "Cleanup options for this game", ctx_.icon("storage"), mw)) { storage_selected_ = {id}; go(Page::storage); }
  menu_separator(ctx_, mw);
  if (menu_item(ctx_, "m_cover", "Change cover...", "Choose a local PNG or JPEG", ctx_.icon("edit"), mw)) browse_cover(id);
  if (!entry->cover.empty() && menu_item(ctx_, "m_cover_rm", "Remove cover", nullptr, ctx_.icon("close"), mw)) {
    remove_cover_file(covers_dir_, entry->cover);
    if (auto it = cover_cache_.find(entry->cover); it != cover_cache_.end()) { if (it->second) SDL_DestroyTexture(it->second); cover_cache_.erase(it); }
    entry->cover.clear();
    save_catalog();
    toast("Cover removed; the title card is shown.");
  }
  {
    const auto names = libretro_candidate_names(entry->title(), settings_.preferred_regions);
    const std::string url = names.empty() ? std::string() : libretro_candidate_url(entry->platform_id, names.front());
    if (menu_item(ctx_, "m_web", "Find cover online", "Opens a Libretro name guess in your browser", ctx_.icon("search"), mw, !url.empty())) SDL_OpenURL(url.c_str());
  }
  if (menu_item(ctx_, "m_sha", "Copy ROM SHA-256", nullptr, ctx_.icon("file"), mw)) { SDL_SetClipboardText(entry->sha256.c_str()); toast("SHA-256 copied."); }
  if (info.rom_missing && menu_item(ctx_, "m_locate", "Locate ROM...", "Re-link a moved ROM (same SHA-256)", ctx_.icon("folder"), mw)) browse_locate(id);
  menu_separator(ctx_, mw);
  if (menu_item(ctx_, "m_remove", "Remove from library...", "ROM files are never deleted", ctx_.icon("trash"), mw, !game_ || game_id_ != id, true)) ask_remove(*entry);
  end_menu();
}

// ---------------------------------------------------------------- A. identity

void App::draw_details_identity(LibraryEntry &entry, const StatusInfo &info, float w, float cover_w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 top = ImGui::GetCursorScreenPos();
  const bool is_building = job_ && job_id_ == entry.id(), is_running = game_ && game_id_ == entry.id();
  const float cover_h = std::floor(cover_w * 1.33F);
  {  // cover, with a discoverable change-cover button on its corner
    const ImVec2 a = top, b(top.x + cover_w, top.y + cover_h);
    dl->AddRectFilled(ImVec2(a.x - ctx_.u(2.0F), a.y - ctx_.u(2.0F)), ImVec2(b.x + ctx_.u(2.0F), b.y + ctx_.u(2.0F)), col::glow_soft, ctx_.u(7.0F));
    draw_cover(dl, entry, a, b, ctx_.u(5.0F));
    dl->AddRect(a, b, col::glow, ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.5F)));
    const float bs = ctx_.u(26.0F);
    ImGui::SetCursorScreenPos(ImVec2(b.x - bs - ctx_.u(5.0F), a.y + ctx_.u(5.0F)));
    if (icon_button(ctx_, "cover_change", ctx_.icon("edit"), bs, false, true, "Change cover (local image)")) browse_cover(entry.id());
  }
  const float x0 = top.x + cover_w + ctx_.u(18.0F), tw = w - cover_w - ctx_.u(18.0F);
  float y = top.y;
  {  // title row: editable display name, favorite
    const float bsz = ctx_.u(28.0F);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    if (editing_title_) {
      bool active = false;
      input_text(ctx_, "title", title_edit_, sizeof title_edit_, tw - bsz * 2 - ctx_.u(16.0F), nullptr, &active);
      ImGui::SameLine(0.0F, ctx_.u(6.0F));
      if (icon_button(ctx_, "title_ok", ctx_.icon("check"), bsz, true, true, "Save name") || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        const std::string trimmed = clean_title(title_edit_);
        entry.display_title = trimmed == entry.detected_title ? std::string() : trimmed;
        editing_title_ = false;
        save_catalog();
      }
      ImGui::SameLine(0.0F, ctx_.u(4.0F));
      if (icon_button(ctx_, "title_cancel", ctx_.icon("close"), bsz, false, true, "Cancel")) editing_title_ = false;
      y += ctx_.u(42.0F);
    } else {
      float tpx = ctx_.u(26.0F);
      while (tpx > ctx_.u(18.0F) && text_width(ctx_.bold, tpx, entry.title().c_str()) > tw - bsz * 2 - ctx_.u(18.0F)) tpx -= ctx_.u(1.0F);
      const std::string shown = ellipsize(ctx_.bold, tpx, tw - bsz * 2 - ctx_.u(18.0F), entry.title());
      draw_text(dl, ctx_.bold, tpx, col::text, ImVec2(x0, y + (bsz - tpx) * 0.5F), shown.c_str());
      if (shown != entry.title() && ImGui::IsMouseHoveringRect(ImVec2(x0, y), ImVec2(x0 + tw, y + bsz))) tooltip(ctx_, entry.title().c_str());
      ImGui::SetCursorScreenPos(ImVec2(x0 + text_width(ctx_.bold, tpx, shown.c_str()) + ctx_.u(12.0F), y));
      if (icon_button(ctx_, "fav", ctx_.icon("star"), bsz, entry.favorite, true, entry.favorite ? "Remove from favorites" : "Add to favorites")) { entry.favorite = !entry.favorite; save_catalog(); }
      ImGui::SameLine(0.0F, ctx_.u(4.0F));
      if (icon_button(ctx_, "title_edit", ctx_.icon("edit"), bsz, false, true, "Rename (display name only; the ROM header name is kept)")) { std::snprintf(title_edit_, sizeof title_edit_, "%s", entry.title().c_str()); editing_title_ = true; }
      y += ctx_.u(40.0F);
    }
  }
  {  // platform, build state, ROM health
    ImVec2 p(x0, y), size;
    pill(ctx_, dl, p, entry.platform_id == "master-system" ? "Master System" : "Genesis / Mega Drive", col::info, ctx_.icon("controller"), ctx_.u(12.0F), &size);
    p.x += size.x + ctx_.u(8.0F);
    const ImU32 sc = is_building ? col::warn : (is_running ? col::ok : status_color(info));
    const std::string state = is_building ? "Building..." : (is_running ? "Playing" : status_label(info));
    pill(ctx_, dl, p, state.c_str(), sc, nullptr, ctx_.u(12.0F), &size);
    p.x += size.x + ctx_.u(8.0F);
    const bool linked = !info.rom_missing;
    const char *rom_text = linked ? "ROM linked" : (entry.rom_path_unknown ? "ROM path not recorded" : "ROM missing");
    if (p.x + text_width(ctx_.bold, ctx_.u(12.0F), rom_text) + ctx_.u(40.0F) > x0 + tw) { p.x = x0; p.y += size.y + ctx_.u(6.0F); }
    pill(ctx_, dl, p, rom_text, linked ? col::text2 : col::warn, ctx_.icon(linked ? "check" : "warning"), ctx_.u(12.0F), &size);
    y = p.y + size.y + ctx_.u(10.0F);
  }
  // one sentence of what matters now (never more than one line in normal state)
  {
    std::string line;
    ImU32 color = col::text2;
    const std::string mode = entry.aot_policy == "optimized" ? "Optimized" : "Compatibility";
    const bool mapper_needed = entry.platform_id == "master-system" && entry.mapper.empty() && entry.mapper_manifest_sha256.empty();
    if (is_running) { line = running_label(game_report_, restarted_this_play_); color = col::ok; }
    else if (is_building) { line = "Recompiling with " + mode + ". This can take several minutes the first time."; color = col::warn; }
    else if (mapper_needed) { line = "Declare the cartridge mapper below before building."; color = col::warn; }
    else if (info.rom_missing && info.status != GameStatus::ready) { line = "The ROM is not at its recorded location. Locate it to build."; color = col::warn; }
    else if (info.status == GameStatus::ready) { line = "Ready to play. Last played " + fmt_relative(entry.last_played_at, std::time(nullptr)) + "."; color = col::text2; }
    else if (info.status == GameStatus::metadata_only) { line = "Build removed on purpose: history kept, a new build is needed to play."; color = col::info; }
    else if (info.status == GameStatus::failed) { line = "The last build failed. See the build log, then try again."; color = col::err; }
    else if (info.status == GameStatus::stale) { line = info.other_mode_ready ? "No " + mode + " build yet; the other build mode's executable is still available." : "The stored build is from another release or option set. Recompile to play."; color = col::warn; }
    else { line = "Imported, not built yet. Build creates the native program (" + mode + ")."; }
    const auto lines = wrap(ctx_.regular, ctx_.u(12.5F), tw, line);
    for (std::size_t i = 0; i < lines.size() && i < 2; ++i) { draw_text(dl, ctx_.regular, ctx_.u(12.5F), color, ImVec2(x0, y), lines[i].c_str()); y += ctx_.u(16.0F); }
    y += ctx_.u(8.0F);
  }
  // primary actions: Play and Build dominate; everything else is in More
  const float bh = ctx_.u(44.0F);
  ImGui::SetCursorScreenPos(ImVec2(x0, y));
  if (is_building) {
    static const char *names[BuildJob::stage_count] = {"Analyze", "Generate C", "Compile", "Link"};
    float sx = x0;
    for (int i = 0; i < BuildJob::stage_count; ++i) {
      float cw = 0.0F;
      if (sx + ctx_.u(110.0F) > x0 + tw) { sx = x0; y += ctx_.u(32.0F); }
      stage_chip(ctx_, dl, ImVec2(sx, y + ctx_.u(8.0F)), names[i], job_->stage(i), &cw);
      sx += cw + ctx_.u(6.0F);
    }
    y += ctx_.u(8.0F) + ctx_.u(26.0F);
  } else {
    const bool locate_primary = info.rom_missing && info.status != GameStatus::ready && !entry.rom_paths.empty();
    const float play_w = std::min(ctx_.u(150.0F), (tw - ctx_.u(16.0F)) * 0.36F), build_w = std::min(ctx_.u(148.0F), (tw - ctx_.u(16.0F)) * 0.36F), more_w = std::min(ctx_.u(96.0F), tw - play_w - build_w - ctx_.u(16.0F));
    if (locate_primary) {
      if (button(ctx_, "locate", "Locate ROM...", ImVec2(play_w, bh), Style::primary, true, ctx_.icon("folder"))) browse_locate(entry.id());
    } else if (button(ctx_, "play", is_running ? "Playing" : "Play", ImVec2(play_w, bh), Style::primary, can_play(info), ctx_.icon("play"))) play(entry.id());
    if (ImGui::IsItemHovered() && !can_play(info) && !locate_primary) tooltip(ctx_, info.status == GameStatus::ready ? "Wait for the running game or build to finish." : "No executable matches the current build configuration yet.");
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    const bool prominent_build = info.status != GameStatus::ready;
    if (button(ctx_, "build", build_action_label(info).c_str(), ImVec2(build_w, bh), prominent_build && !locate_primary ? Style::primary : Style::secondary, can_build(entry, info), ctx_.icon("reset"))) { notices_.erase(entry.id()); start_build(entry.id()); }
    if (ImGui::IsItemHovered()) tooltip(ctx_, info.status == GameStatus::ready ? "Recompile the native program with the current configuration." : "Recompile with the configuration below.");
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "more", "More", ImVec2(more_w, bh), Style::ghost, true, ctx_.icon("more"))) ImGui::OpenPopup("##details_more");
    if (ImGui::IsItemHovered()) tooltip(ctx_, "More: logs, build folder, storage, cover, remove");
    if (ImGui::IsItemFocused()) focus_label_ = "More";
    draw_game_menu("##details_more", entry.id(), false);
    y += bh;
  }
  ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(top.y + cover_h, y) + ctx_.u(14.0F)));
  ImGui::Dummy(ImVec2(w, 0.0F));
}

// ---------------------------------------------------------------- B. build configuration

float App::draw_build_config(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool busy_now = job_ != nullptr || game_ != nullptr || maintenance_running_;
  const bool sms = entry.platform_id == "master-system";
  const RomView probe = view_for(entry);
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  const bool side_labels = inner > ctx_.u(470.0F);
  const float label_w = side_labels ? ctx_.u(112.0F) : 0.0F, ctl_w = inner - label_w;
  const float tile_h = ctx_.u(64.0F);
  bool changed = false;
  // The card is drawn behind its content: content first on a channel above, then the frame with the measured height.
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  float y = a.y + ctx_.u(40.0F);
  const auto row_label = [&](const char *label, const char *sub, float row_y, float row_h) {
    if (!side_labels) return;
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(a.x + pad, row_y + (sub != nullptr ? row_h * 0.5F - ctx_.u(15.0F) : row_h * 0.5F - ctx_.u(8.0F))), label);
    if (sub != nullptr) draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(a.x + pad, row_y + row_h * 0.5F + ctx_.u(2.0F)), sub);
  };
  const auto stacked_label = [&](const char *label) {
    if (side_labels) return;
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(a.x + pad, y), label);
    y += ctx_.u(20.0F);
  };
  const float cx = a.x + pad + label_w;
  // ---- Master System: the cartridge mapper comes first, because nothing can be built until it is declared ----
  if (sms) {
    stacked_label("Cartridge mapper");
    if (!entry.mapper_manifest_sha256.empty() && entry.mapper.empty()) {
      row_label("Cartridge mapper", "required", y, ctx_.u(36.0F));
      ImGui::SetCursorScreenPos(ImVec2(cx, y));
      paragraph(ctx_, ctx_.regular, ctx_.u(12.5F), col::ok, ctl_w, "Declared by the sidecar manifest next to the ROM file (verified by digest).");
      y = std::max(y + ctx_.u(36.0F), ImGui::GetCursorScreenPos().y);
    } else {
      row_label("Cartridge mapper", entry.mapper.empty() ? "not declared" : "declared", y, ctx_.u(46.0F));
      ImGui::SetCursorScreenPos(ImVec2(cx, y));
      int sel = entry.mapper == "sega" ? 0 : (entry.mapper == "rom_only" ? 1 : -1);
      ImGui::BeginDisabled(busy_now);
      if (option_tiles(ctx_, "mapper", {"Sega mapper", "ROM only (32 KiB)"}, {"Banked cartridges", "No banking, up to 32 KiB"}, &sel, ctl_w, ctx_.u(46.0F))) {
        entry.mapper = sel == 0 ? "sega" : "rom_only";
        changed = true;
      }
      ImGui::EndDisabled();
      y += ctx_.u(52.0F);
      if (entry.mapper.empty()) {
        dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + ctl_w, y + ctx_.u(26.0F)), rgba(70, 50, 14, 150), ctx_.u(4.0F));
        icon_image(dl, ctx_.icon("warning"), ImVec2(cx + ctx_.u(8.0F), y + ctx_.u(5.0F)), ctx_.u(16.0F), col::warn);
        draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::warn, ImVec2(cx + ctx_.u(30.0F), y + ctx_.u(6.0F)), ellipsize(ctx_.regular, ctx_.u(12.0F), ctl_w - ctx_.u(40.0F), "Required: the header never names the mapper, so it is never guessed.").c_str());
        y += ctx_.u(32.0F);
      } else {
        ImGui::SetCursorScreenPos(ImVec2(cx + ctl_w - ctx_.u(140.0F), y - ctx_.u(2.0F)));
        ImGui::BeginDisabled(busy_now);
        if (button(ctx_, "mapper_clear", "Clear declaration", ImVec2(ctx_.u(140.0F), ctx_.u(22.0F)), Style::ghost)) { entry.mapper.clear(); changed = true; }
        ImGui::EndDisabled();
        draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(cx, y + ctx_.u(2.0F)), "Part of the build: changing it needs a rebuild.");
        y += ctx_.u(26.0F);
      }
    }
    y += ctx_.u(6.0F);
  }
  // ---- Build mode ----
  stacked_label("Build mode");
  row_label("Build mode", "part of the build", y, tile_h);
  {
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    int sel = entry.aot_policy == "optimized" && probe.supports_optimized() ? 1 : 0;
    const bool optimized_offered = probe.supports_optimized();
    const std::string compat_desc = "Broad, conservative recompilation. The default.";
    const std::string opt_desc = !optimized_offered ? "Not offered by this product build for Master System."
                                 : (sms ? "Experimental: may restart in Compatibility."
                                        : "Selective AOT policy; may fall back to Compatibility.");
    ImGui::BeginDisabled(busy_now);
    if (option_tiles(ctx_, "mode", {"Compatibility", "Optimized"}, {compat_desc, opt_desc}, &sel, ctl_w, tile_h, {true, optimized_offered})) {
      entry.aot_policy = sel == 1 ? "optimized" : "compatibility";
      if (sel == 0) entry.aot_model = "v1";
      changed = true;
    }
    ImGui::EndDisabled();
    y += tile_h + ctx_.u(8.0F);
  }
  // what the executable actually is, versus what is selected
  {
    const std::string selected = entry.aot_policy == "optimized" ? "Optimized" : "Compatibility";
    const std::string other = entry.aot_policy == "optimized" ? "Compatibility" : "Optimized";
    std::string text;
    ImU32 color = col::text2;
    const char *icon = "info";
    if (job_ && job_id_ == entry.id()) { text = "Building the " + selected + " executable..."; color = col::warn; icon = "reset"; }
    else if (info.status == GameStatus::ready && info.policy.present && info.policy.fallback) { text = "Optimized was requested; this executable fell back to Compatibility (see Build provenance)."; color = col::warn; icon = "warning"; }
    else if (info.status == GameStatus::ready) { text = "Executable built as " + (info.policy.present ? std::string(effective_mode(info.policy) == "optimized" ? "Optimized" : "Compatibility") : selected) + ": matches this selection, ready to play."; color = col::ok; icon = "check"; }
    else if (info.other_mode_ready) { text = "No " + selected + " executable yet. Your " + other + " build is kept: switch back to play it, or Build."; color = col::warn; icon = "warning"; }
    else if (info.status == GameStatus::metadata_only) { text = "No executable (metadata only). Build to play with " + selected + "."; color = col::info; }
    else if (info.status == GameStatus::failed) { text = "The last " + selected + " build failed. Check the build log, then try again."; color = col::err; icon = "warning"; }
    else if (info.status == GameStatus::stale) { text = "The stored build does not match this release or configuration. Recompile to apply " + selected + "."; color = col::warn; icon = "warning"; }
    else { text = "Not built yet. Build creates the " + selected + " executable."; }
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    const auto lines = wrap(ctx_.regular, ctx_.u(12.0F), ctl_w - ctx_.u(26.0F), text);
    icon_image(dl, ctx_.icon(icon), ImVec2(cx, y), ctx_.u(15.0F), color);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, ctx_.u(12.0F), color, ImVec2(cx + ctx_.u(22.0F), y), l.c_str()); y += ctx_.u(15.0F); }
    y += ctx_.u(10.0F);
  }
  // ---- Console region (Genesis): a run-time choice, never part of the build identity ----
  if (probe.supports_region()) {
    stacked_label("Console region");
    row_label("Console region", "runtime only", y, ctx_.u(36.0F));
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    int sel = entry.region == "japan" ? 1 : (entry.region == "europe" ? 2 : 0);
    ImGui::BeginDisabled(game_ != nullptr);
    if (option_tiles(ctx_, "region", {"USA NTSC", "Japan NTSC", "Europe PAL"}, {}, &sel, ctl_w, ctx_.u(36.0F))) {
      entry.region = sel == 1 ? "japan" : (sel == 2 ? "europe" : "usa");
      changed = true;
    }
    ImGui::EndDisabled();
    y += ctx_.u(42.0F);
    std::string note = "Applies the next time the game starts; the same executable serves every region.";
    if (entry.region == "europe") note = "PAL is selected, but the machine runs on the 60 Hz schedule (50 Hz timing is not implemented).";
    if (info.status == GameStatus::ready) if (const std::string c = region_coverage_note(info.observed_region, entry.region); !c.empty()) note += " " + c;
    for (const auto &l : wrap(ctx_.regular, ctx_.u(11.5F), ctl_w, note)) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), entry.region == "europe" ? col::warn : col::dim, ImVec2(cx, y), l.c_str()); y += ctx_.u(14.5F); }
    y += ctx_.u(8.0F);
  } else if (sms) {
    stacked_label("Console profile");
    row_label("Console profile", nullptr, y, ctx_.u(20.0F));
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text2, ImVec2(cx, y + ctx_.u(2.0F)), ellipsize(ctx_.regular, ctx_.u(12.5F), ctl_w, std::string("Master System II, NTSC (") + sms_profile + ")").c_str());
    y += ctx_.u(28.0F);
  }
  // ---- Advanced build options (expander) ----
  {
    const float rule_y = std::floor(y);
    dl->AddRectFilled(ImVec2(a.x + pad, rule_y), ImVec2(a.x + w - pad, rule_y + 1.0F), rgba(46, 80, 150, 140));
    y += ctx_.u(6.0F);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    const bool pressed = ui::nav_button("##advanced", ImVec2(inner, ctx_.u(28.0F)));
    const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    if (ImGui::IsItemFocused()) focus_label_ = "Advanced build options";
    const ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    if (hov) dl->AddRectFilled(ra, rb, col::panel_hover, ctx_.u(4.0F));
    const ImVec2 c(ra.x + ctx_.u(10.0F), (ra.y + rb.y) * 0.5F);
    if (show_advanced_) dl->AddTriangleFilled(ImVec2(c.x - ctx_.u(5.0F), c.y - ctx_.u(3.0F)), ImVec2(c.x + ctx_.u(5.0F), c.y - ctx_.u(3.0F)), ImVec2(c.x, c.y + ctx_.u(4.0F)), col::text2);
    else dl->AddTriangleFilled(ImVec2(c.x - ctx_.u(3.0F), c.y - ctx_.u(5.0F)), ImVec2(c.x + ctx_.u(4.0F), c.y), ImVec2(c.x - ctx_.u(3.0F), c.y + ctx_.u(5.0F)), col::text2);
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(ra.x + ctx_.u(24.0F), c.y - ctx_.u(7.5F)), "Advanced build options");
    std::string summary;
    if (!sms) summary = std::string(entry.aot_model == "gen2" && entry.aot_policy == "optimized" ? "Experimental model" : "Standard model") + (entry.hint_alias_discovery ? " / H-int discovery on" : " / H-int discovery off");
    else summary = "none for this console";
    const std::string sum_shown = ellipsize(ctx_.regular, ctx_.u(11.5F), inner * 0.5F, summary);
    draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::dim, ImVec2(rb.x - ctx_.u(8.0F) - text_width(ctx_.regular, ctx_.u(11.5F), sum_shown.c_str()), c.y - ctx_.u(7.0F)), sum_shown.c_str());
    if (foc) focus_ring(ctx_, dl, ra, rb);
    if (pressed) show_advanced_ = !show_advanced_;
    y += ctx_.u(32.0F);
    if (show_advanced_) {
      if (sms) {
        draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(a.x + pad, y), "Master System builds have no further options: the mapper and build mode above are the build identity.");
        y += ctx_.u(22.0F);
      } else {
        // Experimental Optimized model (Genesis)
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        draw_text(dl, ctx_.bold, ctx_.u(12.0F), col::text, ImVec2(a.x + pad, y + ctx_.u(6.0F)), "Optimized model");
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad + ctx_.u(140.0F), y));
        int model = entry.aot_model == "gen2" ? 1 : 0;
        const bool model_enabled = entry.aot_policy == "optimized" && !busy_now;
        ImGui::BeginDisabled(!model_enabled);
        if (segmented(ctx_, "model", {"Standard", "Experimental model"}, &model, 0.0F)) { entry.aot_model = model == 1 ? "gen2" : "v1"; changed = true; }
        ImGui::EndDisabled();
        y = ImGui::GetCursorScreenPos().y + ctx_.u(4.0F);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner, entry.aot_policy == "optimized" ? "The experimental model family may admit different code and may fall back to Compatibility. It changes the build." : "Only used by Optimized builds. Select Optimized to change it.") + ctx_.u(6.0F);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        bool on = entry.hint_alias_discovery;
        ImGui::BeginDisabled(busy_now);
        if (checkbox(ctx_, "hint", "H-interrupt alias discovery (strict build pass)", &on)) { entry.hint_alias_discovery = on; changed = true; }
        ImGui::EndDisabled();
        y = ImGui::GetCursorScreenPos().y + ctx_.u(2.0F);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner, "Default off. For games whose raster / H-interrupt effects fail otherwise: the build stops on a RAM-resident H-interrupt vector to derive its alias. Builds may take longer. It changes the build.") + ctx_.u(4.0F);
      }
      ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
      y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, inner, "Build mode, model, H-interrupt discovery and the mapper are part of the build identity: changing them needs a new build, and the previous executable is kept until Storage cleans it. Region applies at launch.") + ctx_.u(4.0F);
    }
  }
  if (entry.aot_policy == "optimized" && rom_policy_identity_cached(layout_, probe).empty() && !status_pending_) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::err, inner, "Optimized cannot verify the build tool identity: saved Optimized builds are not reused.");
  }
  const float h = y - a.y + ctx_.u(10.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), "BUILD CONFIGURATION", rgba(9, 22, 54, 245));
  dl->AddRectFilled(ImVec2(a.x + ctx_.u(1.0F), a.y + ctx_.u(2.0F)), ImVec2(a.x + ctx_.u(4.0F), a.y + h - ctx_.u(2.0F)), col::btn_top, ctx_.u(2.0F));
  {
    const bool ready = info.status == GameStatus::ready;
    const std::string sel = entry.aot_policy == "optimized" ? "Optimized" : "Compatibility";
    state_dot_text(ctx_, dl, ImVec2(a.x + w - pad, a.y + ctx_.u(13.0F)), ready ? sel + " - up to date" : sel + " - build required", ready ? col::ok : col::warn, w * 0.5F);
  }
  dl->ChannelsMerge();
  if (changed) { save_catalog(); request_status_refresh(); }
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
  ImGui::Dummy(ImVec2(w, 0.0F));
  return h;
}

// ---------------------------------------------------------------- C. provenance

void App::draw_provenance(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  const float lw = std::min(ctx_.u(150.0F), inner * 0.32F), vw = inner - lw;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  float y = a.y + ctx_.u(40.0F);
  int row_index = 0;
  const float px = ctx_.u(12.5F);
  // one readable row: label, value (wrapped), optional small action buttons on the right
  const auto row = [&](const char *label, const std::string &value, ImU32 color, const std::string &sub = std::string(), const char *action = nullptr, const char *action_id = nullptr) -> bool {
    const float act_w = action != nullptr ? ctx_.u(72.0F) : 0.0F;
    const auto lines = wrap(ctx_.regular, px, vw - act_w - ctx_.u(8.0F), value);
    const auto sub_lines = sub.empty() ? std::vector<std::string>{} : wrap(ctx_.regular, ctx_.u(11.0F), vw - act_w - ctx_.u(8.0F), sub);
    const float rh = std::max(ctx_.u(30.0F), static_cast<float>(lines.size()) * (px + ctx_.u(3.0F)) + static_cast<float>(sub_lines.size()) * ctx_.u(14.0F) + ctx_.u(12.0F));
    if (row_index++ % 2 == 0) dl->AddRectFilled(ImVec2(a.x + pad - ctx_.u(6.0F), y), ImVec2(a.x + w - pad + ctx_.u(6.0F), y + rh), rgba(14, 30, 66, 120), ctx_.u(4.0F));
    draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::dim, ImVec2(a.x + pad, y + ctx_.u(7.0F)), label);
    float ly = y + ctx_.u(6.0F);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, px, color, ImVec2(a.x + pad + lw, ly), l.c_str()); ly += px + ctx_.u(3.0F); }
    for (const auto &l : sub_lines) { draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(a.x + pad + lw, ly), l.c_str()); ly += ctx_.u(14.0F); }
    bool pressed = false;
    if (action != nullptr) {
      ImGui::SetCursorScreenPos(ImVec2(a.x + w - pad - act_w, y + (rh - ctx_.u(24.0F)) * 0.5F));
      pressed = button(ctx_, action_id, action, ImVec2(act_w, ctx_.u(24.0F)), Style::ghost);
    }
    y += rh + ctx_.u(2.0F);
    return pressed;
  };
  const long long now = std::time(nullptr);
  if (row("ROM SHA-256", entry.sha256.substr(0, 16) + " ... " + entry.sha256.substr(48), col::text, "Identifies the game: the library never matches by name.", "Copy", "copy_sha")) { SDL_SetClipboardText(entry.sha256.c_str()); toast("SHA-256 copied."); }
  if (entry.rom_paths.empty()) {
    if (row("ROM file", entry.rom_path_unknown ? "Location not recorded (older cache record)" : "No location", col::warn, "Locate the ROM to rebuild; the SHA-256 must match.", "Locate", "locate_rom")) browse_locate(entry.id());
  }
  for (std::size_t i = 0; i < entry.rom_paths.size(); ++i) {
    std::error_code ec;
    const fs::path rom_file = fs::path(std::u8string(reinterpret_cast<const char8_t *>(entry.rom_paths[i].c_str())));
    const bool exists = fs::is_regular_file(rom_file, ec);
    const std::uintmax_t rom_size = exists ? fs::file_size(rom_file, ec) : 0U;
    ImGui::PushID(static_cast<int>(i));
    const std::string value = u8s(rom_file.filename()) + (exists ? "  (" + format_bytes(rom_size) + ", linked)" : "  (missing)");
    if (row(i == 0 ? "ROM file" : "Also found at", value, exists ? col::text : col::warn, u8s(rom_file.parent_path()), exists ? "Folder" : "Locate", "rom_action")) {
      if (exists) SDL_OpenURL(("file://" + u8s(rom_file.parent_path())).c_str());
      else browse_locate(entry.id());
    }
    ImGui::PopID();
  }
  {
    std::string built = entry.last_built_at > 0 ? fmt_date(entry.last_built_at) + "  (" + fmt_relative(entry.last_built_at, now) + ")" : "never recorded";
    if (info.build_known && info.build.version.empty() && entry.last_built_at > 0) built += "  (file time)";
    row("Last recompiled", built, col::text);
  }
  row("Last played", entry.last_played_at > 0 ? fmt_date(entry.last_played_at) + "  (" + fmt_relative(entry.last_played_at, now) + ")" : "never", col::text,
      entry.last_run == "failed" ? "Exit code " + std::to_string(entry.last_exit_code) : std::string());
  if (info.build_known && (!info.build.version.empty() || !info.build.toolchain.empty()))
    row("Build version", (info.build.version.empty() ? std::string("unknown release") : "Segarecomp " + info.build.version) + (info.build.toolchain.empty() ? std::string() : "  -  " + info.build.toolchain), col::text);
  else row("Build version", info.status == GameStatus::ready ? "unknown (adopted build)" : "no current build", col::text2, std::string("This release: Segarecomp ") + SEGARECOMP_LAUNCHER_VERSION + ", zig " + SEGARECOMP_ZIG_VERSION);
  {
    std::string identity = info.key.empty() ? std::string("-") : info.key.substr(0, 16) + "...";
    std::string detail = std::string(entry.aot_policy == "optimized" ? "Optimized" : "Compatibility") + " selected";
    if (entry.platform_id == "genesis" && entry.aot_policy == "optimized") detail += entry.aot_model == "gen2" ? ", experimental model" : ", standard model";
    if (entry.platform_id == "genesis") detail += entry.hint_alias_discovery ? ", H-int discovery on" : ", H-int discovery off";
    if (entry.platform_id == "master-system") detail += ", mapper " + (entry.mapper.empty() ? (entry.mapper_manifest_sha256.empty() ? std::string("undeclared") : std::string("from manifest")) : entry.mapper);
    std::error_code ec;
    if (row("Build identity", identity, col::text, detail, fs::is_directory(info.entry, ec) ? "Open" : nullptr, "open_build")) SDL_OpenURL(("file://" + u8s(info.entry)).c_str());
  }
  if (info.status == GameStatus::ready && info.policy.present) {
    const std::string summary = policy_summary(info.policy);
    const std::string eff = effective_mode(info.policy) == "optimized" ? "Optimized" : "Compatibility";
    if (row("Executable", eff + (info.policy.fallback ? " (Optimized requested, fell back)" : ""), info.policy.fallback ? col::warn : col::text, summary, info.policy.requested == "optimized" ? (show_fallback_details_ ? "Less" : (info.policy.fallback ? "Why?" : "More")) : nullptr, "why"))
      show_fallback_details_ = !show_fallback_details_;
    if (show_fallback_details_ && info.policy.requested == "optimized") {
      ImGui::SetCursorScreenPos(ImVec2(a.x + pad + lw, y));
      y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, vw, (info.policy.fallback ? "Optimized was requested but this game was built as Compatibility (the broad, conservative program).\n" : std::string()) + policy_details(info.policy)) + ctx_.u(6.0F);
    }
  }
  row("Last run status", entry.last_played_at > 0 ? std::string(run_label(entry.last_run)) : "never played", entry.last_run == "failed" ? col::err : col::text);
  row("Diagnostics", info.compat_known ? "Experimental analysis metadata for this ROM ships with this release." : "No ROM-specific analysis metadata ships with this release.", col::text2,
      info.has_failed_dir ? "A failed build folder with its sanitized log is kept for review." : std::string());
  const float h = y - a.y + ctx_.u(10.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), "BUILD PROVENANCE");
  dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
  ImGui::Dummy(ImVec2(w, 0.0F));
}

// ---------------------------------------------------------------- D. storage rail

float App::draw_storage_rail(const LibraryEntry &entry, const StatusInfo &info, ImVec2 a, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const EntryUsage *u = report_valid_ ? report_.find(entry.id()) : nullptr;
  const float pad = ctx_.u(14.0F), row = ctx_.u(23.0F);
  struct Slice { const char *name; ImU32 color; std::uintmax_t bytes; };
  const Slice slices[] = {{"Native executable", rgba(52, 150, 255), u ? u->total.executable : 0}, {"Generated C", rgba(176, 92, 230), u ? u->total.generated_c : 0},
                          {"Object files", rgba(80, 200, 160), u ? u->total.objects : 0}, {"Logs", rgba(240, 170, 60), u ? u->total.logs : 0},
                          {"Metadata", rgba(120, 210, 90), u ? u->total.metadata : 0}, {"Other build files", rgba(150, 165, 200), u ? u->total.other : 0}};
  const bool archived = info.status == GameStatus::metadata_only;
  const auto note_lines = wrap(ctx_.regular, ctx_.u(11.5F), w - 2 * pad, archived ? "Metadata only: the library entry, history and cover are kept. A new build is needed to play."
                                                                               : (u != nullptr && u->total.total() == 0 ? "No build files on disk for this game." : "Measured from this game's build folders. ROM files and covers are not counted."));
  const float h = ctx_.u(52.0F) + ctx_.u(14.0F) + 6.0F * row + ctx_.u(8.0F) + static_cast<float>(note_lines.size()) * ctx_.u(14.5F) + ctx_.u(12.0F) + 2.0F * ctx_.u(38.0F) + ctx_.u(8.0F);
  const ImVec2 b(a.x + w, a.y + h);
  section_card(ctx_, dl, a, b, "STORAGE");
  const std::string total = u ? format_bytes(u->total.total()) : (scanning_ ? "measuring..." : "-");
  draw_text(dl, ctx_.bold, ctx_.u(20.0F), col::info, ImVec2(a.x + pad, a.y + ctx_.u(36.0F)), total.c_str());
  float y = a.y + ctx_.u(66.0F);
  // a proportional bar of the measured kinds
  if (u != nullptr && u->total.total() > 0) {
    float x = a.x + pad;
    const float bw = w - 2 * pad;
    for (const auto &s : slices) {
      const float sw = bw * static_cast<float>(static_cast<double>(s.bytes) / static_cast<double>(u->total.total()));
      if (sw >= 1.0F) { dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sw, y + ctx_.u(6.0F)), s.color); x += sw; }
    }
  } else dl->AddRectFilled(ImVec2(a.x + pad, y), ImVec2(a.x + w - pad, y + ctx_.u(6.0F)), rgba(20, 36, 76));
  y += ctx_.u(14.0F);
  for (const auto &s : slices) {
    dl->AddRectFilled(ImVec2(a.x + pad, y + ctx_.u(4.0F)), ImVec2(a.x + pad + ctx_.u(10.0F), y + ctx_.u(14.0F)), s.color, ctx_.u(2.0F));
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text, ImVec2(a.x + pad + ctx_.u(18.0F), y + ctx_.u(1.0F)), s.name);
    const std::string v = u ? format_bytes(s.bytes) : "-";
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text2, ImVec2(b.x - pad - text_width(ctx_.regular, ctx_.u(12.5F), v.c_str()), y + ctx_.u(1.0F)), v.c_str());
    y += row;
  }
  y += ctx_.u(4.0F);
  for (const auto &l : note_lines) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), archived ? col::info : col::dim, ImVec2(a.x + pad, y), l.c_str()); y += ctx_.u(14.5F); }
  y += ctx_.u(10.0F);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
  const bool can_clean = u != nullptr && u->intermediates > 0 && report_valid_ && !maintenance_running_ && !busy_.any_activity();
  if (button(ctx_, "clean_inter", "Clean intermediates...", ImVec2(w - 2 * pad, ctx_.u(32.0F)), Style::secondary, can_clean, ctx_.icon("trash"))) {
    modal_ = Modal{};
    modal_.kind = Modal::Kind::plan;
    modal_.title = "Remove build intermediates?";
    modal_.plan = plan_intermediates(report_, {entry.id()}, busy_);
  }
  if (ImGui::IsItemHovered()) tooltip(ctx_, can_clean ? "Deletes generated C and objects; the game stays playable." : "Nothing to remove, or a build / game / cleanup is running.");
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y + ctx_.u(38.0F)));
  if (button(ctx_, "manage_storage", "Manage storage...", ImVec2(w - 2 * pad, ctx_.u(32.0F)), Style::ghost, true, ctx_.icon("storage"))) { storage_selected_ = {entry.id()}; go(Page::storage); }
  return h;
}

// ---------------------------------------------------------------- the page

void App::draw_details(float w, float h) {
  LibraryEntry *entry = catalog_.find_id(detail_id_);
  if (entry == nullptr) { page_ = Page::library; return; }
  const StatusInfo info = status_of(detail_id_);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool is_building = job_ && job_id_ == detail_id_;
  {  // breadcrumb: Back returns to the exact library state it came from
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const Page back_to = previous_page_ == Page::details ? Page::library : previous_page_;
    const char *back_label = back_to == Page::import_page ? "Import ROMs" : (back_to == Page::storage ? "Storage" : (back_to == Page::settings ? "Settings" : "Library"));
    const float bw = text_width(ctx_.bold, ctx_.u(13.0F), back_label) + ctx_.u(46.0F);
    if (button(ctx_, "back", back_label, ImVec2(bw, ctx_.u(30.0F)), Style::ghost, true, ctx_.icon("back"))) { close_details(); return; }
    if (ImGui::IsItemFocused()) focus_label_ = "Back";
    const float cx = p.x + bw + ctx_.u(12.0F);
    draw_text(dl, ctx_.display, ctx_.u(12.5F), rgba(120, 205, 255), ImVec2(cx, p.y + ctx_.u(9.0F)), "GAME DETAILS");
    const float hw = text_width(ctx_.display, ctx_.u(12.5F), "GAME DETAILS");
    const std::string crumb = ellipsize(ctx_.regular, ctx_.u(12.0F), w - (cx - p.x) - hw - ctx_.u(30.0F), "/  " + entry->title());
    draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::dim, ImVec2(cx + hw + ctx_.u(12.0F), p.y + ctx_.u(9.0F)), crumb.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(42.0F)));
  }
  const bool rail = w > ctx_.u(700.0F);
  const float gap = ctx_.u(16.0F), rail_w = rail ? std::clamp(w * 0.29F, ctx_.u(220.0F), ctx_.u(262.0F)) : 0.0F;
  const float main_w = rail ? w - rail_w - gap : w;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  float rail_bottom = top.y;
  if (rail) rail_bottom = top.y + draw_storage_rail(*entry, info, ImVec2(top.x + main_w + gap, top.y), rail_w);
  ImGui::SetCursorScreenPos(top);
  const float cover_w = std::clamp(main_w * 0.24F, ctx_.u(96.0F), ctx_.u(136.0F));
  draw_details_identity(*entry, info, main_w, cover_w);
  draw_build_config(*entry, info, main_w);
  ImGui::Dummy(ImVec2(1, ctx_.u(12.0F)));
  // notices of this session (restart, failure) sit right under the configuration
  const auto banner = [&](ImU32 color, const char *icon, const std::string &text) {
    const auto lines = wrap(ctx_.regular, ctx_.u(12.5F), main_w - ctx_.u(50.0F), text);
    const float bh2 = static_cast<float>(lines.size()) * (ctx_.u(12.5F) + ctx_.u(3.0F)) + ctx_.u(18.0F);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(a, ImVec2(a.x + main_w, a.y + bh2), (color & 0x00FFFFFF) | 0x26000000, ctx_.u(5.0F));
    dl->AddRect(a, ImVec2(a.x + main_w, a.y + bh2), (color & 0x00FFFFFF) | 0xAA000000, ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
    icon_image(dl, ctx_.icon(icon), ImVec2(a.x + ctx_.u(12.0F), a.y + ctx_.u(9.0F)), ctx_.u(16.0F), color);
    float ly = a.y + ctx_.u(9.0F);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text, ImVec2(a.x + ctx_.u(38.0F), ly), l.c_str()); ly += ctx_.u(12.5F) + ctx_.u(3.0F); }
    ImGui::Dummy(ImVec2(main_w, bh2 + ctx_.u(8.0F)));
  };
  if (const auto notice = notices_.find(entry->id()); notice != notices_.end() && !notice->second.empty()) banner(col::err, "warning", notice->second);
  if (const auto failure = failures_.find(entry->id()); failure != failures_.end() && !is_building) {
    banner(col::err, "warning", "Last attempt failed: " + failure->second);
    if (button(ctx_, "dismiss_fail", "Dismiss", ImVec2(ctx_.u(100.0F), ctx_.u(26.0F)), Style::ghost)) failures_.erase(failure);
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  if (!is_building && info.rom_missing)
    banner(col::warn, "warning", entry->rom_path_unknown ? "The original ROM location was never recorded (this entry came from an older cache record). Locate ROM to enable rebuilding; playing an existing build still works."
                                                         : "The original ROM is not at its recorded location. Playing an existing build still works; Locate ROM re-links it (the SHA-256 must match).");
  // build log: live while building, on request otherwise
  if (is_building || show_log_) {
    std::string text;
    if (is_building) {
      if (SDL_GetTicks() - live_log_at_ > 700U) {
        live_log_at_ = SDL_GetTicks();
        live_log_ = read_tail(fs::path(job_->entry()).parent_path() / (u8s(job_->entry().filename()) + ".partial") / "build.log", 1400);
      }
      text = live_log_.empty() ? "Waiting for the build tool's first message..." : live_log_;
    } else if (const auto d = diagnostics_.find(entry->id()); d != diagnostics_.end()) text = d->second;
    else {
      const fs::path failed = info.entry.parent_path() / (info.key + ".failed");
      text = read_tail(info.entry / "build.log", 6000);
      if (text.empty()) text = read_tail(failed / "build.log", 6000);
      const std::string run = read_tail(info.entry / "run.log", 2000);
      if (!run.empty()) text += "\n--- last run ---\n" + run;
    }
    if (text.empty()) text = "No log exists for this game's current build.";
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw_text(dl, ctx_.display, ctx_.u(12.5F), is_building ? col::warn : rgba(120, 205, 255), p, is_building ? "BUILD LOG (LIVE)" : "BUILD LOG");
    if (!is_building) {
      ImGui::SetCursorScreenPos(ImVec2(p.x + main_w - ctx_.u(80.0F), p.y - ctx_.u(4.0F)));
      if (button(ctx_, "hide_log", "Hide", ImVec2(ctx_.u(80.0F), ctx_.u(24.0F)), Style::ghost)) show_log_ = false;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(24.0F)));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const auto lines = wrap(ctx_.regular, ctx_.u(11.5F), main_w - ctx_.u(20.0F), text);
    const float lh = ctx_.u(11.5F) + ctx_.u(3.0F);
    const std::size_t max_lines = is_building ? 10 : 24;
    const std::size_t first = lines.size() > max_lines ? lines.size() - max_lines : 0;
    const float box_h = static_cast<float>(lines.size() - first) * lh + ctx_.u(18.0F);
    dl->AddRectFilled(q, ImVec2(q.x + main_w, q.y + box_h), rgba(4, 9, 22, 240), ctx_.u(5.0F));
    dl->AddRect(q, ImVec2(q.x + main_w, q.y + box_h), rgba(34, 70, 140), ctx_.u(5.0F));
    float ly = q.y + ctx_.u(9.0F);
    for (std::size_t i = first; i < lines.size(); ++i) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::text2, ImVec2(q.x + ctx_.u(10.0F), ly), lines[i].c_str()); ly += lh; }
    ImGui::Dummy(ImVec2(main_w, box_h + ctx_.u(12.0F)));
    if (is_building) { paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, main_w, "The build reports stages, not a percentage, and cannot be cancelled safely; closing the launcher waits for it."); ImGui::Dummy(ImVec2(1, ctx_.u(10.0F))); }
  }
  if (!rail) {  // narrow window: the storage card follows the configuration
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float sh = draw_storage_rail(*entry, info, p, main_w);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sh + ctx_.u(12.0F)));
    ImGui::Dummy(ImVec2(main_w, 0.0F));
  }
  draw_provenance(*entry, info, main_w);
  const float bottom = std::max(rail_bottom, ImGui::GetCursorScreenPos().y);
  ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

} // namespace launcher
