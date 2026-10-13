// Game details, import, storage and settings screens.

#include "app.hpp"

#include "app_helpers.hpp"
#include "covers.hpp"
#include "pixel_assets.hpp"

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
  return "not played yet";
}

void spinner(Ctx &ctx, ImDrawList *dl, ImVec2 c, float r, ImU32 color) {
  const float t = static_cast<float>(SDL_GetTicks()) / 1000.0F;
  dl->PathArcTo(c, r, t * 6.0F, t * 6.0F + 4.2F, 16);
  dl->PathStroke(color, 0, std::max(1.0F, ctx.u(2.0F)));
}

void stage_row(Ctx &ctx, ImDrawList *dl, ImVec2 p, const char *label, StageState state) {
  const ImVec2 c(p.x + ctx.u(8.0F), p.y + ctx.u(9.0F));
  const float r = ctx.u(7.0F);
  if (state == StageState::done) {
    dl->PathLineTo(ImVec2(c.x - r * 0.7F, c.y)); dl->PathLineTo(ImVec2(c.x - r * 0.1F, c.y + r * 0.6F)); dl->PathLineTo(ImVec2(c.x + r * 0.85F, c.y - r * 0.55F));
    dl->PathStroke(col::ok, 0, ctx.u(2.0F));
  } else if (state == StageState::running) spinner(ctx, dl, c, r, col::info);
  else dl->AddCircle(c, r * 0.62F, col::dim, 16, ctx.u(1.3F));
  draw_text(dl, ctx.regular, ctx.u(13.0F), state == StageState::pending ? col::dim : col::text, ImVec2(p.x + ctx.u(26.0F), p.y), label);
}

} // namespace

// ---------------------------------------------------------------- details

void App::draw_build_options(LibraryEntry &entry, const StatusInfo &info, float w) {
  heading(ctx_, "Build options");
  const bool busy_now = job_ != nullptr || game_ != nullptr;
  bool changed = false;
  const float lw = ctx_.u(86.0F);
  if (entry.platform_id == "master-system") {
    ImGui::BeginDisabled(busy_now);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw_text(ImGui::GetWindowDrawList(), ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(p.x, p.y + ctx_.u(8.0F)), "Mapper");
    ImGui::SetCursorScreenPos(ImVec2(p.x + lw, p.y));
    if (!entry.mapper_manifest_sha256.empty() && entry.mapper.empty()) {
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w - lw, "Declared by the sidecar manifest next to the ROM file.");
    } else {
      int sel = entry.mapper == "sega" ? 1 : (entry.mapper == "rom_only" ? 2 : 0);
      if (segmented(ctx_, "mapper", {"Undeclared", "Sega mapper", "ROM only (32 KiB)"}, &sel, 0.0F)) { entry.mapper = sel == 1 ? "sega" : (sel == 2 ? "rom_only" : ""); changed = true; }
      if (entry.mapper.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::warn, w, "The header never identifies the cartridge mapper, so it is not guessed. Declare it to build.");
    }
    ImGui::EndDisabled();
  }
  const RomView probe = view_for(entry);
  if (probe.supports_optimized()) {
    ImGui::BeginDisabled(busy_now);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw_text(ImGui::GetWindowDrawList(), ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(p.x, p.y + ctx_.u(8.0F)), "Mode");
    ImGui::SetCursorScreenPos(ImVec2(p.x + lw, p.y));
    int sel = entry.aot_policy == "optimized" ? 1 : 0;
    if (segmented(ctx_, "mode", {"Compatibility", "Optimized"}, &sel, 0.0F)) { entry.aot_policy = sel == 1 ? "optimized" : "compatibility"; if (sel == 0) entry.aot_model = "v1"; changed = true; }
    if (entry.aot_policy == "optimized" && entry.platform_id == "genesis") {
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      int model = entry.aot_model == "gen2" ? 1 : 0;
      if (segmented(ctx_, "model", {"Standard", "Experimental model"}, &model, 0.0F)) { entry.aot_model = model == 1 ? "gen2" : "v1"; changed = true; }
    }
    ImGui::EndDisabled();
    const char *note = entry.platform_id == "master-system"
                           ? (entry.aot_policy == "optimized" ? "Experimental CNN compilation. May restart in Compatibility if omitted code is reached." : "Compatibility builds the broad, always-correct program.")
                           : (entry.aot_policy == "optimized" ? (entry.aot_model == "gen2" ? "Experimental model on (advanced); may fall back to Compatibility." : "Selective compilation of the code the product admits; falls back to Compatibility when unsure.") : "Compatibility builds the broad, always-correct program.");
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + lw, ImGui::GetCursorScreenPos().y));
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w - lw, note);
  }
  if (probe.supports_region()) {
    ImGui::BeginDisabled(game_ != nullptr);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw_text(ImGui::GetWindowDrawList(), ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(p.x, p.y + ctx_.u(8.0F)), "Region");
    ImGui::SetCursorScreenPos(ImVec2(p.x + lw, p.y));
    int sel = entry.region == "japan" ? 1 : (entry.region == "europe" ? 2 : 0);
    if (segmented(ctx_, "region", {"USA NTSC", "Japan NTSC", "Europe PAL"}, &sel, 0.0F)) { entry.region = sel == 1 ? "japan" : (sel == 2 ? "europe" : "usa"); changed = true; }
    ImGui::EndDisabled();
    std::vector<std::string> notes;
    if (entry.region == "europe") notes.push_back(region_note("europe"));
    if (info.status == GameStatus::ready) if (const std::string c = region_coverage_note(info.observed_region, entry.region); !c.empty()) notes.push_back(c);
    notes.push_back("A run-time choice: the same build serves every region.");
    for (const auto &n : notes) { ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + lw, ImGui::GetCursorScreenPos().y)); paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w - lw, n); }
  }
  if (probe.supports_hint_alias_discovery()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    bool on = entry.hint_alias_discovery;
    ImGui::BeginDisabled(busy_now);
    if (checkbox(ctx_, "hint", "H-interrupt alias discovery (strict build pass)", &on)) { entry.hint_alias_discovery = on; changed = true; }
    ImGui::EndDisabled();
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Advanced, default off: for games whose raster/H-interrupt effects fail otherwise. Builds may take longer.");
  }
  if (entry.aot_policy == "optimized" && rom_policy_identity_cached(layout_, probe).empty() && !status_pending_)
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::err, w, "Optimized cannot verify the build tool identity: saved Optimized builds are not reused.");
  if (changed) { save_catalog(); request_status_refresh(); }
}

void App::draw_details(float w, float h) {
  LibraryEntry *entry = catalog_.find_id(detail_id_);
  if (entry == nullptr) { page_ = Page::library; return; }
  const StatusInfo info = status_of(detail_id_);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool is_building = job_ && job_id_ == detail_id_, is_running = game_ && game_id_ == detail_id_;
  if (button(ctx_, "back", "Back", ImVec2(ctx_.u(96.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("back"))) { page_ = previous_page_ == Page::details ? Page::library : previous_page_; return; }
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  const bool two_col = w > ctx_.u(680.0F);
  const float left_w = two_col ? ctx_.u(210.0F) : std::min(w, ctx_.u(210.0F)), gap = ctx_.u(18.0F);
  const float rw = two_col ? w - left_w - gap : w;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  // ---- left: cover ----
  {
    const float ch = left_w * 4.0F / 3.0F;
    const ImVec2 a(top.x, top.y), b(top.x + left_w, top.y + ch);
    if (SDL_Texture *cover = cover_texture(*entry)) {
      float tw = 0, th = 0;
      SDL_GetTextureSize(cover, &tw, &th);
      const float fr = left_w / ch, tr = tw / th;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (tr > fr) { const float k = fr / tr; uv0.x = (1 - k) * 0.5F; uv1.x = uv0.x + k; } else { const float k = tr / fr; uv0.y = (1 - k) * 0.5F; uv1.y = uv0.y + k; }
      dl->AddImage(reinterpret_cast<ImTextureID>(cover), a, b, uv0, uv1);
    }
    dl->AddRect(a, b, col::edge, 0, 0, std::max(1.0F, ctx_.u(2.0F)));
    ImGui::SetCursorScreenPos(ImVec2(top.x, b.y + ctx_.u(8.0F)));
    if (button(ctx_, "cover_change", "Change cover", ImVec2(left_w, ctx_.u(28.0F)), Style::secondary, true, ctx_.icon("edit"))) browse_cover(entry->id());
    if (!entry->cover.empty()) {
      ImGui::SetCursorScreenPos(ImVec2(top.x, ImGui::GetCursorScreenPos().y + ctx_.u(4.0F)));
      if (button(ctx_, "cover_remove", "Remove cover", ImVec2(left_w, ctx_.u(28.0F)), Style::secondary, true, ctx_.icon("trash"))) {
        remove_cover_file(covers_dir_, entry->cover);
        if (auto it = cover_cache_.find(entry->cover); it != cover_cache_.end()) { if (it->second) SDL_DestroyTexture(it->second); cover_cache_.erase(it); }
        entry->cover.clear();
        save_catalog();
        toast("Cover removed; the generic placeholder is shown.");
      }
    }
    {
      const auto names = libretro_candidate_names(entry->title(), settings_.preferred_regions);
      const std::string url = names.empty() ? std::string() : libretro_candidate_url(entry->platform_id, names.front());
      ImGui::SetCursorScreenPos(ImVec2(top.x, ImGui::GetCursorScreenPos().y + ctx_.u(4.0F)));
      if (button(ctx_, "cover_web", "Find cover online", ImVec2(left_w, ctx_.u(28.0F)), Style::ghost, !url.empty(), ctx_.icon("search"))) SDL_OpenURL(url.c_str());
      ImGui::SetCursorScreenPos(ImVec2(top.x, ImGui::GetCursorScreenPos().y + ctx_.u(4.0F)));
      paragraph(ctx_, ctx_.regular, ctx_.u(10.0F), col::dim, left_w, "Opens your browser at a Libretro thumbnail NAME GUESS (it may not exist). Save an image and use Change cover. Nothing is downloaded by Segarecomp.");
    }
  }
  const float left_bottom = ImGui::GetCursorScreenPos().y;
  // ---- right ----
  if (two_col) { ImGui::Indent(left_w + gap); ImGui::SetCursorScreenPos(ImVec2(top.x + left_w + gap, top.y)); }
  else ImGui::SetCursorScreenPos(ImVec2(top.x, left_bottom + ctx_.u(10.0F)));
  const ImVec2 rx = ImGui::GetCursorScreenPos();
  // title row
  {
    const float bsz = ctx_.u(28.0F);
    if (editing_title_) {
      bool active = false;
      if (input_text(ctx_, "title", title_edit_, sizeof title_edit_, rw - bsz * 2 - ctx_.u(16.0F), nullptr, &active) || false) {}
      ImGui::SameLine(0.0F, ctx_.u(6.0F));
      if (icon_button(ctx_, "title_ok", ctx_.icon("check"), bsz, true, true, "Save name") || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        std::string name = title_edit_;
        const std::string trimmed = clean_title(name);
        entry->display_title = trimmed == entry->detected_title ? std::string() : trimmed;
        editing_title_ = false;
        save_catalog();
      }
      ImGui::SameLine(0.0F, ctx_.u(4.0F));
      if (icon_button(ctx_, "title_cancel", ctx_.icon("close"), bsz, false, true, "Cancel")) editing_title_ = false;
    } else {
      const float tpx = ctx_.u(22.0F);
      const std::string shown = ellipsize(ctx_.bold, tpx, rw - bsz * 2 - ctx_.u(14.0F), entry->title());
      draw_text(dl, ctx_.bold, tpx, col::text, ImVec2(rx.x, rx.y + ctx_.u(2.0F)), shown.c_str());
      const float tw = text_width(ctx_.bold, tpx, shown.c_str());
      ImGui::SetCursorScreenPos(ImVec2(rx.x + tw + ctx_.u(10.0F), rx.y));
      if (icon_button(ctx_, "title_edit", ctx_.icon("edit"), bsz, false, true, "Rename (display name only)")) { std::snprintf(title_edit_, sizeof title_edit_, "%s", entry->title().c_str()); editing_title_ = true; }
      ImGui::SameLine(0.0F, ctx_.u(4.0F));
      if (icon_button(ctx_, "fav", ctx_.icon("star"), bsz, entry->favorite, true, entry->favorite ? "Remove from favorites" : "Add to favorites")) { entry->favorite = !entry->favorite; save_catalog(); }
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(rx.x, rx.y + ctx_.u(40.0F)));
  // badges
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float bpx = ctx_.u(11.0F);
    ImVec2 size;
    badge(ctx_, dl, p, platform_short(entry->platform_id), col::info, bpx, &size);
    p.x += size.x + ctx_.u(8.0F);
    badge(ctx_, dl, p, is_building ? "Building" : (is_running ? "Playing" : status_label(info).c_str()), is_building ? col::warn : (is_running ? col::ok : status_color(info)), bpx, &size);
    p.x += size.x + ctx_.u(8.0F);
    if (info.rom_missing) { badge(ctx_, dl, p, entry->rom_path_unknown ? "ROM path not recorded" : "ROM file missing", col::warn, bpx, &size); p.x += size.x + ctx_.u(8.0F); }
    draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::text2, ImVec2(p.x, p.y + ctx_.u(5.0F)), info.compat_known ? "Experimental compatibility" : "Compatibility unknown");
    ImGui::SetCursorScreenPos(ImVec2(rx.x, p.y + ctx_.u(30.0F)));
  }
  // actions
  {
    const float bh = ctx_.u(36.0F);
    const bool can_play = info.status == GameStatus::ready && !game_ && !job_ && !maintenance_running_;
    if (button(ctx_, "play", "Play", ImVec2(ctx_.u(120.0F), bh), Style::primary, can_play, ctx_.icon("play"))) { restarted_this_play_ = false; notices_.erase(entry->id()); show_log_ = false; launch(entry->id(), false); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    const char *build_label = info.status == GameStatus::ready ? "Recompile" : (info.status == GameStatus::metadata_only ? "Rebuild" : (info.status == GameStatus::failed ? "Try again" : (info.status == GameStatus::stale ? "Recompile" : "Build")));
    const bool mapper_needed = entry->platform_id == "master-system" && entry->mapper.empty() && entry->mapper_manifest_sha256.empty();
    const bool can_build = !job_ && !game_ && !maintenance_running_ && !info.rom_missing && !mapper_needed;
    if (button(ctx_, "build", build_label, ImVec2(ctx_.u(130.0F), bh), info.status == GameStatus::ready ? Style::secondary : Style::primary, can_build, ctx_.icon("reset"))) { notices_.erase(entry->id()); start_build(entry->id()); }
    if (info.rom_missing) {
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      if (button(ctx_, "locate", "Locate ROM...", ImVec2(ctx_.u(150.0F), bh), Style::secondary, true, ctx_.icon("folder"))) browse_locate(entry->id());
    }
    ImGui::SetCursorScreenPos(ImVec2(rx.x, ImGui::GetCursorScreenPos().y + ctx_.u(6.0F)));
    const float sh = ctx_.u(28.0F);
    std::error_code ec;
    const bool has_dir = fs::is_directory(info.entry, ec);
    float fx = 0.0F, fy = ImGui::GetCursorScreenPos().y;
    const auto flow_button = [&](const char *id, const char *label, float width, bool enabled, const char *icon) {
      if (fx > 0.0F && fx + width > rw) { fx = 0.0F; fy += sh + ctx_.u(6.0F); }
      ImGui::SetCursorScreenPos(ImVec2(rx.x + fx, fy));
      const bool pressed = button(ctx_, id, label, ImVec2(width, sh), Style::ghost, enabled, ctx_.icon(icon));
      fx += width + ctx_.u(6.0F);
      return pressed;
    };
    if (flow_button("open_dir", "Open build folder", ctx_.u(172.0F), has_dir, "folder")) SDL_OpenURL(("file://" + u8s(info.entry)).c_str());
    if (flow_button("logs", show_log_ ? "Hide logs" : "View logs", ctx_.u(122.0F), true, "file")) show_log_ = !show_log_;
    if (flow_button("cleanup", "Storage cleanup", ctx_.u(172.0F), true, "storage")) { storage_selected_ = {entry->id()}; go(Page::storage); }
    if (flow_button("remove", "Remove from library", ctx_.u(196.0F), true, "trash")) {
      modal_ = Modal{};
      modal_.kind = Modal::Kind::remove_entries;
      modal_.ids = {entry->id()};
      modal_.title = "Remove \"" + ellipsize(ctx_.bold, ctx_.u(15.0F), ctx_.u(300.0F), entry->title()) + "\" from the library?";
    }
    ImGui::SetCursorScreenPos(ImVec2(rx.x, fy + sh + ctx_.u(6.0F)));
    ImGui::Dummy(ImVec2(rw, 1));
    ImGui::SetCursorScreenPos(ImVec2(rx.x, ImGui::GetCursorScreenPos().y + ctx_.u(8.0F)));
  }
  // state notes
  if (is_building) {
    heading(ctx_, "Building", col::warn);
    static const char *names[BuildJob::stage_count] = {"Analyzing ROM", "Generating native C", "Compiling", "Linking"};
    for (int i = 0; i < BuildJob::stage_count; ++i) { stage_row(ctx_, dl, ImGui::GetCursorScreenPos(), names[i], job_->stage(i)); ImGui::Dummy(ImVec2(rw, ctx_.u(23.0F))); }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, rw, "Large games can take several minutes the first time. The build cannot be cancelled safely; closing the launcher waits for it.");
    if (SDL_GetTicks() - live_log_at_ > 700U) {
      live_log_at_ = SDL_GetTicks();
      live_log_ = read_tail(fs::path(job_->entry()).parent_path() / (u8s(job_->entry().filename()) + ".partial") / "build.log", 700);
    }
    if (!live_log_.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(10.0F), col::dim, rw, live_log_, 2.0F);
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  } else if (is_running) {
    paragraph(ctx_, ctx_.bold, ctx_.u(15.0F), col::ok, rw, running_label(game_report_, restarted_this_play_));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, rw, "Close the game window (or use Return to Launcher in its menu) to come back.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  if (const auto notice = notices_.find(entry->id()); notice != notices_.end() && !notice->second.empty()) { paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, rw, notice->second); ImGui::Dummy(ImVec2(1, ctx_.u(6.0F))); }
  if (const auto failure = failures_.find(entry->id()); failure != failures_.end() && !is_building) {
    heading(ctx_, "Last attempt failed", col::err);
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, rw, failure->second);
    if (button(ctx_, "dismiss_fail", "Dismiss", ImVec2(ctx_.u(100.0F), ctx_.u(26.0F)), Style::ghost)) { failures_.erase(failure); }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  if (!is_building) {
    if (info.status == GameStatus::metadata_only) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::info, rw, "Metadata only: rebuild required. This game's build was removed on purpose; its library entry, cover and history are kept. Recompile to play it again.");
    else if (info.status == GameStatus::stale) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, rw, "Stale build: the stored program came from another release or option set, so it is never started. Recompile to use this release.");
    else if (info.status == GameStatus::failed && failures_.count(entry->id()) == 0) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, rw, "The last build failed. Open View logs for the sanitized diagnostics, then Try again.");
    else if (info.status == GameStatus::not_built) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, rw, "Not built yet. Adding a game to the library does not build it; Build compiles it with the bundled toolchain.");
    if (info.rom_missing) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, rw, entry->rom_path_unknown ? "The original ROM location was never recorded (this entry came from an older cache record). Locate ROM to enable rebuilding; playing the existing build still works." : "The original ROM is not at its recorded location. Playing an existing build still works; Locate ROM re-links it (the SHA-256 must match).");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  // policy summary of a ready build
  if (info.status == GameStatus::ready && info.policy.present) {
    const std::string summary = policy_summary(info.policy);
    if (!summary.empty()) {
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), info.policy.fallback ? col::err : col::text2, rw, summary);
      if (info.policy.requested == "optimized") {
        if (button(ctx_, "why", show_fallback_details_ ? "Hide details" : (info.policy.fallback ? "Why?" : "Details"), ImVec2(ctx_.u(110.0F), ctx_.u(24.0F)), Style::ghost)) show_fallback_details_ = !show_fallback_details_;
        if (show_fallback_details_) paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::text2, rw, (info.policy.fallback ? "Optimized was requested but this game was built as Compatibility (the broad, always-correct program).\n" : std::string()) + policy_details(info.policy));
      }
      ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    }
  }
  rule(ctx_, rw);
  // metadata
  {
    const float lw = ctx_.u(122.0F), vw = rw - lw;
    heading(ctx_, "Details");
    label_value(ctx_, lw, vw, "Platform", entry->platform_id == "master-system" ? "Master System (profile " + std::string(sms_profile) + ")" : "Genesis / Mega Drive");
    if (entry->platform_id == "genesis") label_value(ctx_, lw, vw, "Region", entry->region == "japan" ? "Japan NTSC" : (entry->region == "europe" ? "Europe PAL (runs at 60 Hz)" : "USA NTSC"));
    else label_value(ctx_, lw, vw, "Mapper", entry->mapper.empty() ? (entry->mapper_manifest_sha256.empty() ? "undeclared" : "declared by sidecar manifest") : (entry->mapper == "sega" ? "Sega" : "ROM only (32 KiB)"));
    {
      const std::string digest = entry->sha256.substr(0, 16) + "..." + entry->sha256.substr(56);
      const ImVec2 row = ImGui::GetCursorScreenPos();
      label_value(ctx_, lw, vw - ctx_.u(70.0F), "ROM SHA-256", digest);
      ImGui::SetCursorScreenPos(ImVec2(row.x + lw + text_width(ctx_.regular, ctx_.u(12.0F), digest.c_str()) + ctx_.u(12.0F), row.y - ctx_.u(4.0F)));
      if (button(ctx_, "copy_sha", "Copy", ImVec2(ctx_.u(60.0F), ctx_.u(22.0F)), Style::ghost)) { SDL_SetClipboardText(entry->sha256.c_str()); toast("SHA-256 copied."); }
      ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + ctx_.u(26.0F)));
      ImGui::Dummy(ImVec2(1, ctx_.u(2.0F)));
    }
    if (entry->rom_paths.empty()) label_value(ctx_, lw, vw, "ROM file", entry->rom_path_unknown ? "not recorded (older cache record)" : "none", col::warn);
    for (std::size_t i = 0; i < entry->rom_paths.size(); ++i) {
      std::error_code ec;
      const bool exists = fs::is_regular_file(fs::path(std::u8string(reinterpret_cast<const char8_t *>(entry->rom_paths[i].c_str()))), ec);
      label_value(ctx_, lw, vw, i == 0 ? "ROM file" : "", entry->rom_paths[i] + (exists ? "" : "  (missing)"), exists ? col::text : col::warn);
    }
    label_value(ctx_, lw, vw, "Added", fmt_date(entry->added_at));
    label_value(ctx_, lw, vw, "Last built", entry->last_built_at > 0 ? fmt_date(entry->last_built_at) + (info.build_known && info.build.version.empty() ? " (file time)" : "") : "never recorded");
    label_value(ctx_, lw, vw, "Last played", entry->last_played_at > 0 ? fmt_date(entry->last_played_at) + " - " + run_label(entry->last_run) : "never");
    label_value(ctx_, lw, vw, "Build profile", entry->aot_policy == "optimized" ? (entry->platform_id == "genesis" && entry->aot_model == "gen2" ? "Optimized (experimental model)" : "Optimized (experimental)") : "Compatibility");
    if (info.build_known && (!info.build.version.empty() || !info.build.toolchain.empty()))
      label_value(ctx_, lw, vw, "Toolchain", (info.build.toolchain.empty() ? std::string() : info.build.toolchain) + (info.build.version.empty() ? std::string() : "  -  product v" + info.build.version));
    label_value(ctx_, lw, vw, "Build folder", info.key.empty() ? std::string("-") : info.key.substr(0, 12) + "...");
    if (report_valid_) {
      if (const EntryUsage *u = report_.find(entry->id())) {
        label_value(ctx_, lw, vw, "Output size", format_bytes(u->total.total()));
        const std::pair<const char *, std::uintmax_t> parts[] = {{"Executable", u->total.executable}, {"Generated C", u->total.generated_c}, {"Object files", u->total.objects}, {"Logs", u->total.logs}, {"Metadata", u->total.metadata}, {"Other", u->total.other}};
        std::string line;
        for (const auto &part : parts) if (part.second != 0) line += std::string(line.empty() ? "" : "   ") + part.first + " " + format_bytes(part.second);
        if (!line.empty()) label_value(ctx_, lw, vw, "", line, col::text2);
      }
    }
  }
  rule(ctx_, rw);
  draw_build_options(*entry, info, rw);
  if (show_log_) {
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    heading(ctx_, "Logs");
    std::string text;
    if (const auto d = diagnostics_.find(entry->id()); d != diagnostics_.end()) text = d->second;
    else {
      const fs::path failed = info.entry.parent_path() / (info.key + ".failed");
      text = read_tail(info.entry / "build.log", 6000);
      if (text.empty()) text = read_tail(failed / "build.log", 6000);
      const std::string run = read_tail(info.entry / "run.log", 2000);
      if (!run.empty()) text += "\n--- last run ---\n" + run;
    }
    if (text.empty()) text = "No log exists for this game's current build.";
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const auto lines = wrap(ctx_.regular, ctx_.u(10.5F), rw - ctx_.u(20.0F), text);
    const float lh = ctx_.u(10.5F) + ctx_.u(2.0F);
    const std::size_t max_lines = 24;
    const std::size_t first = lines.size() > max_lines ? lines.size() - max_lines : 0;
    const float box_h = static_cast<float>(lines.size() - first) * lh + ctx_.u(18.0F);
    dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + box_h), rgba(5, 9, 20, 235), ctx_.u(3.0F));
    dl->AddRect(p, ImVec2(p.x + rw, p.y + box_h), col::edge, ctx_.u(3.0F));
    float ly = p.y + ctx_.u(9.0F);
    for (std::size_t i = first; i < lines.size(); ++i) { draw_text(dl, ctx_.regular, ctx_.u(10.5F), col::text2, ImVec2(p.x + ctx_.u(10.0F), ly), lines[i].c_str()); ly += lh; }
    ImGui::Dummy(ImVec2(rw, box_h));
  }
  // make sure the scroll area covers the taller of the two columns
  if (two_col) {
    ImGui::Unindent(left_w + gap);
    const float bottom = std::max(left_bottom, ImGui::GetCursorScreenPos().y);
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
  }
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

// ---------------------------------------------------------------- import

void App::draw_import(float w, float h) {
  heading(ctx_, "Add games to the library");
  paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Drop one or many ROM files onto this window, or browse for them. Segarecomp validates each file (Genesis / Mega Drive or Master System), keeps a reference to where it is, and never copies, moves, modifies or uploads it. Adding a game does not build it.");
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  draw_drop_zone(w, h, true);
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  checkbox(ctx_, "import_build", "Also build each newly added game (can take several minutes per game)", &import_build_option_);
  paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Off by default. Duplicates are detected by the ROM's SHA-256: the same game found in another place only adds a second tracked location.");
  if (import_job_.valid()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spinner(ctx_, ImGui::GetWindowDrawList(), ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info);
    draw_text(ImGui::GetWindowDrawList(), ctx_.regular, ctx_.u(13.0F), col::text, ImVec2(p.x + ctx_.u(26.0F), p.y), ("Reading ROMs: " + std::to_string(import_done_.load()) + " of " + std::to_string(import_total_)).c_str());
    ImGui::Dummy(ImVec2(w, ctx_.u(24.0F)));
  }
  if (!import_results_.empty()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(10.0F)));
    heading(ctx_, "Results");
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (std::size_t i = 0; i < import_results_.size(); ++i) {
      const ImportItem &item = import_results_[i];
      const ImVec2 a = ImGui::GetCursorScreenPos();
      const float rh = ctx_.u(46.0F);
      const bool ok = item.outcome == "added", dup = item.outcome == "duplicate" || item.outcome == "location";
      const ImU32 color = ok ? col::ok : (dup ? col::info : col::err);
      bevel(dl, a, ImVec2(a.x + w, a.y + rh), std::max(1.0F, ctx_.u(2.0F)), col::border_dark, col::edge, col::edge, col::edge, col::panel_solid);
      icon_image(dl, ctx_.icon(ok ? "check" : (dup ? "info" : "warning")), ImVec2(a.x + ctx_.u(10.0F), a.y + (rh - ctx_.u(16.0F)) * 0.5F), ctx_.u(16.0F), color);
      const std::string title = item.title.empty() ? fs::path(std::u8string(reinterpret_cast<const char8_t *>(item.path.c_str()))).filename().string() : item.title;
      draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(a.x + ctx_.u(36.0F), a.y + ctx_.u(7.0F)), ellipsize(ctx_.bold, ctx_.u(12.5F), w - ctx_.u(220.0F), title).c_str());
      draw_text(dl, ctx_.regular, ctx_.u(11.0F), color, ImVec2(a.x + ctx_.u(36.0F), a.y + ctx_.u(25.0F)), ellipsize(ctx_.regular, ctx_.u(11.0F), w - ctx_.u(220.0F), item.message).c_str());
      if (!item.entry_id.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + w - ctx_.u(114.0F), a.y + (rh - ctx_.u(28.0F)) * 0.5F));
        ImGui::PushID(static_cast<int>(i));
        if (button(ctx_, "open", "Open", ImVec2(ctx_.u(100.0F), ctx_.u(28.0F)), Style::secondary)) open_details(item.entry_id);
        ImGui::PopID();
      }
      ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rh + ctx_.u(4.0F)));
      ImGui::Dummy(ImVec2(w, 1));
    }
    if (button(ctx_, "clear_results", "Clear results", ImVec2(ctx_.u(140.0F), ctx_.u(28.0F)), Style::ghost)) import_results_.clear();
  }
}

// ---------------------------------------------------------------- storage

void App::draw_storage(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool maintenance = maintenance_running_ || busy_.any_activity();
  // summary tiles
  {
    const float tile_w = (w - ctx_.u(8.0F) * 3) / 4.0F, tile_h = ctx_.u(58.0F);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const std::pair<std::string, std::string> tiles[] = {
        {"Builds", report_valid_ ? format_bytes(report_.builds_total()) : (scanning_ ? "scanning..." : "-")},
        {"Compiler cache", report_valid_ ? format_bytes(report_.zig_cache) + (report_.zig_truncated ? "+" : "") : "-"},
        {"Library data", report_valid_ ? format_bytes(report_.catalog_and_covers) : "-"},
        {"Free disk", report_valid_ ? format_bytes(report_.free_bytes) : "-"}};
    for (int i = 0; i < 4; ++i) {
      const ImVec2 a(o.x + static_cast<float>(i) * (tile_w + ctx_.u(8.0F)), o.y), b(a.x + tile_w, a.y + tile_h);
      bevel(dl, a, b, std::max(1.0F, ctx_.u(2.0F)), col::border_dark, col::edge, col::edge, col::edge, col::panel_solid);
      draw_text(dl, ctx_.regular, ctx_.u(10.5F), col::dim, ImVec2(a.x + ctx_.u(10.0F), a.y + ctx_.u(9.0F)), tiles[i].first.c_str());
      draw_text(dl, ctx_.bold, ctx_.u(15.0F), col::text, ImVec2(a.x + ctx_.u(10.0F), a.y + ctx_.u(28.0F)), tiles[i].second.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + tile_h + ctx_.u(8.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  }
  if (scanning_) { const ImVec2 p = ImGui::GetCursorScreenPos(); spinner(ctx_, dl, ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info); draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(p.x + ctx_.u(26.0F), p.y + ctx_.u(2.0F)), "Measuring the cache in the background (bounded, read-only)..."); ImGui::Dummy(ImVec2(w, ctx_.u(22.0F))); }
  else {
    if (button(ctx_, "rescan", "Rescan", ImVec2(ctx_.u(100.0F), ctx_.u(26.0F)), Style::ghost)) request_scan();
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(p.x, p.y + ctx_.u(6.0F)), ("Cache: " + u8s(cache_dir_)).c_str());
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  }
  if (soft_limit_exceeded_) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, "Builds use more than your soft limit of " + std::to_string(settings_.cleanup.soft_limit_mb) + " MiB. Choose what to reclaim below; nothing is deleted without confirmation.");
  // selection controls
  const std::vector<const LibraryEntry *> games = [&] {
    std::vector<const LibraryEntry *> v;
    for (const auto &e : catalog_.entries()) v.push_back(&e);
    std::stable_sort(v.begin(), v.end(), [&](const LibraryEntry *a, const LibraryEntry *b) {
      if (storage_sort_ == 1) { const EntryUsage *ua = report_.find(a->id()), *ub = report_.find(b->id()); return (ua ? ua->total.total() : 0) > (ub ? ub->total.total() : 0); }
      return lower(a->title()) < lower(b->title());
    });
    return v;
  }();
  {
    if (button(ctx_, "st_all", "Select all", ImVec2(ctx_.u(110.0F), ctx_.u(26.0F)), Style::secondary)) for (const auto *e : games) storage_selected_.insert(e->id());
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "st_none", "Clear", ImVec2(ctx_.u(80.0F), ctx_.u(26.0F)), Style::secondary)) storage_selected_.clear();
    ImGui::SameLine(0.0F, ctx_.u(14.0F));
    int sort = storage_sort_;
    if (segmented(ctx_, "st_sort", {"Name", "Size"}, &sort, ctx_.u(70.0F))) storage_sort_ = sort;
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  // table
  const float rh = ctx_.u(36.0F);
  const auto table_row = [&](const LibraryEntry *e) {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const EntryUsage *u = report_valid_ ? report_.find(e->id()) : nullptr;
    const bool sel = storage_selected_.count(e->id()) != 0;
    bevel(dl, a, ImVec2(a.x + w, a.y + rh), std::max(1.0F, ctx_.u(2.0F)), col::border_dark, sel ? col::btn_inner : col::edge, sel ? col::btn_top : col::edge, col::edge, col::panel_solid);
    bool on = sel;
    ImGui::PushID(e->id().c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x + ctx_.u(8.0F), a.y + (rh - ctx_.u(22.0F)) * 0.5F));
    if (checkbox(ctx_, "sel", "", &on)) { if (on) storage_selected_.insert(e->id()); else storage_selected_.erase(e->id()); }
    ImGui::PopID();
    const float px = ctx_.u(12.0F);
    const float name_w = std::max(ctx_.u(120.0F), w * 0.38F);
    draw_text(dl, ctx_.bold, px, col::text, ImVec2(a.x + ctx_.u(40.0F), a.y + rh * 0.5F - px * 0.9F), ellipsize(ctx_.bold, px, name_w, e->title()).c_str());
    draw_text(dl, ctx_.regular, ctx_.u(10.5F), col::dim, ImVec2(a.x + ctx_.u(40.0F), a.y + rh * 0.5F + ctx_.u(1.0F)), platform_short(e->platform_id));
    float x = a.x + ctx_.u(40.0F) + name_w + ctx_.u(8.0F);
    const StatusInfo &info = status_of(e->id());
    draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::text, ImVec2(x, a.y + rh * 0.5F - ctx_.u(6.0F)), u ? format_bytes(u->total.total()).c_str() : "-");
    x += ctx_.u(92.0F);
    if (w > ctx_.u(560.0F)) { draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::text2, ImVec2(x, a.y + rh * 0.5F - ctx_.u(6.0F)), u ? (format_bytes(u->intermediates) + " reclaimable").c_str() : "-"); x += ctx_.u(150.0F); }
    draw_text(dl, ctx_.regular, ctx_.u(11.0F), status_color(info), ImVec2(x, a.y + rh * 0.5F - ctx_.u(6.0F)), status_label(info).c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rh + ctx_.u(3.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  };
  const float table_h = std::min(static_cast<float>(games.size()) * (rh + ctx_.u(3.0F)) + ctx_.u(16.0F), std::max(ctx_.u(150.0F), h * 0.48F));
  if (games.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "The library is empty: no games, nothing to clean.");
  else {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
    ImGui::BeginChild("##storage_table", ImVec2(w + ctx_.u(8.0F), table_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(games.size()), rh + ctx_.u(3.0F));
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) table_row(games[static_cast<std::size_t>(i)]);
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }
  if (report_valid_ && !report_.unlinked.empty()) {
    std::uintmax_t unlinked = 0;
    for (const auto &d : report_.unlinked) unlinked += d.use.total();
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, std::to_string(report_.unlinked.size()) + " folder(s) in the cache belong to no library entry (" + format_bytes(unlinked) + "). They are never deleted here, except failed/partial ones through 'Clean failed / abandoned builds'.");
  }
  // selected breakdown
  if (report_valid_ && storage_selected_.size() == 1) {
    if (const EntryUsage *u = report_.find(*storage_selected_.begin())) {
      const std::pair<const char *, std::uintmax_t> parts[] = {{"Executable", u->total.executable}, {"Generated C", u->total.generated_c}, {"Object files", u->total.objects}, {"Logs", u->total.logs}, {"Metadata", u->total.metadata}, {"Other", u->total.other}};
      std::string line = "Selected game: ";
      for (const auto &p : parts) if (p.second != 0) line += std::string(p.first) + " " + format_bytes(p.second) + "   ";
      line += "(" + std::to_string(u->dirs.size()) + " folder(s))";
      paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::text2, w, line);
    }
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  rule(ctx_, w);
  heading(ctx_, "Reclaim space");
  const auto open_plan = [&](Plan plan, const char *title) {
    modal_ = Modal{};
    modal_.kind = Modal::Kind::plan;
    modal_.title = title;
    modal_.plan = std::move(plan);
  };
  const std::vector<std::string> ids(storage_selected_.begin(), storage_selected_.end());
  const bool have = !ids.empty() && report_valid_ && !maintenance;
  struct Act { const char *id, *label, *icon, *explain; };
  const float bw = std::min(ctx_.u(300.0F), w);
  const auto action = [&](const char *id, const char *label, const char *explain, bool enabled, Style style, const std::function<void()> &fn) {
    if (button(ctx_, id, label, ImVec2(bw, ctx_.u(32.0F)), style, enabled, ctx_.icon("trash"))) fn();
    ImGui::SameLine(0.0F, ctx_.u(10.0F));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tw = w - bw - ctx_.u(10.0F);
    if (tw > ctx_.u(140.0F)) {
      const auto lines = wrap(ctx_.regular, ctx_.u(10.5F), tw, explain);
      float y = 0;
      for (std::size_t i = 0; i < std::min<std::size_t>(lines.size(), 2); ++i) { draw_text(dl, ctx_.regular, ctx_.u(10.5F), col::dim, ImVec2(p.x, p.y + y + ctx_.u(2.0F)), lines[i].c_str()); y += ctx_.u(13.0F); }
    }
    ImGui::NewLine();
    ImGui::Dummy(ImVec2(1, ctx_.u(2.0F)));
  };
  action("a_inter", "Remove build intermediates", "Deletes generated C and object files of the selected games. Executables stay: games remain playable.", have, Style::secondary, [&] { open_plan(plan_intermediates(report_, ids, busy_), "Remove build intermediates"); });
  action("a_old", "Remove old builds", "Deletes builds of earlier releases or option sets for the selected games; their current build stays.", have, Style::secondary, [&] { open_plan(plan_superseded(report_, ids, busy_), "Remove old builds"); });
  action("a_archive", "Keep metadata only", "Deletes the selected games' executables and all build files. The library entry, ROM reference, history, cover and settings stay; each shows 'Rebuild to play'.", have, Style::danger, [&] { open_plan(plan_archive(report_, ids, busy_), "Keep metadata only (delete builds)"); });
  action("a_failed", "Clean failed / abandoned builds", "Deletes verified *.failed and *.partial build folders of all games (optionally keeping their logs).", report_valid_ && !maintenance, Style::secondary, [&] { Plan p = plan_failed_partial(report_, keep_failed_logs_, busy_); open_plan(p, "Clean failed / abandoned builds"); modal_.keep_logs = keep_failed_logs_; });
  action("a_zig", "Clear shared compiler cache", "Deletes the shared Zig cache only. Built games are untouched; the next Recompile of any game is slower.", report_valid_ && !maintenance, Style::secondary, [&] { open_plan(plan_zig_cache(report_, cache_dir_, busy_), "Clear shared compiler cache"); });
  ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  checkbox(ctx_, "keep_logs", "When cleaning failed builds, keep build.log and status.json", &keep_failed_logs_);
  if (maintenance) paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::warn, w, maintenance_running_ ? "A cleanup is running..." : "Cleanup is unavailable while a build or game is running.");
  paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Cleanup only touches the Segarecomp cache. Your ROM files, covers, screenshots and the library itself are never deleted. Playing never cleans anything.");
}

// ---------------------------------------------------------------- settings

void App::draw_settings(float w, float h) {
  int tab = settings_tab_;
  if (segmented(ctx_, "tabs", {"Library", "Metadata & Covers", "Build & Runtime", "Video & Controls", "Advanced / About"}, &tab, 0.0F)) settings_tab_ = tab;
  ImGui::Dummy(ImVec2(1, ctx_.u(12.0F)));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  bool save = false;
  const float lw = ctx_.u(170.0F);
  const auto row_label = [&](const char *label) { const ImVec2 p = ImGui::GetCursorScreenPos(); draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(p.x, p.y + ctx_.u(8.0F)), label); ImGui::SetCursorScreenPos(ImVec2(p.x + lw, p.y)); };
  const auto next_row = [&] { ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ctx_.u(14.0F) - ImGui::GetScrollX(), ImGui::GetCursorScreenPos().y + ctx_.u(6.0F))); };
  (void)next_row;
  if (settings_tab_ == 0) {
    heading(ctx_, "Library");
    row_label("View");
    int view = settings_.view == "list" ? 1 : 0;
    if (segmented(ctx_, "v", {"Grid", "List"}, &view, ctx_.u(90.0F))) { settings_.view = view == 1 ? "list" : "grid"; save = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Default sort");
    static const char *ids[] = {"last_played", "title", "last_built", "added", "size"};
    int sort = 0;
    for (int i = 0; i < 5; ++i) if (settings_.sort == ids[i]) sort = i;
    if (segmented(ctx_, "s", {"Last played", "Title", "Last built", "Added", "Size"}, &sort, 0.0F)) { settings_.sort = ids[sort]; save = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Layout");
    if (checkbox(ctx_, "compact", "Compact mode (smaller cards and text)", &settings_.compact)) save = true;
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Existing builds");
    if (checkbox(ctx_, "adopt", "At start, adopt finished builds found in the cache folder", &settings_.adopt_cache_on_start)) save = true;
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Reads only the Segarecomp cache folder (never your disks). No ROM is ever searched for: use Import ROM or drop files. Adopted builds have no ROM path until you Locate ROM.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("ROM links");
    std::size_t missing = 0;
    for (const auto &[id, info] : statuses_) if (info.rom_missing) ++missing;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text, ImVec2(p.x, p.y + ctx_.u(8.0F)), (std::to_string(catalog_.entries().size()) + " game(s), " + std::to_string(missing) + " without a reachable ROM").c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(300.0F), p.y));
    if (button(ctx_, "recheck", "Re-check", ImVec2(ctx_.u(110.0F), ctx_.u(28.0F)), Style::secondary)) { request_status_refresh(); toast("Checking ROM links..."); }
    ImGui::Dummy(ImVec2(1, ctx_.u(14.0F)));
    rule(ctx_, w);
    heading(ctx_, "Reset");
    if (button(ctx_, "restore", "Restore launcher defaults", ImVec2(ctx_.u(260.0F), ctx_.u(32.0F)), Style::secondary)) { modal_ = Modal{}; modal_.kind = Modal::Kind::restore_defaults; modal_.title = "Restore launcher defaults?"; }
    ImGui::SameLine(0.0F, ctx_.u(10.0F));
    if (button(ctx_, "clear_lib", "Clear library...", ImVec2(ctx_.u(200.0F), ctx_.u(32.0F)), Style::danger, !catalog_.entries().empty())) { modal_ = Modal{}; modal_.kind = Modal::Kind::clear_library; modal_.title = "Clear the whole library?"; }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Restoring defaults and clearing the library are separate actions. Neither deletes ROMs or build folders.");
  } else if (settings_tab_ == 1) {
    heading(ctx_, "Metadata & Covers");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Titles come from the ROM header or file name and can be renamed on the game's page. Covers are images you choose; the generic placeholder is shown otherwise.");
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Online lookup");
    bool off = false;
    checkbox(ctx_, "lookup", "Download covers (unavailable in this build)", &off, false);
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::warn, w, "Not offered: a trusted HTTPS client that works in all three self-contained releases is not packaged, and shelling out to curl or using plain HTTP would be unsafe. No ROM bytes or hashes are ever sent anywhere. Supported instead: Change cover (local image) and 'Find cover online' (opens your browser).");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Provider");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text, w - lw, "Libretro thumbnails (thumbnails.libretro.com): Named_Boxarts under 'Sega - Mega Drive - Genesis' and 'Sega - Master System - Mark III'. Images belong to their rights holders and the provider's terms apply; Segarecomp bundles none.");
    row_label("Preferred regions");
    {
      std::string text;
      for (const auto &r : settings_.preferred_regions) text += (text.empty() ? "" : " > ") + r;
      const ImVec2 p = ImGui::GetCursorScreenPos();
      draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text, ImVec2(p.x, p.y + ctx_.u(8.0F)), text.c_str());
      ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(250.0F), p.y));
      if (button(ctx_, "rot", "Rotate order", ImVec2(ctx_.u(140.0F), ctx_.u(28.0F)), Style::secondary)) { std::rotate(settings_.preferred_regions.begin(), settings_.preferred_regions.begin() + 1, settings_.preferred_regions.end()); save = true; }
      ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Used only to order the guessed file names ('Title (USA)', 'Title (Europe)', ...) when you open the Libretro page. A guess is never presented as a verified match.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Offline");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w - lw, "Everything works offline. Covers you choose are stored in the covers folder next to the library and survive cleanups; removing a cover returns the placeholder.");
    if (button(ctx_, "open_covers", "Open covers folder", ImVec2(ctx_.u(200.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) { std::error_code ec; fs::create_directories(covers_dir_, ec); SDL_OpenURL(("file://" + u8s(covers_dir_)).c_str()); }
  } else if (settings_tab_ == 2) {
    heading(ctx_, "Build & Runtime");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Compatibility, Optimized, the Genesis region, the Master System mapper and H-interrupt discovery are chosen per game on its page (they are part of what a build is). Toolchain: bundled Zig " SEGARECOMP_ZIG_VERSION ".");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Master System Optimized");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), sms_optimized_available_ ? col::ok : col::dim, w - lw, sms_optimized_available_ ? "Offered by this product build (experimental)." : "Not offered by this product build.");
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    rule(ctx_, w);
    heading(ctx_, "After a successful build");
    int mode = 0;
    static const char *modes[] = {"never", "intermediates", "keep_last_n", "soft_limit"};
    for (int i = 0; i < 4; ++i) if (settings_.cleanup.mode == modes[i]) mode = i;
    if (segmented(ctx_, "cleanup", {"Never", "Remove intermediates", "Keep last N builds", "Soft limit and ask"}, &mode, 0.0F)) { settings_.cleanup.mode = modes[mode]; save = true; soft_limit_exceeded_ = false; if (mode >= 2) request_scan(); }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    if (settings_.cleanup.mode == "keep_last_n") {
      row_label("Keep newest");
      if (button(ctx_, "n_minus", "-", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { settings_.cleanup.keep_last_n = std::max(1, settings_.cleanup.keep_last_n - 1); save = true; }
      ImGui::SameLine(0.0F, ctx_.u(6.0F));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(p.x, p.y + ctx_.u(7.0F)), (std::to_string(settings_.cleanup.keep_last_n) + " games").c_str());
      ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(90.0F), p.y));
      if (button(ctx_, "n_plus", "+", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { settings_.cleanup.keep_last_n = std::min(500, settings_.cleanup.keep_last_n + 1); save = true; }
      ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    } else if (settings_.cleanup.mode == "soft_limit") {
      row_label("Soft limit");
      if (button(ctx_, "l_minus", "-", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { settings_.cleanup.soft_limit_mb = std::max(256LL, settings_.cleanup.soft_limit_mb - 512); save = true; }
      ImGui::SameLine(0.0F, ctx_.u(6.0F));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(p.x, p.y + ctx_.u(7.0F)), (std::to_string(settings_.cleanup.soft_limit_mb) + " MiB").c_str());
      ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(110.0F), p.y));
      if (button(ctx_, "l_plus", "+", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { settings_.cleanup.soft_limit_mb += 512; save = true; }
      ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    }
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), settings_.cleanup.mode == "keep_last_n" ? col::warn : col::text2, w, describe_policy(settings_.cleanup));
    if (button(ctx_, "go_storage", "Manage Storage", ImVec2(ctx_.u(200.0F), ctx_.u(32.0F)), Style::secondary, true, ctx_.icon("storage"))) go(Page::storage);
  } else if (settings_tab_ == 3) {
    heading(ctx_, "Video & Controls defaults");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "These are the same settings as the game's in-game menu (Esc), stored in the viewer settings file. They apply the next time a game starts. Key and gamepad bindings are remapped in the game's Controls page.");
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    bool v_changed = false;
    SvVideoSettings &v = viewer_settings_.video;
    bool fullscreen = v.fullscreen != 0, vsync = v.vsync != 0, fps = v.show_fps != 0, muted = viewer_settings_.audio.muted != 0;
    row_label("Window");
    if (checkbox(ctx_, "fs", "Start fullscreen (borderless)", &fullscreen)) { v.fullscreen = fullscreen; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Scaling");
    int scale = v.scale == SV_SCALE_INTEGER ? 1 : 0;
    if (segmented(ctx_, "sc", {"Fit window", "Integer"}, &scale, ctx_.u(120.0F))) { v.scale = scale == 1 ? SV_SCALE_INTEGER : SV_SCALE_FIT; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Aspect");
    int aspect = v.aspect == SV_ASPECT_4_3 ? 1 : 0;
    if (segmented(ctx_, "as", {"Original", "4:3 corrected"}, &aspect, ctx_.u(140.0F))) { v.aspect = aspect == 1 ? SV_ASPECT_4_3 : SV_ASPECT_ORIGINAL; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Display preset");
    int preset = static_cast<int>(std::min<int>(v.preset, SV_CRT_ARCADE));
    if (segmented(ctx_, "pr", {"Pixel Perfect", "CRT Soft", "CRT Classic", "CRT Arcade"}, &preset, 0.0F)) { sv_apply_crt_preset(&v, static_cast<SvCrtPreset>(preset)); v_changed = true; }
    if (v.preset == SV_CRT_CUSTOM) paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Custom tuning is active (set in the game's Video page).");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Presentation");
    if (checkbox(ctx_, "vs", "VSync", &vsync)) { v.vsync = vsync; v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(18.0F));
    if (checkbox(ctx_, "fp", "FPS counter", &fps)) { v.show_fps = fps; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Audio");
    if (button(ctx_, "vol_minus", "-", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { viewer_settings_.audio.volume = std::max(0, viewer_settings_.audio.volume - 10); v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    { const ImVec2 p = ImGui::GetCursorScreenPos(); draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(p.x, p.y + ctx_.u(7.0F)), ("Volume " + std::to_string(viewer_settings_.audio.volume) + "%").c_str()); ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(120.0F), p.y)); }
    if (button(ctx_, "vol_plus", "+", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { viewer_settings_.audio.volume = std::min(100, viewer_settings_.audio.volume + 10); v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(14.0F));
    if (checkbox(ctx_, "mu", "Mute", &muted)) { viewer_settings_.audio.muted = muted; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(10.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Volume is playback gain only; the console's generated sound is unchanged. Genesis has two ports (Player 2: I/K/J/L, U/O/P, Right Shift); Master System Player 2 uses the same keys. The Master System Pause button is P, separate from the menu key (Esc).");
    if (button(ctx_, "viewer_defaults", "Restore video defaults", ImVec2(ctx_.u(240.0F), ctx_.u(30.0F)), Style::secondary)) { SvVideoSettings keep = v; sv_video_defaults(&v); v.window_w = keep.window_w; v.window_h = keep.window_h; viewer_settings_.audio.volume = 100; viewer_settings_.audio.muted = 0; v_changed = true; }
    if (v_changed) viewer_settings_changed();
  } else {
    heading(ctx_, "Advanced / About");
    const float vw = w - lw;
    label_value(ctx_, lw, vw, "Version", std::string("Segarecomp ") + SEGARECOMP_LAUNCHER_VERSION);
    label_value(ctx_, lw, vw, "Toolchain", std::string("Zig ") + SEGARECOMP_ZIG_VERSION + " (bundled, used only to compile generated C)");
    label_value(ctx_, lw, vw, "SDL", std::to_string(SDL_VERSIONNUM_MAJOR(SDL_GetVersion())) + "." + std::to_string(SDL_VERSIONNUM_MINOR(SDL_GetVersion())) + "." + std::to_string(SDL_VERSIONNUM_MICRO(SDL_GetVersion())) + ", renderer " + (SDL_GetRendererName(renderer_) != nullptr ? SDL_GetRendererName(renderer_) : "?"));
    label_value(ctx_, lw, vw, "Host", host_description());
    rule(ctx_, w);
    heading(ctx_, "Where things live");
    label_value(ctx_, lw, vw, "Installation", u8s(layout_.root) + (layout_.problem.empty() ? "" : "  (incomplete)"), layout_.problem.empty() ? col::text : col::err);
    label_value(ctx_, lw, vw, "Cache (disposable)", u8s(cache_dir_) + "  - games/ and zig-cache/ only");
    label_value(ctx_, lw, vw, "Library (kept)", u8s(library_dir_) + "  - library.json, covers, settings");
    label_value(ctx_, lw, vw, "Viewer settings", u8s(viewer_settings_file_));
    if (!layout_.problem.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, w, layout_.problem);
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    if (button(ctx_, "open_cache", "Open cache folder", ImVec2(ctx_.u(190.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) SDL_OpenURL(("file://" + u8s(cache_dir_)).c_str());
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "open_lib", "Open library folder", ImVec2(ctx_.u(200.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) { std::error_code ec; fs::create_directories(library_dir_, ec); SDL_OpenURL(("file://" + u8s(library_dir_)).c_str()); }
    rule(ctx_, w);
    heading(ctx_, "Diagnostics");
    label_value(ctx_, lw, vw, "Library", std::to_string(catalog_.entries().size()) + " game(s), schema " + std::to_string(Catalog::schema_version) + (catalog_.read_only() ? " (read-only: written by a newer release)" : ""));
    if (!catalog_notice_.empty()) label_value(ctx_, lw, vw, "Notice", catalog_notice_, col::warn);
    if (button(ctx_, "adopt_now", "Adopt existing builds now", ImVec2(ctx_.u(250.0F), ctx_.u(30.0F)), Style::secondary, !catalog_.read_only())) {
      const std::size_t added = catalog_.migrate_cache(cache_dir_, now_seconds());
      save_catalog();
      request_status_refresh();
      toast(added == 0 ? "No new verifiable builds found in the cache." : "Adopted " + std::to_string(added) + " game(s) from the cache.");
    }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Errors are shown where they happen (build failures on the game's page with the sanitized log). Nothing is sent anywhere; Segarecomp makes no network requests.");
  }
  if (save) save_settings();
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

} // namespace launcher
