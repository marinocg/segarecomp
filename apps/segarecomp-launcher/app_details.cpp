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

#include "cover_download.hpp"

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

fs::path path_from_text(const std::string &text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str()))); }
std::string short_hash(const std::string &digest) { return digest.size() >= 16 ? digest.substr(0, 16) + "..." : digest; }
std::string file_name_text(const std::string &path) { return path.empty() ? std::string("not chosen") : u8s(path_from_text(path).filename()); }
// The product-facing name of a Sega CD build mode code.
std::string scd_mode_name(const std::string &code) { return code == "file_first" ? "File-first" : code == "optimized" ? "Optimized (experimental)" : "Compatibility"; }

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
    case GameStatus::playable_older: return "Recompile";
    case GameStatus::not_built: return "Build";
  }
  return "Build";
}

bool App::can_build(const LibraryEntry &entry, const StatusInfo &info) const {
  if (entry.platform_id == "sega-cd") return scd_can_build(entry, info);   // SEG-078-T003: its own job and its own preconditions
  const bool mapper_needed = entry.platform_id == "master-system" && entry.mapper.empty() && entry.mapper_manifest_sha256.empty();
  return !game_ && !is_building(entry.id()) && !is_queued(entry.id()) && !info.rom_missing && !mapper_needed;
}

bool App::can_play(const StatusInfo &info) const {
  if (info.sega_cd) return info.scd.state == scd::State::ready && !game_ && !maintenance_running_ && !scd_launch_job_.valid() && !scd_import_job_.valid() && !(scd_build_ && scd_build_key_ == info.key);
  return info.runnable() && !game_ && !maintenance_running_ && !(job_ && job_key_ == info.key); }

void App::play(const std::string &id) {
  if (const LibraryEntry *e = catalog_.find_id(id); e != nullptr && e->platform_id == "sega-cd") { trace("play=requested title=" + e->title()); scd_play(id, 0); return; }
  trace("play=requested title=" + (catalog_.find_id(id) != nullptr ? catalog_.find_id(id)->title() : std::string("?")));
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
    if (menu_item(ctx_, "m_build", build_action_label(info).c_str(), nullptr, ctx_.icon("reset"), mw, can_build(*entry, info))) { notices_.erase(id); request_build(id); }
    if (is_queued(id) && menu_item(ctx_, "m_unqueue", "Remove from build queue", nullptr, ctx_.icon("trash"), mw)) dequeue_build(id);
    if (is_building(id) && menu_item(ctx_, "m_cancel_build", is_stopping(id) ? "Stopping build..." : "Cancel build", nullptr, ctx_.icon("close"), mw, !is_stopping(id))) cancel_build(id);
    if (menu_item(ctx_, "m_fav", entry->favorite ? "Remove from favorites" : "Add to favorites", nullptr, ctx_.icon("star"), mw)) { entry->favorite = !entry->favorite; save_catalog(); }
    menu_separator(ctx_, mw);
  }
  if (!include_details) {
    // Details' own menu: the same two stop affordances, because the Details actions row shows no Build button while a build runs.
    if (is_queued(id) && menu_item(ctx_, "m_unqueue", "Remove from build queue", nullptr, ctx_.icon("trash"), mw)) dequeue_build(id);
    if (is_building(id) && menu_item(ctx_, "m_cancel_build", is_stopping(id) ? "Stopping build..." : "Cancel build", nullptr, ctx_.icon("close"), mw, !is_stopping(id))) cancel_build(id);
  }
  const bool scd_game = entry->platform_id == "sega-cd";
  if (menu_item(ctx_, "m_logs", show_log_ && !include_details ? "Hide build log" : "View build log", nullptr, ctx_.icon("file"), mw)) { if (include_details) open_details(id); show_log_ = include_details ? true : !show_log_; }
  if (menu_item(ctx_, "m_folder", "Open build folder", nullptr, ctx_.icon("folder"), mw, fs::is_directory(info.entry, ec))) SDL_OpenURL(("file://" + u8s(info.entry)).c_str());
  if (scd_game) {   // SEG-109: the Sega CD game's own extras, beside the shared items
    if (menu_item(ctx_, "m_recordings", "Open recordings folder", nullptr, ctx_.icon("folder"), mw, fs::is_directory(scd_dir(*entry) / "recordings", ec))) SDL_OpenURL(("file://" + u8s(scd_dir(*entry) / "recordings")).c_str());
    const bool scd_busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr;
    if (menu_item(ctx_, "m_scd_attach", entry->sega_cd.program.path.empty() ? "Import program..." : "Replace program...", "A verified prebuilt viewer program", ctx_.icon("import"), mw, !scd_busy)) scd_browse("scd_attach", id);
    if (menu_item(ctx_, "m_scd_verify", "Verify again", "Hashes the program and the disc files", ctx_.icon("check"), mw, !scd_busy && !entry->sega_cd.program.path.empty())) scd_start_import(id, {}, {}, {});
  }
  if (menu_item(ctx_, "m_storage", "Manage storage...", "Cleanup options for this game", ctx_.icon("storage"), mw)) { storage_selected_ = {id}; go(Page::storage); }
  menu_separator(ctx_, mw);
  if (menu_item(ctx_, "m_cover", "Change cover...", "Choose a local PNG or JPEG", ctx_.icon("edit"), mw)) browse_cover(id);
  if (!entry->cover.empty() && menu_item(ctx_, "m_cover_rm", "Remove cover", nullptr, ctx_.icon("close"), mw)) {
    remove_cover_file(covers_dir_, entry->cover);
    if (auto it = cover_cache_.find(entry->cover); it != cover_cache_.end()) { if (it->second) SDL_DestroyTexture(it->second); cover_cache_.erase(it); }
    entry->cover.clear();
    save_catalog();
    toast("Cover removed; the placeholder is shown.");
  }
  if (menu_item(ctx_, "m_dl_cover", cover_job_.valid() ? "Downloading cover..." : "Download cover (Libretro)", "Looks up the box art by name only", ctx_.icon("import"), mw, cover_download_possible(*entry)))
    start_cover_download({id});
  {
    CoverQuery q{entry->platform_id, {entry->title(), entry->detected_title}, {}, settings_.preferred_regions};
    for (const auto &rom : entry->rom_paths) q.rom_names.push_back(rom);  // only the file name is used
    const auto names = libretro_cover_candidates(q, 1);
    const std::string url = names.empty() ? std::string() : libretro_candidate_url(entry->platform_id, names.front());
    if (menu_item(ctx_, "m_web", "Find cover online", "Opens your web browser", ctx_.icon("search"), mw, !url.empty())) SDL_OpenURL(url.c_str());
  }
  if (menu_item(ctx_, "m_sha", entry->platform_id == "sega-cd" ? "Copy game identity" : "Copy ROM SHA-256", nullptr, ctx_.icon("file"), mw)) { SDL_SetClipboardText(entry->sha256.c_str()); toast("SHA-256 copied."); }
  if (info.rom_missing && !scd_game && menu_item(ctx_, "m_locate", "Locate ROM...", "Re-link a moved ROM (same SHA-256)", ctx_.icon("folder"), mw)) browse_locate(id);
  if (scd_game && info.scd.refusal == scd::Refusal::bios_missing && menu_item(ctx_, "m_locate_bios", "Locate BIOS...", "Re-link a moved BIOS (same SHA-256)", ctx_.icon("folder"), mw)) scd_browse("scd_locate_bios", id);
  if (scd_game && info.scd.refusal == scd::Refusal::cue_missing && menu_item(ctx_, "m_locate_cue", "Locate disc...", "Re-link a moved CUE file (same SHA-256)", ctx_.icon("folder"), mw)) scd_browse("scd_locate_cue", id);
  menu_separator(ctx_, mw);
  if (menu_item(ctx_, "m_remove", "Remove from library...", scd_game ? "BIOS, disc files and saves stay" : "ROM files and extracted copies stay", ctx_.icon("trash"), mw, !game_ || game_id_ != id, true)) ask_remove(*entry);
  end_menu();
}

// ---------------------------------------------------------------- A. identity

void App::draw_details_identity(LibraryEntry &entry, const StatusInfo &info, float w, float cover_w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 top = ImGui::GetCursorScreenPos();
  const bool is_building = this->is_building(entry.id()), is_running = game_ && game_id_ == entry.id();
  const float cover_h = std::floor(cover_w * 1.33F);
  {  // cover, with a discoverable change-cover button on its corner
    const ImVec2 a = top, b(top.x + cover_w, top.y + cover_h);
    dl->AddRectFilled(ImVec2(a.x - ctx_.u(2.0F), a.y - ctx_.u(2.0F)), ImVec2(b.x + ctx_.u(2.0F), b.y + ctx_.u(2.0F)), col::glow_soft, ctx_.u(7.0F));
    draw_cover(dl, entry, a, b, ctx_.u(5.0F));
    dl->AddRect(a, b, col::glow, ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.5F)));
    const float bs = ctx_.u(26.0F);
    ImGui::SetCursorScreenPos(ImVec2(b.x - bs - ctx_.u(5.0F), b.y - bs - ctx_.u(5.0F)));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);  // pointer shortcut over the cover art; a controller changes the cover from the game's menu (Change cover...)
    if (icon_button(ctx_, "cover_change", ctx_.icon("edit"), bs, false, true, "Change cover (local image)")) browse_cover(entry.id());
    ImGui::PopItemFlag();
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
    pill(ctx_, dl, p, entry.platform_id == "master-system" ? "Master System" : entry.platform_id == "sega-cd" ? "Sega CD" : "Genesis / Mega Drive", col::info, ctx_.icon("controller"), ctx_.u(12.0F), &size);
    p.x += size.x + ctx_.u(8.0F);
    const ImU32 sc = is_building ? col::warn : (is_running ? col::ok : status_color(info));
    const std::string state = is_building ? "Building..." : (is_running ? (info.sega_cd ? "Running" : "Playing") : status_label(info));
    pill(ctx_, dl, p, state.c_str(), sc, nullptr, ctx_.u(12.0F), &size);
    p.x += size.x + ctx_.u(8.0F);
    const bool linked = !info.rom_missing;
    const char *rom_text = info.sega_cd ? (linked ? "BIOS and disc linked" : "BIOS or disc missing") : (linked ? "ROM linked" : (entry.rom_path_unknown ? "ROM path not recorded" : "ROM missing"));
    if (p.x + text_width(ctx_.bold, ctx_.u(12.0F), rom_text) + ctx_.u(40.0F) > x0 + tw) { p.x = x0; p.y += size.y + ctx_.u(6.0F); }
    pill(ctx_, dl, p, rom_text, linked ? col::text2 : col::warn, ctx_.icon(linked ? "check" : "warning"), ctx_.u(12.0F), &size);
    y = p.y + size.y + ctx_.u(10.0F);
  }
  // one sentence of what matters now (never more than one line in normal state)
  {
    std::string line;
    ImU32 color = col::text2;
    const std::string mode = selected_mode_label(entry);
    const bool mapper_needed = entry.platform_id == "master-system" && entry.mapper.empty() && entry.mapper_manifest_sha256.empty();
    if (info.sega_cd) {
      if (is_running) { line = "Running in the viewer. Close its window to return here."; color = col::ok; }
      else if (is_building) { line = "Building with " + mode + ". This can take several minutes the first time."; color = col::warn; }
      else if (info.status == GameStatus::metadata_only) { line = "Build removed on purpose: history, saves and recordings kept, a new build is needed to play."; color = col::info; }
      else if (scd_launch_job_.valid() && scd_launch_id_ == entry.id()) { line = "Checking the BIOS, the program and the disc files before launch..."; color = col::warn; }
      else if (info.scd.state == scd::State::ready) { line = "Ready to play. Last played " + fmt_relative(entry.last_played_at, std::time(nullptr)) + "."; color = col::text2; }
      else { line = scd_hint(info); color = col::warn; }
    }
    else if (is_running) { line = running_label(game_report_, restarted_this_play_); color = col::ok; }
    else if (is_building) { line = "Recompiling with " + mode + ". This can take several minutes the first time."; color = col::warn; }
    else if (mapper_needed) { line = "Declare the cartridge mapper below before building."; color = col::warn; }
    else if (info.status == GameStatus::playable_older) { line = "Playable: an earlier release built this game. Recompile is recommended, not required."; color = col::warn; }
    else if (info.older.playable) { line = "The last build did not finish, but an earlier build is still playable."; color = col::warn; }
    else if (info.rom_missing && !info.runnable()) { line = "The ROM is not at its recorded location. Locate it to build."; color = col::warn; }
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
    float sx = x0;
    for (int i = 0; i < build_stage_count(entry.id()); ++i) {
      float cw = 0.0F;
      if (sx + ctx_.u(110.0F) > x0 + tw) { sx = x0; y += ctx_.u(32.0F); }
      stage_chip(ctx_, dl, ImVec2(sx, y + ctx_.u(8.0F)), build_stage_label(entry.id(), i), build_stage(entry.id(), i), &cw);
      sx += cw + ctx_.u(6.0F);
    }
    y += ctx_.u(8.0F) + ctx_.u(26.0F) + ctx_.u(10.0F);  // clear gap below the stage chips
    // SEG-069-T001: the visible, focusable way to stop the running build. Disabled (and relabelled) while the attempt shuts down.
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    const bool stopping = is_stopping(entry.id());
    const bool cancel_pressed = button(ctx_, "cancel_build", stopping ? "Stopping..." : "Cancel build", ImVec2(std::min(ctx_.u(180.0F), tw), bh), Style::secondary, !stopping, ctx_.icon("close"));
    note_item_layout("build");
    take_details_focus("cancel_build");
    if (cancel_pressed) { details_refocus_ = ImGui::IsItemFocused() ? "build" : std::string(); details_refocus_ttl_ = 30; cancel_build(entry.id()); }
    if (ImGui::IsItemHovered()) tooltip(ctx_, "Stop this build and discard its partial files. A previously built program is kept.");
    y += bh;
  } else {
    const bool scd_locate = info.sega_cd && (info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::cue_missing);   // the Sega CD "Locate ROM"
    const bool locate_primary = (info.rom_missing && !info.runnable() && !entry.rom_paths.empty() && !info.sega_cd) || scd_locate;  // a playable build needs no ROM: Play stays primary
    const float play_w = std::min(ctx_.u(150.0F), (tw - ctx_.u(16.0F)) * 0.36F), build_w = std::min(ctx_.u(148.0F), (tw - ctx_.u(16.0F)) * 0.36F), more_w = std::min(ctx_.u(96.0F), tw - play_w - build_w - ctx_.u(16.0F));
    if (locate_primary) {
      if (button(ctx_, "locate", scd_locate ? (info.scd.refusal == scd::Refusal::bios_missing ? "Locate BIOS..." : "Locate disc...") : "Locate ROM...", ImVec2(play_w, bh), Style::primary, true, ctx_.icon("folder"))) {
        if (scd_locate) scd_browse(info.scd.refusal == scd::Refusal::bios_missing ? "scd_locate_bios" : "scd_locate_cue", entry.id());
        else browse_locate(entry.id());
      }
    } else if (button(ctx_, "play", is_running ? "Playing" : "Play", ImVec2(play_w, bh), Style::primary, can_play(info), ctx_.icon("play"))) play(entry.id());
    note_item_layout("play");
    if (ImGui::IsItemHovered() && !can_play(info) && !locate_primary) {
      const std::string why = info.sega_cd ? (info.scd.state == scd::State::ready ? std::string("Wait for the running game or check to finish.") : scd::message(info.scd.refusal, info.scd.detail))
                              : info.status == GameStatus::ready ? std::string("Wait for the running game or build to finish.") : std::string("No executable matches the current build configuration yet.");
      tooltip(ctx_, why.c_str());
    }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    const bool prominent_build = info.status != GameStatus::ready && !info.older.playable;  // Play stays the dominant action while an earlier build is playable
    if (is_queued(entry.id())) {
      // SEG-069-T001: a queued game's build action is its way out of the queue (it owns no process or folder yet).
      const bool unqueue_pressed = button(ctx_, "unqueue", "Unqueue", ImVec2(build_w, bh), Style::secondary, true, ctx_.icon("close"));
      take_details_focus("unqueue");
      if (unqueue_pressed) { details_refocus_ = ImGui::IsItemFocused() ? "build" : std::string(); details_refocus_ttl_ = 30; dequeue_build(entry.id()); }
    } else {
      const bool build_pressed = button(ctx_, "build", build_action_label(info).c_str(), ImVec2(build_w, bh), prominent_build && !locate_primary ? Style::primary : Style::secondary, can_build(entry, info), ctx_.icon("reset"));
      take_details_focus("build");
      if (build_pressed) { details_refocus_ = ImGui::IsItemFocused() ? "after_build" : std::string(); details_refocus_ttl_ = 30; notices_.erase(entry.id()); request_build(entry.id()); }
    }
    if (ImGui::IsItemHovered())
      tooltip(ctx_, is_queued(entry.id()) ? "Remove this game from the build queue; nothing has been built yet."
                    : info.sega_cd && !can_build(entry, info) ? "A build needs a free viewer, the BIOS and the disc in place."
                    : info.sega_cd ? "Build a playable program from your BIOS and disc on this computer (several minutes the first time)."
                    : info.status == GameStatus::ready ? "Recompile the native program with the current configuration." : "Recompile with the configuration below.");
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    note_item_layout("build");   // Build / Rebuild / Unqueue: the item drawn just before this line
    if (button(ctx_, "more", "More", ImVec2(more_w, bh), Style::ghost, true, ctx_.icon("more")) || std::exchange(script_open_more_, false)) ImGui::OpenPopup("##details_more");
    note_item_layout("more");
    if (ImGui::IsItemHovered()) tooltip(ctx_, "More: logs, build folder, storage, cover, remove");
    if (ImGui::IsItemFocused()) focus_label_ = "More";
    draw_game_menu("##details_more", entry.id(), false);
    y += bh;
  }
  ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(top.y + cover_h, y) + ctx_.u(14.0F)));
  ImGui::Dummy(ImVec2(w, 0.0F));
}

void App::take_details_focus(const char *id) {
  if (details_refocus_.empty()) return;
  if (--details_refocus_ttl_ <= 0) { details_refocus_.clear(); return; }
  const bool mine = details_refocus_ == id || (details_refocus_ == "after_build" && (std::string(id) == "cancel_build" || std::string(id) == "unqueue"));
  if (!mine) return;
  ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
  ImGuiContext &g = *ImGui::GetCurrentContext();
  g.NavCursorVisible = true;
  g.NavHighlightItemUnderNav = true;
  details_refocus_.clear();
}

// ---------------------------------------------------------------- B. build configuration

// SEG-069-T005: one line naming exactly what an earlier release's executable is (its own recorded mode and model, never the current selection).
static std::string older_build_description(const OlderBuild &older) {
  std::string text = older.mode == "optimized" ? "Optimized" : "Compatibility";
  if (older.mode == "optimized" && !older.model.empty()) text += older.model == "v1" ? " (v1 model)" : " (" + older.model + " model)";
  text += older.version.empty() ? ", built by an earlier release" : ", built by Segarecomp " + older.version;
  return text;
}

// The player's save of a Sega CD game (ADR 0252): size and last-stored time of the canonical backup RAM image, never its content.
std::string App::scd_save_summary(const LibraryEntry &entry) const {
  const fs::path save = scd_dir(entry) / "backup-ram.bin";
  SDL_PathInfo path_info;
  if (!SDL_GetPathInfo(u8s(save).c_str(), &path_info) || path_info.type != SDL_PATHTYPE_FILE) return "No save yet: the game's internal backup RAM is created on the first play.";
  return format_bytes(static_cast<std::uintmax_t>(path_info.size)) + " internal backup RAM, last stored " + fmt_date(static_cast<long long>(path_info.modify_time / 1000000000LL)) + ".";
}

std::string App::selected_mode_label(const LibraryEntry &entry) const {
  if (entry.platform_id == "sega-cd") return scd_mode_name(scd::selected_build_mode(entry.sega_cd));
  if (entry.aot_policy != "optimized") return "Compatibility";
  if (entry.platform_id == "master-system" && !sms_probe_.valid() && !sms_optimized_available_) return "Compatibility";
  return "Optimized";
}

float App::draw_build_config(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool scd_game = entry.platform_id == "sega-cd";
  const bool busy_now = is_building(entry.id()) || is_queued(entry.id()) || game_ != nullptr || maintenance_running_ ||
                        (scd_game && (scd_import_job_.valid() || scd_launch_job_.valid()));  // other games' builds never lock this game's options
  const bool sms = entry.platform_id == "master-system";
  const RomView probe = scd_game ? RomView{} : view_for(entry);
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  const bool side_labels = inner > ctx_.u(470.0F);
  const float label_w = side_labels ? ctx_.u(112.0F) : 0.0F, ctl_w = inner - label_w;
  const float tile_h = scd_game ? ctx_.u(84.0F) : ctx_.u(64.0F);
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
  // ---- Sega CD: the BIOS and the disc come first, because nothing can be built without them (the counterpart of the Master System mapper) ----
  if (scd_game) {
    const scd::EntryRecord &r = entry.sega_cd;
    const auto input_row = [&](const char *label, const char *sub_label, const std::string &value, ImU32 color) {
      stacked_label(label);
      row_label(label, sub_label, y, ctx_.u(34.0F));
      ImGui::SetCursorScreenPos(ImVec2(cx, y));
      const float used = paragraph(ctx_, ctx_.regular, ctx_.u(12.5F), color, ctl_w, value);
      y += std::max(ctx_.u(34.0F), used + ctx_.u(8.0F));
    };
    const bool bios_bad = info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed || info.scd.refusal == scd::Refusal::bios_unreadable || info.scd.refusal == scd::Refusal::bios_too_large;
    input_row("BIOS", bios_bad ? "needs attention" : "verified", file_name_text(r.bios_path) + "  (" + format_bytes(r.bios_size) + ", SHA-256 " + short_hash(r.bios_sha256) + ")", bios_bad ? col::warn : col::text);
    const bool cue_bad = info.scd.refusal == scd::Refusal::cue_missing || info.scd.refusal == scd::Refusal::cue_changed;
    input_row("Disc (CUE)", cue_bad ? "needs attention" : (r.disc_verified ? "verified" : "linked"),
              file_name_text(r.cue_path) + "  (SHA-256 " + short_hash(r.cue_sha256) + "), " +
                  (r.disc_files.empty() ? std::string("track files are listed once a program is verified") : std::to_string(r.disc_files.size()) + " track file(s) beside it; " + (r.disc_verified ? "sizes and SHA-256 verified" : "not verified")),
              cue_bad || (!r.disc_files.empty() && !r.disc_verified) ? col::warn : col::text);
    y += ctx_.u(4.0F);
  }
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
  if (scd_game) {   // three tiles need the whole card width: the label sits above them
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(a.x + pad, y), "Build mode");
    y += ctx_.u(20.0F);
  } else {
    stacked_label("Build mode");
    row_label("Build mode", "part of the build", y, tile_h);
  }
  {
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    if (scd_game) {   // SEG-097-T003 / SEG-103-T007: Compatibility stays the default; File-first is the recommended broader choice; Optimized is EXPERIMENTAL
      const std::string selected_code = scd::selected_build_mode(entry.sega_cd);
      int sel = selected_code == "file_first" ? 1 : selected_code == "optimized" ? 2 : 0;
      const bool narrow = inner < ctx_.u(560.0F);   // short labels so no tile title or recommendation is cut off at the narrowest widths
      ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
      ImGui::BeginDisabled(busy_now);
      if (option_tiles(ctx_, "scd_policy",
                       narrow ? std::vector<std::string>{"Compatibility", "File-first", "Optimized (exp.)"} : std::vector<std::string>{"Compatibility", "File-first (structural)", "Optimized (experimental)"},
                       narrow ? std::vector<std::string>{"Run-reached code only.", "Recommended. Adds disc files.", "Model-picked; may omit code."}
                              : std::vector<std::string>{"Compiles what the build's run reaches. May miss later play.", "Recommended. Also precompiles eligible disc files.", "A model picks the code; may omit code the game needs."},
                       &sel, inner, tile_h, {true, true, true})) {
        scd::select_build_mode(entry.sega_cd, sel == 1 ? "file_first" : sel == 2 ? "optimized" : "compatibility");
        trace(std::string("scd=policy_selected mode=") + scd::selected_build_mode(entry.sega_cd) + (sel == 2 ? " policy=experimental_optimized" : " policy=other"));
        changed = true;
      }
      ImGui::EndDisabled();
      y += tile_h + ctx_.u(8.0F);
    } else {
    int sel = entry.aot_policy == "optimized" && probe.supports_optimized() ? 1 : 0;
    const bool optimized_offered = probe.supports_optimized();
    const std::string compat_desc = "Broad, conservative recompilation.";
    const std::string opt_desc = !optimized_offered ? (sms_probe_.valid() ? "Checking Master System availability..." : "Not offered by this product build for Master System.")
                                 : (sms ? "The default. Selective policy; may restart in Compatibility."
                                        : "The default. Selective AOT policy; may fall back to Compatibility.");
    ImGui::BeginDisabled(busy_now);
    if (option_tiles(ctx_, "mode", {"Compatibility", "Optimized"}, {compat_desc, opt_desc}, &sel, ctl_w, tile_h, {true, optimized_offered})) {
      entry.aot_policy = sel == 1 ? "optimized" : "compatibility";
      entry.aot_model = sms ? "v1" : "gen2";
      changed = true;
    }
    ImGui::EndDisabled();
    y += tile_h + ctx_.u(8.0F);
    // SEG-069-T002: no model choice is offered. Genesis Optimized is the former old Optimized + gen2 request, with no extra control.
    if (sms && entry.aot_policy == "optimized") {
      ImGui::SetCursorScreenPos(ImVec2(cx, y - ctx_.u(2.0F)));
      if (sms_probe_.valid()) y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, ctl_w, "Checking whether this product build offers Optimized for Master System...") + ctx_.u(4.0F);
      else if (!optimized_offered) y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::warn, ctl_w, "Optimized is the default, but this product build does not offer it for Master System, so this game builds as Compatibility.") + ctx_.u(4.0F);
    }
    }
  }
  // what the executable actually is, versus what is selected
  {
    const std::string selected = selected_mode_label(entry);
    const std::string other = selected == "Optimized" ? "Compatibility" : "Optimized";
    std::string text;
    ImU32 color = col::text2;
    const char *icon = "info";
    const std::string scd_sel_code = scd_game ? std::string(scd::selected_build_mode(entry.sega_cd)) : std::string(), scd_built_code = scd_game ? entry.sega_cd.program_built_mode : std::string();
    if (scd_game) {   // the same slot says what the program is, versus what is selected
      if (is_building(entry.id())) { text = "Building the " + selected + " program..."; color = col::warn; icon = "reset"; }
      else if (scd_import_job_.valid() && scd_import_target_ == entry.id()) { text = "Verifying: " + format_bytes(scd_done_.load()) + (scd_total_ > 0 ? " of " + format_bytes(scd_total_) : std::string()); color = col::warn; icon = "reset"; }
      else if (info.status == GameStatus::metadata_only) { text = "No program (build removed on purpose). Build to play with " + selected + "."; color = col::info; }
      else if (info.scd.state == scd::State::ready && scd_built_code.empty())
        text = scd_sel_code == "compatibility" ? "Program ready. How it was built is not recorded (it was imported or built by an earlier release)."
                                               : "Program ready. How it was built is not recorded: Build to apply " + selected + ".";
      else if (info.scd.state == scd::State::ready && scd_built_code == scd_sel_code) { text = "Program built with " + selected + ": matches this selection, ready to play."; color = col::ok; icon = "check"; }
      else if (info.scd.state == scd::State::ready) { text = "The program was built with " + scd_mode_name(scd_built_code) + ". " + selected + " is selected: Build to apply it. Changing the selection does not change the current program."; color = col::warn; icon = "warning"; }
      else if (info.scd.state == scd::State::no_program) text = layout_.scd_build_problem.empty() ? "Not built yet. Build creates the " + selected + " program, or import a verified prebuilt one under Advanced build options." : scd::message(scd::Refusal::unsupported_build);
      else { text = scd_hint(info); color = col::warn; icon = "warning"; }
    }
    else if (is_building(entry.id())) { text = "Building the " + selected + " executable..."; color = col::warn; icon = "reset"; }
    else if (info.status == GameStatus::ready && info.policy.present && info.policy.fallback) { text = "Optimized was requested; this executable fell back to Compatibility (see Build provenance)."; color = col::warn; icon = "warning"; }
    else if (info.status == GameStatus::ready) { text = "Executable built as " + (info.policy.present ? std::string(effective_mode(info.policy) == "optimized" ? "Optimized" : "Compatibility") : selected) + ": matches this selection, ready to play."; color = col::ok; icon = "check"; }
    else if (info.status == GameStatus::playable_older) { text = "Playing an earlier build: " + older_build_description(info.older) + ". It keeps its own settings; Recompile applies this release and the " + selected + " selection."; color = col::warn; icon = "warning"; }
    else if (info.older.playable) { text = "An earlier build is still playable (" + older_build_description(info.older) + "). The " + selected + " build can be retried with Recompile."; color = col::warn; icon = "warning"; }
    else if (info.other_mode_ready) { text = "No " + selected + " executable yet. Your " + other + " build is kept: switch back to play it, or Build."; color = col::warn; icon = "warning"; }
    else if (info.status == GameStatus::metadata_only) { text = "No executable (metadata only). Build to play with " + selected + "."; color = col::info; }
    else if (info.status == GameStatus::failed) { text = "The last " + selected + " build failed. Check the build log, then try again."; color = col::err; icon = "warning"; }
    else if (info.status == GameStatus::stale) {
      text = "Rebuild required: the stored build does not match this release or configuration" + (info.older.candidate_seen ? " and the earlier build cannot be played because " + info.older.reason : std::string()) + ". Recompile to apply " + selected + ".";
      color = col::warn;
      icon = "warning";
    }
    else { text = "Not built yet. Build creates the " + selected + " executable."; }
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    const auto lines = wrap(ctx_.regular, ctx_.u(12.0F), ctl_w - ctx_.u(26.0F), text);
    icon_image(dl, ctx_.icon(icon), ImVec2(cx, y), ctx_.u(15.0F), color);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, ctx_.u(12.0F), color, ImVec2(cx + ctx_.u(22.0F), y), l.c_str()); y += ctx_.u(15.0F); }
    y += ctx_.u(10.0F);
  }
  // ---- Sega CD: what the selected mode means and what the last run says (the same notes slot the Master System uses for its mapper) ----
  if (scd_game) {
    const std::string selected_code = scd::selected_build_mode(entry.sega_cd), built = entry.sega_cd.program_built_mode;
    const bool has_program = !entry.sega_cd.program.path.empty();
    const auto note = [&](ImU32 color, const std::string &text, float size = 11.5F) {
      ImGui::SetCursorScreenPos(ImVec2(cx, y));
      y += paragraph(ctx_, ctx_.regular, ctx_.u(size), color, ctl_w, text) + ctx_.u(4.0F);
    };
    if (selected_code == "optimized")
      note(col::warn, "Experimental: the program built this way can stop at an uncompiled entry, and the model has no proven complete coverage on Sega CD games. It applies at the next Build; Compatibility stays the safe choice.");
    else if (selected_code == "file_first")
      note(col::text2, "File-first compiles the structurally supported instructions of eligible disc files before the game runs; the game's own loads are still verified byte for byte before any of that code runs. "
                       "Files that are oversize, duplicated or without code structure are not covered, and transformed or generated code is never guessed. It applies at the next Build.");
    if (has_program && built == "file_first")
      note(col::text2, std::string("The disc inventory of the current program is ") + (entry.sega_cd.program_file_first_inventory == "complete" ? "complete." : "incomplete (a structurally filtered subset): code outside it still stops at an uncompiled entry."));
    if (has_program && scd::is_uncompiled_entry_stop(entry.sega_cd.last_stop)) {
      const auto rebuild = [&](const char *id, const char *label, const char *mode_code, const char *trace_line) {
        ImGui::SetCursorScreenPos(ImVec2(cx, y));
        if (button(ctx_, id, label, ImVec2(std::min(ctx_.u(240.0F), ctl_w), ctx_.u(32.0F)), Style::primary, can_build(entry, info), ctx_.icon("reset"))) {
          scd::select_build_mode(entry.sega_cd, mode_code);
          trace(trace_line);
          notices_.erase(entry.id());
          changed = true;
          request_build(entry.id());
        }
        y += ctx_.u(38.0F);
      };
      if (built == "file_first")
        note(col::warn, "The last run ended at an uncompiled entry (" + entry.sega_cd.last_stop + ") even with File-first: that code is probably not in an eligible disc file (oversize, duplicated, without structure, transformed or generated at run time), so rebuilding will not change it.", 12.0F);
      else if (built == "optimized") {
        note(col::warn, "The last run ended at an uncompiled entry (" + entry.sega_cd.last_stop + "): the experimental build omitted code the game needed. A full Compatibility rebuild starts the game again from reset.", 12.0F);
        rebuild("scd_rebuild_compat", "Rebuild with Compatibility", "compatibility", "scd=compat_rebuild_after_uncompiled_entry");
      } else {
        note(col::warn, "The last run ended at an uncompiled entry (" + entry.sega_cd.last_stop + "): the build only compiled the code its own run reached. Rebuilding with File-first (structural) can precompile eligible disc files; it does not fix transformed or unavailable code.", 12.0F);
        rebuild("scd_rebuild_file_first", "Rebuild with File-first", "file_first", "scd=file_first_rebuild_after_uncompiled_entry");
      }
    }
    if (const auto last = scd_build_notes_.find(entry.id()); last != scd_build_notes_.end()) note(col::text, "Last build: " + last->second, 12.0F);
    y += ctx_.u(4.0F);
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
  } else if (scd_game) {
    stacked_label("Console audio");
    row_label("Console audio", nullptr, y, ctx_.u(34.0F));
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    const float used = paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, ctl_w, "CD music (CD-DA) and PCM sound effects play through the viewer's volume and mute. FM and PSG sound are not available.");
    y += std::max(ctx_.u(34.0F), used + ctx_.u(6.0F));
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
    if (scd_game) summary = entry.sega_cd.program.path.empty() ? "no program imported" : "program: " + file_name_text(entry.sega_cd.program.path);
    else if (!sms) summary = entry.hint_alias_discovery ? "H-int discovery on" : "H-int discovery off";
    else summary = "none for this console";
    const std::string sum_shown = ellipsize(ctx_.regular, ctx_.u(11.5F), inner * 0.5F, summary);
    draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::dim, ImVec2(rb.x - ctx_.u(8.0F) - text_width(ctx_.regular, ctx_.u(11.5F), sum_shown.c_str()), c.y - ctx_.u(7.0F)), sum_shown.c_str());
    if (foc) focus_ring(ctx_, dl, ra, rb);
    if (pressed) show_advanced_ = !show_advanced_;
    y += ctx_.u(32.0F);
    if (show_advanced_) {
      if (scd_game) {   // the verified program: import one built elsewhere, or check the current one again (Locate BIOS / disc: the More menu and the provenance rows)
        const scd::EntryRecord &r = entry.sega_cd;
        const bool scd_busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr;
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (button(ctx_, "scd_attach", r.program.path.empty() ? "Import program..." : "Replace program...", ImVec2(ctx_.u(180.0F), ctx_.u(32.0F)), Style::secondary, !scd_busy, ctx_.icon("import"))) scd_browse("scd_attach", entry.id());
        ImGui::SameLine(0.0F, ctx_.u(8.0F));
        if (button(ctx_, "scd_verify", "Verify again", ImVec2(ctx_.u(140.0F), ctx_.u(32.0F)), Style::secondary, !scd_busy && !r.program.path.empty(), ctx_.icon("check"))) scd_start_import(entry.id(), {}, {}, {});
        if (ImGui::IsItemHovered()) tooltip(ctx_, "Hashes the program and every disc file again (takes a few seconds for a full disc).");
        y = ImGui::GetCursorScreenPos().y + ctx_.u(6.0F);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner, "A program is checked against exactly this BIOS and disc; it is not signed or compiled by Segarecomp when imported. Importing or replacing a program never changes the build mode you chose.") + ctx_.u(4.0F);
      } else if (sms) {
        draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(a.x + pad, y), "Master System builds have no further options: the mapper and build mode above are the build identity.");
        y += ctx_.u(22.0F);
      } else {
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
      y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, inner, scd_game ? "The BIOS, the disc and the build mode are part of the build identity: changing the mode needs a new build, and the current program is kept until the new one is verified. Saves and recordings are never touched by a build."
                                                                     : "Build mode, H-interrupt discovery and the mapper are part of the build identity: changing them needs a new build, and the previous executable is kept until Storage cleans it. Region applies at launch.") + ctx_.u(4.0F);
    }
  }
  if (probe.optimized() && rom_policy_identity_cached(layout_, probe).empty() && !status_pending_) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::err, inner, "Optimized cannot verify the build tool identity: saved Optimized builds are not reused.");
  }
  const float h = y - a.y + ctx_.u(10.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), "BUILD CONFIGURATION", rgba(9, 22, 54, 245));
  note_layout("config", a, ImVec2(a.x + w, a.y + h));
  dl->AddRectFilled(ImVec2(a.x + ctx_.u(1.0F), a.y + ctx_.u(2.0F)), ImVec2(a.x + ctx_.u(4.0F), a.y + h - ctx_.u(2.0F)), col::btn_top, ctx_.u(2.0F));
  {
    // A Sega CD program is "up to date" only when it was built with the selected mode (an imported program of unknown mode matches Compatibility only).
    const std::string scd_built = entry.sega_cd.program_built_mode, scd_sel = scd_game ? std::string(scd::selected_build_mode(entry.sega_cd)) : std::string();
    const bool ready = info.status == GameStatus::ready && (!scd_game || (scd_built.empty() ? scd_sel == "compatibility" : scd_built == scd_sel));
    const std::string sel = selected_mode_label(entry);
    state_dot_text(ctx_, dl, ImVec2(a.x + w - pad, a.y + ctx_.u(13.0F)), ready ? sel + " - up to date" : (info.older.playable ? sel + " - recompile recommended" : sel + " - build required"), ready ? col::ok : col::warn, w * 0.5F);
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
  const bool scd_game = entry.platform_id == "sega-cd";
  if (scd_game) {   // SEG-109: the Sega CD counterparts of the ROM rows (the BIOS and the disc are the game's inputs), then the program
    const scd::EntryRecord &r = entry.sega_cd;
    const auto input_file = [&](const char *label, const std::string &path, std::uint64_t size, const std::string &digest, const char *purpose, const char *action_id) {
      std::error_code ec;
      const fs::path file = path_from_text(path);
      const bool exists = !path.empty() && fs::is_regular_file(file, ec);
      const std::string value = file_name_text(path) + (exists ? "  (" + (size > 0 ? format_bytes(size) + ", " : std::string()) + "SHA-256 " + short_hash(digest) + ", linked)" : "  (missing)");
      if (row(label, value, exists ? col::text : col::warn, u8s(file.parent_path()), exists ? "Folder" : "Locate", action_id)) {
        if (exists) SDL_OpenURL(("file://" + u8s(file.parent_path())).c_str());
        else scd_browse(purpose, entry.id());
      }
    };
    input_file("BIOS", r.bios_path, r.bios_size, r.bios_sha256, "scd_locate_bios", "bios_action");
    input_file("Disc (CUE)", r.cue_path, r.cue_size, r.cue_sha256, "scd_locate_cue", "cue_action");
    if (row("Game identity", entry.sha256.substr(0, 16) + " ... " + entry.sha256.substr(48), col::text, "Identifies the game: derived from the BIOS and disc digests; the library never matches by name.", "Copy", "copy_sha")) { SDL_SetClipboardText(entry.sha256.c_str()); toast("SHA-256 copied."); }
    if (r.program.path.empty()) row("Program", "none yet", col::text2, "Build one, or import a verified prebuilt program (Advanced build options).");
    else {
      const fs::path program = path_from_text(r.program.path);
      row("Program", file_name_text(r.program.path) + "  (" + format_bytes(r.program.size) + ", SHA-256 " + short_hash(r.program.sha256) + ")", col::text, u8s(program.parent_path()));
      row("Program kind", std::string(r.program.viewer ? "Viewer build (playable)" : "Headless-only build (not playable in the viewer)") + ", runtime contract " + std::to_string(r.program.runtime_abi),
          r.program.viewer && r.program.runtime_abi == scd::supported_runtime_abi ? col::text : col::warn);
      row("Verified", fmt_date(r.program.verified_at), col::text2, "The program's own statement matches your BIOS and disc; it is not signed or compiled by Segarecomp when imported.");
    }
  } else {
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
  }
  {
    std::string built = entry.last_built_at > 0 ? fmt_date(entry.last_built_at) + "  (" + fmt_relative(entry.last_built_at, now) + ")" : "never recorded";
    if (info.build_known && info.build.version.empty() && entry.last_built_at > 0) built += "  (file time)";
    row("Last recompiled", built, col::text);
  }
  row("Last played", entry.last_played_at > 0 ? fmt_date(entry.last_played_at) + "  (" + fmt_relative(entry.last_played_at, now) + ")" : "never", col::text,
      entry.last_run == "failed" ? "Exit code " + std::to_string(entry.last_exit_code) : std::string());
  if (scd_game) {}
  else if (info.build_known && (!info.build.version.empty() || !info.build.toolchain.empty()))
    row("Build version", (info.build.version.empty() ? std::string("unknown release") : "Segarecomp " + info.build.version) + (info.build.toolchain.empty() ? std::string() : "  -  " + info.build.toolchain), col::text);
  else row("Build version", info.status == GameStatus::ready ? "unknown (adopted build)" : "no current build", col::text2, std::string("This release: Segarecomp ") + SEGARECOMP_LAUNCHER_VERSION + ", zig " + SEGARECOMP_ZIG_VERSION);
  if (scd_game) {
    const scd::EntryRecord &r = entry.sega_cd;
    std::error_code ec;
    const std::string built = r.program_built_mode;
    const std::string inventory = built == "file_first" ? std::string("; disc inventory ") + (r.program_file_first_inventory == "complete" ? "complete" : "incomplete (a structurally filtered subset)") : std::string();
    if (row("Build identity", r.program.program_identity.empty() ? std::string("-") : short_hash(r.program.program_identity), col::text,
            scd_mode_name(scd::selected_build_mode(r)) + " selected", fs::is_directory(info.entry, ec) ? "Open" : nullptr, "open_build")) SDL_OpenURL(("file://" + u8s(info.entry)).c_str());
    if (!r.program.path.empty())
      row("Executable", built.empty() ? std::string("built mode not recorded") : scd_mode_name(built) + inventory, built.empty() ? col::text2 : col::text,
          built.empty() ? "Imported or built by an earlier release: nothing is invented." : std::string());
    if (row("Save data", scd_save_summary(entry), col::text, "Protected player data: cleanup, rebuilds, program replacement and library removal never delete or reset it.", "Folder", "open_save")) {
      std::error_code sec;
      if (fs::is_directory(scd_dir(entry), sec)) SDL_OpenURL(("file://" + u8s(scd_dir(entry))).c_str());
    }
  } else {
    std::string identity = info.key.empty() ? std::string("-") : info.key.substr(0, 16) + "...";
    std::string detail = selected_mode_label(entry) + " selected";
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
  if (info.older.playable) {
    // The build that Play will start: it keeps its own recorded mode and model; nothing here is relabelled as the current selection.
    row("Playable build", older_build_description(info.older), col::warn, "From an earlier release; its recorded settings are shown, not the current selection. Recompile to use this release.");
  } else if (info.status == GameStatus::stale && info.older.candidate_seen) {
    row("Earlier build", "cannot be played", col::warn, "Rebuild required: " + info.older.reason + ".");
  }
  row("Last run status", entry.last_played_at > 0 ? std::string(run_label(entry.last_run)) : "never played", entry.last_run == "failed" ? col::err : col::text,
      scd_game && scd_last_outcome_.count(entry.id()) != 0 ? scd_last_outcome_.at(entry.id()) : std::string());
  if (scd_game)
    row("Diagnostics", entry.sega_cd.last_stop.empty() ? std::string("No typed execution stop recorded.") : "Last typed execution stop: " + entry.sega_cd.last_stop + ".", col::text2,
        info.has_failed_dir ? "A failed build folder with its sanitized log is kept for review." : std::string());
  else
  row("Diagnostics", info.compat_known ? "Experimental analysis metadata for this ROM ships with this release." : "No ROM-specific analysis metadata ships with this release.", col::text2,
      info.has_failed_dir ? "A failed build folder with its sanitized log is kept for review." : std::string());
  const float h = y - a.y + ctx_.u(10.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), "BUILD PROVENANCE");
  note_layout("provenance", a, ImVec2(a.x + w, a.y + h));
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
  const bool scd_game = entry.platform_id == "sega-cd";
  const auto note_lines = wrap(ctx_.regular, ctx_.u(11.5F), w - 2 * pad, archived ? (scd_game ? "Metadata only: the library entry, history, saves and recordings are kept. A new build is needed to play."
                                                                                              : "Metadata only: the library entry, history and cover are kept. A new build is needed to play.")
                                                                               : (u != nullptr && u->total.total() == 0 ? "No build files on disk for this game."
                                                                                  : scd_game ? "Measured from this game's build folders. The BIOS, disc files and covers are not counted; saves and recordings are shown apart and are never removed by cleanup."
                                                                                             : "Measured from this game's build folders. ROM files and covers are not counted."));
  const float h = ctx_.u(52.0F) + ctx_.u(14.0F) + (scd_game ? 7.0F : 6.0F) * row + ctx_.u(8.0F) + static_cast<float>(note_lines.size()) * ctx_.u(14.5F) + ctx_.u(12.0F) + 2.0F * ctx_.u(38.0F) + ctx_.u(8.0F);
  const ImVec2 b(a.x + w, a.y + h);
  section_card(ctx_, dl, a, b, "STORAGE");
  note_layout("storage", a, b);
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
  if (scd_game) {   // SEG-109-T004: the player's own data, reported apart from the removable build files and never offered to a cleanup action
    dl->AddRectFilled(ImVec2(a.x + pad, y + ctx_.u(4.0F)), ImVec2(a.x + pad + ctx_.u(10.0F), y + ctx_.u(14.0F)), col::ok, ctx_.u(2.0F));
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text, ImVec2(a.x + pad + ctx_.u(18.0F), y + ctx_.u(1.0F)), "Saves & recordings");
    const std::string v = u ? format_bytes(u->user_data) : "-";
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::ok, ImVec2(b.x - pad - text_width(ctx_.regular, ctx_.u(12.5F), v.c_str()), y + ctx_.u(1.0F)), v.c_str());
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


// ---------------------------------------------------------------- Sega CD: input recording (the one section a native game does not have)

float App::draw_scd_recordings(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  const bool busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr;
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + ctx_.u(38.0F)));
  checkbox(ctx_, "scd_record", "Record this session's controller input when I press Play", &scd_record_);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + ctx_.u(2.0F)));
  paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner,
            "A recording is a small text script of button presses at exact frame boundaries; replaying it reproduces the session exactly. It holds no game data. "
            "The viewer's Pause menu is Esc; F6 holds the machine and F7 steps one frame while it is held.");
  const auto recordings = scd_recordings(entry);
  if (recordings.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + ctx_.u(2.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner, "No recordings yet.");
  }
  for (std::size_t i = 0; i < recordings.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    std::error_code ec;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const std::string name = u8s(recordings[i].filename());
    draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text, ImVec2(a.x + pad, p.y + ctx_.u(8.0F)), (name + "  (" + format_bytes(fs::file_size(recordings[i], ec)) + ")").c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - pad - ctx_.u(250.0F), p.y));
    if (button(ctx_, "replay", "Replay", ImVec2(ctx_.u(120.0F), ctx_.u(28.0F)), Style::secondary, !busy && info.scd.state == scd::State::ready)) scd_play(entry.id(), 1, recordings[i]);
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "replay_fast", "No window", ImVec2(ctx_.u(120.0F), ctx_.u(28.0F)), Style::ghost, !busy && info.scd.state == scd::State::ready)) scd_play(entry.id(), 2, recordings[i]);
    if (ImGui::IsItemHovered()) tooltip(ctx_, "Replays the recording as fast as possible without a window and reports the result.");
    ImGui::PopID();
  }
  const float h = ImGui::GetCursorScreenPos().y - a.y + ctx_.u(14.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), "INPUT RECORDING");
  note_layout("recording", a, ImVec2(a.x + w, a.y + h));
  dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
  ImGui::Dummy(ImVec2(w, 0.0F));
  return h;
}

// ---------------------------------------------------------------- the page

void App::draw_details(float w, float h) {
  LibraryEntry *entry = catalog_.find_id(detail_id_);
  if (entry == nullptr) { page_ = Page::library; return; }
  const StatusInfo info = status_of(detail_id_);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool is_building = this->is_building(detail_id_);
  details_layout_.clear();
  details_origin_ = ImGui::GetCursorScreenPos();
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
  const bool sega_cd = entry->platform_id == "sega-cd";
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
  if (!is_building && info.rom_missing && sega_cd)
    banner(col::warn, "warning", scd::message(info.scd.refusal, info.scd.detail) + " Playing is unavailable until it is located again; the game's library data, saves and recordings are unchanged.");
  if (!is_building && info.rom_missing && !sega_cd)
    banner(col::warn, "warning", entry->rom_path_unknown ? "The original ROM location was never recorded (this entry came from an older cache record). Locate ROM to enable rebuilding; playing an existing build still works."
                                                         : "The original ROM is not at its recorded location. Playing an existing build still works; Locate ROM re-links it (the SHA-256 must match).");
  // build log: live while building, on request otherwise
  if (is_building || show_log_) {
    std::string text;
    if (is_building) {
      if (SDL_GetTicks() - live_log_at_ > 700U) {
        live_log_at_ = SDL_GetTicks();
        live_log_ = read_tail(build_live_log(entry->id()), 1400);
      }
      text = live_log_.empty() ? "Waiting for the build tool's first message..." : live_log_;
    } else if (const auto d = diagnostics_.find(entry->id()); d != diagnostics_.end()) text = d->second;
    else {
      const fs::path failed = info.entry.parent_path() / (sega_cd ? std::string("build.failed") : info.key + ".failed");
      text = read_tail(info.entry / "build.log", 6000);
      if (text.empty()) text = read_tail(failed / "build.log", 6000);
      const std::string run = read_tail(sega_cd ? scd_dir(*entry) / "run.log" : info.entry / "run.log", 2000);
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
    if (is_building) { paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, main_w, "The build reports stages, not a percentage. Cancel build stops it and discards its partial files; closing the launcher waits for it."); ImGui::Dummy(ImVec2(1, ctx_.u(10.0F))); }
  }
  if (!rail) {  // narrow window: the storage card follows the configuration
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float sh = draw_storage_rail(*entry, info, p, main_w);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sh + ctx_.u(12.0F)));
    ImGui::Dummy(ImVec2(main_w, 0.0F));
  }
  draw_provenance(*entry, info, main_w);
  if (sega_cd) {   // the one section only a Sega CD game has: its input recordings and replays (SEG-078), in the same section-card presentation
    ImGui::Dummy(ImVec2(1, ctx_.u(12.0F)));
    draw_scd_recordings(*entry, info, main_w);
  }
  const float bottom = std::max(rail_bottom, ImGui::GetCursorScreenPos().y);
  ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

} // namespace launcher
