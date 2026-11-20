// SEG-078-T002 (ADR 0204): Sega CD in the launcher window - the game's details panel (inputs, program, verification, recordings), the verification worker
// and the run of an imported viewer program. SEG-150-T005: a Sega CD game is ADDED through the one Add game review (disc verdict + the user's BIOS step below). The checks themselves live in sega_cd.cpp (no window); this file
// is the screen and the job plumbing, reusing the launcher's pickers, status badges, modals, process orchestration and failure banners.

#include "app.hpp"

#include "app_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <thread>

namespace fs = std::filesystem;
using namespace ui;
using namespace launcher::uih;

namespace launcher {

namespace {

fs::path from_u8(const std::string &text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str()))); }

long long unix_now() { return static_cast<long long>(std::time(nullptr)); }

// The refusals that mean the BIOS or the disc itself is not usable (a build cannot start); every other refusal is about the program, which a build replaces.
bool inputs_unusable(scd::Refusal refusal) {
  switch (refusal) {
    case scd::Refusal::inputs_missing: case scd::Refusal::bios_missing: case scd::Refusal::bios_unreadable: case scd::Refusal::bios_too_large: case scd::Refusal::bios_changed:
    case scd::Refusal::cue_missing: case scd::Refusal::cue_changed: return true;
    default: return false;
  }
}

// Test hook (like SEGARECOMP_SCD_BUILD_ARGS): hold a check's worker for N ms, polling the cancel flag, so a cancel can be exercised deterministically on a tiny synthetic disc.
void hold_for_test(const std::atomic<bool> &cancel) {
  int hold_ms = 0;
  if (const char *hold = std::getenv("SEGARECOMP_SCD_CHECK_HOLD_MS")) hold_ms = std::clamp(std::atoi(hold), 0, 60000);
  for (int waited = 0; waited < hold_ms && !cancel.load(); waited += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

}  // namespace

// ---------------------------------------------------------------- labels

std::string App::scd_label(const StatusInfo &info) { return scd::state_label(info.scd.state); }

std::string App::scd_hint(const StatusInfo &info) const {
  switch (info.scd.state) {
    case scd::State::ready:
      return "Verified: the BIOS, the disc files and the program match each other (the program is bound to exactly these two files).";
    case scd::State::no_program:
      return layout_.scd_build_problem.empty() ? std::string("No program yet. Choose Build: Segarecomp compiles one from your BIOS and disc on this computer. You can also import a verified prebuilt program.")
                                               : scd::message(scd::Refusal::unsupported_build);
    default: return scd::message(info.scd.refusal, info.scd.detail);
  }
}

fs::path App::scd_dir(const LibraryEntry &entry) const { return library_dir_ / "sega-cd" / entry.sha256.substr(0, 16); }

std::vector<fs::path> App::scd_recordings(const LibraryEntry &entry) const {
  std::vector<fs::path> out;
  std::error_code ec;
  const fs::path dir = scd_dir(entry) / "recordings";
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end && out.size() < 4096; it.increment(ec)) {
    std::error_code sec;
    if (it->is_regular_file(sec) && !it->is_symlink(sec) && it->path().extension() == ".txt") out.push_back(it->path());
  }
  std::sort(out.begin(), out.end(), [](const fs::path &a, const fs::path &b) { return a.filename() > b.filename(); });
  if (out.size() > 6) out.resize(6);
  return out;
}

// SEG-146-T006: a Genesis game's recordings live beside its Sega CD siblings, one folder per image (the first 16 hex digits of its digest).
fs::path App::session_dir(const LibraryEntry &entry) const { return library_dir_ / "sessions" / "genesis" / entry.sha256.substr(0, 16); }

std::vector<fs::path> App::session_recordings(const LibraryEntry &entry) const {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(session_dir(entry), ec), end; !ec && it != end && out.size() < 4096; it.increment(ec)) {
    std::error_code sec;
    if (it->is_regular_file(sec) && !it->is_symlink(sec) && it->path().extension() == ".rec") out.push_back(it->path());
  }
  std::sort(out.begin(), out.end(), [](const fs::path &a, const fs::path &b) { return a.filename() > b.filename(); });
  if (out.size() > 6) out.resize(6);
  return out;
}

// ---------------------------------------------------------------- jobs

void App::scd_start_import(const std::string &target_id, const fs::path &bios, const fs::path &cue, const fs::path &program) {
  (void)bios;
  (void)cue;   // a NEW game is added through the Add game review (commit_review); this is only the attach / re-verify of an existing entry
  if (scd_import_job_.valid()) { toast("A Sega CD check is already running.", true); return; }
  const LibraryEntry *existing = target_id.empty() ? nullptr : catalog_.find_id(target_id);
  if (existing == nullptr) return;
  scd_done_ = 0;
  scd_cancel_ = false;
  scd_import_target_ = target_id;
  scd_import_reverify_ = program.empty();
  std::uint64_t total = 0;
  std::error_code ec;
  const auto size_of = [&](const fs::path &p) { return p.empty() ? std::uint64_t{0} : static_cast<std::uint64_t>(fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0); };
  total = size_of(from_u8(existing->sega_cd.bios_path)) + size_of(from_u8(existing->sega_cd.cue_path)) + size_of(program.empty() ? from_u8(existing->sega_cd.program.path) : program);
  for (const auto &f : existing->sega_cd.disc_files) total += f.size;
  scd_total_ = total;
  // Re-verify (no program given) or attach a program: both measure everything against the entry's recorded BIOS and CUE.
  scd::EntryRecord record = existing->sega_cd;
  const fs::path chosen = program.empty() ? from_u8(record.program.path) : program;
  scd_import_job_ = std::async(std::launch::async, [record, chosen, this] {
    hold_for_test(scd_cancel_);
    if (scd_cancel_.load()) return scd::ImportResult{};   // cancelled before it began: nothing was measured
    return scd::attach_program(record, chosen, &scd_done_, &scd_cancel_);
  });
  trace("scd=verify_started target=existing");
}

bool App::scd_can_build(const LibraryEntry &entry, const StatusInfo &info) const {
  if (!info.sega_cd || !layout_.scd_build_problem.empty() || !layout_.problem.empty()) return false;
  // SEG-109-T003: like a native game, Build is offered while another build runs or a check/cleanup is busy (the request joins the FIFO); only this game's own
  // running/queued build and a running or starting game refuse it.
  if (is_building(entry.id()) || is_queued(entry.id()) || game_ || scd_launch_job_.valid()) return false;
  if (inputs_unusable(info.scd.refusal)) return false;   // the inputs must be in place; a program problem (missing, mismatched, stale) is what a build fixes
  return !entry.sega_cd.bios_path.empty() && !entry.sega_cd.cue_path.empty();
}

void App::scd_start_build(const std::string &id) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || entry->platform_id != "sega-cd") return;
  if (!layout_.scd_build_problem.empty() || !layout_.problem.empty()) {
    show_message("Sega CD games cannot be built here", {scd::message(scd::Refusal::unsupported_build), layout_.scd_build_problem.empty() ? layout_.problem : layout_.scd_build_problem,
                                                          "Open the game and use Import program... to attach a prebuilt program."});
    trace("build=refused reason=unsupported_build platform=sega-cd");
    return;
  }
  if (scd_build_ || game_ || scd_import_job_.valid() || scd_launch_job_.valid()) { toast("Wait for the running game, build or check to finish.", true); return; }
  scd_build_key_ = "scd-" + entry->sha256.substr(0, 16);
  if (!busy_.try_begin_build(scd_build_key_)) { scd_build_key_.clear(); toast("Cleanup is running; try again in a moment.", true); return; }
  const scd::Status quick = scd::quick_status(entry->sega_cd);
  if (inputs_unusable(quick.refusal)) {
    busy_.end_build(scd_build_key_);
    scd_build_key_.clear();
    show_message("The BIOS or the disc is not available", {scd::message(quick.refusal, quick.detail), "Locate the file first; the build reads both."});
    trace(std::string("build=refused reason=") + scd::code(quick.refusal) + " platform=sega-cd");
    return;
  }
  std::error_code ec;
  fs::create_directories(scd_dir(*entry), ec);
  ScdBuildRequest request;
  request.bios = from_u8(entry->sega_cd.bios_path);
  request.cue = from_u8(entry->sega_cd.cue_path);
  request.out_dir = scd_dir(*entry) / "build";
  {   // SEG-103-T007: the stored selection, exactly; Compatibility unless the user chose file-first or the experimental Optimized for this game
    const std::string mode = scd::selected_build_mode(entry->sega_cd);
    request.mode = mode == "file_first" ? ScdBuildMode::file_first : mode == "optimized" ? ScdBuildMode::optimized : ScdBuildMode::compatibility;
  }
  const ScdBuildMode request_mode = request.mode;
  scd_build_ = std::make_unique<ScdBuildJob>(std::move(request), layout_);
  scd_build_id_ = id;
  failures_.erase(id);
  diagnostics_.erase(id);
  scd_build_notes_.erase(id);
  scd_built_.erase(id);   // a leftover result of an earlier build whose import was refused must never label another program
  trace(std::string("build=started platform=sega-cd mode=") + scd_build_mode_code(request_mode));
}

void App::scd_finish_build() {
  scd_build_->wait();
  const BuildOutcome outcome = scd_build_->outcome();
  const std::string message = scd_build_->message(), summary = scd_build_->summary();
  const bool built_experimental = scd_build_->built_experimental();
  const std::string built_mode = scd_build_->built_mode();
  const ScdFileFirstReport inventory = scd_build_->file_first_report();
  const fs::path program = scd_build_->program(), out_dir = scd_build_->out_dir();
  const std::string id = scd_build_id_;
  busy_.end_build(scd_build_key_);
  scd_build_.reset();
  scd_build_id_.clear();
  scd_build_key_.clear();
  if (outcome == BuildOutcome::cancelled) {
    last_build_outcome_ = "cancelled";
    trace("build=cancelled platform=sega-cd");
    toast("Build cancelled.");
    request_status_refresh();
    request_scan();
    return;
  }
  if (outcome != BuildOutcome::succeeded) {
    last_build_outcome_ = "failed";
    failures_[id] = message;
    diagnostics_[id] = read_tail(fs::path(out_dir.parent_path() / (out_dir.filename().string() + ".failed")) / "build.log", 24000);
    trace("build=failed platform=sega-cd message=" + message);
    toast("Build failed: " + message, true);
    request_status_refresh();
    request_scan();
    return;
  }
  last_build_outcome_ = "ok";
  scd_build_notes_[id] = summary;
  scd_built_[id] = ScdBuiltInfo{built_experimental, built_mode, built_mode == "file_first" ? (inventory.complete ? "complete" : "incomplete") : ""};
  trace(std::string("build=ok platform=sega-cd policy=") + (built_experimental ? "experimental_optimized" : "compatibility") + " mode=" + built_mode +
        (built_mode == "file_first" ? std::string(" file_first_inventory=") + (inventory.complete ? "complete" : "incomplete") : std::string()));
  // The built program is verified exactly like an imported one: it is bound to this entry's BIOS and disc only if its own statement matches.
  scd_start_import(id, {}, {}, program);
}

void App::scd_poll() {
  if (scd_build_ && scd_build_->finished()) scd_finish_build();
  if (scd_import_job_.valid() && scd_import_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    scd::ImportResult result = scd_import_job_.get();
    const std::string target = scd_import_target_;
    scd_import_target_.clear();
    if (scd_cancel_.load() && !result.program_attached) {   // the user cancelled the check: nothing is changed and no refusal is shown for what was only interrupted
      scd_cancel_ = false;
      scd_built_.erase(target);
      request_status_refresh();
      toast("Check cancelled. Nothing was changed.");
      trace("scd=verify_cancelled");
      return;
    }
    scd_cancel_ = false;
    if (LibraryEntry *entry = catalog_.find_id(target)) {
      if (result.program_attached) {
        const bool keep_choice = entry->sega_cd.experimental_m3, keep_file_first = entry->sega_cd.file_first;   // the build choice is the user's; importing a program never changes it
        const bool same_program = !entry->sega_cd.program.sha256.empty() && entry->sega_cd.program.sha256 == result.record.program.sha256;
        const bool keep_experimental = entry->sega_cd.program_experimental;
        const std::string keep_mode = entry->sega_cd.program_built_mode, keep_inventory = entry->sega_cd.program_file_first_inventory;
        entry->sega_cd = result.record;
        entry->sega_cd.experimental_m3 = keep_choice;
        entry->sega_cd.file_first = keep_file_first;
        const auto built = scd_built_.find(target);   // a program the launcher just built carries the product's structured result
        const bool just_built = built != scd_built_.end();
        if (just_built) {
          entry->last_built_at = now_seconds();   // the same "Last recompiled" fact a native build records
          entry->archived = false;
          // The keep-last-N / soft-limit policies run as after a native build (they only ever remove generated build folders, never a save; see storage.hpp).
          if (settings_.cleanup.mode == "keep_last_n" || settings_.cleanup.mode == "soft_limit") policy_eval_pending_ = true;
        }
        if (built != scd_built_.end()) {
          entry->sega_cd.program_experimental = built->second.experimental;
          entry->sega_cd.program_built_mode = built->second.mode;
          entry->sega_cd.program_file_first_inventory = built->second.inventory;
          scd_built_.erase(built);
        } else if (same_program) {   // verifying the same program again does not forget how it was built
          entry->sega_cd.program_experimental = keep_experimental;
          entry->sega_cd.program_built_mode = keep_mode;
          entry->sega_cd.program_file_first_inventory = keep_inventory;
        } else {   // an imported different program: how it was built is unknown and is not invented (the earlier program's label must not carry over)
          entry->sega_cd.program_experimental = false;
          entry->sega_cd.program_built_mode.clear();
          entry->sega_cd.program_file_first_inventory.clear();
        }
        save_catalog();
        request_status_refresh();
        request_scan();
        toast(just_built ? "Build finished: " + entry->title() + "." : "Program verified and bound to this game.");
        trace("scd=program_attached");
      } else {
        scd_built_.erase(target);   // the just-built program was refused: its build result describes nothing
        request_scan();
        if (scd_import_reverify_) {  // "Verify again" found a difference: the earlier verification no longer stands
          entry->sega_cd.disc_verified = false;
          save_catalog();
          request_status_refresh();
        }
        show_message(scd_import_reverify_ ? "Verification failed" : "This program cannot be used", {scd::message(result.refusal, result.detail)});
        trace(std::string("scd=program_refused refusal=") + scd::code(result.refusal));
      }
    }
  }
  if (scd_launch_job_.valid() && scd_launch_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const scd::Plan plan = scd_launch_job_.get();
    const std::string id = scd_launch_id_;
    scd_launch_id_.clear();
    LibraryEntry *entry = catalog_.find_id(id);
    if (entry == nullptr) return;
    if (plan.status.state != scd::State::ready) {
      failures_[id] = scd::message(plan.status.refusal, plan.status.detail);
      diagnostics_[id] = std::string("Refused before launch: ") + scd::code(plan.status.refusal) + "\n" + failures_[id];
      trace(std::string("scd=launch_refused refusal=") + scd::code(plan.status.refusal) + " state=" + scd::state_label(plan.status.state));
      request_status_refresh();
      return;
    }
    if (plan.program_mtime != 0 && plan.program_mtime != entry->sega_cd.program.mtime) { entry->sega_cd.program.mtime = plan.program_mtime; save_catalog(); }
    scd_spawn(id, plan);
  }
}

void App::scd_play(const std::string &id, int mode, const fs::path &script) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || entry->platform_id != "sega-cd") return;
  if (game_ || scd_launch_job_.valid() || scd_import_job_.valid() || (scd_build_ && scd_build_id_ == id)) { toast("Wait for the running game, build or check to finish.", true); return; }
  {
    std::error_code ec;
    fs::create_directories(scd_dir(*entry) / "recordings", ec);
    if (ec) { show_message("The game could not be prepared", {"The folder for this game's run files cannot be written: " + ec.message()}); return; }
  }
  scd::RunOptions options;
  options.report = scd_dir(*entry) / "last-report.json";
  // Normal play loads and persists the player's save; a replay or a recording starts from a defined image and never touches it (ADR 0252).
  const scd::BackupStatus save = scd::prepare_backup_ram(options, scd_dir(*entry), mode == 0 && !scd_record_);
  if (save != scd::BackupStatus::ok && save != scd::BackupStatus::created) { show_message("The game could not be prepared", {scd::describe_backup(save)}); return; }
  if (mode == 0 && scd_record_) options.record_to = scd_dir(*entry) / "recordings" / ("session-" + std::to_string(unix_now()) + ".txt");
  if (mode != 0) options.replay_from = script;
  if (mode == 2) options.until_tick = scd::replay_end_tick(script);  // nobody closes a window that is not there: the replay ends shortly after its last event
  scd_launch_options_ = options;
  scd_launch_windowless_ = mode == 2;
  scd_launch_id_ = id;
  failures_.erase(id);
  diagnostics_.erase(id);
  scd_last_outcome_.erase(id);
  scd::EntryRecord record = entry->sega_cd;
  scd_done_ = 0;
  scd_cancel_ = false;
  scd_launch_job_ = std::async(std::launch::async, [record, this] { return scd::prepare_launch(record, false, &scd_done_, &scd_cancel_); });
  trace(std::string("scd=play_requested mode=") + (mode == 0 ? (scd_record_ ? "record" : "live") : (mode == 1 ? "replay" : "replay_windowless")));
}

bool App::scd_spawn(const std::string &id, const scd::Plan &plan) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr) return false;
  const std::string key = "scd-" + entry->sha256.substr(0, 16);
  if (!busy_.try_begin_run(key)) { toast("Cleanup is running; try again in a moment.", true); return false; }
  std::error_code ec;
  fs::remove(scd_launch_options_.report, ec);
  std::vector<std::pair<std::string, std::string>> environment;
  if (scd_launch_windowless_) {
    environment = {{"SDL_VIDEODRIVER", "dummy"}, {"SDL_AUDIODRIVER", "dummy"}, {"SEGARECOMP_VIEWER_UNTHROTTLED", "1"}, {"SEGARECOMP_VIEWER_NO_STOP_SCREEN", "1"}};
  }
  // SEG-132 (ADR 0279): save states and rewind live in the game's own folder (<game folder>/states/) and are offered only for normal play -- the same sessions that load and persist the
  // player's backup RAM. A replay, a "No window" replay and a recording never receive the folder (and the viewer refuses a scripted or recorded input regardless).
  if (!scd_launch_options_.backup_ram_out.empty()) environment.emplace_back("SEGARECOMP_SAVE_DIR", scd_dir(*entry).string());
  if (entry->sha256.size() == 64U) environment.emplace_back("SEGARECOMP_PROFILE_ID", entry->sha256);  // SEG-123: the per-game widescreen profile key (platform sega-cd + this identity)
  RomView view = view_from_entry(*entry);
  view.sha256.clear();  // the entry identity is not a ROM digest: the viewer's System page must not present it as one
  sv_diag_yield_exclusive_devices(diag_);  // a libusb-driven controller can be opened by one process only: free it for the viewer
  game_ = std::make_unique<GameRun>(view, layout_, plan.program, scd_dir(*entry) / "run.log", scd::run_arguments(plan, scd_launch_options_), environment, true);
  if (!game_->started()) {
    sv_diag_reclaim_exclusive_devices(diag_);
    busy_.end_run(key);
    game_.reset();
    failures_[id] = "The program could not be started.";
    diagnostics_[id] = failures_[id];
    trace("scd=spawn_failed");
    request_status_refresh();
    return false;
  }
  game_is_scd_ = true;
  game_id_ = id;
  game_key_ = key;
  game_dir_ = scd_dir(*entry);
  game_scd_options_ = scd_launch_options_;
  game_view_ = view;
  restarted_this_play_ = false;
  trace(std::string("scd=run_started windowless=") + (scd_launch_windowless_ ? "1" : "0") + " recording=" + (scd_launch_options_.record_to.empty() ? "0" : "1"));
  return true;
}

void App::scd_finish_run() {
  const int exit_code = game_->exit_code();
  const std::string id = game_id_;
  const scd::RunOptions options = game_scd_options_;
  game_.reset();
  sv_diag_reclaim_exclusive_devices(diag_);
  busy_.end_run(game_key_);
  game_id_.clear();
  game_is_scd_ = false;
  const scd::Outcome outcome = scd::read_outcome(options.report, exit_code);
  std::string text = scd::describe_outcome(outcome);
  std::string save_problem;
  switch (scd::finish_backup_ram(options, exit_code)) {
    case scd::SaveOutcome::stored: text += " Save data stored."; break;
    case scd::SaveOutcome::kept_run_failed: text += " The run did not end cleanly, so its save data was not stored; the previous save was kept."; break;
    case scd::SaveOutcome::kept_commit_failed: save_problem = "The game's save data could not be stored; the previous save was kept."; break;
    case scd::SaveOutcome::not_requested: break;
  }
  if (outcome.report_read && !options.replay_from.empty()) text += " Replay: " + std::to_string(outcome.frames) + " frames, " + std::to_string(outcome.input_events) + " input events.";
  trace("scd=run_exited code=" + std::to_string(exit_code) + " stop=" + (outcome.stop_class.empty() ? std::string("none") : outcome.stop_class) + " live=" + (outcome.live_outcome.empty() ? std::string("none") : outcome.live_outcome));
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry != nullptr) {
    catalog_.record_run(*entry, exit_code, exit_code == 0 ? "clean" : "failed", now_seconds());
    entry->sega_cd.last_stop = outcome.result == "stopped" ? outcome.stop_class : std::string();
    save_catalog();
    if (!options.record_to.empty()) {
      std::error_code ec;
      if (fs::is_regular_file(options.record_to, ec)) toast("Input recording saved (" + std::to_string(outcome.input_events) + " events).");
    }
  }
  if (!save_problem.empty()) { text += " " + save_problem; failures_[id] = save_problem; toast(save_problem, true); }
  scd_last_outcome_[id] = text;
  // A clean close and a typed execution stop are shown as the end of the run; a refusal or an unexpected status is a failure with its log.
  const bool typed_stop = exit_code == 10;
  if (exit_code != 0 && !typed_stop && exit_code != 11) {
    failures_[id] = text;
    diagnostics_[id] = read_tail(game_dir_ / "run.log", 24000);
  }
  request_status_refresh();
  request_scan();   // the run may have stored a save: the storage measurements follow
}

// ---------------------------------------------------------------- dialogs

void App::scd_dialog_result(const std::string &purpose, const std::string &id, const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  const std::string &path = paths.front();
  if (purpose == "scd_review_bios") {   // key: "<expected sha256 or empty>:<cue path>" - the review item this BIOS is for
    const std::size_t colon = id.find(':');
    const std::string expected = colon == std::string::npos ? std::string() : id.substr(0, colon), cue = colon == std::string::npos ? id : id.substr(colon + 1);
    for (std::size_t i = 0; i < review_.size(); ++i)
      if (review_[i].sega_cd && review_[i].path == cue) { scd_review_choose_bios(i, from_u8(path), expected); break; }
  }
  else if (purpose == "scd_attach") scd_start_import(id, {}, {}, from_u8(path));
  else if (purpose == "scd_locate_bios" || purpose == "scd_locate_cue") {
    LibraryEntry *entry = catalog_.find_id(id);
    if (entry == nullptr) return;
    const bool bios = purpose == "scd_locate_bios";
    const scd::Digest d = scd::hash_file(from_u8(path), bios ? scd::max_bios_bytes : scd::max_cue_bytes);
    if (!d.ok) { show_message("The file could not be read", {d.error}); return; }
    if (d.sha256 != (bios ? entry->sega_cd.bios_sha256 : entry->sega_cd.cue_sha256)) {
      show_message(bios ? "That is a different BIOS" : "That is a different CUE file",
                   {"The chosen file does not match this game (its SHA-256 differs).", "Pick the original file, or add it as a separate game."});
      return;
    }
    (bios ? entry->sega_cd.bios_path : entry->sega_cd.cue_path) = path;
    if (!bios) entry->rom_paths.assign(1, path);
    if (bios) {   // the remembered profile of this BIOS follows its new location (same SHA-256, measured just above)
      scd::BiosProfile profile;
      profile.sha256 = d.sha256;
      profile.size = d.size;
      profile.region = scd::bios_region_of_file(from_u8(path));
      profile.path = path;
      profile.verified_at = unix_now();
      scd_remember_bios(profile);
    }
    save_catalog();
    request_status_refresh();
    toast(bios ? "BIOS location updated." : "Disc location updated.");
  }
}

// ---------------------------------------------------------------- Add game: a Sega CD disc (.cue)

void App::scd_start_inspect(std::vector<std::string> cue_paths) {
  if (cue_paths.empty()) return;
  if (scd_inspect_job_.valid()) { for (auto &p : cue_paths) deferred_paths_.push_back(std::move(p)); return; }
  scd_inspect_cancel_ = false;
  scd_inspect_done_ = 0;
  scd_inspect_total_ = static_cast<int>(cue_paths.size());
  std::vector<scd::BiosProfile> profiles = bios_profiles_;
  scd_inspect_job_ = std::async(std::launch::async, [paths = std::move(cue_paths), profiles = std::move(profiles), this] {
    std::vector<ScdInspected> out;
    hold_for_test(scd_inspect_cancel_);
    for (const std::string &p : paths) {
      ScdInspected one;
      one.path = p;
      one.disc = scd::inspect_disc(from_u8(p), &scd_inspect_cancel_);
      if (one.disc.verdict == scd::DiscInspection::Verdict::ready) {
        for (const scd::BiosProfile &profile : profiles) {   // revalidated NOW: a remembered profile is only a reference to a file
          ScdReviewBios o;
          o.profile = profile;
          o.state = scd::check_bios_file(from_u8(profile.path), profile.sha256).state;
          one.options.push_back(std::move(o));
        }
      }
      out.push_back(std::move(one));
      scd_inspect_done_.fetch_add(1);
    }
    return out;
  });
  trace("scd=inspect_started count=" + std::to_string(scd_inspect_total_));
}

namespace {
// Does a BIOS header's region contradict the disc's own region characters? Unknown on either side never conflicts.
bool region_conflicts(const std::string &bios_region, const std::string &disc_region) {
  if (bios_region.empty() || disc_region.empty()) return false;
  const char letter = bios_region == "Japan" ? 'J' : bios_region == "USA" ? 'U' : 'E';
  return disc_region.find(letter) == std::string::npos;
}
std::string region_word(const std::string &region) { return region.empty() ? std::string("region not stated") : region; }
std::string file_name_of(const std::string &path) { return u8s(from_u8(path).filename()); }
}  // namespace

// The BIOS step of one review item, from what is known now: nothing chosen yet (request / ambiguity / refusal text) or a verified choice.
static void scd_refresh_review(ReviewItem &item, const std::function<const LibraryEntry *(const std::string &)> &find_identity) {
  ScdReview &r = item.scd;
  item.sega_cd = true;
  item.title = r.disc.title;
  item.entry_id.clear();
  if (r.disc.verdict != scd::DiscInspection::Verdict::ready) {
    item.verdict = ReviewItem::Verdict::invalid;
    item.message = r.disc.detail;
    item.next_action = r.disc.next_action;
    item.selected = false;
    return;
  }
  item.message = "Sega CD disc: " + r.disc.detail;
  if (r.chosen) {
    const std::string identity = scd::entry_identity(r.bios_sha256, r.disc.cue_sha256);
    if (const LibraryEntry *existing = find_identity(identity); existing != nullptr) {
      item.verdict = ReviewItem::Verdict::duplicate;
      item.entry_id = existing->id();
      item.title = existing->title();
      item.message = "Already in your library (same BIOS and disc). Nothing is added or replaced; its saves and recordings stay as they are.";
      item.next_action = "Open the game.";
      item.selected = false;
      return;
    }
    item.verdict = ReviewItem::Verdict::valid;
    item.selected = true;
    item.next_action = "Ready to add with the BIOS below. Add & prepare compiles a playable version from your BIOS and disc on this computer (experimental); Add only keeps it in your library.";
    return;
  }
  item.verdict = ReviewItem::Verdict::needs_choice;
  item.selected = false;
  item.next_action = "A Sega CD game needs your own BIOS image. Choose it below; nothing is added until you do.";
}

void App::poll_scd_inspect() {
  if (!scd_inspect_job_.valid() || scd_inspect_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  auto inspected = scd_inspect_job_.get();
  const auto find_identity = [this](const std::string &identity) { return catalog_.find("sega-cd", identity); };
  int cancelled = 0;
  for (auto &one : inspected) {
    if (one.disc.verdict == scd::DiscInspection::Verdict::cancelled) { ++cancelled; continue; }
    ReviewItem item;
    item.path = one.path;
    item.sega_cd = true;
    item.scd.disc = std::move(one.disc);
    item.scd.options = std::move(one.options);
    if (!layout_.scd_build_problem.empty() || !layout_.problem.empty()) item.prepare_blocked = "This installation cannot prepare Sega CD games here (its Sega CD runtime is missing), so only Add only is available; a verified prebuilt program can be attached on the game's page (Advanced).";
    if (std::any_of(review_.begin(), review_.end(), [&](const ReviewItem &o) { return o.sega_cd && o.path == item.path; })) continue;   // the same disc dropped twice
    if (item.scd.disc.verdict == scd::DiscInspection::Verdict::ready) {
      // Which remembered BIOS may be used without asking? Exactly one that is verified now, and whose header region does not contradict the disc.
      std::size_t usable = 0, pick = 0;
      for (std::size_t i = 0; i < item.scd.options.size(); ++i) {
        ScdReviewBios &o = item.scd.options[i];
        o.region_conflict = region_conflicts(o.profile.region, item.scd.disc.region);
        if (o.state == scd::BiosCheckState::ok) { ++usable; pick = i; }
      }
      const std::size_t total = item.scd.options.size();
      scd_refresh_review(item, find_identity);
      if (usable == 1 && !item.scd.options[pick].region_conflict) {
        item.scd.chosen = true;
        item.scd.bios_path = item.scd.options[pick].profile.path;
        item.scd.bios_sha256 = item.scd.options[pick].profile.sha256;
        item.scd.bios_region = item.scd.options[pick].profile.region;
        item.scd.note = "Using your remembered BIOS (checked just now). Choose another below if you want a different one.";
        scd_refresh_review(item, find_identity);
      } else if (usable > 1) {
        item.scd.note = "More than one verified BIOS is remembered. Choose which one this disc uses; it is never guessed.";
      } else if (usable == 1) {
        item.scd.note = "The remembered BIOS declares a different region than this disc. Use it only if you mean to, or choose another.";
      } else if (total > 0) {
        item.scd.note = "The remembered BIOS cannot be used as it is (see below). Locate it, or choose another.";
        item.scd.note_error = true;
      } else {
        item.scd.note = "No BIOS is remembered yet. Choose your own Sega CD BIOS image (Segarecomp does not include or download one).";
      }
    } else {
      scd_refresh_review(item, find_identity);
    }
    review_.push_back(std::move(item));
  }
  if (cancelled > 0) toast("Disc check cancelled.");
  go(Page::import_page);
  trace("scd=inspected total=" + std::to_string(inspected.size()) + " cancelled=" + std::to_string(cancelled) + " reviewed=" + std::to_string(review_.size()));
  if (!deferred_paths_.empty()) { auto next = std::move(deferred_paths_); deferred_paths_.clear(); start_import(std::move(next)); }
}

// A BIOS file chosen (or located) for review item `index`. `expected_sha256` set = "locate the remembered BIOS": the file must be that very BIOS.
void App::scd_review_choose_bios(std::size_t index, const fs::path &file, const std::string &expected_sha256) {
  if (index >= review_.size() || !review_[index].sega_cd) return;
  ReviewItem &item = review_[index];
  ScdReview &r = item.scd;
  const scd::BiosCheck check = scd::check_bios_file(file, expected_sha256);
  const auto find_identity = [this](const std::string &identity) { return catalog_.find("sega-cd", identity); };
  if (check.state != scd::BiosCheckState::ok) {
    r.note = (check.state == scd::BiosCheckState::changed ? std::string("That file is not the remembered BIOS (its SHA-256 differs).") : scd::bios_check_message(check.state)) +
             " Locate the right file, or choose another BIOS.";
    r.note_error = true;
    trace(std::string("scd=bios_refused reason=") + scd::bios_check_code(check.state));
    return;
  }
  r.chosen = true;
  r.bios_path = u8s(file);
  r.bios_sha256 = check.sha256;
  r.bios_region = check.region;
  r.note_error = false;
  r.note = "BIOS verified: " + file_name_of(r.bios_path) + " (" + region_word(check.region) + ", " + format_bytes(check.size) + "). It is remembered on this computer as a reference only and checked again every time it is used.";
  if (region_conflicts(check.region, r.disc.region)) r.note += " Its header declares a different region than this disc.";
  for (auto &o : r.options)   // a located remembered BIOS is valid again
    if (o.profile.sha256 == check.sha256) { o.state = scd::BiosCheckState::ok; o.profile.path = r.bios_path; }
  scd_refresh_review(item, find_identity);
  trace(std::string("scd=bios_chosen region=") + (check.region.empty() ? "unknown" : check.region) + (expected_sha256.empty() ? " via=choose" : " via=locate"));
}

void App::scd_review_use_profile(std::size_t index, std::size_t option) {
  if (index >= review_.size() || !review_[index].sega_cd || option >= review_[index].scd.options.size()) return;
  const scd::BiosProfile profile = review_[index].scd.options[option].profile;   // used only after it is measured again, right now
  scd_review_choose_bios(index, from_u8(profile.path), profile.sha256);
}

void App::scd_remember_bios(const scd::BiosProfile &profile) {
  scd::remember_bios_profile(bios_profiles_, profile);
  if (!scd::save_bios_profiles(bios_profiles_, bios_profiles_file_)) toast("The BIOS could not be remembered (settings folder not writable).", true);
}

// Add / Add & prepare of one Sega CD review item. Everything is measured again NOW (the files may have changed since the review): the BIOS, the CUE and the track files.
bool App::scd_commit_review_item(ReviewItem &item, std::vector<std::string> &added_ids, std::vector<std::string> &prepare_ids) {
  ScdReview &r = item.scd;
  const scd::BiosCheck bios = scd::check_bios_file(from_u8(r.bios_path), r.bios_sha256);
  if (bios.state != scd::BiosCheckState::ok) {
    r.chosen = false;
    r.note = std::string(bios.state == scd::BiosCheckState::changed ? "The BIOS changed after you chose it. " : "") + scd::bios_check_message(bios.state) + " Choose the BIOS again.";
    r.note_error = true;
    item.verdict = ReviewItem::Verdict::needs_choice;
    item.selected = false;
    trace(std::string("scd=add_refused refusal=") + scd::bios_check_code(bios.state));
    return false;
  }
  const scd::DiscInspection disc = scd::inspect_disc(from_u8(item.path));
  if (disc.verdict != scd::DiscInspection::Verdict::ready || disc.cue_sha256 != r.disc.cue_sha256) {
    r.disc = disc;
    item.verdict = ReviewItem::Verdict::invalid;
    item.message = disc.verdict == scd::DiscInspection::Verdict::ready ? std::string("The CUE sheet changed after it was checked.") : disc.detail;
    item.next_action = disc.verdict == scd::DiscInspection::Verdict::ready ? std::string("Add the disc again.") : disc.next_action;
    item.selected = false;
    trace(std::string("scd=add_refused refusal=") + disc.reason);
    return false;
  }
  scd::ImportRequest request;
  request.bios = from_u8(r.bios_path);
  request.cue = from_u8(item.path);
  const scd::ImportResult imported = scd::import_entry(request);   // measures the BIOS and the CUE once more and builds the record (no program: that is Game > Advanced)
  if (imported.record.bios_sha256.empty() || imported.record.cue_sha256.empty()) {
    r.note = scd::message(imported.refusal, imported.detail);
    r.note_error = true;
    item.verdict = ReviewItem::Verdict::needs_choice;
    item.selected = false;
    trace(std::string("scd=add_refused refusal=") + scd::code(imported.refusal));
    return false;
  }
  const auto added = catalog_.import_sega_cd(imported.record, imported.title, unix_now());
  if (added.entry == nullptr) { toast("Could not add " + item.title + ": " + added.error, true); return false; }
  scd::BiosProfile profile;
  profile.sha256 = bios.sha256;
  profile.size = bios.size;
  profile.region = bios.region;
  profile.path = r.bios_path;
  profile.verified_at = unix_now();
  scd_remember_bios(profile);
  added_ids.push_back(added.entry->id());
  if (item.preparable()) prepare_ids.push_back(added.entry->id());
  trace(std::string("scd=added program=none new=") + (added.added ? "1" : "0") + " region=" + (bios.region.empty() ? "unknown" : bios.region));
  return true;
}

}  // namespace launcher
