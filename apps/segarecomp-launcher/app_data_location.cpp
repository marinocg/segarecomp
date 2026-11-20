// SEG-150-T009 (ADR 0317): Settings > Storage & Data > Change data location. This file is only the screen and the worker plumbing around the T008 core
// (data_location.hpp): the plan, the migration with its phases and cancel, and the separate old-location cleanup all run on worker threads, and every outcome shown here
// is the core's typed result. Nothing here decides what is safe to move or remove.

#include "app.hpp"

#include "app_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace fs = std::filesystem;
using namespace ui;
using namespace launcher::uih;

namespace launcher {

namespace {

bool ready(const auto &future) { return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }

const char *area_title(const std::string &area) {
  if (area == "catalog") return "Game library (the catalog)";
  if (area == "settings") return "Settings";
  if (area == "bios_profiles") return "Sega CD BIOS records";
  if (area == "covers") return "Cover images";
  if (area == "managed_roms") return "ROMs unpacked from ZIP files";
  if (area == "mods") return "Installed mods";
  if (area == "saves") return "Saves and save states";
  if (area == "recordings") return "Input recordings";
  if (area == "sega_cd_data") return "Sega CD backup RAM and recordings";
  if (area == "builds") return "Prepared games (builds)";
  if (area == "compiler_cache") return "Compiler cache";
  if (area == "downloads") return "Cover lookup downloads";
  if (area == "catalog_backup") return "Library backup copy";
  if (area == "migrated_copy") return "Old copies of moved data";
  return "Other";
}

std::string count_and_bytes(std::size_t entries, std::uintmax_t bytes) {
  return std::to_string(entries) + (entries == 1 ? " item, " : " items, ") + format_bytes(bytes);
}

} // namespace

const char *recovery_message_text(RecoveryAction action) {
  switch (action) {
    case RecoveryAction::rolled_back: return "A data move was interrupted last time. It was rolled back: your data was never switched, and the unfinished copy was removed. Your previous location is still in use.";
    case RecoveryAction::completed: return "A data move was interrupted just after it switched. It is now recorded as complete: your data lives at the new location, and the previous location was left as it was.";
    case RecoveryAction::owner_alive: return "Another Segarecomp window is still moving your data. This one did not touch that move.";
    case RecoveryAction::journal_invalid: return "A record of an earlier data move cannot be read (damaged, or written by a newer version) and was left alone. Moving data stays unavailable until you choose Forget this record (your data is not touched).";
    case RecoveryAction::rollback_incomplete: return "An interrupted data move could not be fully rolled back yet; Segarecomp tries again at every start. Your data was never switched, and the previous location is still in use.";
    default: return "";
  }
}


// ---------------------------------------------------------------- state and workers

const char *App::move_stage_name() const {
  switch (move_.stage) {
    case MoveStage::choose: return "choose";
    case MoveStage::planning: return "planning";
    case MoveStage::plan: return "plan";
    case MoveStage::running: return "running";
    case MoveStage::finished: return "finished";
    case MoveStage::cleanup_planning: return "cleanup_planning";
    case MoveStage::cleanup: return "cleanup";
    case MoveStage::cleaning: return "cleaning";
    case MoveStage::cleaned: return "cleaned";
  }
  return "?";
}

// The new location takes effect when the process starts: the bootstrap is read once. True when the saved location is not the one this process resolved. Evaluated in
// refresh_completed_move() (not per frame: it reads the pointer file).
bool App::restart_needed_for_move() const { return restart_needed_; }

MigrationOptions App::data_move_options(const fs::path &destination) const {
  const EffectivePaths ep = effective_paths(layout_);
  MigrationOptions o;
  o.source = ep.cache;
  o.destination = destination;
  o.env = data_environment();
  o.bootstrap_file = ep.bootstrap_file;
  o.protected_paths = {layout_.root};   // the installation: a data folder inside it would be wiped by an update
  if (const char *seam = SDL_getenv("SEGARECOMP_TEST_FREE_SPACE_BYTES")) {   // a test seam (like SEGARECOMP_SCD_BUILD_ARGS), never set by Segarecomp
    const std::uintmax_t bytes = static_cast<std::uintmax_t>(std::strtoull(seam, nullptr, 10));
    o.free_space = [bytes](const fs::path &) { return bytes; };
  }
  return o;
}

void App::refresh_completed_move() {
  const EffectivePaths ep = effective_paths(layout_);
  completed_move_ = ep.cache_overridden ? CompletedMigration{} : read_completed_migration(ep.bootstrap_file);
  restart_needed_ = move_done_this_session_;
  if (!restart_needed_ && !ep.cache_overridden) {
    const Bootstrap boot = read_bootstrap(ep.bootstrap_file);
    std::error_code ec;
    restart_needed_ = boot.state == BootstrapState::valid && !(boot.root == ep.cache || fs::equivalent(boot.root, ep.cache, ec));
  }
}

void App::request_inventory() {
  if (inventory_job_.valid()) return;
  const MigrationOptions o = data_move_options(fs::path{});
  inventory_job_ = std::async(std::launch::async, [o] { return build_migration_plan(o); });
}

void App::data_move_enter() {
  refresh_completed_move();
  if (move_.stage == MoveStage::running || move_.stage == MoveStage::cleaning || move_.stage == MoveStage::finished || move_.stage == MoveStage::cleaned) return;   // the work or its result is still being shown
  data_move_reset();
}

void App::data_move_reset() {
  if (data_move_running()) return;
  move_.plan_cancel = true;
  move_.stage = MoveStage::choose;
  move_.destination.clear();
  move_.pending_destination.clear();
  move_.plan = MigrationPlan{};
  move_.result = MigrationResult{};
  move_.cleanup_plan = CleanupPlan{};
  move_.cleanup_result = CleanupResult{};
  move_.cleanup_refusal.clear();
  move_.include_copies = false;
  move_.cancel_requested = false;
  move_.focus_pending = true;
  { std::lock_guard<std::mutex> lock(move_.mutex); move_.progress = MigrationProgress{}; }
}

// A folder chosen by either chooser: canonicalized once (the core assumes a canonical path; macOS /tmp and /var are links), then planned on a worker.
void App::data_move_set_destination(const fs::path &destination) {
  if (data_move_running()) return;
  std::error_code ec;
  fs::path canonical = fs::weakly_canonical(destination, ec);
  if (ec || canonical.empty()) canonical = destination;
  data_move_start_plan(canonical);
}

void App::data_move_start_plan(const fs::path &destination) {
  move_.destination = destination;
  move_.plan = MigrationPlan{};
  move_.stage = MoveStage::planning;
  move_.focus_pending = true;
  if (move_.plan_job.valid()) {   // an earlier plan is still being computed: its result is discarded and this one starts right after it
    move_.pending_destination = destination;
    move_.plan_cancel = true;
    return;
  }
  move_.plan_cancel = false;
  const MigrationOptions o = data_move_options(destination);
  move_.plan_job = std::async(std::launch::async, [o, cancel = &move_.plan_cancel] { return build_migration_plan(o, cancel); });
  trace("data_move=planning");
}

// The chosen folder already holds files: offer a new folder inside it (the core creates it only when the move starts; a plan creates nothing).
void App::data_move_use_subfolder() {
  if (move_.stage != MoveStage::plan || data_move_running()) return;
  data_move_start_plan(move_.destination / "Segarecomp");
}

void App::data_move_confirm() {
  if (move_.stage != MoveStage::plan || !move_.plan.ok() || data_move_running()) return;
  if (restart_needed_for_move()) { toast("Restart Segarecomp before moving the data again.", true); return; }
  move_.cancel = false;
  move_.cancel_requested = false;
  move_.result = MigrationResult{};
  { std::lock_guard<std::mutex> lock(move_.mutex); move_.progress = MigrationProgress{"plan", 0, 0, 0, 0, ""}; }
  move_.stage = MoveStage::running;
  move_.focus_pending = true;
  const MigrationOptions o = data_move_options(move_.destination);
  trace("data_move=started");
  move_.run_job = std::async(std::launch::async, [this, o] {
    // Test seams only (never set by Segarecomp): a pause after each copied file so a script can cancel mid-copy, and a simulated process death at a core hook point.
    MigrationHooks hooks;
    const char *hold = SDL_getenv("SEGARECOMP_TEST_MIGRATION_HOLD_MS");
    const long hold_ms = hold != nullptr ? std::strtol(hold, nullptr, 10) : 0;
    if (hold_ms > 0) hooks.after_copy = [hold_ms](const fs::path &) { std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms)); };
    if (const char *crash = SDL_getenv("SEGARECOMP_TEST_MIGRATION_CRASH")) {   // "<point>" or "<point>:<index>", e.g. copy:1, verify, commit_before
      const std::string text = crash;
      const std::string point = text.substr(0, text.find(':'));
      const std::size_t index = text.find(':') == std::string::npos ? 0 : static_cast<std::size_t>(std::strtoul(text.c_str() + text.find(':') + 1, nullptr, 10));
      hooks.crash_at = [point, index](const char *at, std::size_t i) { return point == at && (point != "copy" || i == index); };
    }
    return run_migration(o, busy_, &move_.cancel, [this](const MigrationProgress &p) { std::lock_guard<std::mutex> lock(move_.mutex); move_.progress = p; }, &hooks);
  });
}

void App::data_move_request_cancel() {
  if (!move_.run_job.valid()) return;
  std::string phase;
  { std::lock_guard<std::mutex> lock(move_.mutex); phase = move_.progress.phase; }
  if (phase == "commit" || phase == "done") return;   // too late: the switch is being made
  move_.cancel = true;
  move_.cancel_requested = true;
  trace("data_move=cancel_requested phase=" + phase);
}

void App::data_move_back() {
  switch (move_.stage) {
    case MoveStage::running:
    case MoveStage::cleaning:
      go(Page::settings);   // the work continues; Settings shows where it stands
      return;
    case MoveStage::finished:
    case MoveStage::cleaned:
      data_move_reset();
      go(Page::settings);
      return;
    default:
      trace(move_.stage == MoveStage::plan ? "data_move=declined" : "data_move=left");
      data_move_reset();
      go(Page::settings);
      return;
  }
}

void App::data_cleanup_open() {
  if (data_move_running()) return;
  if (page_ != Page::data_location) go(Page::data_location);   // resets to `choose`; the cleanup stage is set below
  refresh_completed_move();
  move_.cleanup_refusal.clear();
  move_.cleanup_plan = CleanupPlan{};
  move_.cleanup_result = CleanupResult{};
  move_.include_copies = false;
  move_.prev_source = completed_move_.source;
  move_.prev_destination = completed_move_.destination;
  if (restart_needed_for_move()) { move_.cleanup_refusal = "restart_needed"; move_.stage = MoveStage::cleanup; move_.focus_pending = true; return; }
  data_cleanup_replan();
}

void App::data_cleanup_replan() {
  if (move_.cleanup_plan_job.valid() || data_move_running()) return;
  move_.stage = MoveStage::cleanup_planning;
  move_.focus_pending = true;
  const EffectivePaths ep = effective_paths(layout_);
  move_.cleanup_plan_job = std::async(std::launch::async, [file = ep.bootstrap_file, env = data_environment(), copies = move_.include_copies] { return plan_old_root_cleanup(file, env, copies); });
}

void App::data_cleanup_confirm() {
  if (move_.stage != MoveStage::cleanup || !move_.cleanup_plan.eligible || move_.cleanup_plan.items.empty() || data_move_running()) return;
  move_.cancel = false;
  move_.stage = MoveStage::cleaning;
  move_.focus_pending = true;
  const EffectivePaths ep = effective_paths(layout_);
  trace("data_cleanup=started items=" + std::to_string(move_.cleanup_plan.items.size()));
  move_.cleanup_job = std::async(std::launch::async, [this, file = ep.bootstrap_file, env = data_environment(), copies = move_.include_copies] { return run_old_root_cleanup(file, env, copies, busy_, &move_.cancel); });
}

void App::data_dismiss_previous() {
  if (data_move_running()) return;
  const EffectivePaths ep = effective_paths(layout_);
  if (dismiss_completed_migration(ep.bootstrap_file)) {
    trace("data_move=previous_dismissed");
    toast("Kept. The previous location is left as it is and will not be offered for cleanup again.");
  } else toast("The record of the previous location could not be cleared.", true);
  refresh_completed_move();
  if (move_.stage == MoveStage::cleanup || move_.stage == MoveStage::cleaned) data_move_reset();
}

void App::poll_data_move() {
  if (ready(inventory_job_)) { inventory_ = inventory_job_.get(); inventory_valid_ = true; }
  if (ready(move_.plan_job)) {
    MigrationPlan plan = move_.plan_job.get();
    if (!move_.pending_destination.empty()) {   // a newer choice was made meanwhile
      const fs::path next = move_.pending_destination;
      move_.pending_destination.clear();
      data_move_start_plan(next);
    } else if (move_.stage == MoveStage::planning) {
      move_.plan = std::move(plan);
      move_.stage = MoveStage::plan;
      move_.focus_pending = true;
      std::string codes;
      for (const auto &r : move_.plan.refusals) codes += (codes.empty() ? "" : ",") + std::string(refusal_name(r.code));
      trace(move_.plan.ok() ? "data_move=plan ok=1 files=" + std::to_string(move_.plan.items.size()) + " bytes=" + std::to_string(move_.plan.durable_bytes) : "data_move=plan ok=0 refused=" + codes);
    }
  }
  if (ready(move_.run_job)) {
    move_.result = move_.run_job.get();
    move_.stage = MoveStage::finished;
    move_.focus_pending = true;
    static const char *names[] = {"completed", "refused", "busy", "cancelled", "failed", "interrupted"};
    trace(std::string("data_move=finished outcome=") + names[static_cast<int>(move_.result.outcome)] + " reason=" + (move_.result.reason.empty() ? "-" : move_.result.reason));
    if (move_.result.ok()) { move_done_this_session_ = true; refresh_completed_move(); }
    request_scan();
  }
  if (ready(move_.cleanup_plan_job)) {
    move_.cleanup_plan = move_.cleanup_plan_job.get();
    move_.stage = MoveStage::cleanup;
    move_.focus_pending = true;
    trace(move_.cleanup_plan.eligible ? "data_cleanup=plan items=" + std::to_string(move_.cleanup_plan.items.size()) + " bytes=" + std::to_string(move_.cleanup_plan.bytes) : "data_cleanup=refused reason=" + move_.cleanup_plan.reason);
  }
  if (ready(move_.cleanup_job)) {
    move_.cleanup_result = move_.cleanup_job.get();
    move_.stage = MoveStage::cleaned;
    move_.focus_pending = true;
    trace("data_cleanup=finished removed=" + std::to_string(move_.cleanup_result.removed) + " freed=" + std::to_string(move_.cleanup_result.bytes_freed) + " problems=" + std::to_string(move_.cleanup_result.problems.size()));
    refresh_completed_move();
    request_scan();
  }
}

// ---------------------------------------------------------------- screen

// A stage's safe default control (Cancel on a confirmation, Done on a result) takes keyboard / gamepad focus once when the stage appears.
void App::note_move_focus_default() {
  if (!move_.focus_pending) return;
  move_.focus_pending = false;
  ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
  ImGuiContext &g = *ImGui::GetCurrentContext();
  if (g.NavInputSource == ImGuiInputSource_Keyboard || g.NavInputSource == ImGuiInputSource_Gamepad) { g.NavCursorVisible = true; g.NavHighlightItemUnderNav = true; }
}

void App::draw_data_location(float w, float h) {
  (void)h;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const EffectivePaths ep = effective_paths(layout_);
  const float pad = ctx_.u(16.0F), iw = w - 2.0F * pad, bh = ctx_.u(34.0F);

  // ---- header with Back ----
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    page_header(ctx_, w, "CHANGE DATA LOCATION", "");
    const float bw = text_width(ctx_.bold, ctx_.u(13.0F), "Settings") + ctx_.u(46.0F);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw, p.y - ctx_.u(6.0F)));
    if (button(ctx_, "dm_back", "Settings", ImVec2(bw, ctx_.u(32.0F)), Style::ghost, true, ctx_.icon("back"))) { data_move_back(); return; }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ctx_.u(34.0F)));
  }

  // ---- cards (the same card language as the Settings groups) ----
  ImVec2 card_origin(0, 0);
  bool card_open = false;
  const auto card_end = [&] {
    if (!card_open) return;
    ImGui::Unindent(pad);
    const float end_y = ImGui::GetCursorScreenPos().y + ctx_.u(6.0F);
    dl->ChannelsSetCurrent(0);
    card(ctx_, dl, card_origin, ImVec2(card_origin.x + w, end_y), false, false, rgba(8, 19, 46, 240));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(card_origin.x, end_y + ctx_.u(12.0F)));
    ImGui::Dummy(ImVec2(w, 0.0F));
    card_open = false;
  };
  const auto card_begin = [&](const char *title) {
    card_end();
    card_origin = ImGui::GetCursorScreenPos();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    draw_text(dl, ctx_.display, ctx_.u(12.5F), rgba(120, 205, 255), ImVec2(card_origin.x + pad, card_origin.y + ctx_.u(13.0F)), title);
    ImGui::SetCursorScreenPos(ImVec2(card_origin.x, card_origin.y + ctx_.u(38.0F)));
    ImGui::Indent(pad);
    ImGui::SetCursorScreenPos(ImVec2(card_origin.x + pad, card_origin.y + ctx_.u(38.0F)));
    card_open = true;
  };
  const auto text = [&](ImU32 color, const std::string &s, float px = 12.0F) { paragraph(ctx_, ctx_.regular, ctx_.u(px), color, iw, s); };
  const auto gap = [&](float g) { ImGui::Dummy(ImVec2(1, ctx_.u(g))); };
  // A row of buttons that wraps when the next one would pass the card's edge.
  float row_right = -1.0F;
  const auto row_begin = [&] { row_right = -1.0F; };
  const auto row_button = [&](const char *id, const char *label, Style style, bool enabled = true, const char *icon = nullptr) {
    const float bw = std::max(ctx_.u(96.0F), text_width(ctx_.bold, ctx_.u(13.0F), label) + (icon != nullptr ? ctx_.u(62.0F) : ctx_.u(40.0F)));
    if (row_right >= 0.0F && row_right + ctx_.u(8.0F) + bw <= card_origin.x + w - pad) ImGui::SameLine(0.0F, ctx_.u(8.0F));
    const bool pressed = button(ctx_, id, label, ImVec2(bw, bh), style, enabled, icon != nullptr ? ctx_.icon(icon) : nullptr);
    row_right = ImGui::GetItemRectMax().x;
    return pressed;
  };
  // A label column and a value column, each wrapped inside its own width (label_value would let a long label run into its value).
  const auto kv = [&](const std::string &label, const std::string &value, ImU32 color = col::text) {
    const float px = ctx_.u(12.0F), lw = std::clamp(iw * 0.38F, ctx_.u(112.0F), ctx_.u(320.0F));
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float yl = 0.0F, yv = 0.0F;
    for (const auto &line : wrap(ctx_.regular, px, lw - ctx_.u(12.0F), label)) { draw_text(dl, ctx_.regular, px, col::dim, ImVec2(o.x, o.y + yl), line.c_str()); yl += px + ctx_.u(3.0F); }
    for (const auto &line : wrap(ctx_.regular, px, iw - lw, value)) { draw_text(dl, ctx_.regular, px, color, ImVec2(o.x + lw, o.y + yv), line.c_str()); yv += px + ctx_.u(3.0F); }
    ImGui::Dummy(ImVec2(iw, std::max({yl, yv, px}) + ctx_.u(5.0F)));
  };

  // ---- an environment override governs the data folder: the page only explains it (the Settings control is disabled too) ----
  if (ep.cache_overridden || ep.library_overridden) {
    card_begin("LOCATION IS SET BY THE ENVIRONMENT");
    const PlainText t = refusal_text(ep.cache_overridden ? MigrationRefusal::env_override_cache : MigrationRefusal::env_override_library);
    text(col::warn, t.what);
    text(col::text2, t.next);
    gap(4.0F);
    kv("Data folder", u8s(ep.cache));
    kv("Library folder", u8s(ep.library));
    row_begin();
    if (row_button("dm_env_back", "Back to Settings", Style::primary)) { data_move_back(); card_end(); return; }
    note_move_focus_default();
    card_end();
    return;
  }

  const bool busy_now = build_running() || game_ != nullptr || maintenance_running_ || scd_import_job_.valid() || (busy_.any_activity() && !data_move_running());
  const MigrationPlan &plan = move_.plan;

  switch (move_.stage) {
    // ------------------------------------------------------------ choose
    case MoveStage::choose:
    case MoveStage::planning: {
      card_begin("CURRENT LOCATION");
      kv("Data folder", u8s(ep.cache) + (ep.relocated ? "  (a location you chose)" : "  (default location)"));
      kv("Library", u8s(ep.library));
      card_end();
      card_begin("NEW LOCATION");
      if (move_.stage == MoveStage::planning) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        spinner(ctx_, dl, ImVec2(c.x + ctx_.u(8.0F), c.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info);
        ImGui::SetCursorScreenPos(ImVec2(c.x + ctx_.u(24.0F), c.y));
        paragraph(ctx_, ctx_.regular, ctx_.u(12.5F), col::text, iw - ctx_.u(24.0F), "Checking " + u8s(move_.destination) + " ...");
        text(col::text2, "Reading only: nothing is written yet.", 11.5F);
        gap(4.0F);
        row_begin();
        if (row_button("dm_choose", "Choose a different folder", Style::secondary, true, "folder")) data_move_choose_folder();
      } else {
        text(col::text2, "Choose an empty folder, or a new one, on the drive you want to use. Segarecomp shows exactly what will move and what it needs before anything is copied; nothing changes until you confirm.");
        gap(4.0F);
        row_begin();
        if (row_button("dm_choose", "Choose folder...", Style::primary, true, "folder")) data_move_choose_folder();
        note_move_focus_default();
      }
      card_end();
      card_begin("WHAT HAPPENS");
      text(col::text, "Your library, saves, recordings, mods, covers and settings are copied and checked, then Segarecomp switches to the new folder at the next start.");
      text(col::text2, "Prepared games are not copied: they are rebuilt on demand at the new location. Your ROM, BIOS and disc files stay where they are and are not touched. Nothing is deleted automatically; the old folder stays until you clean it up yourself.", 11.5F);
      card_end();
      break;
    }

    // ------------------------------------------------------------ plan (preview) and its refusals
    case MoveStage::plan: {
      card_begin("FROM AND TO");
      kv("Current location", u8s(plan.source.empty() ? ep.cache : plan.source));
      kv("New location", u8s(move_.destination));
      card_end();
      if (!plan.ok()) {
        card_begin("THIS FOLDER CANNOT BE USED");
        bool not_empty = false;
        for (const auto &r : plan.refusals) {
          const PlainText t = refusal_text(r.code);
          text(col::err, t.what, 12.5F);
          text(col::text2, t.next, 12.0F);
          if (!r.detail.empty()) text(col::dim, r.detail, 11.0F);
          gap(4.0F);
          not_empty = not_empty || r.code == MigrationRefusal::destination_not_empty;
        }
        text(col::ok, "Nothing was changed. Your data stays where it is.", 12.0F);
        gap(4.0F);
        row_begin();
        if (row_button("dm_choose", "Choose another folder", Style::primary, true, "folder")) data_move_choose_folder();
        note_move_focus_default();
        if (not_empty && row_button("dm_use_sub", "Use a new folder inside it", Style::secondary)) data_move_use_subfolder();
        if (row_button("dm_cancel_plan", "Cancel", Style::ghost)) { data_move_back(); card_end(); return; }
        card_end();
      }
      if (plan.ok()) {
        card_begin("CONFIRM");
        text(col::text, "Copies " + count_and_bytes(plan.items.size(), plan.durable_bytes) + " of your data. Needs " + format_bytes(plan.required_bytes) + " free; " + format_bytes(plan.available_bytes) + " is available. Details below.", 12.5F);
        text(col::ok, "Your current data is kept; nothing is deleted automatically. If anything fails or you cancel, you keep using the current location.");
        text(col::text2, "The new location is used from the next time Segarecomp starts. Afterwards you can choose to clean up the previous location; Segarecomp never does it by itself.", 11.5F);
        if (busy_now) text(col::warn, "A game, a preparation, an import or a cleanup is running. Finish or cancel it first.", 11.5F);
        gap(4.0F);
        row_begin();
        if (row_button("dm_cancel_plan", "Cancel", Style::secondary)) { data_move_back(); card_end(); return; }
        note_move_focus_default();   // Cancel is the default: a stray Enter or A cannot start a move
        if (row_button("dm_confirm", "Move my data", Style::primary, !busy_now && !plan.items.empty())) data_move_confirm();
        if (row_button("dm_choose", "Choose another folder", Style::ghost, true, "folder")) data_move_choose_folder();
        card_end();
      }
      if (plan.ok() || !plan.items.empty() || plan.durable_bytes > 0) {
        card_begin("WHAT WILL MOVE");
        if (plan.items.empty()) text(col::text2, "No data was found in the current folder.");
        for (const auto &a : plan.areas) {
          if (a.cls != ItemClass::durable) continue;
          kv(area_title(a.area), count_and_bytes(a.entries, a.bytes));
        }
        gap(2.0F);
        kv("Total copied", count_and_bytes(plan.items.size(), plan.durable_bytes), col::ok);
        card_end();
        card_begin("WHAT DOES NOT MOVE");
        for (const auto &a : plan.areas) {
          if (a.cls != ItemClass::regenerable) continue;
          kv(area_title(a.area), count_and_bytes(a.entries, a.bytes) + (a.area == "builds" ? " - those games will need to be prepared again" : " - rebuilt automatically when needed"));
        }
        if (plan.regenerable_entries == 0) text(col::text2, "No prepared games or caches in the current folder.", 11.5F);
        if (!plan.left_behind.empty()) {
          std::string names;
          for (std::size_t i = 0; i < plan.left_behind.size() && i < 4; ++i) names += (i ? ", " : "") + plan.left_behind[i];
          if (plan.left_behind.size() > 4) names += ", ... (" + std::to_string(plan.left_behind.size()) + " in all)";
          kv("Other files here", names + " - not moved and not removed");
        }
        kv("Your game files", "ROM, BIOS and disc files you added stay where they are and are not touched", col::text2);
        card_end();
        card_begin("SPACE");
        kv("Needed", format_bytes(plan.required_bytes) + " (your data plus a safety margin)");
        const bool short_space = plan.available_bytes < plan.required_bytes;
        kv("Available", format_bytes(plan.available_bytes), short_space ? col::err : col::ok);
        card_end();
      }
      break;
    }

    // ------------------------------------------------------------ running
    case MoveStage::running: {
      MigrationProgress p;
      { std::lock_guard<std::mutex> lock(move_.mutex); p = move_.progress; }
      card_begin("MOVING YOUR DATA");
      text(col::text2, "Copying and checking your library. The current location stays in use and untouched until the final step.");
      gap(6.0F);
      static const char *labels[] = {"Check", "Copy", "Library", "Verify", "Switch"};
      const int index = p.phase == "plan" ? 0 : p.phase == "copy" ? 1 : p.phase == "rebase" ? 2 : p.phase == "verify" ? 3 : p.phase == "commit" ? 4 : 5;
      const ImVec2 o = ImGui::GetCursorScreenPos();
      const float col_w = iw / 5.0F, r = ctx_.u(11.0F);
      for (int i = 0; i < 5; ++i) {
        const ImVec2 c(o.x + col_w * (static_cast<float>(i) + 0.5F), o.y + r + ctx_.u(2.0F));
        const bool done = i < index, now = i == index;
        dl->AddCircleFilled(c, r, now ? col::btn : (done ? rgba(24, 92, 62) : rgba(12, 28, 64)));
        dl->AddCircle(c, r, now ? col::btn_top : (done ? col::ok : rgba(46, 96, 190)), 24, std::max(1.0F, ctx_.u(1.2F)));
        if (done) icon_image(dl, ctx_.icon("check"), ImVec2(c.x - ctx_.u(7.0F), c.y - ctx_.u(7.0F)), ctx_.u(14.0F), col::ok);
        else { const std::string n = std::to_string(i + 1); const float px = ctx_.u(12.0F); draw_text(dl, ctx_.bold, px, now ? col::btn_text : col::dim, ImVec2(c.x - text_width(ctx_.bold, px, n.c_str()) * 0.5F, c.y - px * 0.55F), n.c_str()); }
        const float px = ctx_.u(11.0F), tw = text_width(ctx_.regular, px, labels[i]);
        draw_text(dl, now ? ctx_.bold : ctx_.regular, px, now ? col::info : (done ? col::ok : col::dim), ImVec2(c.x - tw * 0.5F, c.y + r + ctx_.u(5.0F)), labels[i]);
      }
      ImGui::Dummy(ImVec2(iw, r * 2.0F + ctx_.u(26.0F)));
      // A determinate bar only where the core reports real totals (copy: bytes and files; verify: files); otherwise an indeterminate one. No time estimate is ever shown.
      const ImVec2 b0 = ImGui::GetCursorScreenPos();
      const ImVec2 b1(b0.x + iw, b0.y + ctx_.u(10.0F));
      const bool known = (p.phase == "copy" || p.phase == "verify") && p.files_total > 0;
      std::string line = "Working...";
      if (known) {
        const float frac = p.bytes_total > 0 ? static_cast<float>(static_cast<double>(p.bytes_done) / static_cast<double>(p.bytes_total)) : static_cast<float>(p.files_done) / static_cast<float>(p.files_total);
        progress_bar(ctx_, dl, b0, b1, frac, col::info);
        line = std::string(p.phase == "copy" ? "Copying" : "Verifying") + ": " + std::to_string(p.files_done) + " of " + std::to_string(p.files_total) + " files";
        if (p.bytes_total > 0) line += " (" + format_bytes(p.bytes_done) + " of " + format_bytes(p.bytes_total) + ")";
      } else {
        dl->AddRectFilled(b0, b1, col::field, ctx_.u(5.0F));
        const float t = static_cast<float>(SDL_GetTicks() % 1600U) / 1600.0F, seg = iw * 0.22F, x0 = b0.x + (iw - seg) * t;
        dl->PushClipRect(b0, b1, true);
        dl->AddRectFilled(ImVec2(x0, b0.y), ImVec2(x0 + seg, b1.y), col::info, ctx_.u(5.0F));
        dl->PopClipRect();
        line = p.phase == "rebase" ? "Updating the copied library for the new location..." : p.phase == "commit" ? "Switching to the new location..." : p.phase == "done" ? "Finishing..." : "Checking the plan...";
      }
      ImGui::Dummy(ImVec2(iw, ctx_.u(14.0F)));
      text(col::text, line, 12.5F);
      if (!p.current.empty()) {
        const std::string shown = ellipsize(ctx_.regular, ctx_.u(11.0F), iw, p.current);
        draw_text(dl, ctx_.regular, ctx_.u(11.0F), col::dim, ImGui::GetCursorScreenPos(), shown.c_str());
        ImGui::Dummy(ImVec2(iw, ctx_.u(16.0F)));
      }
      const bool late = p.phase == "commit" || p.phase == "done";
      text(late ? col::warn : col::ok, late ? "Too late to cancel: the switch is being made." : "You can cancel until the final step. Cancelling removes the partial copy and keeps your current location.", 11.5F);
      if (move_.cancel_requested && !late) text(col::warn, "Cancelling...", 11.5F);
      gap(4.0F);
      row_begin();
      if (row_button("dm_stop", move_.cancel_requested && !late ? "Cancelling..." : "Cancel move", Style::secondary, !late && !move_.cancel_requested)) data_move_request_cancel();
      note_move_focus_default();
      if (row_button("dm_bg", "Back to Settings", Style::ghost)) { go(Page::settings); card_end(); return; }
      card_end();
      break;
    }

    // ------------------------------------------------------------ finished
    case MoveStage::finished: {
      const MigrationResult &r = move_.result;
      if (r.ok()) {
        card_begin("YOUR DATA WAS MOVED");
        text(col::ok, "The copy was verified and the new location is saved.", 13.0F);
        kv("New location", u8s(move_.destination));
        kv("Copied", count_and_bytes(r.files, r.bytes));
        if (r.rebased_paths > 0) kv("Library updated", std::to_string(r.rebased_paths) + " reference(s) to files kept in your library now point at the new location");
        if (r.entries_needing_rebuild > 0) kv("To prepare again", std::to_string(r.entries_needing_rebuild) + " game(s) had a prepared version; prepare them again at the new location when you play them", col::warn);
        if (r.cleared_programs > 0) kv("Sega CD", std::to_string(r.cleared_programs) + " imported program(s) lived in a rebuildable folder; import or prepare them again", col::warn);
        for (const auto &n : r.notes) text(col::dim, n, 11.0F);
        card_end();
        card_begin("RESTART REQUIRED");
        text(col::warn, "Segarecomp starts using the new location the next time it starts. Until then it keeps using the previous one: anything you add or save in this window stays in the previous location and will not be in the new one.", 12.0F);
        text(col::text2, "Quit now and open Segarecomp again. Your previous location still holds your old data; nothing was deleted. After the restart you can clean it up from Settings > Storage & Data.", 11.5F);
        gap(4.0F);
        row_begin();
        if (row_button("dm_done", "Done", Style::secondary)) data_move_back();
        note_move_focus_default();
        if (row_button("dm_quit", "Quit Segarecomp now", Style::primary)) { SDL_Event quit; SDL_zero(quit); quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit); }
        card_end();
      } else {
        const bool cancelled = r.outcome == MigrationOutcome::cancelled;
        card_begin(cancelled ? "MOVE CANCELLED" : "THE DATA WAS NOT MOVED");
        PlainText t = failure_text(r.reason);
        if (r.outcome == MigrationOutcome::refused && !r.refusals.empty()) t = refusal_text(r.refusals.front().code);
        text(cancelled ? col::warn : col::err, t.what, 13.0F);
        text(col::text2, t.next, 12.0F);
        if (!r.detail.empty() && !cancelled) text(col::dim, r.detail, 11.0F);
        for (const auto &n : r.notes) text(col::warn, n, 11.5F);
        gap(2.0F);
        text(col::ok, "Your current location is unchanged and still in use. Nothing was switched; any partial copy was removed.", 12.0F);
        gap(4.0F);
        row_begin();
        if (row_button("dm_done", "Done", Style::primary)) data_move_back();
        note_move_focus_default();
        if (row_button("dm_again", "Start again", Style::secondary)) data_move_reset();
        card_end();
      }
      break;
    }

    // ------------------------------------------------------------ previous location cleanup (a separate, explicit operation)
    case MoveStage::cleanup_planning:
    case MoveStage::cleanup:
    case MoveStage::cleaning:
    case MoveStage::cleaned: {
      card_begin("PREVIOUS LOCATION");
      kv("Folder", u8s(move_.prev_source));
      kv("New location", u8s(move_.prev_destination));
      card_end();
      if (move_.stage == MoveStage::cleanup_planning) {
        card_begin("MEASURING");
        const ImVec2 c = ImGui::GetCursorScreenPos();
        spinner(ctx_, dl, ImVec2(c.x + ctx_.u(8.0F), c.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info);
        ImGui::SetCursorScreenPos(ImVec2(c.x + ctx_.u(24.0F), c.y));
        paragraph(ctx_, ctx_.regular, ctx_.u(12.5F), col::text, iw - ctx_.u(24.0F), "Working out what could be removed...");
        card_end();
        break;
      }
      if (move_.stage == MoveStage::cleaning) {
        card_begin("REMOVING");
        const ImVec2 c = ImGui::GetCursorScreenPos();
        spinner(ctx_, dl, ImVec2(c.x + ctx_.u(8.0F), c.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info);
        ImGui::SetCursorScreenPos(ImVec2(c.x + ctx_.u(24.0F), c.y));
        paragraph(ctx_, ctx_.regular, ctx_.u(12.5F), col::text, iw - ctx_.u(24.0F), "Removing the listed items from the previous location...");
        gap(4.0F);
        row_begin();
        if (row_button("dm_c_stop", move_.cancel ? "Stopping..." : "Stop", Style::secondary, !move_.cancel)) move_.cancel = true;
        note_move_focus_default();
        card_end();
        break;
      }
      if (move_.stage == MoveStage::cleaned) {
        const CleanupResult &cr = move_.cleanup_result;
        card_begin(cr.refused ? "NOTHING WAS REMOVED" : "CLEANUP FINISHED");
        if (cr.refused) {
          const PlainText t = cleanup_refusal_text(cr.reason);
          text(col::err, t.what, 12.5F);
          text(col::text2, t.next, 12.0F);
        } else {
          text(cr.problems.empty() ? col::ok : col::warn, "Removed " + std::to_string(cr.removed) + (cr.removed == 1 ? " file or folder" : " files and folders") + ", freeing " + format_bytes(cr.bytes_freed) + ".", 13.0F);
          for (std::size_t i = 0; i < cr.problems.size() && i < 6; ++i) text(col::warn, "Not removed: " + cr.problems[i], 11.0F);
          if (cr.problems.size() > 6) text(col::dim, "... and " + std::to_string(cr.problems.size() - 6) + " more.", 11.0F);
          text(col::text2, "Your data at the new location was not touched.", 11.5F);
        }
        gap(4.0F);
        row_begin();
        if (row_button("dm_done", "Done", Style::primary)) data_move_back();
        note_move_focus_default();
        card_end();
        break;
      }
      // the preview
      const CleanupPlan &cp = move_.cleanup_plan;
      if (!move_.cleanup_refusal.empty() || !cp.eligible) {
        card_begin("NOT AVAILABLE");
        const PlainText t = cleanup_refusal_text(move_.cleanup_refusal.empty() ? cp.reason : move_.cleanup_refusal);
        text(col::warn, t.what, 12.5F);
        text(col::text2, t.next, 12.0F);
        gap(4.0F);
        row_begin();
        if (row_button("dm_c_cancel", "Back", Style::primary)) data_move_back();
        note_move_focus_default();
        card_end();
        break;
      }
      card_begin("WHAT WOULD BE REMOVED");
      std::vector<std::pair<std::string, std::pair<std::size_t, std::uintmax_t>>> groups;
      for (const auto &item : cp.items) {
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.first == item.area; });
        if (it == groups.end()) { groups.push_back({item.area, {0, 0}}); it = groups.end() - 1; }
        ++it->second.first;
        it->second.second += item.bytes;
      }
      if (groups.empty()) text(col::text2, move_.include_copies ? "There is nothing to remove from the previous location." : "There are no prepared games or caches left in the previous location.");
      for (const auto &g : groups) kv(area_title(g.first), count_and_bytes(g.second.first, g.second.second));
      kv("Total", format_bytes(cp.bytes), groups.empty() ? col::text2 : col::warn);
      gap(2.0F);
      bool copies = move_.include_copies;
      if (checkbox(ctx_, "dm_c_copies", "Also remove the old copies of the data that was moved", &copies)) { move_.include_copies = copies; data_cleanup_replan(); }
      text(col::text2, move_.include_copies ? "Listed: files still identical to what was moved and present at the new location. Anything changed since the move, unknown files and the library backup stay." : "Off: only prepared games and caches, which can be made again. Your library, saves, recordings, mods and settings are not touched; tick this to also remove their old copies.", 11.0F);
      card_end();
      card_begin("CONFIRM");
      text(col::warn, "This permanently removes the listed items from the previous location. It cannot be undone.", 12.0F);
      gap(4.0F);
      row_begin();
      if (row_button("dm_c_cancel", "Cancel", Style::secondary)) data_move_back();
      note_move_focus_default();
      const std::string remove_label = cp.items.empty() ? std::string("Nothing to remove") : "Remove " + format_bytes(cp.bytes);
      if (row_button("dm_c_confirm", remove_label.c_str(), Style::danger, !cp.items.empty() && !busy_now)) data_cleanup_confirm();
      card_end();
      if (!cp.kept.empty()) {
        card_begin("STAYS IN THE PREVIOUS LOCATION");
        for (std::size_t i = 0; i < cp.kept.size() && i < 5; ++i) text(col::dim, cp.kept[i], 11.0F);
        if (cp.kept.size() > 5) text(col::dim, "... and " + std::to_string(cp.kept.size() - 5) + " more.", 11.0F);
        card_end();
      }
      break;
    }
  }
  card_end();
  ImGui::Dummy(ImVec2(w, ctx_.u(12.0F)));
}

} // namespace launcher
