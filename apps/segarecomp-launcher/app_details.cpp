// The contextual Game page (opened from a card, a list row or a game's context menu; never a sidebar entry).
//
// Always on top: the identity header (cover, editable title, platform, ONE state in the library's words, a sentence of what happens next and the state's
// single primary action: Play, Prepare & play, Retry, Find game file, Choose mapper, Cancel or Unqueue; everything else is in "More"). Below it the tabs
// (ordinary view state, `details_tab_`):
//   Overview       notices, what happened after a failure (reason, next step, log), and the rows that open the other tabs ("Advanced details").
//   Mods           the per-game Mods card: No mod (original game) and the installed mods, one selected variant, typed status, install / select / prepare / play / remove
//                  (Genesis only: other platforms get an honest "not available" card).
//   Game settings  per-game preparation options (mapper, mode, region, cartridge hardware, advanced build options), input recording, display and audio
//                  (where they live), the library entry (favorite, cover, remove); every card says its scope.
//   Advanced       build provenance and identity, older versions, the build log, the game file row with Locate, the storage card, diagnostics.
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
  if (!info.variant.empty()) {   // SEG-122: the selected variant is a Patched one
    switch (info.status) {
      case GameStatus::ready: return "Rebuild";   // short on purpose: the Play button already says Patched and the tooltip names the variant
      case GameStatus::failed: return "Try again";
      case GameStatus::stale:
      case GameStatus::playable_older: return "Rebuild";
      default: return "Build patched";
    }
  }
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
  if (info.sega_cd) return info.scd.state == scd::State::ready && !game_ && !maintenance_active() && !scd_launch_job_.valid() && !scd_import_job_.valid() && !(scd_build_ && scd_build_key_ == info.key);
  return info.runnable() && !game_ && !maintenance_active() && !(job_ && job_key_ == info.key); }

void App::play(const std::string &id) {
  if (const LibraryEntry *e = catalog_.find_id(id); e != nullptr && e->platform_id == "sega-cd") { trace("play=requested title=" + e->title()); scd_play(id, 0); return; }
  trace("play=requested title=" + (catalog_.find_id(id) != nullptr ? catalog_.find_id(id)->title() : std::string("?")));
  restarted_this_play_ = false;
  notices_.erase(id);
  show_log_ = false;
  launch(id, false);
}

// The Details words for the preparation action: the library's vocabulary (Prepare & play / Prepare / Retry), never Build / Rebuild / Recompile.
std::string App::prepare_label(const LibraryEntry &entry, const StatusInfo &info) const {
  if (!info.variant.empty()) {   // SEG-122: the selected variant is a Patched one
    switch (info.status) {
      case GameStatus::ready:
      case GameStatus::stale:
      case GameStatus::playable_older: return "Prepare again";
      case GameStatus::failed: return "Retry";
      default: return "Prepare selected mod";
    }
  }
  switch (info.status) {
    case GameStatus::ready: return "Prepare again";
    case GameStatus::failed: return "Retry";
    case GameStatus::stale:
    case GameStatus::metadata_only:
    case GameStatus::playable_older: return "Prepare";
    case GameStatus::not_built: return info.sega_cd || entry.platform_id == "sega-cd" ? "Prepare" : "Prepare & play";
  }
  return "Prepare";
}

std::string App::details_primary_label(const LibraryEntry &entry, const StatusInfo &info) const {
  if (is_stopping(entry.id())) return "Stopping...";
  if (is_building(entry.id())) return "Cancel";
  const bool scd_locate = info.sega_cd && ((info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed) || (info.scd.refusal == scd::Refusal::cue_missing || info.scd.refusal == scd::Refusal::cue_changed));
  if ((info.rom_missing && !info.runnable() && !entry.rom_paths.empty() && !info.sega_cd) || scd_locate) return scd_locate ? ((info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed) ? "Locate BIOS..." : "Locate disc...") : "Find game file";
  if (!info.sega_cd && entry.platform_id == "master-system" && entry.mapper.empty() && entry.mapper_manifest_sha256.empty() && !info.runnable()) return "Choose mapper";
  if (info.sega_cd ? info.scd.state == scd::State::ready : info.runnable()) return "Play";
  if (is_queued(entry.id())) return "Unqueue";
  return prepare_label(entry, info);
}

void App::remove_cover(LibraryEntry &entry) {
  remove_cover_file(covers_dir_, entry.cover);
  if (auto it = cover_cache_.find(entry.cover); it != cover_cache_.end()) { if (it->second) SDL_DestroyTexture(it->second); cover_cache_.erase(it); }
  entry.cover.clear();
  save_catalog();
  toast("Cover removed; the placeholder is shown.");
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
    const bool scd_busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr || scd_build_ != nullptr;
    if (menu_item(ctx_, "m_scd_attach", entry->sega_cd.program.path.empty() ? "Import program..." : "Replace program...", "A verified prebuilt viewer program", ctx_.icon("import"), mw, !scd_busy)) scd_browse("scd_attach", id);
    if (menu_item(ctx_, "m_scd_verify", "Verify again", "Hashes the program and the disc files", ctx_.icon("check"), mw, !scd_busy && !entry->sega_cd.program.path.empty())) scd_start_import(id, {}, {}, {});
  }
  if (menu_item(ctx_, "m_storage", "Manage storage...", "Cleanup options for this game", ctx_.icon("storage"), mw)) { storage_selected_ = {id}; go(Page::storage); }
  menu_separator(ctx_, mw);
  if (menu_item(ctx_, "m_cover", "Change cover...", "Choose a local PNG or JPEG", ctx_.icon("edit"), mw)) browse_cover(id);
  if (!entry->cover.empty() && menu_item(ctx_, "m_cover_rm", "Remove cover", nullptr, ctx_.icon("close"), mw)) remove_cover(*entry);
  if (menu_item(ctx_, "m_dl_cover", cover_job_.valid() ? "Downloading cover..." : "Download cover (Libretro)", "Looks up the box art by name only", ctx_.icon("import"), mw, cover_download_possible(*entry)))
    start_cover_download({id});
  {
    CoverQuery q{entry->platform_id, {entry->title(), entry->detected_title}, {}, settings_.preferred_regions};
    for (const auto &rom : entry->rom_paths) q.rom_names.push_back(rom);  // only the file name is used
    if (scd_game && !entry->sega_cd.cue_path.empty()) q.rom_names.push_back(entry->sega_cd.cue_path);
    const auto names = libretro_cover_candidates(q, 1);
    const std::string url = names.empty() ? std::string() : libretro_candidate_url(entry->platform_id, names.front());
    if (menu_item(ctx_, "m_web", "Find cover online", "Opens your web browser", ctx_.icon("search"), mw, !url.empty())) SDL_OpenURL(url.c_str());
  }
  if (menu_item(ctx_, "m_sha", entry->platform_id == "sega-cd" ? "Copy game identity" : "Copy ROM SHA-256", nullptr, ctx_.icon("file"), mw)) { SDL_SetClipboardText(entry->sha256.c_str()); toast("SHA-256 copied."); }
  if (info.rom_missing && !scd_game && menu_item(ctx_, "m_locate", "Locate ROM...", "Re-link a moved ROM (same SHA-256)", ctx_.icon("folder"), mw)) browse_locate(id);
  if (scd_game && (info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed) && menu_item(ctx_, "m_locate_bios", "Locate BIOS...", "Point to the original BIOS (same SHA-256)", ctx_.icon("folder"), mw)) scd_browse("scd_locate_bios", id);
  if (scd_game && (info.scd.refusal == scd::Refusal::cue_missing || info.scd.refusal == scd::Refusal::cue_changed) && menu_item(ctx_, "m_locate_cue", "Locate disc...", "Point to the original CUE file (same SHA-256)", ctx_.icon("folder"), mw)) scd_browse("scd_locate_cue", id);
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
  {  // platform and ONE status (text + symbol + color, the library's words); a second pill only when the game file is missing but an existing version still plays
    ImVec2 p(x0, y), size;
    pill(ctx_, dl, p, entry.platform_id == "master-system" ? "Master System" : entry.platform_id == "sega-cd" ? "Sega CD" : "Genesis / Mega Drive", col::info, ctx_.icon("controller"), ctx_.u(12.0F), &size);
    p.x += size.x + ctx_.u(8.0F);
    ImU32 sc = col::text2;
    std::string state;
    if (is_running) { sc = col::ok; state = info.sega_cd ? "Running" : "Playing"; }
    else { const Badge badge = badge_for(entry, info); sc = badge.color; state = badge.label; }
    const char *state_icon = is_building ? "reset" : tone_icon(sc);
    pill(ctx_, dl, p, state.c_str(), sc, state_icon != nullptr ? ctx_.icon(state_icon) : nullptr, ctx_.u(12.0F), &size);
    p.x += size.x + ctx_.u(8.0F);
    if (info.rom_missing && !info.sega_cd && state != "Game file missing") {
      const char *rom_text = entry.rom_path_unknown ? "Game file location not recorded" : "Game file missing";
      if (p.x + text_width(ctx_.bold, ctx_.u(12.0F), rom_text) + ctx_.u(40.0F) > x0 + tw) { p.x = x0; p.y += size.y + ctx_.u(6.0F); }
      pill(ctx_, dl, p, rom_text, col::warn, ctx_.icon("warning"), ctx_.u(12.0F), &size);
    }
    y = p.y + size.y + ctx_.u(10.0F);
  }
  // one sentence of what happens next (never more than two lines)
  {
    std::string line;
    ImU32 color = col::text2;
    const std::string mode = selected_mode_label(entry);
    const bool mapper_needed = entry.platform_id == "master-system" && entry.mapper.empty() && entry.mapper_manifest_sha256.empty();
    if (info.sega_cd) {
      if (is_running) { line = "Running in the viewer. Close its window to return here."; color = col::ok; }
      else if (is_building) { line = "Preparing with " + mode + ". This can take several minutes the first time."; color = col::warn; }
      else if (info.status == GameStatus::metadata_only) { line = "The prepared version was removed on purpose: history, saves and recordings are kept. Prepare it again to play."; color = col::info; }
      else if (scd_launch_job_.valid() && scd_launch_id_ == entry.id()) { line = "Checking the BIOS, the program and the disc files before launch..."; color = col::warn; }
      else if (info.scd.state == scd::State::ready) { line = "Ready to play. Last played " + fmt_relative(entry.last_played_at, std::time(nullptr)) + ". Sega CD support is experimental: a disc may not run correctly."; color = col::text2; }
      else { line = scd_hint(info); color = col::warn; }
    }
    else if (is_running) { line = running_label(game_report_, restarted_this_play_) + (game_view_.patched() ? " Version: Patched - " + game_view_.patch_label + "." : (entry.patches.empty() ? std::string() : std::string(" Version: Vanilla."))); color = col::ok; }
    else if (is_building) { line = "Preparing with " + mode + ". This can take several minutes the first time."; color = col::warn; }
    else if (is_queued(entry.id())) { line = "Waiting for the preparation in progress to finish. Unqueue takes it out of the queue; nothing was prepared yet."; color = col::info; }
    else if (mapper_needed) { line = "Choose the cartridge mapper in Game settings before this game can be prepared. It is never guessed from the file."; color = col::warn; }
    else if (info.hardware_changed && !info.older.playable && !info.rom_missing) { line = "The cartridge hardware changed, so the previous version cannot be used. Prepare the game again to play."; color = col::warn; }
    else if (info.status == GameStatus::playable_older) { line = "Playable: an earlier release prepared this game. Preparing again is recommended, not required."; color = col::warn; }
    else if (info.older.playable) { line = "The last preparation did not finish, but an earlier version is still playable."; color = col::warn; }
    else if (info.rom_missing && !info.runnable()) { line = "Game file missing: it is not where it was. Find game file points the library at it again (it must be the same file)."; color = col::warn; }
    else if (info.status == GameStatus::ready) { line = "Ready to play. Last played " + fmt_relative(entry.last_played_at, std::time(nullptr)) + "."; color = col::text2; }
    else if (info.status == GameStatus::metadata_only) { line = "The prepared version was removed on purpose: history kept. Prepare & play makes it again."; color = col::info; }
    else if (info.status == GameStatus::failed) { line = "Preparation failed. See what happened below, then Retry."; color = col::err; }
    else if (info.status == GameStatus::stale) { line = info.other_mode_ready ? "No " + mode + " version yet; the other mode's version is still available." : "The stored version is from another release or option set. Prepare to play."; color = col::warn; }
    else { line = "Not prepared yet. Prepare & play makes the native program (" + mode + ") on this computer, then starts the game."; }
    if (!entry.patches.empty() && !info.sega_cd && !is_running && !is_building) {   // SEG-122: always say which version Play starts
      const InstalledPatch *sel = entry.selected_patch();
      line += sel == nullptr ? " Play starts the original game." : " Play starts the selected mod.";
    }
    const auto lines = wrap(ctx_.regular, ctx_.u(12.5F), tw, line);
    for (std::size_t i = 0; i < lines.size() && i < 3; ++i) { draw_text(dl, ctx_.regular, ctx_.u(12.5F), color, ImVec2(x0, y), lines[i].c_str()); y += ctx_.u(16.0F); }
    y += ctx_.u(8.0F);
  }
  // primary actions: one primary per state (Play, Prepare & play, Retry, Find game file, Choose mapper, Cancel, Unqueue); everything else is in More
  const float bh = ctx_.u(44.0F);
  ImGui::SetCursorScreenPos(ImVec2(x0, y));
  if (is_building) {
    // SEG-113-T006: the Sega CD build repeats generate / compile / run in rounds; the round number tells the player the build is advancing, not restarting.
    if (scd_build_ && scd_build_id_ == entry.id() && scd_build_->round() > 0) {
      const std::string round_text = "Round " + std::to_string(scd_build_->round());
      draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text2, ImVec2(x0, y), round_text.c_str());
      y += ctx_.u(18.0F);
    }
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
    const bool cancel_pressed = button(ctx_, "cancel_build", stopping ? "Stopping..." : "Cancel", ImVec2(std::min(ctx_.u(180.0F), tw), bh), Style::secondary, !stopping, ctx_.icon("close"));
    note_item_layout("build");
    take_details_focus("cancel_build");
    if (cancel_pressed) { details_refocus_ = ImGui::IsItemFocused() ? "build" : std::string(); details_refocus_ttl_ = 30; cancel_build(entry.id()); }
    if (ImGui::IsItemHovered()) tooltip(ctx_, "Stop preparing and discard its partial files. A version prepared earlier is kept.");
    y += bh;
  } else if (info.sega_cd && scd_import_job_.valid() && scd_import_target_ == entry.id()) {
    // SEG-150-T005: the verification of a program (attach, Verify again, or the program a build just made) hashes the program and the disc files: it can be cancelled at any time
    // and nothing is changed by a cancelled check.
    const bool stopping_check = scd_cancel_.load();
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    spinner(ctx_, dl, ImVec2(sp.x + ctx_.u(8.0F), sp.y + bh * 0.5F), ctx_.u(7.0F), col::info);
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text, ImVec2(sp.x + ctx_.u(26.0F), sp.y + bh * 0.5F - ctx_.u(7.0F)), stopping_check ? "Cancelling..." : "Verifying the program and the disc files...");
    ImGui::SetCursorScreenPos(ImVec2(x0 + ctx_.u(250.0F) < x0 + tw - ctx_.u(120.0F) ? x0 + ctx_.u(250.0F) : x0 + tw - ctx_.u(120.0F), y));
    if (button(ctx_, "scd_cancel_check", "Cancel check", ImVec2(ctx_.u(120.0F), bh), Style::secondary, !stopping_check, ctx_.icon("close"))) scd_cancel_ = true;
    note_item_layout("build");
    y += bh;
  } else {
    const bool scd_locate = info.sega_cd && ((info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed) || (info.scd.refusal == scd::Refusal::cue_missing || info.scd.refusal == scd::Refusal::cue_changed));   // the Sega CD "Locate ROM"
    const bool locate_primary = (info.rom_missing && !info.runnable() && !entry.rom_paths.empty() && !info.sega_cd) || scd_locate;  // a playable version needs no game file: Play stays primary
    const bool mapper_needed = !info.sega_cd && entry.platform_id == "master-system" && entry.mapper.empty() && entry.mapper_manifest_sha256.empty() && !info.runnable();
    const bool runnable_now = info.sega_cd ? info.scd.state == scd::State::ready : info.runnable();
    const bool queued = is_queued(entry.id());
    // SEG-122: with an installed patch Play becomes a split button even when the selected version is not built: Vanilla stays one chevron away.
    const bool split = !entry.patches.empty() && !info.sega_cd;
    const bool play_slot = !locate_primary && !mapper_needed && (runnable_now || split);
    const bool prepare_primary = !runnable_now && !locate_primary && !mapper_needed && !queued;   // the "build" slot is the one primary
    const float play_w = std::min(ctx_.u(150.0F), (tw - ctx_.u(16.0F)) * 0.36F), build_w = std::min(ctx_.u(170.0F), (tw - ctx_.u(16.0F)) * 0.40F), more_w = std::min(ctx_.u(96.0F), tw - play_w - build_w - ctx_.u(16.0F));
    bool slot_drawn = false;
    if (locate_primary) {
      if (button(ctx_, "locate", scd_locate ? ((info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed) ? "Locate BIOS..." : "Locate disc...") : "Find game file", ImVec2(std::max(play_w, build_w), bh), Style::primary, true, ctx_.icon("folder"))) {
        if (scd_locate) scd_browse((info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed) ? "scd_locate_bios" : "scd_locate_cue", entry.id());
        else browse_locate(entry.id());
      }
      note_item_layout("play");
      if (ImGui::IsItemHovered()) tooltip(ctx_, scd_locate ? "Point the library at the moved file again (it must be the same file)." : "Point the library at the moved game file again. It must be the same file (same SHA-256).");
      slot_drawn = true;
    } else if (mapper_needed) {
      if (button(ctx_, "choose_mapper", "Choose mapper", ImVec2(std::max(play_w, build_w), bh), Style::primary, !is_queued(entry.id()), ctx_.icon("settings"))) set_details_tab(DetailsTab::settings);
      note_item_layout("play");
      if (ImGui::IsItemHovered()) tooltip(ctx_, "The header never names the cartridge mapper, so it is never guessed. Choose it in Game settings.");
      slot_drawn = true;
    } else if (play_slot) {
      // SEG-122: with an installed patch Play becomes a split button: the main part plays the selected variant, the chevron opens "Play Vanilla / Play Patched / Prepare patched version"
      const float chev_w = split ? ctx_.u(34.0F) : 0.0F;
      const char *play_label = is_running ? "Playing" : (entry.patches.empty() ? "Play" : (info.variant.empty() ? "Play Vanilla" : "Play Patched"));
      if (split && text_width(ctx_.bold, ctx_.u(13.0F), play_label) + ctx_.u(58.0F) > play_w - chev_w) play_label = is_running ? "Playing" : "Play";   // never a truncated label: the menu and the state line name the version
      if (button(ctx_, "play", play_label, ImVec2(play_w - chev_w, bh), runnable_now ? Style::primary : Style::secondary, can_play(info), ctx_.icon("play"))) play(entry.id());
      note_item_layout("play");
      if (ImGui::IsItemHovered() && !can_play(info)) {
        const std::string why = info.sega_cd ? (info.scd.state == scd::State::ready ? std::string("Wait for the running game or check to finish.") : scd::message(info.scd.refusal, info.scd.detail))
                                : info.status == GameStatus::ready ? std::string("Wait for the running game or preparation to finish.") : std::string("No version matches the current options yet: prepare it first.");
        tooltip(ctx_, why.c_str());
      }
      if (split) {
        ImGui::SameLine(0.0F, ctx_.u(2.0F));
        const bool open = button(ctx_, "play_variants", "", ImVec2(chev_w, bh), runnable_now ? Style::primary : Style::secondary, !game_ && !is_building && !maintenance_active()) || std::exchange(script_open_play_menu_, false);
        const ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax(), c((ra.x + rb.x) * 0.5F, (ra.y + rb.y) * 0.5F);
        const float k = ctx_.u(4.5F);
        dl->AddTriangleFilled(ImVec2(c.x - k, c.y - k * 0.5F), ImVec2(c.x + k, c.y - k * 0.5F), ImVec2(c.x, c.y + k * 0.6F), IM_COL32_WHITE);   // chevron
        if (ImGui::IsItemHovered()) tooltip(ctx_, "Choose which version to play: Vanilla or a patched version");
        if (open) ImGui::OpenPopup("##play_variants");
        if (ImGui::IsPopupOpen("##play_variants")) ImGui::SetNextWindowPos(ImVec2(x0, rb.y + ctx_.u(4.0F)));   // hangs under the split button, right edges aligned
        draw_play_variants_menu("##play_variants", entry, info);
      }
      slot_drawn = true;
    }
    if (locate_primary || mapper_needed) {
      // the preparation itself is unavailable until the file / mapper is resolved: no disabled twin next to the one primary action
    } else {
      if (slot_drawn) ImGui::SameLine(0.0F, ctx_.u(8.0F));
      if (queued) {
        // SEG-069-T001: a queued game's build action is its way out of the queue (it owns no process or folder yet).
        const bool unqueue_pressed = button(ctx_, "unqueue", "Unqueue", ImVec2(build_w, bh), Style::secondary, true, ctx_.icon("close"));
        take_details_focus("unqueue");
        if (unqueue_pressed) { details_refocus_ = ImGui::IsItemFocused() ? "build" : std::string(); details_refocus_ttl_ = 30; dequeue_build(entry.id()); }
      } else {
        const std::string label = prepare_label(entry, info);
        const bool build_pressed = button(ctx_, "build", label.c_str(), ImVec2(build_w, bh), prepare_primary ? Style::primary : Style::secondary, can_build(entry, info), ctx_.icon(label == "Prepare & play" ? "play" : "reset"));
        take_details_focus("build");
        if (build_pressed) {
          details_refocus_ = ImGui::IsItemFocused() ? "after_build" : std::string(); details_refocus_ttl_ = 30; notices_.erase(entry.id());
          if (label == "Prepare & play") prepare_and_play(entry.id()); else request_build(entry.id());
        }
      }
      if (ImGui::IsItemHovered())
        tooltip(ctx_, queued ? "Take this game out of the queue; nothing has been prepared yet."
                      : info.sega_cd && !can_build(entry, info) ? "Preparing needs a free viewer, the BIOS and the disc in place."
                      : info.sega_cd ? "Prepare a playable program from your BIOS and disc on this computer (several minutes the first time)."
                      : !can_build(entry, info) ? "Wait for the running game or preparation to finish."
                      : !info.variant.empty() ? (info.status == GameStatus::ready ? "Prepare the patched version again (a separate program; Vanilla is not touched)." : "Prepare the patched version: a separate program made from the verified patched image. Vanilla is not touched.")
                      : prepare_primary && info.status == GameStatus::not_built ? "Prepare the native program on this computer, then start the game."
                      : info.status == GameStatus::failed ? "Try the preparation again with the same options. Your game file and any earlier working version are unchanged."
                      : info.status == GameStatus::ready ? "Prepare the native program again with the current options." : "Prepare with the options in Game settings.");
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      note_item_layout("build");   // Prepare / Unqueue: the item drawn just before this line
    }
    if (locate_primary || mapper_needed) ImGui::SameLine(0.0F, ctx_.u(8.0F));
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

// SEG-122: the dropdown of the Play button: pick the version to start. Choosing a built version selects it and starts it; an unbuilt Patched version offers its build instead
// (never a silent fall back to Vanilla).
void App::draw_play_variants_menu(const char *popup_id, LibraryEntry &entry, const StatusInfo &info) {
  if (!begin_menu(ctx_, popup_id)) return;
  const float mw = ctx_.u(270.0F);
  const bool idle = !game_ && !maintenance_active() && !is_building(entry.id()) && !is_queued(entry.id());
  const std::string vsub = entry.selected_variant.empty() ? "selected" : "";
  if (menu_item(ctx_, "pv_vanilla", "Play Vanilla", info.vanilla_ready ? vsub.c_str() : "not built", ctx_.icon("play"), mw, idle && info.vanilla_ready)) { select_variant(entry.id(), std::string()); play(entry.id()); }
  for (std::size_t i = 0; i < entry.patches.size(); ++i) {
    const InstalledPatch &p = entry.patches[i];
    bool built = false;
    for (const auto &b : info.patches) if (b.identity == p.identity) built = b.built;
    const std::string tag = entry.patches.size() == 1 ? std::string() : " " + std::to_string(i + 1);   // same numbering as the variant tiles; the name rides in the hint line
    const std::string hint = ellipsize(ctx_.regular, ctx_.u(11.5F), ctx_.u(210.0F), patch_label(p) + (entry.selected_variant == p.identity ? ", selected" : (built ? "" : ", not built")));
    if (built) {
      if (menu_item(ctx_, ("pv_play_" + p.identity.substr(0, 8)).c_str(), ("Play Patched" + tag).c_str(), hint.c_str(), ctx_.icon("play"), mw, idle)) { select_variant(entry.id(), p.identity); play(entry.id()); }
    } else if (menu_item(ctx_, ("pv_build_" + p.identity.substr(0, 8)).c_str(), ("Build patched version" + tag).c_str(), hint.c_str(), ctx_.icon("reset"), mw, idle && can_build(entry, info))) {
      select_variant(entry.id(), p.identity);
      notices_.erase(entry.id());
      request_build(entry.id());
    }
  }
  end_menu();
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
  const bool busy_now = is_building(entry.id()) || is_queued(entry.id()) || game_ != nullptr || maintenance_active() ||
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
      row_label("Cartridge mapper", "this game", y, ctx_.u(36.0F));
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
        draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(cx, y + ctx_.u(2.0F)), "Part of the build identity: changing it needs Prepare again.");
        y += ctx_.u(26.0F);
      }
    }
    y += ctx_.u(6.0F);
  }
  // ---- Build mode ----
  if (scd_game) {   // three tiles need the whole card width: the label sits above them
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(a.x + pad, y), "Mode");
    y += ctx_.u(20.0F);
  } else {
    stacked_label("Mode");
    row_label("Mode", "this game", y, tile_h);
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
      if (is_building(entry.id())) { text = "Preparing the " + selected + " program..."; color = col::warn; icon = "reset"; }
      else if (scd_import_job_.valid() && scd_import_target_ == entry.id()) { text = "Verifying: " + format_bytes(scd_done_.load()) + (scd_total_ > 0 ? " of " + format_bytes(scd_total_) : std::string()); color = col::warn; icon = "reset"; }
      else if (info.status == GameStatus::metadata_only) { text = "No program (removed on purpose). Prepare to play with " + selected + "."; color = col::info; }
      else if (info.scd.state == scd::State::ready && scd_built_code.empty())
        text = scd_sel_code == "compatibility" ? "Program ready. How it was built is not recorded (it was imported or built by an earlier release)."
                                               : "Program ready. How it was built is not recorded: Prepare to apply " + selected + ".";
      else if (info.scd.state == scd::State::ready && scd_built_code == scd_sel_code) { text = "Program built with " + selected + ": matches this selection, ready to play."; color = col::ok; icon = "check"; }
      else if (info.scd.state == scd::State::ready) { text = "The program was built with " + scd_mode_name(scd_built_code) + ". " + selected + " is selected: Prepare again to apply it. Changing the selection does not change the current program."; color = col::warn; icon = "warning"; }
      else if (info.scd.state == scd::State::no_program) text = layout_.scd_build_problem.empty() ? "Not prepared yet. Prepare creates the " + selected + " program, or import a verified prebuilt one under Advanced build options." : scd::message(scd::Refusal::unsupported_build);
      else { text = scd_hint(info); color = col::warn; icon = "warning"; }
    }
    else if (is_building(entry.id())) { text = "Preparing the " + selected + " version..."; color = col::warn; icon = "reset"; }
    else if (info.status == GameStatus::ready && info.policy.present && info.policy.fallback) { text = "Optimized was requested; this version fell back to Compatibility (see Advanced)."; color = col::warn; icon = "warning"; }
    else if (info.status == GameStatus::ready) { text = "Prepared as " + (info.policy.present ? std::string(effective_mode(info.policy) == "optimized" ? "Optimized" : "Compatibility") : selected) + ": matches this selection, ready to play."; color = col::ok; icon = "check"; }
    else if (info.status == GameStatus::playable_older) { text = "Playing an earlier version: " + older_build_description(info.older) + ". It keeps its own settings; Prepare applies this release and the " + selected + " selection."; color = col::warn; icon = "warning"; }
    else if (info.older.playable) { text = "An earlier version is still playable (" + older_build_description(info.older) + "). The " + selected + " version can be retried with Retry."; color = col::warn; icon = "warning"; }
    else if (info.other_mode_ready) { text = "No " + selected + " version yet. Your " + other + " version is kept: switch back to play it, or Prepare."; color = col::warn; icon = "warning"; }
    else if (info.status == GameStatus::metadata_only) { text = "No prepared version (metadata only). Prepare to play with " + selected + "."; color = col::info; }
    else if (info.status == GameStatus::failed) { text = "The last " + selected + " preparation failed. Check the log under Advanced, then Retry."; color = col::err; icon = "warning"; }
    else if (info.status == GameStatus::stale) {
      text = "Needs rebuild: the stored version does not match this release or configuration" + (info.older.candidate_seen ? " and the earlier version cannot be played because " + info.older.reason : std::string()) + ". Prepare to apply " + selected + ".";
      color = col::warn;
      icon = "warning";
    }
    else { text = "Not prepared yet. Prepare creates the " + selected + " version."; }
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
    row_label("Console region", "this game, at start", y, ctx_.u(36.0F));
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
  // SEG-122-T009: a subtle confirmation of an unusual cartridge; an ordinary game shows nothing here.
  if (const std::string hardware = cartridge_status_text(entry); !hardware.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), !entry.mapper_explicit && entry.mapper.empty() ? col::warn : col::text2, inner, hardware) + ctx_.u(6.0F);
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
    else if (!sms) summary = std::string(entry.hint_alias_discovery ? "H-int discovery on" : "H-int discovery off") + (entry.mapper.empty() ? "" : ", lock-on cartridge");
    else summary = "none for this console";
    const std::string sum_shown = ellipsize(ctx_.regular, ctx_.u(11.5F), inner * 0.5F, summary);
    draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::dim, ImVec2(rb.x - ctx_.u(8.0F) - text_width(ctx_.regular, ctx_.u(11.5F), sum_shown.c_str()), c.y - ctx_.u(7.0F)), sum_shown.c_str());
    if (foc) focus_ring(ctx_, dl, ra, rb);
    if (pressed) show_advanced_ = !show_advanced_;
    y += ctx_.u(32.0F);
    if (show_advanced_) {
      if (scd_game) {   // the verified program: import one built elsewhere, or check the current one again (Locate BIOS / disc: the More menu and the provenance rows)
        const scd::EntryRecord &r = entry.sega_cd;
        const bool scd_busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr || scd_build_ != nullptr;
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
        // SEG-122-T009: the cartridge hardware. Automatic is the recommendation; an explicit choice outranks recognition and survives reimports and restarts.
        draw_text(dl, ctx_.bold, ctx_.u(12.0F), col::text, ImVec2(a.x + pad, y), "Cartridge hardware");
        y += ctx_.u(20.0F);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        int cartridge_choice = !entry.mapper_explicit ? 0 : (entry.mapper.empty() ? 1 : 2);
        if (option_tiles(ctx_, "cartridge", {"Automatic", "Standard", "Lock-on"}, {"Recommended", "Ordinary cartridge", "Pass-through cartridge"}, &cartridge_choice, inner, ctx_.u(46.0F),
                         {!busy_now, !busy_now, !busy_now})) {
          select_cartridge(entry, cartridge_choice);
          changed = true;
        }
        y += ctx_.u(52.0F);
        {
          const std::string hardware = cartridge_status_text(entry);
          ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
          y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner, hardware.empty() ? std::string("Standard cartridge - automatically selected") : hardware) + ctx_.u(8.0F);
        }
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
      y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, inner, scd_game ? "The BIOS, the disc and the build mode are part of the build identity: changing the mode needs a new preparation, and the current program is kept until the new one is verified. Saves and recordings are never touched by a build."
                                                                     : "Build mode, H-interrupt discovery and the cartridge hardware are part of the build identity: changing them needs a new preparation, and the previous version is kept until Storage cleans it. Region applies at launch.") + ctx_.u(4.0F);
    }
  }
  if (probe.optimized() && rom_policy_identity_cached(layout_, probe).empty() && !status_pending_) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::err, inner, "Optimized cannot verify the build tool identity: saved Optimized builds are not reused.");
  }
  const float h = y - a.y + ctx_.u(10.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), "PREPARATION - THIS GAME", rgba(9, 22, 54, 245));
  note_layout("config", a, ImVec2(a.x + w, a.y + h));
  dl->AddRectFilled(ImVec2(a.x + ctx_.u(1.0F), a.y + ctx_.u(2.0F)), ImVec2(a.x + ctx_.u(4.0F), a.y + h - ctx_.u(2.0F)), col::btn_top, ctx_.u(2.0F));
  {
    // A Sega CD program is "up to date" only when it was built with the selected mode (an imported program of unknown mode matches Compatibility only).
    const std::string scd_built = entry.sega_cd.program_built_mode, scd_sel = scd_game ? std::string(scd::selected_build_mode(entry.sega_cd)) : std::string();
    const bool ready = info.status == GameStatus::ready && (!scd_game || (scd_built.empty() ? scd_sel == "compatibility" : scd_built == scd_sel));
    const std::string sel = selected_mode_label(entry);
    state_dot_text(ctx_, dl, ImVec2(a.x + w - pad, a.y + ctx_.u(13.0F)), ready ? sel + " - up to date" : (info.older.playable ? sel + " - prepare again recommended" : sel + " - not prepared"), ready ? col::ok : col::warn, w * 0.5F);
  }
  dl->ChannelsMerge();
  if (changed) { save_catalog(); request_status_refresh(); }
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
  ImGui::Dummy(ImVec2(w, 0.0F));
  return h;
}

// ---------------------------------------------------------------- B2. Mods (SEG-122 ADR 0266, SEG-150-T006)
// The per-game Mods tab: "No mod (original game)" and every installed mod as a row, ONE selected variant (composition is not implemented, so nothing here toggles two
// mods), the typed status of each (Installed / Selected / Needs preparation / Prepared / Needs approval / Incompatible) and the actions of the existing flow: install
// (chooser -> verified preview sheet -> identity-bound approval where code is carried), select, prepare, play, remove. Only recorded facts are shown.

float App::draw_variants(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  const bool busy_now = is_building(entry.id()) || is_queued(entry.id()) || game_ != nullptr || maintenance_active();
  const std::string unavailable = mods_unavailable_reason(entry.platform_id);
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  float y = a.y + ctx_.u(40.0F);
  const InstalledPatch *selected = entry.selected_patch();
  const auto tone_color = [](ModTone t) { return t == ModTone::ok ? col::ok : t == ModTone::warn ? col::warn : t == ModTone::err ? col::err : col::text2; };
  const auto built_of = [&](const std::string &identity) {
    for (const auto &p : info.patches) if (p.identity == identity) return p.built;
    return false;
  };
  std::string header_state = "Original game selected";
  ImU32 header_color = col::text2;
  if (!unavailable.empty()) {
    header_state = "Not available for this platform";
    header_color = col::warn;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner, unavailable) + ctx_.u(6.0F);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner, "Mod packages are verified and played for Genesis games only; Master System and Sega CD games keep their own preparation and play flow on the Overview.") + ctx_.u(4.0F);
  } else {
    if (selected != nullptr) { header_state = "Mod selected: " + selected->name; header_color = col::info; }
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner,
                   "Mods are optional packages (.segapatch) made for this exact game. Each one is verified against your original game before anything is installed, and is prepared as its own separate program. The original game, its program and its save data are never changed.") + ctx_.u(8.0F);
    const std::string block = mod_switch_block(entry);
    // ---- the actions first, so they are in view without scrolling: Install mod, and for the selected version Prepare selected mod until built, then Play ----
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    const float bh = ctx_.u(34.0F);
    const bool needs_prep = selected != nullptr && !built_of(selected->identity);
    const ModStatus sel_status = selected != nullptr ? mod_status_of(entry, info, *selected) : ModStatus{};
    const bool selectable_mod = selected != nullptr && sel_status.code != "needs_approval" && sel_status.code != "incompatible_package_missing";
    if (button(ctx_, "patch_install", "Install mod...", ImVec2(ctx_.u(150.0F), bh), Style::secondary, !(busy_now || info.rom_missing), ctx_.icon("import"))) browse_patch(entry.id());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(ctx_, info.rom_missing ? "The original ROM file is needed to verify a mod. Locate it first." : "Choose a .segapatch package. It is verified against your original game and previewed before anything is installed. Nothing is downloaded.");
    if (selected != nullptr) {
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      const bool preparing = is_building(entry.id());
      const char *prep_label = preparing ? "Preparing..." : (info.status == GameStatus::failed ? "Retry preparing" : (needs_prep ? "Prepare selected mod" : "Prepare again"));
      if (button(ctx_, "mods_prepare", prep_label, ImVec2(ctx_.u(190.0F), bh), needs_prep && !preparing ? Style::primary : Style::secondary, selectable_mod && can_build(entry, info) && !maintenance_active(), ctx_.icon("reset"))) { notices_.erase(entry.id()); request_build(entry.id()); }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(ctx_, selectable_mod ? "Prepare the selected mod as its own program on this computer. The original game's program is not touched." : "This mod cannot be prepared until the problem shown on its row is resolved.");
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      if (button(ctx_, "mods_play", "Play modded game", ImVec2(ctx_.u(170.0F), bh), needs_prep || !selectable_mod ? Style::secondary : Style::primary, can_play(info) && !needs_prep && selectable_mod, ctx_.icon("play"))) play(entry.id());
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(ctx_, needs_prep ? "Play is available once the selected mod is prepared. Nothing falls back to the original game silently." : "Start the selected mod.");
    } else {
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      if (button(ctx_, "mods_play", "Play original game", ImVec2(ctx_.u(180.0F), bh), Style::primary, can_play(info), ctx_.icon("play"))) play(entry.id());
    }
    y = ImGui::GetCursorScreenPos().y + ctx_.u(8.0F);
    if (selected != nullptr && needs_prep && selectable_mod) {
      ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
      y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::warn, inner, "Needs preparation: Play stays disabled until the selected mod is prepared. The original game stays playable by selecting No mod (original game).") + ctx_.u(4.0F);
    }
    if (!block.empty() && !entry.patches.empty()) {   // the disabled switch / remove buttons below say why, in text as well as in the tooltip
      ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
      y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::warn, inner, "Switching and removing are paused: " + block) + ctx_.u(4.0F);
    }
    // the outcome of the last install / select / remove (a refusal in the error tone, with its next step) sits right under the actions it answers
    if (const auto note = patch_notices_.find(entry.id()); note != patch_notices_.end() && !note->second.empty()) {
      const bool refused = patch_refused_.count(entry.id()) != 0;
      ImGui::SetCursorScreenPos(ImVec2(a.x + pad + (refused ? ctx_.u(22.0F) : 0.0F), y));
      const ImU32 nc = refused ? col::err : col::info;
      if (refused) tone_mark(ctx_, dl, ImVec2(a.x + pad + ctx_.u(8.0F), y + ctx_.u(8.0F)), ctx_.u(15.0F), nc);
      y += paragraph(ctx_, refused ? ctx_.bold : ctx_.regular, ctx_.u(12.0F), nc, inner - (refused ? ctx_.u(22.0F) : 0.0F), note->second) + ctx_.u(2.0F);
      if (refused) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad + ctx_.u(22.0F), y));
        y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, inner - ctx_.u(22.0F), "Next step: " + mod_refusal_next_step(note->second) + " Nothing was installed or approved.") + ctx_.u(4.0F);
      }
      note_layout("mod_notice", ImVec2(a.x + pad, y - ctx_.u(8.0F)), ImVec2(a.x + pad + inner, y));
    }
    // ---- the rows: the original game first (always one click away), then every installed mod; exactly one is selected ----
    const float btn_w = ctx_.u(128.0F), gap = ctx_.u(10.0F);
    const float text_w = inner - btn_w - 3 * gap - ctx_.u(8.0F);
    struct Row { bool original = false; const InstalledPatch *patch = nullptr; std::size_t index = 0; };
    std::vector<Row> rows{Row{true, nullptr, 0}};
    for (std::size_t i = 0; i < entry.patches.size(); ++i) rows.push_back(Row{false, &entry.patches[i], i});
    std::string pending_select;
    bool select_requested = false;
    for (const Row &row : rows) {
      const bool is_sel = row.original ? selected == nullptr : selected != nullptr && selected->identity == row.patch->identity;
      std::string title, kind_line, target_line, status_label, reason;
      ModTone tone = ModTone::neutral;
      ModStatus st;
      if (row.original) {
        title = "No mod (original game)";
        kind_line = "The original verified game and its existing program. Its saves are separate from every mod's.";
        const bool vanilla_selected_state = is_sel;
        status_label = vanilla_selected_state ? (info.vanilla_ready ? "Selected - Prepared" : "Selected - Needs preparation") : (info.vanilla_ready ? "Prepared" : "Not prepared");
        tone = is_sel ? (info.vanilla_ready ? ModTone::ok : ModTone::warn) : ModTone::neutral;
      } else {
        const InstalledPatch &p = *row.patch;
        st = mod_status_of(entry, info, p);
        title = patch_label(p);
        kind_line = mod_kind_label(p) + (p.author.empty() ? std::string(". Author not recorded.") : " - by " + p.author);
        target_line = "Target: " + entry.title() + ", exact original image " + (entry.sha256.size() >= 12 ? entry.sha256.substr(0, 12) + "..." : entry.sha256) + ". " +
                      (p.schema3 ? std::string("Schema 3 mod") : std::string("Schema 1/2 patch package")) + ". " +
                      (mod_capabilities_label(p).empty() ? std::string("Capabilities: not recorded here.") : "Reads: " + mod_capabilities_label(p) + ".");
        status_label = st.label;
        tone = st.tone;
        reason = st.reason.empty() ? std::string() : st.reason + " Next step: " + st.next;
        if (st.reason.empty() && !st.next.empty()) reason = "Next step: " + st.next;
      }
      const float fpx = ctx_.u(12.0F), spx = ctx_.u(11.5F);
      const auto kind_lines = wrap(ctx_.regular, fpx, text_w, kind_line);
      const auto target_lines = target_line.empty() ? std::vector<std::string>{} : wrap(ctx_.regular, spx, text_w, target_line);
      const auto reason_lines = reason.empty() ? std::vector<std::string>{} : wrap(ctx_.regular, spx, text_w, reason);
      const float line_h = ctx_.u(16.0F), small_h = ctx_.u(15.0F);
      const float rh = std::max(ctx_.u(86.0F), ctx_.u(14.0F) + ctx_.u(20.0F) + static_cast<float>(kind_lines.size()) * line_h + static_cast<float>(target_lines.size()) * small_h + ctx_.u(22.0F) + static_cast<float>(reason_lines.size()) * small_h + ctx_.u(10.0F));
      const ImVec2 r0(a.x + pad, y), r1(a.x + pad + inner, y + rh);
      card(ctx_, dl, r0, r1, is_sel, false, rgba(11, 27, 62, 250));
      float ty = r0.y + ctx_.u(12.0F);
      const float tx = r0.x + ctx_.u(14.0F);
      draw_text(dl, ctx_.bold, ctx_.u(13.5F), col::text, ImVec2(tx, ty), ellipsize(ctx_.bold, ctx_.u(13.5F), text_w, title).c_str());
      ty += ctx_.u(20.0F);
      for (const auto &l : kind_lines) { draw_text(dl, ctx_.regular, fpx, col::text2, ImVec2(tx, ty), l.c_str()); ty += line_h; }
      for (const auto &l : target_lines) { draw_text(dl, ctx_.regular, spx, col::text2, ImVec2(tx, ty), l.c_str()); ty += small_h; }
      ty += ctx_.u(4.0F);
      const ImU32 color = tone_color(tone);
      tone_mark(ctx_, dl, ImVec2(tx + ctx_.u(7.0F), ty + ctx_.u(8.0F)), ctx_.u(15.0F), color);   // symbol + text + color: never color alone
      draw_text(dl, ctx_.bold, ctx_.u(12.5F), color, ImVec2(tx + ctx_.u(20.0F), ty + ctx_.u(1.0F)), status_label.c_str());
      ty += ctx_.u(20.0F);
      for (const auto &l : reason_lines) { draw_text(dl, ctx_.regular, spx, color, ImVec2(tx, ty), l.c_str()); ty += small_h; }
      // the right column: Selected tag / Use this mod, then Remove (a mod only)
      const float bx = r1.x - gap - btn_w;
      const std::string tag = row.original ? "orig" : std::to_string(row.index);
      float by = r0.y + ctx_.u(12.0F);
      if (is_sel) {
        ImVec2 sz;
        pill(ctx_, dl, ImVec2(bx, by + ctx_.u(4.0F)), "Selected", col::info, ctx_.icon("check"), ctx_.u(12.0F), &sz);
      } else {
        const bool blocked_state = !row.original && (st.code == "needs_approval" || st.code == "incompatible_package_missing");
        const char *label = row.original ? "Use original game" : (selected != nullptr ? "Switch to this mod" : "Use this mod");
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (button(ctx_, ("mod_select_" + tag).c_str(), label, ImVec2(btn_w, ctx_.u(30.0F)), row.original ? Style::primary : Style::secondary, block.empty() && !blocked_state)) {
          select_requested = true;
          pending_select = row.original ? std::string() : row.patch->identity;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(ctx_, !block.empty() ? block.c_str() : blocked_state ? "This mod cannot be selected until the problem shown on its row is resolved." : row.original ? "Return to the original game. Your mods stay installed." : "Select this mod. Only one mod is used at a time; it replaces the selected one.");
      }
      if (!row.original) {
        ImGui::SetCursorScreenPos(ImVec2(bx, by + ctx_.u(38.0F)));
        if (button(ctx_, ("mod_remove_" + tag).c_str(), "Remove", ImVec2(btn_w, ctx_.u(28.0F)), Style::ghost, block.empty(), ctx_.icon("trash"))) ask_remove_patch(entry, row.patch->identity);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) tooltip(ctx_, !block.empty() ? block.c_str() : "Remove this mod's package after a confirmation. The original game and its Vanilla version are untouched.");
      }
      note_layout(row.original ? "mod_row_original" : "mod_row", r0, r1);
      y += rh + ctx_.u(8.0F);
    }
    if (select_requested) select_variant(entry.id(), pending_select);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    if (entry.patches.empty()) {
      y += paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner, "No mod is installed on this game yet. Install mod verifies a package and shows what it is, who made it and what it needs before anything is added.") + ctx_.u(6.0F);
    } else {
      y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, inner, "One mod at a time: selecting another mod replaces the selected one, and combining mods is not supported. Each mod is its own program with its own save data; No mod (original game) is always one click away.") + ctx_.u(6.0F);
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
  if (unavailable.empty()) y += paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, inner, "Installing a mod does not by itself make the picture authored widescreen: the Widescreen settings apply unchanged and Vanilla keeps the generic widescreen. A mod changes the game's own behaviour in its own program only.") + ctx_.u(4.0F);
  const float h = y - a.y + ctx_.u(10.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), "MODS", rgba(9, 22, 54, 245));
  note_layout("variants", a, ImVec2(a.x + w, a.y + h));
  dl->AddRectFilled(ImVec2(a.x + ctx_.u(1.0F), a.y + ctx_.u(2.0F)), ImVec2(a.x + ctx_.u(4.0F), a.y + h - ctx_.u(2.0F)), col::btn_top, ctx_.u(2.0F));
  state_dot_text(ctx_, dl, ImVec2(a.x + w - pad, a.y + ctx_.u(13.0F)), header_state, header_color, w * 0.5F);
  dl->ChannelsMerge();
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
    // SEG-150-T005: attaching or re-verifying a prebuilt program is reachable right here (Advanced); each is checked against exactly this BIOS and disc and refused with a typed reason otherwise.
    const bool prog_busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr || scd_build_ != nullptr;
    if (r.program.path.empty()) {
      if (row("Program", "none yet", col::text2, "Prepare builds one on this computer, or import a verified prebuilt program: it is checked against exactly this BIOS and disc.", "Import", "prog_import") && !prog_busy) scd_browse("scd_attach", entry.id());
    } else {
      const fs::path program = path_from_text(r.program.path);
      if (row("Program", file_name_text(r.program.path) + "  (" + format_bytes(r.program.size) + ", SHA-256 " + short_hash(r.program.sha256) + ")", col::text, u8s(program.parent_path()), "Replace", "prog_replace") && !prog_busy) scd_browse("scd_attach", entry.id());
      row("Program kind", std::string(r.program.viewer ? "Viewer build (playable)" : "Headless-only build (not playable in the viewer)") + ", runtime contract " + std::to_string(r.program.runtime_abi),
          r.program.viewer && r.program.runtime_abi == scd::supported_runtime_abi ? col::text : col::warn);
      if (row("Verified", fmt_date(r.program.verified_at), col::text2, "The program's own statement matches your BIOS and disc; it is not signed or compiled by Segarecomp when imported. Verify checks it and every disc file again.", "Verify", "prog_verify") && !prog_busy) scd_start_import(entry.id(), {}, {}, {});
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
    if (entry.platform_id == "genesis" && !entry.mapper.empty()) detail += ", lock-on cartridge";
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
  if (entry.platform_id == "genesis") {
    // ADR 0291: the audio implementation of the build Play will launch, from its own recorded metadata only (never from the title, the file name or the selection).
    const bool launchable = info.status == GameStatus::ready || info.status == GameStatus::playable_older;
    AudioReport audio = launchable ? info.audio : AudioReport{};
    std::string detail = audio_detail(audio);
    if (!launchable) detail = "There is no playable build of this selection, so no audio implementation is recorded for it.";
    row("Audio implementation", audio_headline(audio), col::text, detail);
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
  const bool can_clean = u != nullptr && u->intermediates > 0 && report_valid_ && !maintenance_active() && !busy_.any_activity();
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


// ---------------------------------------------------------------- Genesis: session recording (SEG-146-T006)

float App::draw_session_recordings(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  const bool can = info.runnable() && !game_ && !maintenance_active();
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + ctx_.u(38.0F)));
  checkbox(ctx_, "session_record", "Record my next play session from power-on when I press Play", &session_record_);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + ctx_.u(2.0F)));
  paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner,
            "A recording is a small text file of your button presses at exact frame boundaries, tied to this exact game image and version. It holds no game data. "
            "While a session is recorded or replayed, saves, save states and rewind are off. End it from the pause menu (Esc > Stop recording) or just quit. "
            "The Record switch is launcher-wide and not saved: it applies to the next game you start from here; the recordings listed are this game's.");
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + ctx_.u(2.0F)));
  checkbox(ctx_, "session_variant", "Replay even if the game version differs from the recording (checkpoints are then not enforced)", &session_allow_variant_);
  const auto recordings = session_recordings(entry);
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
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - pad - ctx_.u(130.0F), p.y));
    if (button(ctx_, "session_replay", "Replay", ImVec2(ctx_.u(120.0F), ctx_.u(28.0F)), Style::secondary, can)) { session_replay_ = recordings[i]; launch(entry.id(), false); }
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

// ---------------------------------------------------------------- Sega CD: input recording (the one section a native game does not have)

float App::draw_scd_recordings(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  const bool busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr || scd_build_ != nullptr;
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + ctx_.u(38.0F)));
  checkbox(ctx_, "scd_record", "Record this session's controller input when I press Play", &scd_record_);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + ctx_.u(2.0F)));
  paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner,
            "A recording is a small text script of button presses at exact frame boundaries; replaying it reproduces the session exactly. It holds no game data. "
            "The viewer's Pause menu is Esc; F6 holds the machine and F9 steps one frame while it is held (F7 is rewind, F5 / F8 quick save / load). "
            "The Record switch is launcher-wide and not saved: it applies to the next game you start from here; the recordings listed are this game's.");
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

// ---------------------------------------------------------------- the page: Overview / Mods / Game settings / Advanced

namespace {

// Closes a card whose content was drawn on channel 1 down to `content_bottom`: frame on channel 0, accent bar, merge, cursor below it. Returns the height.
float close_card(Ctx &ctx, ImDrawList *dl, ImVec2 a, float w, float content_bottom, const char *title, ImU32 accent = col::btn_top) {
  const float h = content_bottom - a.y + ctx.u(10.0F);
  dl->ChannelsSetCurrent(0);
  section_card(ctx, dl, a, ImVec2(a.x + w, a.y + h), title, rgba(9, 22, 54, 245));
  dl->AddRectFilled(ImVec2(a.x + ctx.u(1.0F), a.y + ctx.u(2.0F)), ImVec2(a.x + ctx.u(4.0F), a.y + h - ctx.u(2.0F)), accent, ctx.u(2.0F));
  dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
  ImGui::Dummy(ImVec2(w, 0.0F));
  return h;
}

const char *details_tab_name(DetailsTab tab) { return tab == DetailsTab::mods ? "mods" : tab == DetailsTab::settings ? "settings" : tab == DetailsTab::advanced ? "advanced" : "overview"; }

// One whole-width row that opens another tab: a title, one dim line and a chevron (the Overview's way into Mods, Game settings and Advanced).
bool link_row(Ctx &ctx, const char *id, const char *title, const std::string &sub, float w, std::string *focus_label) {
  const float h = ctx.u(54.0F);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::PushID(id);
  const bool pressed = ui::nav_button("##link", ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
  if (ImGui::IsItemFocused()) { ui::focus_id = id; if (focus_label != nullptr) *focus_label = title; }
  ImGui::PopID();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 b(a.x + w, a.y + h);
  card(ctx, dl, a, b, false, hov, rgba(9, 22, 54, 245));
  draw_text(dl, ctx.bold, ctx.u(13.5F), col::text, ImVec2(a.x + ctx.u(16.0F), a.y + ctx.u(10.0F)), title);
  draw_text(dl, ctx.regular, ctx.u(12.0F), col::dim, ImVec2(a.x + ctx.u(16.0F), a.y + ctx.u(31.0F)), ellipsize(ctx.regular, ctx.u(12.0F), w - ctx.u(60.0F), sub).c_str());
  const ImVec2 c(b.x - ctx.u(20.0F), (a.y + b.y) * 0.5F);
  const float k = ctx.u(5.0F);
  dl->AddLine(ImVec2(c.x - k * 0.5F, c.y - k), ImVec2(c.x + k * 0.5F, c.y), col::text2, std::max(1.0F, ctx.u(1.6F)));
  dl->AddLine(ImVec2(c.x + k * 0.5F, c.y), ImVec2(c.x - k * 0.5F, c.y + k), col::text2, std::max(1.0F, ctx.u(1.6F)));
  if (foc) focus_ring(ctx, dl, a, b);
  ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ctx.u(8.0F)));
  ImGui::Dummy(ImVec2(w, 0.0F));
  return pressed;
}

}  // namespace

void App::set_details_tab(DetailsTab tab) {
  if (tab == details_tab_) return;
  details_tab_scroll_[static_cast<int>(details_tab_)] = details_scroll_now_;   // each tab keeps its own scroll while it is away
  details_tab_ = tab;
  details_scroll_pending_ = details_tab_scroll_[static_cast<int>(tab)];
  trace(std::string("details=tab ") + details_tab_name(tab));
}

void App::draw_details_tabs(const LibraryEntry &entry, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  struct Tab { DetailsTab tab; const char *label; };
  std::vector<Tab> tabs{{DetailsTab::overview, "Overview"}};
  if (details_has_mods(entry)) tabs.push_back({DetailsTab::mods, "Mods"});
  tabs.push_back({DetailsTab::settings, "Game settings"});
  tabs.push_back({DetailsTab::advanced, "Advanced"});
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float th = ctx_.u(36.0F), px = ctx_.u(13.0F);
  float x = origin.x;
  details_tabs_text_.clear();
  for (const Tab &t : tabs) {
    const float tw = text_width(ctx_.bold, px, t.label) + ctx_.u(28.0F);
    // Advanced is the secondary group: it sits at the far right instead of in the row of everyday tabs.
    const float tx = t.tab == DetailsTab::advanced ? origin.x + w - tw : x;
    ImGui::SetCursorScreenPos(ImVec2(tx, origin.y));
    ImGui::PushID(static_cast<int>(t.tab));
    const bool pressed = ui::nav_button("##dtab", ImVec2(tw, th));
    const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    if (ImGui::IsItemFocused()) { ui::focus_id = std::string("tab_") + details_tab_name(t.tab); focus_label_ = std::string("Tab: ") + t.label; }
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool on = details_tab_ == t.tab;
    if (hov && !on) dl->AddRectFilled(a, b, col::panel_hover, ctx_.u(4.0F));
    draw_text(dl, ctx_.bold, px, on ? col::text : (t.tab == DetailsTab::advanced ? col::dim : col::text2), ImVec2(a.x + ctx_.u(14.0F), a.y + (th - px) * 0.5F - ctx_.u(2.0F)), t.label);
    if (on) dl->AddRectFilled(ImVec2(a.x + ctx_.u(6.0F), b.y - ctx_.u(3.0F)), ImVec2(b.x - ctx_.u(6.0F), b.y), col::btn_top, ctx_.u(1.5F));
    if (foc) focus_ring(ctx_, dl, a, b);
    if (pressed) set_details_tab(t.tab);
    details_tabs_text_ += (details_tabs_text_.empty() ? "" : ",") + std::string(details_tab_name(t.tab));
    if (t.tab != DetailsTab::advanced) x += tw + ctx_.u(4.0F);
  }
  dl->AddRectFilled(ImVec2(origin.x, origin.y + th), ImVec2(origin.x + w, origin.y + th + 1.0F), rgba(46, 80, 150, 140));
  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + th + ctx_.u(14.0F)));
  ImGui::Dummy(ImVec2(w, 0.0F));
}

// The build log panel: live while a build runs, on request otherwise (More > View build log, the failure card, Advanced).
void App::draw_build_log(const LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool building = is_building(entry.id());
  const bool sega_cd = entry.platform_id == "sega-cd";
  std::string text;
  if (building) {
    if (SDL_GetTicks() - live_log_at_ > 700U) {
      live_log_at_ = SDL_GetTicks();
      live_log_ = read_tail(build_live_log(entry.id()), 1400);
    }
    text = live_log_.empty() ? "Waiting for the build tool's first message..." : live_log_;
  } else if (const auto d = diagnostics_.find(entry.id()); d != diagnostics_.end()) text = d->second;
  else {
    const fs::path failed = info.entry.parent_path() / (sega_cd ? std::string("build.failed") : info.key + ".failed");
    text = read_tail(info.entry / "build.log", 6000);
    if (text.empty()) text = read_tail(failed / "build.log", 6000);
    const std::string run = read_tail(sega_cd ? scd_dir(entry) / "run.log" : info.entry / "run.log", 2000);
    if (!run.empty()) text += "\n--- last run ---\n" + run;
  }
  if (text.empty()) text = "No log exists for this game's current build.";
  const ImVec2 p = ImGui::GetCursorScreenPos();
  draw_text(dl, ctx_.display, ctx_.u(12.5F), building ? col::warn : rgba(120, 205, 255), p, building ? "BUILD LOG (LIVE)" : "BUILD LOG");
  if (!building) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - ctx_.u(80.0F), p.y - ctx_.u(4.0F)));
    if (button(ctx_, "hide_log", "Hide", ImVec2(ctx_.u(80.0F), ctx_.u(24.0F)), Style::ghost)) show_log_ = false;
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(24.0F)));
  const ImVec2 q = ImGui::GetCursorScreenPos();
  const auto lines = wrap(ctx_.regular, ctx_.u(11.5F), w - ctx_.u(20.0F), text);
  const float lh = ctx_.u(11.5F) + ctx_.u(3.0F);
  const std::size_t max_lines = building ? 10 : 24;
  const std::size_t first = lines.size() > max_lines ? lines.size() - max_lines : 0;
  const float box_h = static_cast<float>(lines.size() - first) * lh + ctx_.u(18.0F);
  dl->AddRectFilled(q, ImVec2(q.x + w, q.y + box_h), rgba(4, 9, 22, 240), ctx_.u(5.0F));
  dl->AddRect(q, ImVec2(q.x + w, q.y + box_h), rgba(34, 70, 140), ctx_.u(5.0F));
  float ly = q.y + ctx_.u(9.0F);
  for (std::size_t i = first; i < lines.size(); ++i) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::text2, ImVec2(q.x + ctx_.u(10.0F), ly), lines[i].c_str()); ly += lh; }
  ImGui::Dummy(ImVec2(w, box_h + ctx_.u(12.0F)));
  if (building) { paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, w, "The build reports stages, not a percentage. Cancel stops it and discards its partial files; closing the launcher waits for it."); ImGui::Dummy(ImVec2(1, ctx_.u(10.0F))); }
}

// Overview: what is wrong (if anything) with the next step, and the way into the other tabs. The state, the one primary action and the sentence of what happens next are in the header above.
void App::draw_details_overview(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool building = is_building(entry.id()), sega_cd = entry.platform_id == "sega-cd";
  const auto banner = [&](ImU32 color, const char *icon, const std::string &text) {
    const auto lines = wrap(ctx_.regular, ctx_.u(12.5F), w - ctx_.u(50.0F), text);
    const float bh2 = static_cast<float>(lines.size()) * (ctx_.u(12.5F) + ctx_.u(3.0F)) + ctx_.u(18.0F);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + bh2), (color & 0x00FFFFFF) | 0x26000000, ctx_.u(5.0F));
    dl->AddRect(a, ImVec2(a.x + w, a.y + bh2), (color & 0x00FFFFFF) | 0xAA000000, ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
    icon_image(dl, ctx_.icon(icon), ImVec2(a.x + ctx_.u(12.0F), a.y + ctx_.u(9.0F)), ctx_.u(16.0F), color);
    float ly = a.y + ctx_.u(9.0F);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text, ImVec2(a.x + ctx_.u(38.0F), ly), l.c_str()); ly += ctx_.u(12.5F) + ctx_.u(3.0F); }
    ImGui::Dummy(ImVec2(w, bh2 + ctx_.u(8.0F)));
  };
  // notices of this session (restart, failure) come first
  if (const auto notice = notices_.find(entry.id()); notice != notices_.end() && !notice->second.empty()) banner(col::err, "warning", notice->second);
  const auto failure = failures_.find(entry.id());
  if (failure != failures_.end() && !building) {
    banner(col::err, "warning", "Last attempt failed: " + failure->second);
    if (button(ctx_, "dismiss_fail", "Dismiss", ImVec2(ctx_.u(100.0F), ctx_.u(26.0F)), Style::ghost)) failures_.erase(failure);
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  if (!building && info.rom_missing && sega_cd)
    banner(col::warn, "warning", scd::message(info.scd.refusal, info.scd.detail) + " Playing is unavailable until it is located again; the game's library data, saves and recordings are unchanged.");
  if (!building && info.rom_missing && !sega_cd && (info.runnable() || entry.rom_path_unknown)) {   // without a playable version the header sentence already says it
    banner(col::warn, "warning", entry.rom_path_unknown ? "Game file missing: its location was never recorded (this entry came from an older cache record). Find game file enables preparing again; playing an existing version still works."
                                                         : "Game file missing: it is not at its recorded location. Playing an existing version still works; Find game file re-links it (the SHA-256 must match).");
    if (info.runnable() && button(ctx_, "locate_banner", "Find game file", ImVec2(ctx_.u(160.0F), ctx_.u(28.0F)), Style::secondary, true, ctx_.icon("folder"))) browse_locate(entry.id());
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  // a failed preparation: what happened, what can be done, where the log is (never buried under the options)
  if (info.status == GameStatus::failed && !building) {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    // the two follow-ups sit in the card's title row so they stay in view whatever the window height
    const float bw1 = ctx_.u(96.0F), bw2 = ctx_.u(132.0F), bh = ctx_.u(26.0F);
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - pad - bw1 - bw2 - ctx_.u(8.0F), a.y + ctx_.u(6.0F)));
    if (button(ctx_, "view_log", show_log_ ? "Hide log" : "View log", ImVec2(bw1, bh), Style::secondary, true)) show_log_ = !show_log_;
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "check_options", "Game settings", ImVec2(bw2, bh), Style::ghost)) set_details_tab(DetailsTab::settings);
    float y = a.y + ctx_.u(40.0F);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    std::string reason = failure != failures_.end() ? failure->second : std::string("The last preparation did not produce a usable program.");
    if (failure == failures_.end()) {   // the product's own last message, from the sanitized log kept with the failed attempt
      const fs::path failed_dir = info.entry.parent_path() / (sega_cd ? std::string("build.failed") : info.key + ".failed");
      std::string tail = read_tail(failed_dir / "build.log", 700);
      while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r' || tail.back() == ' ')) tail.pop_back();
      const std::size_t cut = tail.find_last_of('\n');
      const std::string last = cut == std::string::npos ? tail : tail.substr(cut + 1);
      if (!last.empty()) reason += " Last message: " + ellipsize(ctx_.regular, ctx_.u(12.5F), inner * 1.6F, last);
    }
    y += paragraph(ctx_, ctx_.bold, ctx_.u(12.5F), col::err, inner, reason) + ctx_.u(6.0F);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner,
                   info.older.playable ? "An earlier version still works: Play uses it. Your original game file is unchanged. Retry prepares it again with the same options."
                                       : "Your original game file is unchanged. Retry prepares the game again with the same options. If it keeps failing, check Game settings or try a newer release; the log is for bug reports.") + ctx_.u(2.0F);
    const float ch = close_card(ctx_, dl, a, w, y, "WHAT HAPPENED", col::err);
    note_layout("failure", a, ImVec2(a.x + w, a.y + ch));
    ImGui::Dummy(ImVec2(1, ctx_.u(12.0F)));
  }
  // the way into the other tabs
  if (details_has_mods(entry)) {
    const InstalledPatch *sel = entry.selected_patch();
    const std::string unavailable = mods_unavailable_reason(entry.platform_id);
    const std::string sub = !unavailable.empty() ? std::string("Not available for this platform: mods are for Genesis games only.")
                            : entry.patches.empty() ? std::string("No mod installed  -  original game selected. Install one from a .segapatch package.")
                                                    : std::to_string(entry.patches.size()) + (entry.patches.size() == 1 ? " mod installed" : " mods installed") + "  -  " + (sel == nullptr ? "original game selected" : "mod selected: " + sel->name);
    if (link_row(ctx_, "ov_mods", "Mods", sub, w, &focus_label_)) set_details_tab(DetailsTab::mods);
  }
  {
    std::string sub = selected_mode_label(entry) + " mode";
    if (entry.platform_id == "genesis") sub += "  -  region " + std::string(region_label(entry.platform_id, entry.region));
    if (entry.platform_id == "master-system") sub += "  -  mapper " + (entry.mapper.empty() ? (entry.mapper_manifest_sha256.empty() ? std::string("not chosen") : std::string("from manifest")) : (entry.mapper == "sega" ? std::string("Sega") : std::string("ROM only")));
    sub += "  -  favorite, cover, display, input recording";
    if (link_row(ctx_, "ov_settings", "Game settings", sub, w, &focus_label_)) set_details_tab(DetailsTab::settings);
  }
  if (link_row(ctx_, "ov_advanced", "Advanced details", "Build identity, older versions, log, game file location, storage, diagnostics", w, &focus_label_)) set_details_tab(DetailsTab::advanced);
  if (building || show_log_) draw_build_log(entry, info, w);
}

// Game settings: the library entry (per game), display and audio (where they actually live) and one card per per-game option group.
float App::draw_library_entry_card(LibraryEntry &entry, const StatusInfo &info, float w) {
  (void)info;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  float y = a.y + ctx_.u(40.0F);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
  y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner, "These apply to this game's entry only. The game file itself is never changed or deleted.") + ctx_.u(4.0F);
  const float bh = ctx_.u(32.0F);
  float x = a.x + pad;
  const auto place = [&](const char *label) {
    const float bw = text_width(ctx_.bold, ctx_.u(13.0F), label) + ctx_.u(56.0F);
    if (x > a.x + pad && x + bw > a.x + pad + inner) { x = a.x + pad; y += bh + ctx_.u(8.0F); }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    x += bw + ctx_.u(8.0F);
    return ImVec2(bw, bh);
  };
  const char *fav_label = entry.favorite ? "Remove from favorites" : "Add to favorites";
  if (button(ctx_, "gs_fav", fav_label, place(fav_label), Style::secondary, true, ctx_.icon("star"))) { entry.favorite = !entry.favorite; save_catalog(); }
  if (button(ctx_, "gs_cover", "Change cover...", place("Change cover..."), Style::secondary, true, ctx_.icon("edit"))) browse_cover(entry.id());
  if (ImGui::IsItemHovered()) tooltip(ctx_, "Choose a local PNG or JPEG");
  if (!entry.cover.empty() && button(ctx_, "gs_cover_rm", "Remove cover", place("Remove cover"), Style::ghost, true, ctx_.icon("close"))) remove_cover(entry);
  if (button(ctx_, "gs_remove", "Remove from library...", place("Remove from library..."), Style::danger, !game_ || game_id_ != entry.id(), ctx_.icon("trash"))) ask_remove(entry);
  if (ImGui::IsItemHovered()) tooltip(ctx_, entry.platform_id == "sega-cd" ? "BIOS, disc files and saves stay" : "The game file and extracted copies stay");
  y += bh + ctx_.u(6.0F);
  const float h = close_card(ctx_, dl, a, w, y, "LIBRARY ENTRY - THIS GAME");
  note_layout("entry", a, ImVec2(a.x + w, a.y + h));
  return h;
}

float App::draw_display_audio_card(const LibraryEntry &entry, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  float y = a.y + ctx_.u(38.0F);
  const auto block = [&](const char *head, const std::string &body) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(a.x + pad, y), head);
    y += ctx_.u(20.0F);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    y += paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, inner, body) + ctx_.u(6.0F);
  };
  block("Display", "Widescreen mode, side presentation and width are the game's own Video & Display settings (the pause menu while it runs, Esc); the defaults for games without their own setting are under Settings > Appearance & Controls. "
                   "Support depends on the platform and the game, and an unsupported mode shows the original picture.");
  block("Audio", entry.platform_id == "sega-cd" ? "Volume and mute are shared by every game (Settings > Appearance & Controls). The program's audio is described under Console audio above."
                                                : "Volume and mute are shared by every game (Settings > Appearance & Controls). The audio implementation of this game's prepared version is listed under Advanced.");
  const float h = close_card(ctx_, dl, a, w, y, "DISPLAY AND AUDIO - SHARED SETTINGS", col::info);
  note_layout("display", a, ImVec2(a.x + w, a.y + h));
  return h;
}

void App::draw_details(float w, float h) {
  LibraryEntry *entry = catalog_.find_id(detail_id_);
  if (entry == nullptr) { page_ = Page::library; return; }
  const StatusInfo info = status_of(detail_id_);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  details_layout_.clear();
  details_origin_ = ImGui::GetCursorScreenPos();
  details_scroll_now_ = ImGui::GetScrollY();
  if (details_scroll_pending_ >= 0.0F) { ImGui::SetScrollY(details_scroll_pending_); details_scroll_pending_ = -1.0F; }   // a switched-to tab returns to where it was left
  if (details_tab_ == DetailsTab::mods && !details_has_mods(*entry)) details_tab_ = DetailsTab::overview;
  {  // breadcrumb: Back returns to the exact library state it came from
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const Page back_to = previous_page_ == Page::details ? Page::library : previous_page_;
    const char *back_label = back_to == Page::import_page ? "Add game" : (back_to == Page::storage ? "Storage" : (back_to == Page::settings ? "Settings" : "Library"));
    const float bw = text_width(ctx_.bold, ctx_.u(13.0F), back_label) + ctx_.u(46.0F);
    if (button(ctx_, "back", back_label, ImVec2(bw, ctx_.u(30.0F)), Style::ghost, true, ctx_.icon("back"))) { close_details(); return; }
    if (ImGui::IsItemFocused()) focus_label_ = "Back";
    const float cx = p.x + bw + ctx_.u(12.0F);
    draw_text(dl, ctx_.display, ctx_.u(12.5F), rgba(120, 205, 255), ImVec2(cx, p.y + ctx_.u(9.0F)), "GAME");
    const float hw = text_width(ctx_.display, ctx_.u(12.5F), "GAME");
    const std::string crumb = ellipsize(ctx_.regular, ctx_.u(12.0F), w - (cx - p.x) - hw - ctx_.u(30.0F), "/  " + entry->title());
    draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::dim, ImVec2(cx + hw + ctx_.u(12.0F), p.y + ctx_.u(9.0F)), crumb.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(42.0F)));
  }
  const float cover_w = std::clamp(w * 0.2F, ctx_.u(88.0F), ctx_.u(124.0F));
  draw_details_identity(*entry, info, w, cover_w);
  draw_details_tabs(*entry, w);
  switch (details_tab_) {
    case DetailsTab::overview: draw_details_overview(*entry, info, w); break;
    case DetailsTab::mods: draw_variants(*entry, info, w); break;   // SEG-150-T006: the per-game Mods card
    case DetailsTab::settings: {
      draw_build_config(*entry, info, w);
      ImGui::Dummy(ImVec2(1, ctx_.u(12.0F)));
      if (entry->platform_id == "sega-cd") { draw_scd_recordings(*entry, info, w); ImGui::Dummy(ImVec2(1, ctx_.u(12.0F))); }
      else if (entry->platform_id == "genesis") { draw_session_recordings(*entry, info, w); ImGui::Dummy(ImVec2(1, ctx_.u(12.0F))); }   // SEG-146-T006: the shared session recorder (Master System is not wired yet)
      draw_display_audio_card(*entry, w);
      ImGui::Dummy(ImVec2(1, ctx_.u(12.0F)));
      draw_library_entry_card(*entry, info, w);
      break;
    }
    case DetailsTab::advanced: {
      const bool rail = w > ctx_.u(700.0F);
      const float gap = ctx_.u(16.0F), rail_w = rail ? std::clamp(w * 0.29F, ctx_.u(220.0F), ctx_.u(262.0F)) : 0.0F;
      const float main_w = rail ? w - rail_w - gap : w;
      const ImVec2 start = ImGui::GetCursorScreenPos();
      float rail_bottom = start.y;
      if (rail) rail_bottom = start.y + draw_storage_rail(*entry, info, ImVec2(start.x + main_w + gap, start.y), rail_w);
      ImGui::SetCursorScreenPos(start);
      if (is_building(entry->id()) || show_log_) draw_build_log(*entry, info, main_w);
      else {
        if (button(ctx_, "view_log", "View build log", ImVec2(ctx_.u(170.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("file"))) show_log_ = true;
        ImGui::Dummy(ImVec2(1, ctx_.u(10.0F)));
      }
      draw_provenance(*entry, info, main_w);
      if (!rail) {   // narrow window: the storage card follows the provenance
        ImGui::Dummy(ImVec2(1, ctx_.u(12.0F)));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float sh = draw_storage_rail(*entry, info, p, main_w);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sh));
        ImGui::Dummy(ImVec2(main_w, 0.0F));
      }
      ImGui::SetCursorScreenPos(ImVec2(start.x, std::max(rail_bottom, ImGui::GetCursorScreenPos().y)));
      break;
    }
  }
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

} // namespace launcher
