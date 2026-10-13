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

// ---------------------------------------------------------------- details

namespace {

struct Slice { const char *name; ImU32 color; std::uintmax_t bytes; };

// "Storage Usage  312 MB" card of the details page (real measured sizes only).
float usage_card(Ctx &ctx, ImVec2 a, float w, const EntryUsage *u, bool scanning) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float pad = ctx.u(14.0F), row = ctx.u(24.0F);
  const Slice slices[] = {{"Executable (exe)", rgba(52, 150, 255), u ? u->total.executable : 0}, {"Generated C code", rgba(176, 92, 230), u ? u->total.generated_c : 0},
                          {"Object files", rgba(80, 200, 160), u ? u->total.objects : 0}, {"Logs", rgba(240, 170, 60), u ? u->total.logs : 0},
                          {"Metadata", rgba(120, 210, 90), u ? u->total.metadata : 0}, {"Other", rgba(150, 165, 200), u ? u->total.other : 0}};
  const float h = ctx.u(56.0F) + 6.0F * row + ctx.u(58.0F);
  card(ctx, dl, a, ImVec2(a.x + w, a.y + h));
  draw_text(dl, ctx.bold, ctx.u(14.0F), col::text, ImVec2(a.x + pad, a.y + pad), "Storage Usage");
  const std::string total = u ? format_bytes(u->total.total()) : (scanning ? "measuring..." : "-");
  draw_text(dl, ctx.bold, ctx.u(15.0F), col::info, ImVec2(a.x + w - pad - text_width(ctx.bold, ctx.u(15.0F), total.c_str()), a.y + pad), total.c_str());
  float y = a.y + ctx.u(46.0F);
  for (const auto &s : slices) {
    dl->AddRectFilled(ImVec2(a.x + pad, y + ctx.u(4.0F)), ImVec2(a.x + pad + ctx.u(11.0F), y + ctx.u(15.0F)), s.color, ctx.u(2.0F));
    draw_text(dl, ctx.regular, ctx.u(12.5F), col::text, ImVec2(a.x + pad + ctx.u(20.0F), y + ctx.u(1.0F)), s.name);
    const std::string v = u ? format_bytes(s.bytes) : "-";
    draw_text(dl, ctx.regular, ctx.u(12.5F), col::text2, ImVec2(a.x + w - pad - text_width(ctx.regular, ctx.u(12.5F), v.c_str()), y + ctx.u(1.0F)), v.c_str());
    y += row;
  }
  return h;
}

} // namespace

void App::draw_details(float w, float h) {
  LibraryEntry *entry = catalog_.find_id(detail_id_);
  if (entry == nullptr) { page_ = Page::library; return; }
  const StatusInfo info = status_of(detail_id_);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool is_building = job_ && job_id_ == detail_id_, is_running = game_ && game_id_ == detail_id_;
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    page_header(ctx_, w, "GAME DETAILS", "View game information, play, rebuild, and manage files.");
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - ctx_.u(92.0F), p.y - ctx_.u(4.0F)));
    if (button(ctx_, "back", "Back", ImVec2(ctx_.u(92.0F), ctx_.u(28.0F)), Style::secondary, true, ctx_.icon("back"))) { page_ = previous_page_ == Page::details ? Page::library : previous_page_; return; }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(34.0F)));
  }
  const bool three_col = w > ctx_.u(740.0F), two_col = w > ctx_.u(560.0F);
  const float cover_w = two_col ? ctx_.u(170.0F) : std::min(w, ctx_.u(170.0F)), gap = ctx_.u(18.0F), usage_w = three_col ? ctx_.u(230.0F) : 0.0F;
  const float mid_w = two_col ? w - cover_w - gap - (three_col ? usage_w + gap : 0.0F) : w;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  float left_bottom = top.y;
  {  // cover column
    const float ch = cover_w * 1.32F;
    const ImVec2 a(top.x, top.y), b(top.x + cover_w, top.y + ch);
    draw_cover(dl, *entry, a, b, ctx_.u(5.0F));
    dl->AddRect(a, b, col::glow, ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.5F)));
    float y = b.y + ctx_.u(8.0F);
    ImGui::SetCursorScreenPos(ImVec2(top.x, y));
    if (button(ctx_, "cover_change", "Change cover", ImVec2(cover_w, ctx_.u(28.0F)), Style::secondary, true, ctx_.icon("edit"))) browse_cover(entry->id());
    y += ctx_.u(34.0F);
    if (!entry->cover.empty()) {
      ImGui::SetCursorScreenPos(ImVec2(top.x, y));
      if (button(ctx_, "cover_remove", "Remove cover", ImVec2(cover_w, ctx_.u(28.0F)), Style::secondary, true, ctx_.icon("trash"))) {
        remove_cover_file(covers_dir_, entry->cover);
        if (auto it = cover_cache_.find(entry->cover); it != cover_cache_.end()) { if (it->second) SDL_DestroyTexture(it->second); cover_cache_.erase(it); }
        entry->cover.clear();
        save_catalog();
        toast("Cover removed; the generic placeholder is shown.");
      }
      y += ctx_.u(34.0F);
    }
    {
      const auto names = libretro_candidate_names(entry->title(), settings_.preferred_regions);
      const std::string url = names.empty() ? std::string() : libretro_candidate_url(entry->platform_id, names.front());
      ImGui::SetCursorScreenPos(ImVec2(top.x, y));
      if (button(ctx_, "cover_web", "Find cover online", ImVec2(cover_w, ctx_.u(28.0F)), Style::ghost, !url.empty(), ctx_.icon("search"))) SDL_OpenURL(url.c_str());
      y += ctx_.u(34.0F);
      ImGui::SetCursorScreenPos(ImVec2(top.x, y));
      y += paragraph(ctx_, ctx_.regular, ctx_.u(10.5F), col::dim, cover_w, "Opens your browser at a Libretro thumbnail name guess (it may not exist). Save an image and use Change cover. Segarecomp downloads nothing.");
    }
    left_bottom = y;
  }
  float right_bottom = top.y;
  if (three_col) {
    const EntryUsage *u = report_valid_ ? report_.find(entry->id()) : nullptr;
    const ImVec2 a(top.x + w - usage_w, top.y);
    const float ch = usage_card(ctx_, a, usage_w, u, scanning_);
    ImGui::SetCursorScreenPos(ImVec2(a.x + ctx_.u(14.0F), a.y + ch - ctx_.u(48.0F)));
    if (button(ctx_, "manage_storage", "Manage Storage...", ImVec2(usage_w - ctx_.u(28.0F), ctx_.u(34.0F)), Style::secondary, true, ctx_.icon("storage"))) { storage_selected_ = {entry->id()}; go(Page::storage); }
    right_bottom = a.y + ch;
  }
  // ---- middle column ----
  if (two_col) { ImGui::Indent(cover_w + gap); ImGui::SetCursorScreenPos(ImVec2(top.x + cover_w + gap, top.y)); }
  else ImGui::SetCursorScreenPos(ImVec2(top.x, left_bottom + ctx_.u(12.0F)));
  const ImVec2 rx = ImGui::GetCursorScreenPos();
  const float rw = mid_w;
  {
    const float bsz = ctx_.u(28.0F);
    if (editing_title_) {
      bool active = false;
      input_text(ctx_, "title", title_edit_, sizeof title_edit_, rw - bsz * 2 - ctx_.u(16.0F), nullptr, &active);
      ImGui::SameLine(0.0F, ctx_.u(6.0F));
      if (icon_button(ctx_, "title_ok", ctx_.icon("check"), bsz, true, true, "Save name") || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        const std::string trimmed = clean_title(title_edit_);
        entry->display_title = trimmed == entry->detected_title ? std::string() : trimmed;
        editing_title_ = false;
        save_catalog();
      }
      ImGui::SameLine(0.0F, ctx_.u(4.0F));
      if (icon_button(ctx_, "title_cancel", ctx_.icon("close"), bsz, false, true, "Cancel")) editing_title_ = false;
    } else {
      const float tpx = ctx_.u(23.0F);
      const std::string shown = ellipsize(ctx_.bold, tpx, rw - bsz * 2 - ctx_.u(18.0F), entry->title());
      draw_text(dl, ctx_.bold, tpx, col::text, ImVec2(rx.x, rx.y + ctx_.u(2.0F)), shown.c_str());
      const float tw = text_width(ctx_.bold, tpx, shown.c_str());
      ImGui::SetCursorScreenPos(ImVec2(rx.x + tw + ctx_.u(12.0F), rx.y));
      if (icon_button(ctx_, "fav", ctx_.icon("star"), bsz, entry->favorite, true, entry->favorite ? "Remove from favorites" : "Add to favorites")) { entry->favorite = !entry->favorite; save_catalog(); }
      ImGui::SameLine(0.0F, ctx_.u(4.0F));
      if (icon_button(ctx_, "title_edit", ctx_.icon("edit"), bsz, false, true, "Rename (display name only)")) { std::snprintf(title_edit_, sizeof title_edit_, "%s", entry->title().c_str()); editing_title_ = true; }
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(rx.x, rx.y + ctx_.u(40.0F)));
  {
    ImVec2 p = ImGui::GetCursorScreenPos(), size;
    pill(ctx_, dl, p, platform_short(entry->platform_id), col::info, ctx_.icon("controller"), ctx_.u(11.5F), &size);
    p.x += size.x + ctx_.u(8.0F);
    const ImU32 sc = is_building ? col::warn : (is_running ? col::ok : status_color(info));
    dl->AddCircleFilled(ImVec2(p.x + ctx_.u(9.0F), p.y + size.y * 0.5F), ctx_.u(4.0F), sc);
    pill(ctx_, dl, ImVec2(p.x + ctx_.u(14.0F), p.y), is_building ? "Building" : (is_running ? "Playing" : status_label(info).c_str()), sc, nullptr, ctx_.u(11.5F), &size);
    p.x += size.x + ctx_.u(30.0F);
    if (info.rom_missing) { pill(ctx_, dl, p, entry->rom_path_unknown ? "ROM path not recorded" : "ROM file missing", col::warn, ctx_.icon("warning"), ctx_.u(11.0F), &size); p.x += size.x + ctx_.u(8.0F); }
    ImGui::SetCursorScreenPos(ImVec2(rx.x, p.y + ctx_.u(34.0F)));
  }
  {
    const float bh = ctx_.u(40.0F);
    const bool can_play = info.status == GameStatus::ready && !game_ && !job_ && !maintenance_running_;
    if (button(ctx_, "play", "Play", ImVec2(ctx_.u(130.0F), bh), Style::primary, can_play, ctx_.icon("play"))) { restarted_this_play_ = false; notices_.erase(entry->id()); show_log_ = false; launch(entry->id(), false); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    const char *build_label = info.status == GameStatus::ready ? "Rebuild" : (info.status == GameStatus::metadata_only ? "Rebuild" : (info.status == GameStatus::failed ? "Try again" : (info.status == GameStatus::stale ? "Recompile" : "Build")));
    const bool mapper_needed = entry->platform_id == "master-system" && entry->mapper.empty() && entry->mapper_manifest_sha256.empty();
    const bool can_build = !job_ && !game_ && !maintenance_running_ && !info.rom_missing && !mapper_needed;
    if (button(ctx_, "build", build_label, ImVec2(ctx_.u(116.0F), bh), info.status == GameStatus::ready ? Style::secondary : Style::primary, can_build, ctx_.icon("reset"))) { notices_.erase(entry->id()); start_build(entry->id()); }
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    std::error_code ec;
    if (button(ctx_, "open_dir", "Open build folder", ImVec2(ctx_.u(160.0F), bh), Style::secondary, fs::is_directory(info.entry, ec), ctx_.icon("folder"))) SDL_OpenURL(("file://" + u8s(info.entry)).c_str());
    if (info.rom_missing) {
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      if (button(ctx_, "locate", "Locate ROM...", ImVec2(ctx_.u(140.0F), bh), Style::secondary, true, ctx_.icon("folder"))) browse_locate(entry->id());
    }
    ImGui::SetCursorScreenPos(ImVec2(rx.x, ImGui::GetCursorScreenPos().y + ctx_.u(8.0F)));
    float fx = 0.0F, fy = ImGui::GetCursorScreenPos().y;
    const float sh = ctx_.u(26.0F);
    const auto flow_button = [&](const char *id, const char *label, float width, const char *icon) {
      if (fx > 0.0F && fx + width > rw) { fx = 0.0F; fy += sh + ctx_.u(5.0F); }
      ImGui::SetCursorScreenPos(ImVec2(rx.x + fx, fy));
      const bool pressed = button(ctx_, id, label, ImVec2(width, sh), Style::ghost, true, ctx_.icon(icon));
      fx += width + ctx_.u(6.0F);
      return pressed;
    };
    if (flow_button("logs", show_log_ ? "Hide logs" : "View logs", ctx_.u(112.0F), "file")) show_log_ = !show_log_;
    if (flow_button("cleanup", "Storage cleanup", ctx_.u(150.0F), "storage")) { storage_selected_ = {entry->id()}; go(Page::storage); }
    if (flow_button("remove", "Remove from library", ctx_.u(176.0F), "trash")) {
      modal_ = Modal{};
      modal_.kind = Modal::Kind::remove_entries;
      modal_.ids = {entry->id()};
      modal_.title = "Remove \"" + ellipsize(ctx_.bold, ctx_.u(17.0F), ctx_.u(300.0F), entry->title()) + "\" from the library?";
    }
    ImGui::SetCursorScreenPos(ImVec2(rx.x, fy + sh + ctx_.u(10.0F)));
    ImGui::Dummy(ImVec2(rw, 1));
  }
  // key facts, like the concept board
  {
    const float lw = ctx_.u(92.0F), vw = rw - lw;
    label_value(ctx_, lw, vw, "Last played", entry->last_played_at > 0 ? fmt_relative(entry->last_played_at, std::time(nullptr)) + "  (" + fmt_date(entry->last_played_at) + ") - " + run_label(entry->last_run) : "never");
    label_value(ctx_, lw, vw, "Compiled", entry->last_built_at > 0 ? fmt_date(entry->last_built_at) + (info.build_known && info.build.version.empty() ? " (file time)" : "") : "never recorded");
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const std::string digest = entry->sha256.substr(0, 10) + "..." + entry->sha256.substr(58) + "  (SHA-256)";
    label_value(ctx_, lw, vw, "ROM hash", digest);
    ImGui::SetCursorScreenPos(ImVec2(row.x + lw + text_width(ctx_.regular, ctx_.u(12.0F), digest.c_str()) + ctx_.u(10.0F), row.y - ctx_.u(4.0F)));
    if (button(ctx_, "copy_sha", "Copy", ImVec2(ctx_.u(54.0F), ctx_.u(22.0F)), Style::ghost)) { SDL_SetClipboardText(entry->sha256.c_str()); toast("SHA-256 copied."); }
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + ctx_.u(24.0F)));
    ImGui::Dummy(ImVec2(1, ctx_.u(2.0F)));
    label_value(ctx_, lw, vw, "Build profile", entry->aot_policy == "optimized" ? (entry->platform_id == "genesis" && entry->aot_model == "gen2" ? "Optimized (experimental model)" : "Optimized (experimental)") : "Default (Compatibility)");
    if (entry->rom_paths.empty()) label_value(ctx_, lw, vw, "ROM file", entry->rom_path_unknown ? "not recorded (older cache record)" : "none", col::warn);
    for (std::size_t i = 0; i < entry->rom_paths.size(); ++i) {
      std::error_code ec;
      const fs::path rom_file = fs::path(std::u8string(reinterpret_cast<const char8_t *>(entry->rom_paths[i].c_str())));
      const bool exists = fs::is_regular_file(rom_file, ec);
      const std::uintmax_t rom_size = exists ? fs::file_size(rom_file, ec) : 0U;
      const ImVec2 row = ImGui::GetCursorScreenPos();
      label_value(ctx_, lw, vw, i == 0 ? "ROM file" : "", u8s(rom_file.filename()) + (exists ? "  (" + format_bytes(rom_size) + ")" : "  (missing)"), exists ? col::text : col::warn);
      ImGui::SetCursorScreenPos(ImVec2(row.x + lw, ImGui::GetCursorScreenPos().y - ctx_.u(2.0F)));
      const std::string folder = ellipsize(ctx_.regular, ctx_.u(10.5F), vw, u8s(rom_file.parent_path()));
      draw_text(dl, ctx_.regular, ctx_.u(10.5F), col::dim, ImGui::GetCursorScreenPos(), folder.c_str());
      ImGui::Dummy(ImVec2(vw, ctx_.u(15.0F)));
      if (ImGui::IsMouseHoveringRect(row, ImVec2(row.x + lw + vw, ImGui::GetCursorScreenPos().y))) tooltip(ctx_, entry->rom_paths[i].c_str());
    }
    if (entry->platform_id == "genesis") label_value(ctx_, lw, vw, "Region", entry->region == "japan" ? "Japan NTSC" : (entry->region == "europe" ? "Europe PAL (runs at 60 Hz)" : "USA NTSC"));
    else label_value(ctx_, lw, vw, "Mapper", entry->mapper.empty() ? (entry->mapper_manifest_sha256.empty() ? "undeclared" : "declared by sidecar manifest") : (entry->mapper == "sega" ? "Sega" : "ROM only (32 KiB)"));
    if (info.build_known && (!info.build.version.empty() || !info.build.toolchain.empty()))
      label_value(ctx_, lw, vw, "Toolchain", info.build.toolchain + (info.build.version.empty() ? std::string() : "  -  product v" + info.build.version));
    label_value(ctx_, lw, vw, "Build folder", info.key.empty() ? std::string("-") : info.key.substr(0, 12) + "...");
    label_value(ctx_, lw, vw, "Experimental", info.compat_known ? "Experimental compatibility metadata ships with this release" : "Compatibility unknown for this ROM");
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  if (is_building) {
    heading(ctx_, "BUILDING", col::warn);
    static const char *names[BuildJob::stage_count] = {"Analyzing ROM", "Generating native C", "Compiling", "Linking"};
    for (int i = 0; i < BuildJob::stage_count; ++i) { stage_row(ctx_, dl, ImGui::GetCursorScreenPos(), names[i], job_->stage(i)); ImGui::Dummy(ImVec2(rw, ctx_.u(23.0F))); }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, rw, "Large games can take several minutes the first time. The build cannot be cancelled safely; closing the launcher waits for it.");
    if (SDL_GetTicks() - live_log_at_ > 700U) {
      live_log_at_ = SDL_GetTicks();
      live_log_ = read_tail(fs::path(job_->entry()).parent_path() / (u8s(job_->entry().filename()) + ".partial") / "build.log", 700);
    }
    if (!live_log_.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(10.5F), col::dim, rw, live_log_, 2.0F);
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  } else if (is_running) {
    paragraph(ctx_, ctx_.bold, ctx_.u(15.0F), col::ok, rw, running_label(game_report_, restarted_this_play_));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, rw, "Close the game window (or use Return to Launcher in its menu) to come back.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  const auto banner = [&](ImU32 color, const char *icon, const std::string &text) {
    const auto lines = wrap(ctx_.regular, ctx_.u(12.0F), rw - ctx_.u(50.0F), text);
    const float bh2 = static_cast<float>(lines.size()) * (ctx_.u(12.0F) + ctx_.u(3.0F)) + ctx_.u(18.0F);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(a, ImVec2(a.x + rw, a.y + bh2), (color & 0x00FFFFFF) | 0x22000000, ctx_.u(5.0F));
    dl->AddRect(a, ImVec2(a.x + rw, a.y + bh2), (color & 0x00FFFFFF) | 0xAA000000, ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
    icon_image(dl, ctx_.icon(icon), ImVec2(a.x + ctx_.u(12.0F), a.y + ctx_.u(9.0F)), ctx_.u(16.0F), color);
    float ly = a.y + ctx_.u(9.0F);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text, ImVec2(a.x + ctx_.u(38.0F), ly), l.c_str()); ly += ctx_.u(12.0F) + ctx_.u(3.0F); }
    ImGui::Dummy(ImVec2(rw, bh2 + ctx_.u(8.0F)));
  };
  if (const auto notice = notices_.find(entry->id()); notice != notices_.end() && !notice->second.empty()) banner(col::err, "warning", notice->second);
  if (const auto failure = failures_.find(entry->id()); failure != failures_.end() && !is_building) {
    banner(col::err, "warning", "Last attempt failed: " + failure->second);
    if (button(ctx_, "dismiss_fail", "Dismiss", ImVec2(ctx_.u(100.0F), ctx_.u(26.0F)), Style::ghost)) failures_.erase(failure);
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  }
  if (!is_building) {
    if (info.status == GameStatus::metadata_only) banner(col::info, "info", "Metadata only: rebuild required. This game's build was removed on purpose; its library entry, cover and history are kept. Rebuild to play it again.");
    else if (info.status == GameStatus::stale) banner(col::warn, "warning", "Stale build: the stored program came from another release or option set, so it is never started. Recompile to use this release.");
    else if (info.status == GameStatus::failed && failures_.count(entry->id()) == 0) banner(col::err, "warning", "The last build failed. Open View logs for the sanitized diagnostics, then Try again.");
    else if (info.status == GameStatus::not_built) banner(col::info, "info", "Not built yet. Adding a game to the library does not build it; Build compiles it with the bundled toolchain.");
    if (info.rom_missing) banner(col::warn, "warning", entry->rom_path_unknown ? "The original ROM location was never recorded (this entry came from an older cache record). Locate ROM to enable rebuilding; playing the existing build still works." : "The original ROM is not at its recorded location. Playing an existing build still works; Locate ROM re-links it (the SHA-256 must match).");
  }
  if (info.status == GameStatus::ready && info.policy.present) {
    const std::string summary = policy_summary(info.policy);
    if (!summary.empty()) {
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), info.policy.fallback ? col::err : col::text2, rw, summary);
      if (info.policy.requested == "optimized") {
        if (button(ctx_, "why", show_fallback_details_ ? "Hide details" : (info.policy.fallback ? "Why?" : "Details"), ImVec2(ctx_.u(110.0F), ctx_.u(24.0F)), Style::ghost)) show_fallback_details_ = !show_fallback_details_;
        if (show_fallback_details_) paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, rw, (info.policy.fallback ? "Optimized was requested but this game was built as Compatibility (the broad, always-correct program).\n" : std::string()) + policy_details(info.policy));
      }
      ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    }
  }
  rule(ctx_, rw);
  draw_build_options(*entry, info, rw);
  if (show_log_) {
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    heading(ctx_, "LOGS");
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
    const auto lines = wrap(ctx_.regular, ctx_.u(11.0F), rw - ctx_.u(20.0F), text);
    const float lh = ctx_.u(11.0F) + ctx_.u(2.0F);
    const std::size_t max_lines = 24;
    const std::size_t first = lines.size() > max_lines ? lines.size() - max_lines : 0;
    const float box_h = static_cast<float>(lines.size() - first) * lh + ctx_.u(18.0F);
    dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + box_h), rgba(4, 9, 22, 240), ctx_.u(5.0F));
    dl->AddRect(p, ImVec2(p.x + rw, p.y + box_h), rgba(34, 70, 140), ctx_.u(5.0F));
    float ly = p.y + ctx_.u(9.0F);
    for (std::size_t i = first; i < lines.size(); ++i) { draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::text2, ImVec2(p.x + ctx_.u(10.0F), ly), lines[i].c_str()); ly += lh; }
    ImGui::Dummy(ImVec2(rw, box_h));
  }
  if (two_col) {
    ImGui::Unindent(cover_w + gap);
    const float bottom = std::max({left_bottom, right_bottom, ImGui::GetCursorScreenPos().y});
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom));
  }
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

// ---------------------------------------------------------------- import

void App::draw_import(float w, float h) {
  page_header(ctx_, w, "IMPORT ROMS", "Drag and drop ROM files to add them to your library.");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool two = w > ctx_.u(760.0F);
  const float steps_w = two ? ctx_.u(300.0F) : 0.0F, gap = ctx_.u(18.0F);
  const float drop_w = two ? w - steps_w - gap : w;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  draw_drop_zone(drop_w, h, true);
  const float after_drop = ImGui::GetCursorScreenPos().y;
  float side_bottom = top.y;
  if (two) {
    const ImVec2 a(top.x + drop_w + gap, top.y);
    const char *titles[] = {"Add ROMs", "We identify the game", "Build and play"};
    const char *texts[] = {"Drag and drop files here, or choose them manually. Several at once are fine.", "The platform comes from the ROM header; the same game is recognised by its SHA-256.", "Adding does not build. Open a game, choose Build, then Play."};
    float y = a.y;
    for (int i = 0; i < 3; ++i) {
      const auto lines = wrap(ctx_.regular, ctx_.u(11.5F), steps_w - ctx_.u(70.0F), texts[i]);
      const float ch = ctx_.u(42.0F) + static_cast<float>(lines.size()) * ctx_.u(15.0F);
      card(ctx_, dl, ImVec2(a.x, y), ImVec2(a.x + steps_w, y + ch));
      step_circle(ctx_, dl, ImVec2(a.x + ctx_.u(26.0F), y + ctx_.u(26.0F)), ctx_.u(13.0F), i + 1);
      draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(a.x + ctx_.u(52.0F), y + ctx_.u(10.0F)), titles[i]);
      float ly = y + ctx_.u(28.0F);
      for (const auto &l : lines) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::text2, ImVec2(a.x + ctx_.u(52.0F), ly), l.c_str()); ly += ctx_.u(15.0F); }
      y += ch + ctx_.u(8.0F);
    }
    const auto info_lines = wrap(ctx_.regular, ctx_.u(11.5F), steps_w - ctx_.u(52.0F), "Your original ROM files are never copied, moved, modified or uploaded. Segarecomp only reads them and remembers where they are.");
    const float ih = static_cast<float>(info_lines.size()) * ctx_.u(15.0F) + ctx_.u(22.0F);
    card(ctx_, dl, ImVec2(a.x, y), ImVec2(a.x + steps_w, y + ih), false, false, rgba(10, 30, 66));
    icon_image(dl, ctx_.icon("info"), ImVec2(a.x + ctx_.u(12.0F), y + ctx_.u(12.0F)), ctx_.u(20.0F), col::info);
    float ly = y + ctx_.u(11.0F);
    for (const auto &l : info_lines) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::text, ImVec2(a.x + ctx_.u(42.0F), ly), l.c_str()); ly += ctx_.u(15.0F); }
    side_bottom = y + ih;
  }
  ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(after_drop, side_bottom) + ctx_.u(10.0F)));
  checkbox(ctx_, "import_build", "Also build each newly added game (can take several minutes per game)", &import_build_option_);
  paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, w, "Off by default. Duplicates are detected by the ROM's SHA-256: the same game found in another place only adds a second tracked location.");
  if (import_job_.valid()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spinner(ctx_, dl, ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info);
    draw_text(dl, ctx_.regular, ctx_.u(13.0F), col::text, ImVec2(p.x + ctx_.u(26.0F), p.y), ("Reading ROMs: " + std::to_string(import_done_.load()) + " of " + std::to_string(import_total_)).c_str());
    ImGui::Dummy(ImVec2(w, ctx_.u(24.0F)));
  }
  if (!import_results_.empty()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(10.0F)));
    heading(ctx_, "RESULTS");
    for (std::size_t i = 0; i < import_results_.size(); ++i) {
      const ImportItem &item = import_results_[i];
      const ImVec2 a = ImGui::GetCursorScreenPos();
      const float rh = ctx_.u(46.0F);
      const bool ok = item.outcome == "added", dup = item.outcome == "duplicate" || item.outcome == "location";
      const ImU32 color = ok ? col::ok : (dup ? col::info : col::err);
      card(ctx_, dl, a, ImVec2(a.x + w, a.y + rh));
      icon_image(dl, ctx_.icon(ok ? "check" : (dup ? "info" : "warning")), ImVec2(a.x + ctx_.u(12.0F), a.y + (rh - ctx_.u(16.0F)) * 0.5F), ctx_.u(16.0F), color);
      const std::string title = item.title.empty() ? fs::path(std::u8string(reinterpret_cast<const char8_t *>(item.path.c_str()))).filename().string() : item.title;
      draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(a.x + ctx_.u(40.0F), a.y + ctx_.u(7.0F)), ellipsize(ctx_.bold, ctx_.u(13.0F), w - ctx_.u(220.0F), title).c_str());
      draw_text(dl, ctx_.regular, ctx_.u(11.5F), color, ImVec2(a.x + ctx_.u(40.0F), a.y + ctx_.u(25.0F)), ellipsize(ctx_.regular, ctx_.u(11.5F), w - ctx_.u(220.0F), item.message).c_str());
      if (!item.entry_id.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + w - ctx_.u(114.0F), a.y + (rh - ctx_.u(28.0F)) * 0.5F));
        ImGui::PushID(static_cast<int>(i));
        if (button(ctx_, "open", "Open", ImVec2(ctx_.u(100.0F), ctx_.u(28.0F)), Style::secondary)) open_details(item.entry_id);
        ImGui::PopID();
      }
      ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rh + ctx_.u(5.0F)));
      ImGui::Dummy(ImVec2(w, 1));
    }
    if (button(ctx_, "clear_results", "Clear results", ImVec2(ctx_.u(140.0F), ctx_.u(28.0F)), Style::ghost)) import_results_.clear();
  }
}

// ---------------------------------------------------------------- storage

void App::draw_storage(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool maintenance = maintenance_running_ || busy_.any_activity();
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    page_header(ctx_, w, "STORAGE MANAGEMENT", "Analyze disk usage and clean up generated files.");
    int sort = storage_sort_;
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - ctx_.u(180.0F), p.y - ctx_.u(6.0F)));
    if (dropdown(ctx_, "st_sort", {"Sort by: Name", "Sort by: Size (large first)"}, &sort, ctx_.u(180.0F))) storage_sort_ = sort;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(34.0F)));
  }
  {  // summary
    const float tile_w = (w - ctx_.u(8.0F) * 3) / 4.0F, tile_h = ctx_.u(54.0F);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const std::pair<std::string, std::string> tiles[] = {{"Builds", report_valid_ ? format_bytes(report_.builds_total()) : (scanning_ ? "measuring..." : "-")},
                                                         {"Compiler cache", report_valid_ ? format_bytes(report_.zig_cache) + (report_.zig_truncated ? "+" : "") : "-"},
                                                         {"Library data", report_valid_ ? format_bytes(report_.catalog_and_covers) : "-"}, {"Free disk", report_valid_ ? format_bytes(report_.free_bytes) : "-"}};
    for (int i = 0; i < 4; ++i) {
      const ImVec2 a(o.x + static_cast<float>(i) * (tile_w + ctx_.u(8.0F)), o.y), b(a.x + tile_w, a.y + tile_h);
      card(ctx_, dl, a, b);
      draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImVec2(a.x + ctx_.u(12.0F), a.y + ctx_.u(8.0F)), tiles[i].first.c_str());
      draw_text(dl, ctx_.bold, ctx_.u(16.0F), i == 0 ? col::info : col::text, ImVec2(a.x + ctx_.u(12.0F), a.y + ctx_.u(26.0F)), tiles[i].second.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + tile_h + ctx_.u(8.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  }
  if (scanning_) { const ImVec2 p = ImGui::GetCursorScreenPos(); spinner(ctx_, dl, ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info); draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(p.x + ctx_.u(26.0F), p.y + ctx_.u(2.0F)), "Measuring the cache in the background (bounded, read-only)..."); ImGui::Dummy(ImVec2(w, ctx_.u(22.0F))); }
  if (report_valid_ && report_.free_bytes < (2ULL << 30U)) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, w, "Low disk space: only " + format_bytes(report_.free_bytes) + " free. Builds need room for generated code and objects.");
  if (soft_limit_exceeded_) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, "Builds use more than your soft limit of " + std::to_string(settings_.cleanup.soft_limit_mb) + " MiB. Choose what to reclaim below; nothing is deleted without confirmation.");
  std::vector<const LibraryEntry *> games;
  for (const auto &e : catalog_.entries()) games.push_back(&e);
  std::stable_sort(games.begin(), games.end(), [&](const LibraryEntry *a, const LibraryEntry *b) {
    if (storage_sort_ == 1) { const EntryUsage *ua = report_.find(a->id()), *ub = report_.find(b->id()); return (ua ? ua->total.total() : 0) > (ub ? ub->total.total() : 0); }
    return lower(a->title()) < lower(b->title());
  });
  const bool side = w > ctx_.u(700.0F);
  const float side_w = side ? ctx_.u(236.0F) : 0.0F, gap = ctx_.u(14.0F), tw = side ? w - side_w - gap : w;
  const ImVec2 top = ImGui::GetCursorScreenPos();
  // ---- table ----
  const float rh = ctx_.u(42.0F), hh = ctx_.u(28.0F);
  const float table_h = hh + std::min(static_cast<float>(games.size()) * (rh + ctx_.u(4.0F)) + ctx_.u(6.0F), std::max(ctx_.u(130.0F), h * 0.40F));
  card(ctx_, dl, top, ImVec2(top.x + tw, top.y + table_h), false, false, rgba(7, 17, 40));
  {
    const float x0 = top.x + ctx_.u(40.0F), name_w = std::max(ctx_.u(130.0F), tw * 0.34F);
    const float cols[4] = {x0 + name_w + ctx_.u(40.0F), x0 + name_w + ctx_.u(130.0F), x0 + name_w + ctx_.u(235.0F), x0 + name_w + ctx_.u(330.0F)};
    bool all = !games.empty() && std::all_of(games.begin(), games.end(), [&](const LibraryEntry *e) { return storage_selected_.count(e->id()) != 0; });
    ImGui::SetCursorScreenPos(ImVec2(top.x + ctx_.u(10.0F), top.y + ctx_.u(4.0F)));
    if (checkbox(ctx_, "st_all", "", &all)) { if (all) for (const auto *e : games) storage_selected_.insert(e->id()); else storage_selected_.clear(); }
    const float hpx = ctx_.u(11.5F);
    draw_text(dl, ctx_.bold, hpx, col::text2, ImVec2(x0, top.y + ctx_.u(8.0F)), "Game");
    const char *heads[] = {"Total Size", "Executable", "Intermediate", "Status"};
    for (int i = 0; i < 4; ++i) if (cols[i] + ctx_.u(60.0F) < top.x + tw) draw_text(dl, ctx_.bold, hpx, col::text2, ImVec2(cols[i], top.y + ctx_.u(8.0F)), heads[i]);
    dl->AddRectFilled(ImVec2(top.x + 1, top.y + hh), ImVec2(top.x + tw - 1, top.y + hh + 1), rgba(34, 70, 140));
    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + hh + ctx_.u(3.0F)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
    ImGui::BeginChild("##storage_table", ImVec2(tw, table_h - hh - ctx_.u(4.0F)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList *cl = ImGui::GetWindowDrawList();
    for (const LibraryEntry *e : games) {
      const ImVec2 a = ImGui::GetCursorScreenPos();
      const EntryUsage *u = report_valid_ ? report_.find(e->id()) : nullptr;
      const bool sel = storage_selected_.count(e->id()) != 0;
      const ImVec2 rb(a.x + tw - ctx_.u(14.0F), a.y + rh);
      cl->AddRectFilled(ImVec2(a.x + ctx_.u(4.0F), a.y), rb, sel ? rgba(16, 48, 110) : rgba(9, 21, 48), ctx_.u(5.0F));
      if (sel) cl->AddRect(ImVec2(a.x + ctx_.u(4.0F), a.y), rb, col::btn_top, ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.4F)));
      bool on = sel;
      ImGui::PushID(e->id().c_str());
      ImGui::SetCursorScreenPos(ImVec2(a.x + ctx_.u(12.0F), a.y + (rh - ctx_.u(22.0F)) * 0.5F));
      if (checkbox(ctx_, "sel", "", &on)) { if (on) storage_selected_.insert(e->id()); else storage_selected_.erase(e->id()); }
      ImGui::PopID();
      draw_cover(cl, *e, ImVec2(x0 - top.x + a.x - ctx_.u(6.0F), a.y + ctx_.u(4.0F)), ImVec2(x0 - top.x + a.x + ctx_.u(28.0F), a.y + rh - ctx_.u(4.0F)), ctx_.u(3.0F));
      const float nx = x0 - top.x + a.x + ctx_.u(34.0F);
      draw_text(cl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(nx, a.y + ctx_.u(5.0F)), ellipsize(ctx_.bold, ctx_.u(12.5F), name_w - ctx_.u(40.0F), e->title()).c_str());
      pill(ctx_, cl, ImVec2(nx, a.y + ctx_.u(21.0F)), platform_short(e->platform_id), col::info, nullptr, ctx_.u(9.5F));
      const StatusInfo &info = status_of(e->id());
      const float ox = a.x - top.x;
      const auto cell = [&](int i, const std::string &t, ImU32 c) { if (cols[i] + ctx_.u(60.0F) < top.x + tw) draw_text(cl, ctx_.regular, ctx_.u(12.0F), c, ImVec2(cols[i] + ox, a.y + rh * 0.5F - ctx_.u(7.0F)), t.c_str()); };
      cell(0, u ? format_bytes(u->total.total()) : "-", col::text);
      cell(1, u ? format_bytes(u->total.executable) : "-", col::text2);
      cell(2, u ? format_bytes(u->intermediates) : "-", col::text2);
      if (cols[3] + ctx_.u(60.0F) < top.x + tw) {
        cl->AddCircleFilled(ImVec2(cols[3] + ox + ctx_.u(5.0F), a.y + rh * 0.5F), ctx_.u(4.0F), status_color(info));
        draw_text(cl, ctx_.regular, ctx_.u(12.0F), status_color(info), ImVec2(cols[3] + ox + ctx_.u(14.0F), a.y + rh * 0.5F - ctx_.u(7.0F)), status_label(info).c_str());
      }
      ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rh + ctx_.u(4.0F)));
      ImGui::Dummy(ImVec2(tw, 1));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }
  ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + table_h + ctx_.u(2.0F)));
  float side_bottom = top.y;
  if (side) {  // Selected Game panel
    const ImVec2 a(top.x + tw + gap, top.y);
    const LibraryEntry *sel = nullptr;
    for (const auto *e : games) if (storage_selected_.count(e->id()) != 0) { sel = e; break; }
    const ImVec2 b(a.x + side_w, a.y + table_h);
    card(ctx_, dl, a, b, false, false, rgba(7, 17, 40));
    draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text2, ImVec2(a.x + ctx_.u(14.0F), a.y + ctx_.u(10.0F)), storage_selected_.size() > 1 ? "Selected Games" : "Selected Game");
    if (sel != nullptr) {
      draw_cover(dl, *sel, ImVec2(a.x + ctx_.u(14.0F), a.y + ctx_.u(34.0F)), ImVec2(a.x + ctx_.u(14.0F) + ctx_.u(58.0F), a.y + ctx_.u(34.0F) + ctx_.u(58.0F)), ctx_.u(4.0F));
      draw_text(dl, ctx_.bold, ctx_.u(14.0F), col::text, ImVec2(a.x + ctx_.u(82.0F), a.y + ctx_.u(36.0F)), ellipsize(ctx_.bold, ctx_.u(14.0F), side_w - ctx_.u(96.0F), sel->title()).c_str());
      pill(ctx_, dl, ImVec2(a.x + ctx_.u(82.0F), a.y + ctx_.u(58.0F)), platform_short(sel->platform_id), col::info, nullptr, ctx_.u(10.0F));
      std::uintmax_t total = 0;
      EntryUsage sum;
      for (const auto &id : storage_selected_) if (const EntryUsage *u = report_.find(id)) { sum.total.add(u->total); total += u->total.total(); }
      const std::string t = report_valid_ ? format_bytes(total) + " total" : "-";
      draw_text(dl, ctx_.bold, ctx_.u(15.0F), col::info, ImVec2(a.x + side_w - ctx_.u(14.0F) - text_width(ctx_.bold, ctx_.u(15.0F), t.c_str()), a.y + ctx_.u(100.0F)), t.c_str());
      const Slice slices[] = {{"Executable (exe)", rgba(52, 150, 255), sum.total.executable}, {"Generated C code", rgba(176, 92, 230), sum.total.generated_c}, {"Object files", rgba(80, 200, 160), sum.total.objects},
                              {"Logs", rgba(240, 170, 60), sum.total.logs}, {"Metadata", rgba(120, 210, 90), sum.total.metadata}, {"Other", rgba(150, 165, 200), sum.total.other}};
      float y = a.y + ctx_.u(124.0F);
      for (const auto &s : slices) {
        if (y + ctx_.u(20.0F) > b.y) break;
        dl->AddRectFilled(ImVec2(a.x + ctx_.u(14.0F), y + ctx_.u(3.0F)), ImVec2(a.x + ctx_.u(25.0F), y + ctx_.u(14.0F)), s.color, ctx_.u(2.0F));
        draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text, ImVec2(a.x + ctx_.u(34.0F), y), s.name);
        const std::string v = report_valid_ ? format_bytes(s.bytes) : "-";
        draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text2, ImVec2(b.x - ctx_.u(14.0F) - text_width(ctx_.regular, ctx_.u(12.0F), v.c_str()), y), v.c_str());
        y += ctx_.u(21.0F);
      }
    } else {
      draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::dim, ImVec2(a.x + ctx_.u(14.0F), a.y + ctx_.u(40.0F)), "Select a game in the list.");
    }
    side_bottom = b.y;
  }
  ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(ImGui::GetCursorScreenPos().y, side_bottom + ctx_.u(2.0F))));
  if (report_valid_ && !report_.unlinked.empty()) {
    std::uintmax_t unlinked = 0;
    for (const auto &d : report_.unlinked) unlinked += d.use.total();
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, std::to_string(report_.unlinked.size()) + " folder(s) in the cache belong to no library entry (" + format_bytes(unlinked) + "). They are never deleted here, except failed/partial ones through 'Clean failed / abandoned builds'.");
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  const auto open_plan = [&](Plan plan, const char *title) {
    modal_ = Modal{};
    modal_.kind = Modal::Kind::plan;
    modal_.title = title;
    modal_.plan = std::move(plan);
  };
  const std::vector<std::string> ids(storage_selected_.begin(), storage_selected_.end());
  const bool have = !ids.empty() && report_valid_ && !maintenance;
  const bool any = report_valid_ && !maintenance;
  struct Act { const char *id, *title, *caption, *icon; bool enabled; Style style; std::function<void()> fn; };
  const std::vector<Act> acts = {
      {"a_zig", "Clear shared compiler cache", "Frees the Zig cache; built games are untouched", "reset", any, Style::secondary, [&] { open_plan(plan_zig_cache(report_, cache_dir_, busy_), "Clear shared compiler cache?"); }},
      {"a_inter", "Remove build intermediates", "Generated C and objects of the selection; games stay playable", "trash", have, Style::secondary, [&] { open_plan(plan_intermediates(report_, ids, busy_), "Remove build intermediates?"); }},
      {"a_old", "Remove old builds", "Builds of earlier releases or options; the current build stays", "trash", have, Style::secondary, [&] { open_plan(plan_superseded(report_, ids, busy_), "Remove old builds?"); }},
      {"a_failed", "Clean failed / abandoned builds", "Verified *.failed and *.partial folders of all games", "warning", any, Style::secondary, [&] { Plan p = plan_failed_partial(report_, keep_failed_logs_, busy_); open_plan(p, "Clean failed / abandoned builds?"); modal_.keep_logs = keep_failed_logs_; }},
      {"a_archive", "Keep metadata only", "Deletes the selection's builds; the library entry stays", "trash", have, Style::danger, [&] { open_plan(plan_archive(report_, ids, busy_), "Keep metadata only (delete builds)?"); }}};
  const int per_row = w > ctx_.u(1000.0F) ? 3 : 2;
  const float aw = (w - ctx_.u(10.0F) * static_cast<float>(per_row - 1)) / static_cast<float>(per_row), ah = ctx_.u(60.0F);
  const ImVec2 ao = ImGui::GetCursorScreenPos();
  for (std::size_t i = 0; i < acts.size(); ++i) {
    const Act &act = acts[i];
    const ImVec2 a(ao.x + static_cast<float>(i % static_cast<std::size_t>(per_row)) * (aw + ctx_.u(10.0F)), ao.y + static_cast<float>(i / static_cast<std::size_t>(per_row)) * (ah + ctx_.u(10.0F)));
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(act.id);
    if (!act.enabled) ImGui::BeginDisabled();
    const bool pressed = ui::nav_button("##a", ImVec2(aw, ah));
    if (!act.enabled) ImGui::EndDisabled();
    const bool hov = ImGui::IsItemHovered() && act.enabled, foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    ImGui::PopID();
    const ImVec2 b(a.x + aw, a.y + ah);
    const bool danger = act.style == Style::danger;
    dl->AddRectFilled(a, b, hov ? col::panel_hover : rgba(9, 21, 48), ctx_.u(6.0F));
    dl->AddRect(a, b, !act.enabled ? rgba(30, 50, 96) : (danger ? col::danger_top : (hov ? col::btn_top : rgba(46, 96, 190))), ctx_.u(6.0F), 0, std::max(1.0F, ctx_.u(1.3F)));
    icon_image(dl, ctx_.icon(act.icon), ImVec2(a.x + ctx_.u(12.0F), a.y + (ah - ctx_.u(20.0F)) * 0.5F), ctx_.u(20.0F), !act.enabled ? rgba(90, 105, 140) : (danger ? col::err : IM_COL32_WHITE));
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), act.enabled ? col::text : col::dim, ImVec2(a.x + ctx_.u(44.0F), a.y + ctx_.u(9.0F)), ellipsize(ctx_.bold, ctx_.u(12.5F), aw - ctx_.u(54.0F), act.title).c_str());
    {
      const auto lines = wrap(ctx_.regular, ctx_.u(10.5F), aw - ctx_.u(54.0F), act.caption);
      for (std::size_t li = 0; li < lines.size() && li < 2; ++li) draw_text(dl, ctx_.regular, ctx_.u(10.5F), act.enabled ? col::text2 : col::dim, ImVec2(a.x + ctx_.u(44.0F), a.y + ctx_.u(26.0F) + static_cast<float>(li) * ctx_.u(13.0F)), li == 1 && lines.size() > 2 ? ellipsize(ctx_.regular, ctx_.u(10.5F), aw - ctx_.u(54.0F), lines[li] + " ...").c_str() : lines[li].c_str());
    }
    if (foc) focus_ring(ctx_, dl, a, b);
    if (pressed && act.enabled) act.fn();
  }
  const int arows = (static_cast<int>(acts.size()) + per_row - 1) / per_row;
  ImGui::SetCursorScreenPos(ImVec2(ao.x, ao.y + static_cast<float>(arows) * (ah + ctx_.u(10.0F)) + ctx_.u(4.0F)));
  ImGui::Dummy(ImVec2(w, 1));
  checkbox(ctx_, "keep_logs", "When cleaning failed builds, keep build.log and status.json", &keep_failed_logs_);
  if (maintenance) paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::warn, w, maintenance_running_ ? "A cleanup is running..." : "Cleanup is unavailable while a build or game is running.");
  paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Cleanup only touches the Segarecomp cache. Your ROM files, covers, screenshots and the library itself are never deleted. Playing never cleans anything. Cache: " + u8s(cache_dir_));
}

// ---------------------------------------------------------------- settings

void App::draw_settings(float full_w, float h) {
  page_header(ctx_, full_w, "SETTINGS", "Configure the library, metadata, builds and in-game defaults.");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float tabs_w = ctx_.u(176.0F), tab_gap = ctx_.u(18.0F);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  {
    static const char *labels[] = {"Library", "Metadata & Covers", "Build & Runtime", "Video & Controls", "Input Diagnostics", "Advanced / About"};
    static const char *icons[] = {"library", "folder", "settings", "video", "controller", "info"};
    for (int i = 0; i < 6; ++i) {
      ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(i) * ctx_.u(40.0F)));
      ImGui::PushID(i);
      const bool pressed = ui::nav_button("##tab", ImVec2(tabs_w, ctx_.u(34.0F)));
      const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible, on = settings_tab_ == i;
      ImGui::PopID();
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      dl->AddRectFilled(a, b, on ? rgba(52, 128, 238) : (hov ? col::panel_hover : rgba(10, 24, 56)), ctx_.u(5.0F));
      dl->AddRect(a, b, on ? rgba(120, 190, 255) : rgba(34, 70, 140), ctx_.u(5.0F), 0, std::max(1.0F, ctx_.u(1.2F)));
      icon_image(dl, ctx_.icon(icons[i]), ImVec2(a.x + ctx_.u(10.0F), (a.y + b.y) * 0.5F - ctx_.u(8.0F)), ctx_.u(16.0F));
      draw_text(dl, ctx_.bold, ctx_.u(12.5F), on ? col::btn_text : col::text, ImVec2(a.x + ctx_.u(34.0F), (a.y + b.y) * 0.5F - ctx_.u(7.0F)), labels[i]);
      if (foc) focus_ring(ctx_, dl, a, b);
      if (pressed) settings_tab_ = i;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x + tabs_w + tab_gap, origin.y));
  ImGui::Indent(tabs_w + tab_gap);
  const float w = full_w - tabs_w - tab_gap;
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
    if (toggle(ctx_, "compact", "Compact mode (smaller cards and text)", &settings_.compact, ctx_.u(420.0F))) save = true;
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Existing builds");
    if (toggle(ctx_, "adopt", "At start, adopt finished builds found in the cache folder", &settings_.adopt_cache_on_start, ctx_.u(420.0F))) save = true;
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
    if (toggle(ctx_, "fs", "Start fullscreen (borderless)", &fullscreen, ctx_.u(330.0F))) { v.fullscreen = fullscreen; v_changed = true; }
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
    if (toggle(ctx_, "vs", "VSync", &vsync, ctx_.u(150.0F))) { v.vsync = vsync; v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(18.0F));
    if (toggle(ctx_, "fp", "FPS counter", &fps, ctx_.u(190.0F))) { v.show_fps = fps; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Audio");
    if (button(ctx_, "vol_minus", "-", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { viewer_settings_.audio.volume = std::max(0, viewer_settings_.audio.volume - 10); v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    { const ImVec2 p = ImGui::GetCursorScreenPos(); draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(p.x, p.y + ctx_.u(7.0F)), ("Volume " + std::to_string(viewer_settings_.audio.volume) + "%").c_str()); ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(120.0F), p.y)); }
    if (button(ctx_, "vol_plus", "+", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { viewer_settings_.audio.volume = std::min(100, viewer_settings_.audio.volume + 10); v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(14.0F));
    if (toggle(ctx_, "mu", "Mute", &muted, ctx_.u(120.0F))) { viewer_settings_.audio.muted = muted; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(10.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Volume is playback gain only; the console's generated sound is unchanged. Genesis has two ports (Player 2: I/K/J/L, U/O/P, Right Shift); Master System Player 2 uses the same keys. The Master System Pause button is P, separate from the menu key (Esc).");
    if (button(ctx_, "viewer_defaults", "Restore video defaults", ImVec2(ctx_.u(240.0F), ctx_.u(30.0F)), Style::secondary)) { SvVideoSettings keep = v; sv_video_defaults(&v); v.window_w = keep.window_w; v.window_h = keep.window_h; viewer_settings_.audio.volume = 100; viewer_settings_.audio.muted = 0; v_changed = true; }
    if (v_changed) viewer_settings_changed();
  } else if (settings_tab_ == 4) {
    draw_input_diagnostics(w);
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
  ImGui::Unindent(tabs_w + tab_gap);
  if (save) save_settings();
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

} // namespace launcher
