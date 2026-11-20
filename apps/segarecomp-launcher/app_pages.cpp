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

fs::path from_u8(const std::string &text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str()))); }

} // namespace

// ---------------------------------------------------------------- import

void App::draw_import(float w, float h) {
  page_header(ctx_, w, "ADD GAME", "Choose or drop your game files; you review them before anything is added.");
  draw_import_body(w, h);
}

// The review of inspected files: what each is (valid / needs a choice / already in the library / cannot be added), then the explicit intent.
void App::draw_review(float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const int preparable = review_preparable_count();
  const int addable = review_count(ReviewItem::Verdict::valid, true) + static_cast<int>(std::count_if(review_.begin(), review_.end(), [](const ReviewItem &i) { return i.selectable() && i.verdict == ReviewItem::Verdict::needs_choice && i.selected; }));
  heading(ctx_, "REVIEW");
  paragraph(ctx_, ctx_.bold, ctx_.u(13.0F), col::text, w, review_summary());
  paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, w,
            "Add & prepare builds a playable version on this computer, one game at a time (several minutes each); you can cancel at any time. Add only keeps the game in your library to prepare later. Nothing starts playing by itself, and your original files are never changed.");
  if (std::any_of(review_.begin(), review_.end(), [](const ReviewItem &i) { return i.sega_cd; }))
    paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::warn, w, "Sega CD support is experimental: a successful preparation does not mean the whole game is playable. Your BIOS and disc are only referenced by path, never copied, uploaded or shipped.");
  ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
  const float bh = ctx_.u(36.0F);
  const std::string prepare_label = preparable > 0 ? "Add & prepare (" + std::to_string(preparable) + ")" : std::string("Add & prepare");
  const std::string add_label = "Add only" + (addable > 0 ? " (" + std::to_string(addable) + ")" : std::string());
  const float pw = std::max(ctx_.u(170.0F), text_width(ctx_.bold, ctx_.u(13.0F), prepare_label.c_str()) + ctx_.u(56.0F)), aw = std::max(ctx_.u(120.0F), text_width(ctx_.bold, ctx_.u(13.0F), add_label.c_str()) + ctx_.u(40.0F));
  if (button(ctx_, "review_prepare", prepare_label.c_str(), ImVec2(pw, bh), Style::primary, preparable > 0 && !import_job_.valid() && !scd_inspect_job_.valid())) commit_review(true);
  ImGui::SameLine(0.0F, ctx_.u(8.0F));
  if (button(ctx_, "review_add", add_label.c_str(), ImVec2(aw, bh), Style::secondary, addable > 0 && !import_job_.valid() && !scd_inspect_job_.valid())) commit_review(false);
  ImGui::SameLine(0.0F, ctx_.u(8.0F));
  if (button(ctx_, "review_decline", "Cancel", ImVec2(ctx_.u(96.0F), bh), Style::ghost)) discard_review();
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  const float text_x = ctx_.u(46.0F), btn_w = ctx_.u(88.0F);
  for (std::size_t i = 0; i < review_.size(); ++i) {
    ReviewItem &item = review_[i];
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool dup = item.verdict == ReviewItem::Verdict::duplicate, bad = item.verdict == ReviewItem::Verdict::invalid, choice = item.verdict == ReviewItem::Verdict::needs_choice;
    const ImU32 color = bad ? col::err : (choice ? col::warn : (dup ? col::info : col::ok));
    const char *verdict = bad ? "Cannot be added" : (choice ? "Needs a choice" : (dup ? "Already in library" : "Valid"));
    const float text_w = w - text_x - ctx_.u(14.0F) - (item.entry_id.empty() ? 0.0F : btn_w + ctx_.u(8.0F));
    const auto lines = wrap(ctx_.regular, ctx_.u(11.5F), text_w, item.message + " " + item.next_action + (item.verdict == ReviewItem::Verdict::valid && !item.prepare_blocked.empty() ? " " + item.prepare_blocked : std::string()));
    // SEG-150-T005: the BIOS step of a Sega CD disc whose tracks are complete: a note, the remembered BIOS (each with its measured state) and one chooser.
    const bool bios_step = item.sega_cd && item.scd.disc.verdict == scd::DiscInspection::Verdict::ready && !dup;
    const float ow = text_w, row_h = ctx_.u(30.0F);
    std::vector<std::string> note_lines;
    std::size_t shown = 0;
    if (bios_step) {
      note_lines = wrap(ctx_.regular, ctx_.u(11.5F), ow, item.scd.note);
      shown = std::min<std::size_t>(item.scd.options.size(), 4U);
    }
    const float bios_h = bios_step ? ctx_.u(10.0F) + static_cast<float>(note_lines.size()) * ctx_.u(15.0F) + static_cast<float>(shown) * row_h + ctx_.u(40.0F) : 0.0F;
    const float rh = ctx_.u(34.0F) + static_cast<float>(lines.size()) * ctx_.u(15.0F) + bios_h;
    card(ctx_, dl, a, ImVec2(a.x + w, a.y + rh));
    ImGui::PushID(static_cast<int>(i));
    if (item.selectable()) {
      ImGui::SetCursorScreenPos(ImVec2(a.x + ctx_.u(10.0F), a.y + ctx_.u(8.0F)));
      checkbox(ctx_, "sel", "", &item.selected);
    } else {
      tone_mark(ctx_, dl, ImVec2(a.x + ctx_.u(22.0F), a.y + ctx_.u(19.0F)), ctx_.u(16.0F), color);
    }
    const char *verdict_text = item.sega_cd && choice ? "Needs your BIOS" : verdict;
    const float verdict_w = text_width(ctx_.bold, ctx_.u(11.5F), verdict_text);
    draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(a.x + text_x, a.y + ctx_.u(8.0F)), ellipsize(ctx_.bold, ctx_.u(13.0F), text_w - verdict_w - ctx_.u(14.0F), item.title).c_str());
    draw_text(dl, ctx_.bold, ctx_.u(11.5F), color, ImVec2(a.x + text_x + text_w - verdict_w, a.y + ctx_.u(9.0F)), verdict_text);
    float ly = a.y + ctx_.u(29.0F);
    for (const auto &l : lines) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), col::text2, ImVec2(a.x + text_x, ly), l.c_str()); ly += ctx_.u(15.0F); }
    if (!item.entry_id.empty()) {
      ImGui::SetCursorScreenPos(ImVec2(a.x + w - btn_w - ctx_.u(10.0F), a.y + ctx_.u(8.0F)));
      if (button(ctx_, "open", "Open", ImVec2(btn_w, ctx_.u(28.0F)), Style::secondary)) open_details(item.entry_id);
    }
    if (bios_step) {
      ly += ctx_.u(6.0F);
      dl->AddRectFilled(ImVec2(a.x + text_x, ly - ctx_.u(3.0F)), ImVec2(a.x + text_x + ow, ly - ctx_.u(2.0F)), rgba(34, 70, 140));
      for (const auto &l : note_lines) { draw_text(dl, ctx_.regular, ctx_.u(11.5F), item.scd.note_error ? col::err : (item.scd.chosen ? col::ok : col::warn), ImVec2(a.x + text_x, ly), l.c_str()); ly += ctx_.u(15.0F); }
      for (std::size_t k = 0; k < shown; ++k) {
        const ScdReviewBios &o = item.scd.options[k];
        const bool in_use = item.scd.chosen && item.scd.bios_sha256 == o.profile.sha256;
        const bool usable = o.state == scd::BiosCheckState::ok;
        std::string line = (o.profile.region.empty() ? std::string("BIOS (region not stated)") : o.profile.region + " BIOS") + " - " + u8s(from_u8(o.profile.path).filename()) + ": ";
        line += usable ? "verified just now" + std::string(o.region_conflict ? ", region differs from the disc" : "") : (o.state == scd::BiosCheckState::missing ? "not at its remembered location" : o.state == scd::BiosCheckState::changed ? "the file changed since it was verified" : "cannot be read");
        const float bx = a.x + text_x + ow - btn_w;
        draw_text(dl, ctx_.regular, ctx_.u(11.5F), usable ? col::text : col::warn, ImVec2(a.x + text_x, ly + ctx_.u(7.0F)), ellipsize(ctx_.regular, ctx_.u(11.5F), ow - btn_w - ctx_.u(10.0F), line).c_str());
        ImGui::SetCursorScreenPos(ImVec2(bx, ly + ctx_.u(1.0F)));
        ImGui::PushID(static_cast<int>(k) + 100);
        if (in_use) draw_text(dl, ctx_.bold, ctx_.u(11.5F), col::ok, ImVec2(bx + ctx_.u(14.0F), ly + ctx_.u(7.0F)), "In use");
        else if (usable) { if (button(ctx_, "use", "Use", ImVec2(btn_w, ctx_.u(26.0F)), Style::secondary)) scd_review_use_profile(i, k); }
        else if (button(ctx_, "locate", "Locate...", ImVec2(btn_w, ctx_.u(26.0F)), Style::secondary)) scd_browse("scd_review_bios", o.profile.sha256 + ":" + item.path);
        ImGui::PopID();
        ly += row_h;
      }
      ImGui::SetCursorScreenPos(ImVec2(a.x + text_x, ly + ctx_.u(2.0F)));
      const char *choose_label = item.scd.chosen || !item.scd.options.empty() ? "Choose another BIOS..." : "Choose BIOS...";
      if (button(ctx_, "choose_bios", choose_label, ImVec2(std::max(ctx_.u(190.0F), text_width(ctx_.bold, ctx_.u(13.0F), choose_label) + ctx_.u(56.0F)), ctx_.u(30.0F)), item.scd.chosen ? Style::secondary : Style::primary, true, ctx_.icon("folder")))
        scd_browse("scd_review_bios", ":" + item.path);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rh + ctx_.u(5.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  }
}

void App::draw_import_body(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (!review_.empty()) draw_review(w);
  draw_drop_zone(w, h, true);
  if (import_job_.valid()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spinner(ctx_, dl, ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info);
    draw_text(dl, ctx_.regular, ctx_.u(13.0F), col::text, ImVec2(p.x + ctx_.u(26.0F), p.y), ("Checking files: " + std::to_string(import_done_.load()) + " of " + std::to_string(import_total_)).c_str());
    ImGui::Dummy(ImVec2(w, ctx_.u(24.0F)));
  }
  if (scd_inspect_job_.valid()) {   // SEG-150-T005: the disc check reads and hashes the track files; Cancel is always available
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spinner(ctx_, dl, ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(15.0F)), ctx_.u(7.0F), col::info);
    draw_text(dl, ctx_.regular, ctx_.u(13.0F), col::text, ImVec2(p.x + ctx_.u(26.0F), p.y + ctx_.u(7.0F)), scd_inspect_cancel_.load() ? "Cancelling the disc check..." : "Checking the disc and its track files...");
    ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(300.0F), p.y));
    if (button(ctx_, "scd_inspect_cancel", "Cancel", ImVec2(ctx_.u(90.0F), ctx_.u(30.0F)), Style::ghost, !scd_inspect_cancel_.load())) scd_inspect_cancel_ = true;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(34.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  }
  if (review_.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, w, "Files are added by reference: Segarecomp never modifies or uploads your originals. A ZIP archive is read without modification and its game is copied into your local library. The same game is recognised by its content, so adding it twice never creates a copy. A Sega CD disc is added from its .cue (the track files stay beside it) together with your own BIOS image: both stay where they are.");
}

// ---------------------------------------------------------------- storage

void App::draw_storage(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool maintenance = maintenance_active() || busy_.any_activity();
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    page_header(ctx_, w, "STORAGE & DATA", "Review disk usage and clean up generated files. ROMs, saves, recordings and mod approvals are never removed.");
    {  // Back: Storage lives under Settings (or the library bulk action that opened it); Escape / B do the same
      const char *back_label = storage_return_ == Page::settings ? "Settings" : "Library";
      const float bw = text_width(ctx_.bold, ctx_.u(13.0F), back_label) + ctx_.u(46.0F);
      ImGui::SetCursorScreenPos(ImVec2(p.x + w - ctx_.u(180.0F) - bw - ctx_.u(8.0F), p.y - ctx_.u(6.0F)));
      if (button(ctx_, "storage_back", back_label, ImVec2(bw, ctx_.u(32.0F)), Style::ghost, true, ctx_.icon("back"))) { go(storage_return_); return; }
    }
    int sort = storage_sort_;
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - ctx_.u(180.0F), p.y - ctx_.u(6.0F)));
    if (dropdown(ctx_, "st_sort", {"Sort by: Name", "Sort by: Size (large first)"}, &sort, ctx_.u(180.0F))) storage_sort_ = sort;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(34.0F)));
  }
  {  // summary
    const float tile_w = (w - ctx_.u(8.0F) * 3) / 4.0F, tile_h = ctx_.u(54.0F);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const std::pair<std::string, std::string> tiles[] = {{"Builds", report_valid_ ? format_bytes(report_.builds_total()) : (scanning_ ? "measuring..." : "-")},
                                                         {"Compiler cache", report_valid_ ? format_bytes(report_.zig_cache + report_.scd_object_cache) + (report_.zig_truncated ? "+" : "") : "-"},
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
  // The table keeps its height cap and scrolls inside; explicit D-pad hops (header checkbox <-> rows <-> action buttons) cover the child boundary, which ImGui's
  // directional scoring skips (SEG-069-T004).
  const float table_h = hh + std::min(static_cast<float>(games.size()) * (rh + ctx_.u(4.0F)) + ctx_.u(6.0F), std::max(ctx_.u(130.0F), h * 0.40F));
  card(ctx_, dl, top, ImVec2(top.x + tw, top.y + table_h), false, false, rgba(7, 17, 40));
  {
    const float x0 = top.x + ctx_.u(40.0F), name_w = std::max(ctx_.u(130.0F), tw * 0.34F);
    const float cols[4] = {x0 + name_w + ctx_.u(20.0F), compact_table ? top.x + tw - ctx_.u(124.0F) : x0 + name_w + ctx_.u(130.0F), x0 + name_w + ctx_.u(235.0F), x0 + name_w + ctx_.u(330.0F)};
    bool all = !games.empty() && std::all_of(games.begin(), games.end(), [&](const LibraryEntry *e) { return storage_selected_.count(e->id()) != 0; });
    ImGui::SetCursorScreenPos(ImVec2(top.x + ctx_.u(10.0F), top.y + ctx_.u(4.0F)));
    if (checkbox(ctx_, "st_all", "", &all)) { if (all) for (const auto *e : games) storage_selected_.insert(e->id()); else storage_selected_.clear(); }
    storage_header_ = ui::ZoneEntry{ImGui::GetItemID(), ImGui::GetCurrentWindow(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())};
    storage_first_row_ = ui::ZoneEntry{};
    storage_last_row_ = ui::ZoneEntry{};
    const float hpx = ctx_.u(11.5F);
    draw_text(dl, ctx_.bold, hpx, col::text2, ImVec2(x0, top.y + ctx_.u(8.0F)), "Game");
    const char *heads[] = {"Total Size", compact_table ? "Status" : "Executable", "Intermediate", "Status"};
    for (int i = 0; i < (compact_table ? 2 : 4); ++i) if (cols[i] + ctx_.u(60.0F) < top.x + tw) draw_text(dl, ctx_.bold, hpx, col::text2, ImVec2(cols[i], top.y + ctx_.u(8.0F)), heads[i]);
    dl->AddRectFilled(ImVec2(top.x + 1, top.y + hh), ImVec2(top.x + tw - 1, top.y + hh + 1), rgba(34, 70, 140));
    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + hh + ctx_.u(3.0F)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0, 0, 0, 0));
    ImGui::BeginChild("##storage_table", ImVec2(tw, table_h - hh - ctx_.u(4.0F)), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
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
      {
        const ui::ZoneEntry row{ImGui::GetItemID(), ImGui::GetCurrentWindow(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())};
        if (storage_first_row_.id == 0) storage_first_row_ = row;
        storage_last_row_ = row;
      }
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
    if (i == 0) storage_after_ = ui::ZoneEntry{ImGui::GetItemID(), ImGui::GetCurrentWindow(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())};
    if (!act.enabled) ImGui::EndDisabled();
    const bool hov = ImGui::IsItemHovered() && act.enabled, foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    if (ImGui::IsItemFocused()) ui::focus_id = std::string("act:") + act.id;
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
  if (maintenance) paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::warn, w, move_.run_job.valid() || move_.cleanup_job.valid() ? "A data location change is running..." : (maintenance_running_ ? "A cleanup is running..." : "Cleanup is unavailable while a build or game is running."));
  paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Cleanup only touches the Segarecomp cache. Your ROM files, covers, screenshots and the library itself are never deleted. Playing never cleans anything. Cache: " + u8s(cache_dir_));
  paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "ROMs extracted from ZIP archives are stored separately from generated builds, in the library's roms folder. Clearing build caches or keeping only metadata does not remove those ROM copies.");
}

// ---------------------------------------------------------------- settings

void App::draw_settings(float full_w, float h) {
  page_header(ctx_, full_w, "SETTINGS", "General, appearance and controls, storage and data, and advanced options.");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool narrow = full_w < ctx_.u(760.0F);
  const float tabs_w = narrow ? 0.0F : ctx_.u(176.0F), tab_gap = narrow ? 0.0F : ctx_.u(18.0F);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  {
    static const char *labels[] = {"General", "Appearance & Controls", "Storage & Data", "Advanced"};
    static const char *icons[] = {"library", "controller", "storage", "settings"};
    settings_tab_ = std::clamp(settings_tab_, 0, 3);
    if (narrow) dropdown(ctx_, "settings_section", {labels[0], labels[1], labels[2], labels[3]}, &settings_tab_, full_w);
    for (int i = 0; !narrow && i < 4; ++i) {
      ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + static_cast<float>(i) * ctx_.u(40.0F)));
      ImGui::PushID(i);
      const bool pressed = ui::nav_button("##tab", ImVec2(tabs_w, ctx_.u(34.0F)));
      const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible, on = settings_tab_ == i;
      if (ImGui::IsItemFocused()) ui::focus_id = "tab#" + std::to_string(i);
      settings_tab_entry_[i] = ui::ZoneEntry{ImGui::GetItemID(), ImGui::GetCurrentWindow(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())};
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
  // Settings groups are titled section cards, the same card language as the Details page (the Input diagnostics block of Appearance & Controls draws its own structure).
  const float sec_pad = ctx_.u(16.0F);
  const float w = w_outer - 2 * sec_pad;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  // SEG-069-T004: the tab column and the body are navigated by explicit hops (Right from a tab enters the body, Left at its left edge returns to the tab):
  // ImGui scores a move from the parent into a child clipped, which skips the body's controls. The page-level entry stays the first tab.
  const ui::ZoneEntry outer_first = ui::zone_first;
  ui::zone_first = ui::ZoneEntry{};
  ImGui::BeginChild("##settings_body", ImVec2(w_outer + ctx_.u(18.0F), std::max(ctx_.u(120.0F), ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);   // minus the spacing after the child: the page itself must not overflow
  settings_body_window_ = ImGui::GetCurrentWindow();
  if (const char *shot_scroll = SDL_getenv("SEGARECOMP_SHOT_SCROLL")) ImGui::SetScrollY(std::string(shot_scroll) == "middle" ? ImGui::GetScrollMaxY() * 0.5F : ImGui::GetScrollMaxY());  // review aid, never a persisted preference
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
    // ---------------------------------------------------------------- General
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
    section("PREPARATION");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Adding a game never starts a build by itself: in Add game you choose Add & prepare or Add only each time (a prepared game can always be rebuilt from its page). Your choices for what happens after a build finishes:");
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("Recompiling");
    if (toggle(ctx_, "keep_old", "Keep old build artifacts when a game is recompiled", &settings_.keep_old_artifacts, ctx_.u(420.0F))) save = true;
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Off (default): after a successful recompile the game's older builds (earlier releases, options or modes) are deleted; the new build stays. Turn it on to keep them, e.g. to switch back to an earlier build without recompiling.");
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    row_label("After a successful build");
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
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
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
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text, w - lw, "Libretro thumbnails (thumbnails.libretro.com, or its libretro-thumbnails mirror on raw.githubusercontent.com when it cannot be reached): Named_Boxarts under 'Sega - Mega Drive - Genesis', 'Sega - Master System - Mark III' and 'Sega - Mega-CD - Sega CD'. Images belong to their rights holders and the provider's terms apply; Segarecomp bundles none.");
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
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    section("RESET");
    if (button(ctx_, "restore", "Restore launcher defaults", ImVec2(ctx_.u(260.0F), ctx_.u(32.0F)), Style::secondary)) { modal_ = Modal{}; modal_.kind = Modal::Kind::restore_defaults; modal_.title = "Restore launcher defaults?"; }
    if (w > ctx_.u(480.0F)) ImGui::SameLine(0.0F, ctx_.u(10.0F));
    if (button(ctx_, "clear_lib", "Clear library...", ImVec2(ctx_.u(200.0F), ctx_.u(32.0F)), Style::danger, !catalog_.entries().empty())) { modal_ = Modal{}; modal_.kind = Modal::Kind::clear_library; modal_.title = "Clear the whole library?"; }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Restoring defaults and clearing the library are separate actions. Neither deletes ROMs or build folders.");
  } else if (settings_tab_ == 1) {
    // ---------------------------------------------------------------- Appearance & Controls
    section("INTERFACE");
    row_label("Size");
    if (toggle(ctx_, "compact", "Compact mode (smaller cards and text)", &settings_.compact, ctx_.u(420.0F))) save = true;
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "The interface scale follows the display's scale and pixel density automatically; Compact mode is the only size choice, and it also applies to the Library.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    section("WINDOW & PICTURE");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, "SDL_Renderer presentation; SDL_GPU is not implemented. Audio uses the default output device; device selection is not implemented.");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "These are the same settings as the game's in-game menu (Esc), stored in the viewer settings file. They apply the next time a game starts. Widescreen choices here are only the defaults for games that have none of their own; changing them in a game's menu saves them for that game only. Key and gamepad bindings are remapped in the game's Controls page.");
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
    row_label("Widescreen");
    int wide = static_cast<int>(v.widescreen);
    if (segmented(ctx_, "ws", {"Original", "Extended Backgrounds", "Enhanced Widescreen"}, &wide, 0.0F)) { v.widescreen = static_cast<SvWidescreenMode>(wide); v_changed = true; }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::text2, w, sv_widescreen_description(v.widescreen));
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Widescreen changes the presentation only, not the console's native resolution. Support depends on the platform and the game; an unsupported mode falls back to the original picture, and the game's Video page says so. A program built before widescreen support needs rebuilding to use it.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Widescreen width");
    if (button(ctx_, "wsw_minus", "-", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { v.widescreen_width = std::max(0, v.widescreen_width - 5); v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    { char ratio[24]; const ImVec2 p = ImGui::GetCursorScreenPos(); sv_widescreen_width_text(v.widescreen_width, ratio, sizeof ratio);
      draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(p.x, p.y + ctx_.u(7.0F)), (std::string(ratio) + " (" + std::to_string(v.widescreen_width) + "%)").c_str()); ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(150.0F), p.y)); }
    if (button(ctx_, "wsw_plus", "+", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { v.widescreen_width = std::min(100, v.widescreen_width + 5); v_changed = true; }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Target aspect between 4:3 and 16:9 when a widescreen mode is on. This is the default for games without their own setting; each game keeps its own Widescreen Mode, Side Presentation and Width from its in-game Video & Display page.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Widescreen aspect");
    {   // SEG-139: the exact display aspect, 4:3 through 32:9. "Slider" keeps the width above; "Custom" is any ratio in between (hundredths, stored as N:100), with its own stepper.
      int ratio_choice = 0;
      const bool set = sv_ratio_valid(v.widescreen_ratio);
      if (set) {
        ratio_choice = 5;
        for (int i = 0; i < SV_RATIO_PRESET_COUNT; ++i) if (v.widescreen_ratio.num == sv_ratio_preset(i).num && v.widescreen_ratio.den == sv_ratio_preset(i).den) ratio_choice = i + 1;
      }
      const auto hundredths = [](SvDisplayRatio r) { return static_cast<int>((static_cast<long long>(r.num) * 100 + r.den / 2) / r.den); };
      const auto clamp_custom = [](int h) { return std::clamp(h, SV_RATIO_CUSTOM_MIN, SV_RATIO_CUSTOM_MAX); };
      const int before = ratio_choice;
      if (segmented(ctx_, "wsr", {"Slider", "4:3", "16:9", "21:9", "32:9", "Custom"}, &ratio_choice, 0.0F) && ratio_choice != before) {
        if (ratio_choice == 0) v.widescreen_ratio = SvDisplayRatio{0, 0};
        else if (ratio_choice <= SV_RATIO_PRESET_COUNT) v.widescreen_ratio = sv_ratio_preset(ratio_choice - 1);
        else {   // Custom starts from what is in effect now
          int num = 0, den = 1;
          if (set) { num = v.widescreen_ratio.num; den = v.widescreen_ratio.den; } else sv_widescreen_width_ratio(v.widescreen_width, &num, &den);
          v.widescreen_ratio = SvDisplayRatio{clamp_custom(hundredths(SvDisplayRatio{num, den})), 100};
        }
        viewer_settings_.ratio_rejected = 0;
        v_changed = true;
      }
      if (set && ratio_choice == 5) {
        const int hv = clamp_custom(hundredths(v.widescreen_ratio));
        if (button(ctx_, "wsr_minus", "-", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { v.widescreen_ratio = SvDisplayRatio{clamp_custom(hv - 5), 100}; v_changed = true; }
        ImGui::SameLine(0.0F, ctx_.u(6.0F));
        { char r[24]; const ImVec2 p = ImGui::GetCursorScreenPos(); sv_ratio_format(v.widescreen_ratio, r, sizeof r);
          draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(p.x, p.y + ctx_.u(7.0F)), (std::string(r) + " (" + std::to_string(hv / 100) + "." + (hv % 100 < 10 ? "0" : "") + std::to_string(hv % 100) + ":1)").c_str());
          ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(150.0F), p.y)); }
        ImGui::SameLine(0.0F, ctx_.u(6.0F));
        if (button(ctx_, "wsr_plus", "+", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { v.widescreen_ratio = SvDisplayRatio{clamp_custom(hv + 5), 100}; v_changed = true; }
        paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Custom aspect in steps of 0.05, from 1.34:1 to 3.55:1.");
      }
      if (viewer_settings_.ratio_rejected) paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "The saved widescreen aspect was not a ratio between 4:3 and 32:9 and was ignored; pick one above to replace it.");
    }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "A fixed aspect (4:3 to 32:9) replaces the width above. Beyond 16:9 the extra area shows more of the game's own background and sprites, never new gameplay. A patch that declares a narrower supported range is shown the native picture only outside it (the in-game Video page says so).");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Side presentation");
    int side_style = static_cast<int>(v.widescreen_side_style);
    if (segmented(ctx_, "ss", {"Comfort", "Borders", "Raw Extension"}, &side_style, 0.0F)) { v.widescreen_side_style = static_cast<SvSideStyle>(side_style); v_changed = true; }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::text2, w, sv_side_style_description(v.widescreen_side_style));
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Only the extra side area is affected, never the game picture or the game. Comfort and Borders are decorative and do not add gameplay visibility; a verified game-specific widescreen mode is always shown in full under Comfort.");
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
    section("AUDIO");
    row_label("Volume");
    if (button(ctx_, "vol_minus", "-", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { viewer_settings_.audio.volume = std::max(0, viewer_settings_.audio.volume - 10); v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    { const ImVec2 p = ImGui::GetCursorScreenPos(); draw_text(dl, ctx_.bold, ctx_.u(13.0F), col::text, ImVec2(p.x, p.y + ctx_.u(7.0F)), ("Volume " + std::to_string(viewer_settings_.audio.volume) + "%").c_str()); ImGui::SetCursorScreenPos(ImVec2(p.x + ctx_.u(120.0F), p.y)); }
    if (button(ctx_, "vol_plus", "+", ImVec2(ctx_.u(34.0F), ctx_.u(28.0F)), Style::secondary)) { viewer_settings_.audio.volume = std::min(100, viewer_settings_.audio.volume + 10); v_changed = true; }
    ImGui::SameLine(0.0F, ctx_.u(14.0F));
    if (toggle(ctx_, "mu", "Mute", &muted, ctx_.u(120.0F))) { viewer_settings_.audio.muted = muted; v_changed = true; }
    ImGui::Dummy(ImVec2(1, ctx_.u(10.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Volume is playback gain only; the console's generated sound is unchanged. Genesis has two ports (Player 2: I/K/J/L, U/O/P, Right Shift); Master System Player 2 uses the same keys. The Master System Pause button is P, separate from the menu key (Esc).");
    if (button(ctx_, "viewer_defaults", "Restore video and audio defaults", ImVec2(ctx_.u(300.0F), ctx_.u(30.0F)), Style::secondary)) { SvVideoSettings keep = v; sv_video_defaults(&v); v.window_w = keep.window_w; v.window_h = keep.window_h; viewer_settings_.audio.volume = 100; viewer_settings_.audio.muted = 0; v_changed = true; }
    if (v_changed) viewer_settings_changed();
    section("KEYBOARD & GAMEPAD");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "In the launcher: arrow keys or the D-pad move; Enter or A (South) activates; Esc or B (East) goes back; Tab switches between the sidebar and the page. A controller needs no setup to navigate; if one is not recognised, use Input diagnostics below.");
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "In a game, Esc (or Back / Home on a controller) opens the pause menu, where key and button assignments are changed; they are stored in the same viewer settings file as the defaults above.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    section_end();
    draw_input_diagnostics(w_outer);
  } else if (settings_tab_ == 2) {
    // ---------------------------------------------------------------- Storage & Data
    if (!settings_scan_asked_) { settings_scan_asked_ = true; request_scan(); }   // one bounded read-only measurement per visit
    if (!inventory_asked_) { inventory_asked_ = true; refresh_completed_move(); request_inventory(); }   // SEG-150-T009: the size of the data by class, measured once per visit (read-only)
    const EffectivePaths ep = effective_paths(layout_);
    const float vw = w - lw;
    section("DISK USAGE");
    label_value(ctx_, lw, vw, "Built games", report_valid_ ? format_bytes(report_.builds_total()) : (scanning_ ? "measuring..." : "not measured"));
    label_value(ctx_, lw, vw, "Compiler cache", report_valid_ ? format_bytes(report_.zig_cache + report_.scd_object_cache) + (report_.zig_truncated ? " or more" : "") : (scanning_ ? "measuring..." : "not measured"));
    label_value(ctx_, lw, vw, "Library data", report_valid_ ? format_bytes(report_.catalog_and_covers) : (scanning_ ? "measuring..." : "not measured"));
    label_value(ctx_, lw, vw, "Free disk space", report_valid_ ? format_bytes(report_.free_bytes) : (scanning_ ? "measuring..." : "not measured"), report_valid_ && report_.free_bytes < (2ULL << 30U) ? col::err : col::text);
    if (report_valid_ && report_.free_bytes < (2ULL << 30U)) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, w, "Low disk space: builds need room for generated code and objects.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    section("CLEAN UP");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Clear the shared compiler cache, remove build intermediates or old builds, clean failed or abandoned builds, or keep a game's metadata only. Every plan shows what it will delete and the bytes it frees before anything happens, and nothing is deleted without your confirmation.");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::ok, w, "Never touched by any cleanup: your ROM and disc files, save data and save states, input recordings, covers, mod approvals, the library and these settings.");
    ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    if (button(ctx_, "go_storage", "Review usage and clean up", ImVec2(ctx_.u(250.0F), ctx_.u(32.0F)), Style::primary, true, ctx_.icon("storage"))) go(Page::storage);
    if (settings_focus_storage_) {   // Back from the cleanup page lands here again
      settings_focus_storage_ = false;
      ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
      ImGuiContext &fg = *ImGui::GetCurrentContext();
      fg.NavCursorVisible = true;
      fg.NavHighlightItemUnderNav = true;
    }
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    section("DATA LOCATION");
    // SEG-150-T009 (ADR 0317): where the data lives, how much of it is kept versus rebuildable, and the control that moves it (disabled, with the reason, under an environment override).
    const bool env_governed = ep.cache_overridden || ep.library_overridden;
    const bool move_active = data_move_running();
    if (recovery_.action != RecoveryAction::none && recovery_.action != RecoveryAction::kept_completed && !recovery_dismissed_) {   // shown once, never blocking
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), recovery_.action == RecoveryAction::completed ? col::ok : col::warn, w, recovery_message_text(recovery_.action));
      if (button(ctx_, "dm_notice_ok", "OK", ImVec2(ctx_.u(90.0F), ctx_.u(28.0F)), Style::secondary)) recovery_dismissed_ = true;
      if (recovery_.action == RecoveryAction::journal_invalid) {
        ImGui::SameLine(0.0F, ctx_.u(8.0F));
        if (button(ctx_, "dm_notice_forget", "Forget this record", ImVec2(ctx_.u(190.0F), ctx_.u(28.0F)), Style::secondary, !move_active)) {
          if (dismiss_completed_migration(ep.bootstrap_file)) { recovery_ = RecoveryResult{}; toast("The unreadable record was forgotten. Your data was not touched."); }
          else toast("The record could not be removed.", true);
        }
      }
      ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    }
    if (!ep.data_location_notice.empty())   // a saved location that is not in use: unavailable drive, damaged or newer file, shadowed by the environment
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, ep.data_location_notice);
    if (restart_needed_for_move())
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, "Restart required: your data was moved to " + u8s(read_bootstrap(ep.bootstrap_file).root) + " and that location is used from the next start. Until then this window keeps using the previous location.");
    label_value(ctx_, lw, vw, "Data folder", u8s(ep.cache) + (ep.cache_overridden ? "  - set by SEGARECOMP_CACHE_DIR" : ep.relocated ? "  - a location you chose" : "  - default location"));
    label_value(ctx_, lw, vw, "Library (kept)", u8s(ep.library) + (ep.library_overridden ? "  - set by SEGARECOMP_LIBRARY_DIR" : "  - library folder inside the data folder"));
    if (inventory_valid_ && !env_governed && inventory_.areas.size() > 0) {
      std::size_t kept_files = inventory_.items.size();
      label_value(ctx_, lw, vw, "Your data", std::to_string(kept_files) + (kept_files == 1 ? " file, " : " files, ") + format_bytes(inventory_.durable_bytes) + "  - catalog, settings, covers, mods, saves, recordings, ROMs unpacked from ZIPs; this is what a move copies");
      label_value(ctx_, lw, vw, "Rebuildable", format_bytes(inventory_.regenerable_bytes) + "  - prepared games, compiler cache and downloads; a move does not copy them");
    } else if (!env_governed) {
      label_value(ctx_, lw, vw, "Your data", inventory_job_.valid() ? "measuring..." : "not measured", col::dim);
    }
    label_value(ctx_, lw, vw, "Installation", u8s(ep.home) + (ep.home_overridden ? "  - set by SEGARECOMP_HOME" : "  - where this program runs from") + (layout_.problem.empty() ? "" : "  (incomplete)"), layout_.problem.empty() ? col::text : col::err);
    if (!layout_.problem.empty()) paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::err, w, layout_.problem);
    ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    dm_change_state_ = move_active ? "progress" : (env_governed ? "disabled" : "enabled");
    if (env_governed) {
      const PlainText t = refusal_text(ep.cache_overridden ? MigrationRefusal::env_override_cache : MigrationRefusal::env_override_library);
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::warn, w, t.what);
      paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, w, t.next);
    } else {
      paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::text2, w, "Move your library, saves, mods and settings to another folder or drive. You see exactly what moves and what it needs first, your current data is kept, and nothing is deleted automatically.");
    }
    if (button(ctx_, "dm_change", move_active ? "Show progress" : "Change data location...", ImVec2(ctx_.u(250.0F), ctx_.u(32.0F)), Style::secondary, env_governed ? false : true, ctx_.icon("folder"))) go(Page::data_location);
    if (settings_focus_move_) {   // Back from the data-location page lands here again
      settings_focus_move_ = false;
      ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
      ImGuiContext &fg = *ImGui::GetCurrentContext();
      fg.NavCursorVisible = true;
      fg.NavHighlightItemUnderNav = true;
    }
    if (completed_move_.present) {
      ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
      paragraph(ctx_, ctx_.bold, ctx_.u(12.5F), col::text, w, "Previous location");
      paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, completed_move_.source.empty() ? "A record of an earlier data move cannot be used." : "Your data was moved. The old folder " + u8s(completed_move_.source) + " still holds the previous copy. Segarecomp never removes it by itself.");
      if (button(ctx_, "dm_prev_clean", "Clean up the previous location...", ImVec2(ctx_.u(300.0F), ctx_.u(30.0F)), Style::secondary, !completed_move_.source.empty() && !move_active)) { go(Page::data_location); data_cleanup_open(); }
      ImGui::SameLine(0.0F, ctx_.u(8.0F));
      if (button(ctx_, "dm_prev_keep", "Keep it", ImVec2(ctx_.u(110.0F), ctx_.u(30.0F)), Style::ghost, !move_active)) data_dismiss_previous();
    }
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "A change takes effect the next time Segarecomp starts. SEGARECOMP_CACHE_DIR and SEGARECOMP_LIBRARY_DIR, when set in the environment that starts the launcher, always take precedence over any chosen location.");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    if (button(ctx_, "open_cache", "Open data folder", ImVec2(ctx_.u(190.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) SDL_OpenURL(("file://" + u8s(ep.cache)).c_str());
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "open_lib", "Open library folder", ImVec2(ctx_.u(200.0F), ctx_.u(30.0F)), Style::secondary, true, ctx_.icon("folder"))) { std::error_code ec; fs::create_directories(ep.library, ec); SDL_OpenURL(("file://" + u8s(ep.library)).c_str()); }
  } else {
    // ---------------------------------------------------------------- Advanced
    const float vw = w - lw;
    section("BUILD & RUNTIME");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w, "Compatibility, Optimized, the Genesis region, the Master System mapper and H-interrupt discovery are chosen per game on its page (they are part of what a build is). Toolchain: bundled Zig " SEGARECOMP_ZIG_VERSION ".");
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    row_label("Master System Optimized");
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), sms_optimized_available_ ? col::ok : col::dim, w - lw, sms_optimized_available_ ? "Offered by this product build. It is the default for Master System games." : (sms_probe_.valid() ? "Checking..." : "Not offered by this product build: Master System games build as Compatibility."));
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    section("ABOUT");
    label_value(ctx_, lw, vw, "Version", std::string("Segarecomp ") + SEGARECOMP_LAUNCHER_VERSION);
    label_value(ctx_, lw, vw, "Toolchain", std::string("Zig ") + SEGARECOMP_ZIG_VERSION + " (bundled, used only to compile generated C)");
    label_value(ctx_, lw, vw, "SDL", std::to_string(SDL_VERSIONNUM_MAJOR(SDL_GetVersion())) + "." + std::to_string(SDL_VERSIONNUM_MINOR(SDL_GetVersion())) + "." + std::to_string(SDL_VERSIONNUM_MICRO(SDL_GetVersion())) + ", renderer " + (SDL_GetRendererName(renderer_) != nullptr ? SDL_GetRendererName(renderer_) : "?"));
    label_value(ctx_, lw, vw, "Host", host_description());
    label_value(ctx_, lw, vw, "Installation", u8s(layout_.root) + (layout_.problem.empty() ? "" : "  (incomplete)"), layout_.problem.empty() ? col::text : col::err);
    label_value(ctx_, lw, vw, "Viewer settings", u8s(viewer_settings_file_));
    {   // the license texts the installation ships (MPL-2.0 and the bundled components'): shown with their folder only when it exists
      std::error_code ec;
      const fs::path licenses = layout_.root / "licenses";
      const bool have_licenses = fs::is_directory(licenses, ec);
      label_value(ctx_, lw, vw, "Licenses", have_licenses ? u8s(licenses) : "not found in this installation (a development build does not ship them)", have_licenses ? col::text : col::dim);
      ImGui::Dummy(ImVec2(1, ctx_.u(2.0F)));
      if (button(ctx_, "open_licenses", "Open licenses folder", ImVec2(ctx_.u(210.0F), ctx_.u(30.0F)), Style::secondary, have_licenses, ctx_.icon("folder"))) SDL_OpenURL(("file://" + u8s(licenses)).c_str());
    }
    section("DIAGNOSTICS");
    label_value(ctx_, lw, vw, "Library", std::to_string(catalog_.entries().size()) + " game(s), schema " + std::to_string(Catalog::schema_version) + (catalog_.read_only() ? " (read-only: written by a newer release)" : ""));
    if (!catalog_notice_.empty()) label_value(ctx_, lw, vw, "Notice", catalog_notice_, col::warn);
    if (button(ctx_, "adopt_now", "Adopt existing builds now", ImVec2(ctx_.u(250.0F), ctx_.u(30.0F)), Style::secondary, !catalog_.read_only())) {
      const std::size_t added = catalog_.migrate_cache(cache_dir_, now_seconds());
      save_catalog();
      request_status_refresh();
      toast(added == 0 ? "No new verifiable builds found in the cache." : "Adopted " + std::to_string(added) + " game(s) from the cache.");
    }
    paragraph(ctx_, ctx_.regular, ctx_.u(11.0F), col::dim, w, "Logs: a build failure is shown on the game's page with its sanitized log (Game > Advanced); every build keeps its build.log inside its folder in the cache (Open cache folder, under Storage & Data). Nothing about you is sent anywhere: the only network use is the optional Libretro cover lookup described under General.");
  }

  section_end();
  if (settings_tab_ != 2) settings_focus_storage_ = false;   // the pending focus return belongs to the Storage & Data group only
  ImGui::EndChild();
  settings_body_entry_ = ui::zone_first;   // where Right from a tab lands: the body's first control
  ui::zone_first = outer_first;
  settings_narrow_ = narrow;
  if (narrow) for (auto &tab : settings_tab_entry_) tab = outer_first;  // the section drop-down is the "tab" of the narrow layout (Down enters the body, Up returns)
  ImGui::PopStyleVar();
  if (save) save_settings();
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
  (void)h;
}

} // namespace launcher
