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

struct Slice { const char *name; ImU32 color; std::uintmax_t bytes; };

} // namespace

// ---------------------------------------------------------------- import

void App::draw_import(float w, float h) {
  page_header(ctx_, w, "IMPORT ROMS", "Drag and drop ROM files to add them to your library.");
  draw_import_body(w, h);
}

void App::draw_import_body(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool two = w > ctx_.u(700.0F) && import_results_.empty();  // once there are results they take the guide's place
  const float steps_w = two ? ctx_.u(240.0F) : 0.0F, gap = ctx_.u(18.0F);
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
    const auto info_lines = wrap(ctx_.regular, ctx_.u(11.5F), steps_w - ctx_.u(52.0F), "ROM files are added by reference. Segarecomp doesn't modify or upload your original files.\n\nZIP archives are read without modification. Segarecomp extracts the ROM into your local library so it can be built and played.");
    const float ih = static_cast<float>(info_lines.size()) * ctx_.u(15.0F) + ctx_.u(22.0F);
    card(ctx_, dl, ImVec2(a.x, y), ImVec2(a.x + steps_w, y + ih), false, false, rgba(10, 30, 66));
    icon_image(dl, ctx_.icon("info"), ImVec2(a.x + ctx_.u(12.0F), y + ctx_.u(12.0F)), ctx_.u(20.0F), col::info);
    float ly = y + ctx_.u(11.0F);
    for (const auto &l : info_lines) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::text, ImVec2(a.x + ctx_.u(42.0F), ly), l.c_str()); ly += ctx_.u(15.0F); }
    side_bottom = y + ih;
  }
  ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(after_drop, side_bottom) + ctx_.u(10.0F)));
  if (!two) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "1. Add ROMs   2. Review the detected system   3. Choose Build, then Play. ROM files are added by reference and never modified or uploaded; from a ZIP archive the ROM is extracted into your local library.");
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
  const bool compact_table = tw < ctx_.u(640.0F);
  const ImVec2 top = ImGui::GetCursorScreenPos();
  // ---- table ----
  const float rh = ctx_.u(42.0F), hh = ctx_.u(28.0F);
  const float table_h = hh + std::min(static_cast<float>(games.size()) * (rh + ctx_.u(4.0F)) + ctx_.u(6.0F), std::max(ctx_.u(130.0F), h * 0.40F));
  card(ctx_, dl, top, ImVec2(top.x + tw, top.y + table_h), false, false, rgba(7, 17, 40));
  {
    const float x0 = top.x + ctx_.u(40.0F), name_w = std::max(ctx_.u(130.0F), tw * 0.34F);
    const float cols[4] = {x0 + name_w + ctx_.u(20.0F), compact_table ? top.x + tw - ctx_.u(124.0F) : x0 + name_w + ctx_.u(130.0F), x0 + name_w + ctx_.u(235.0F), x0 + name_w + ctx_.u(330.0F)};
    bool all = !games.empty() && std::all_of(games.begin(), games.end(), [&](const LibraryEntry *e) { return storage_selected_.count(e->id()) != 0; });
    ImGui::SetCursorScreenPos(ImVec2(top.x + ctx_.u(10.0F), top.y + ctx_.u(4.0F)));
    if (checkbox(ctx_, "st_all", "", &all)) { if (all) for (const auto *e : games) storage_selected_.insert(e->id()); else storage_selected_.clear(); }
    const float hpx = ctx_.u(11.5F);
    draw_text(dl, ctx_.bold, hpx, col::text2, ImVec2(x0, top.y + ctx_.u(8.0F)), "Game");
    const char *heads[] = {"Total Size", compact_table ? "Status" : "Executable", "Intermediate", "Status"};
    for (int i = 0; i < (compact_table ? 2 : 4); ++i) if (cols[i] + ctx_.u(60.0F) < top.x + tw) draw_text(dl, ctx_.bold, hpx, col::text2, ImVec2(cols[i], top.y + ctx_.u(8.0F)), heads[i]);
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
      cell(1, compact_table ? status_label(info) : (u ? format_bytes(u->total.executable) : "-"), compact_table ? status_color(info) : col::text2);
      if (!compact_table) cell(2, u ? format_bytes(u->intermediates) : "-", col::text2);
      if (!compact_table && cols[3] + ctx_.u(60.0F) < top.x + tw) {
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
  paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "ROMs extracted from ZIP archives are stored separately from generated builds, in the library's roms folder. Clearing build caches or keeping only metadata does not remove those ROM copies.");
}

// ---------------------------------------------------------------- settings

void App::draw_settings(float full_w, float h) {
  page_header(ctx_, full_w, "SETTINGS", "Configure the library, metadata, builds and in-game defaults.");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool narrow = full_w < ctx_.u(760.0F);
  const float tabs_w = narrow ? 0.0F : ctx_.u(176.0F), tab_gap = narrow ? 0.0F : ctx_.u(18.0F);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  {
    static const char *labels[] = {"Library", "Metadata & Covers", "Build & Runtime", "Video & Controls", "Input Diagnostics", "Advanced / About"};
    static const char *icons[] = {"library", "folder", "settings", "video", "controller", "info"};
    if (narrow) dropdown(ctx_, "settings_section", {labels[0], labels[1], labels[2], labels[3], labels[4], labels[5]}, &settings_tab_, full_w);
    for (int i = 0; !narrow && i < 6; ++i) {
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
  ImGui::SetCursorScreenPos(ImVec2(origin.x + tabs_w + tab_gap, origin.y + (narrow ? ctx_.u(44.0F) : 0.0F)));
  const float w_outer = full_w - tabs_w - tab_gap - ctx_.u(18.0F);
  // Settings groups are titled section cards, the same card language as the Details page (Input Diagnostics draws its own structure).
  const bool carded = settings_tab_ != 4;
  const float sec_pad = carded ? ctx_.u(16.0F) : 0.0F;
  const float w = w_outer - 2 * sec_pad;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##settings_body", ImVec2(w_outer + ctx_.u(18.0F), std::max(ctx_.u(120.0F), ImGui::GetContentRegionAvail().y)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  if (SDL_getenv("SEGARECOMP_SHOT_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY());  // review aid, never a persisted preference
  dl = ImGui::GetWindowDrawList();
  ImVec2 sec_origin(0, 0);
  bool sec_open = false;
  const auto section_end = [&] {
    if (!sec_open) return;
    ImGui::Unindent(sec_pad);
    const float end_y = ImGui::GetCursorScreenPos().y + ctx_.u(6.0F);
    dl->ChannelsSetCurrent(0);
    card(ctx_, dl, sec_origin, ImVec2(sec_origin.x + w_outer, end_y), false, false, rgba(8, 19, 46, 240));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(sec_origin.x, end_y + ctx_.u(12.0F)));
    ImGui::Dummy(ImVec2(w_outer, 0.0F));
    sec_open = false;
  };
  const auto section = [&](const char *title) {
    section_end();
    sec_origin = ImGui::GetCursorScreenPos();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    draw_text(dl, ctx_.display, ctx_.u(12.5F), rgba(120, 205, 255), ImVec2(sec_origin.x + sec_pad, sec_origin.y + ctx_.u(13.0F)), title);
    ImGui::SetCursorScreenPos(ImVec2(sec_origin.x, sec_origin.y + ctx_.u(38.0F)));
    ImGui::Indent(sec_pad);
    ImGui::SetCursorScreenPos(ImVec2(sec_origin.x + sec_pad, sec_origin.y + ctx_.u(38.0F)));
    sec_open = true;
  };
  bool save = false;
  const float lw = ctx_.u(170.0F);
  const auto row_label = [&](const char *label) { paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, label); };
  const auto next_row = [&] { ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ctx_.u(14.0F) - ImGui::GetScrollX(), ImGui::GetCursorScreenPos().y + ctx_.u(6.0F))); };
  (void)next_row;
  if (settings_tab_ == 0) {
    section("LIBRARY");
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
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(28.0F)));
    if (button(ctx_, "recheck", "Re-check", ImVec2(ctx_.u(110.0F), ctx_.u(28.0F)), Style::secondary)) { request_status_refresh(); toast("Checking ROM links..."); }
    ImGui::Dummy(ImVec2(1, ctx_.u(14.0F)));
    section("RESET");
    if (button(ctx_, "restore", "Restore launcher defaults", ImVec2(ctx_.u(260.0F), ctx_.u(32.0F)), Style::secondary)) { modal_ = Modal{}; modal_.kind = Modal::Kind::restore_defaults; modal_.title = "Restore launcher defaults?"; }
    if (w > ctx_.u(480.0F)) ImGui::SameLine(0.0F, ctx_.u(10.0F));
    if (button(ctx_, "clear_lib", "Clear library...", ImVec2(ctx_.u(200.0F), ctx_.u(32.0F)), Style::danger, !catalog_.entries().empty())) { modal_ = Modal{}; modal_.kind = Modal::Kind::clear_library; modal_.title = "Clear the whole library?"; }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Restoring defaults and clearing the library are separate actions. Neither deletes ROMs or build folders.");
  } else if (settings_tab_ == 1) {
    section("METADATA & COVERS");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Titles come from the ROM header or file name and can be renamed on the game's page. Covers are images you choose or download from Libretro; the placeholder is shown otherwise.");
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Online lookup");
    paragraph(ctx_, ctx_.bold, ctx_.u(13.0F), col::info, w, "Libretro box art, on request only");
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::text2, w, "A newly added game's cover is looked up automatically (silently skipped when none is found or the network is unavailable); you can also ask: 'Download cover (Libretro)' in a game's menu, or the button below. Set SEGARECOMP_NO_COVER_FETCH=1 to disable the automatic lookup. Only the thumbnail file name (the game's title and region) is requested, over HTTPS, through this computer's curl; no ROM bytes or hashes are ever sent. Every image is checked like a local one before it is used.");
    {
      std::size_t without = 0;
      std::vector<std::string> ids;
      for (const auto &e : catalog_.entries())
        if (e.cover.empty() && !libretro_system_dir(e.platform_id).empty()) { ++without; ids.push_back(e.id()); }
      const std::string label = cover_job_.valid() ? "Downloading " + std::to_string(cover_done_.load()) + " / " + std::to_string(cover_total_) + "..."
                                                   : "Download missing covers (" + std::to_string(without) + ")";
      if (button(ctx_, "dl_covers", label.c_str(), ImVec2(ctx_.u(280.0F), ctx_.u(30.0F)), Style::secondary, !cover_job_.valid() && without > 0, ctx_.icon("import")))
        start_cover_download(ids);
    }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Provider");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text, w - lw, "Libretro thumbnails (thumbnails.libretro.com, or its libretro-thumbnails mirror on raw.githubusercontent.com when it cannot be reached): Named_Boxarts under 'Sega - Mega Drive - Genesis' and 'Sega - Master System - Mark III'. Images belong to their rights holders and the provider's terms apply; Segarecomp bundles none.");
    row_label("Preferred regions");
    {
      std::string text;
      for (const auto &r : settings_.preferred_regions) text += (text.empty() ? "" : " > ") + r;
      const ImVec2 p = ImGui::GetCursorScreenPos();
      draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text, ImVec2(p.x, p.y + ctx_.u(8.0F)), text.c_str());
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(28.0F)));
      if (button(ctx_, "rot", "Rotate order", ImVec2(ctx_.u(140.0F), ctx_.u(28.0F)), Style::secondary)) { std::rotate(settings_.preferred_regions.begin(), settings_.preferred_regions.begin() + 1, settings_.preferred_regions.end()); save = true; }
      ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Orders the regions when a game's box art is looked up ('Title (USA)', 'Title (Europe)', ...). A beta, prototype or unlicensed cover is chosen only when nothing else exists.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Offline");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w - lw, "Everything else works offline. Covers are stored in the covers folder next to the library and survive cleanups; removing a cover returns the placeholder.");
    if (button(ctx_, "open_covers", "Open covers folder", ImVec2(ctx_.u(200.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) { std::error_code ec; fs::create_directories(covers_dir_, ec); SDL_OpenURL(("file://" + u8s(covers_dir_)).c_str()); }
  } else if (settings_tab_ == 2) {
    section("BUILD & RUNTIME");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Compatibility, Optimized, the Genesis region, the Master System mapper and H-interrupt discovery are chosen per game on its page (they are part of what a build is). Toolchain: bundled Zig " SEGARECOMP_ZIG_VERSION ".");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Master System Optimized");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), sms_optimized_available_ ? col::ok : col::dim, w - lw, sms_optimized_available_ ? "Offered by this product build. It is the default for Master System games." : (sms_probe_.valid() ? "Checking..." : "Not offered by this product build: Master System games build as Compatibility."));
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Recompiling");
    if (toggle(ctx_, "keep_old", "Keep old build artifacts when a game is recompiled", &settings_.keep_old_artifacts, ctx_.u(420.0F))) save = true;
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Off (default): after a successful recompile the game's older builds (earlier releases, options or modes) are deleted; the new build stays. Turn it on to keep them, e.g. to switch back to an earlier build without recompiling.");
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    section("AFTER A SUCCESSFUL BUILD");
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
    section("VIDEO & CONTROLS DEFAULTS");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, "SDL_Renderer presentation; SDL_GPU is not implemented. Audio uses the default output device; device selection is not implemented.");
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
    if (w > ctx_.u(360.0F)) ImGui::SameLine(0.0F, ctx_.u(18.0F));
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
    section("ABOUT");
    const float vw = w - lw;
    label_value(ctx_, lw, vw, "Version", std::string("Segarecomp ") + SEGARECOMP_LAUNCHER_VERSION);
    label_value(ctx_, lw, vw, "Toolchain", std::string("Zig ") + SEGARECOMP_ZIG_VERSION + " (bundled, used only to compile generated C)");
    label_value(ctx_, lw, vw, "SDL", std::to_string(SDL_VERSIONNUM_MAJOR(SDL_GetVersion())) + "." + std::to_string(SDL_VERSIONNUM_MINOR(SDL_GetVersion())) + "." + std::to_string(SDL_VERSIONNUM_MICRO(SDL_GetVersion())) + ", renderer " + (SDL_GetRendererName(renderer_) != nullptr ? SDL_GetRendererName(renderer_) : "?"));
    label_value(ctx_, lw, vw, "Host", host_description());
    section("WHERE THINGS LIVE");
    label_value(ctx_, lw, vw, "Installation", u8s(layout_.root) + (layout_.problem.empty() ? "" : "  (incomplete)"), layout_.problem.empty() ? col::text : col::err);
    label_value(ctx_, lw, vw, "Cache (disposable)", u8s(cache_dir_) + "  - games/ and zig-cache/ only");
    label_value(ctx_, lw, vw, "Library (kept)", u8s(library_dir_) + "  - library.json, covers, settings, ROMs extracted from ZIPs");
    label_value(ctx_, lw, vw, "Viewer settings", u8s(viewer_settings_file_));
    if (!layout_.problem.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, w, layout_.problem);
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    if (button(ctx_, "open_cache", "Open cache folder", ImVec2(ctx_.u(190.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) SDL_OpenURL(("file://" + u8s(cache_dir_)).c_str());
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "open_lib", "Open library folder", ImVec2(ctx_.u(200.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) { std::error_code ec; fs::create_directories(library_dir_, ec); SDL_OpenURL(("file://" + u8s(library_dir_)).c_str()); }
    section("DIAGNOSTICS");
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
  section_end();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  if (save) save_settings();
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

} // namespace launcher
