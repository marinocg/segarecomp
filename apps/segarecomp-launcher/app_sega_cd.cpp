// SEG-078-T002 (ADR 0204): Sega CD in the launcher window - the "Add a Sega CD game" form, the game's details panel (inputs, program, verification,
// recordings), the verification worker and the run of an imported viewer program. The checks themselves live in sega_cd.cpp (no window); this file
// is the screen and the job plumbing, reusing the launcher's pickers, status badges, modals, process orchestration and failure banners.

#include "app.hpp"

#include "app_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

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

// ---------------------------------------------------------------- jobs

void App::scd_start_import(const std::string &target_id, const fs::path &bios, const fs::path &cue, const fs::path &program) {
  if (scd_import_job_.valid()) { toast("A Sega CD check is already running.", true); return; }
  scd_done_ = 0;
  scd_cancel_ = false;
  scd_import_target_ = target_id;
  scd_import_reverify_ = !target_id.empty() && program.empty();
  std::uint64_t total = 0;
  std::error_code ec;
  const LibraryEntry *existing = target_id.empty() ? nullptr : catalog_.find_id(target_id);
  const auto size_of = [&](const fs::path &p) { return p.empty() ? std::uint64_t{0} : static_cast<std::uint64_t>(fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0); };
  total = size_of(bios) + size_of(cue) + size_of(program);
  if (existing != nullptr) {
    for (const auto &f : existing->sega_cd.disc_files) total += f.size;
  }
  scd_total_ = total;
  if (target_id.empty()) {
    scd::ImportRequest request;
    request.bios = bios;
    request.cue = cue;
    request.program = program;
    scd_import_job_ = std::async(std::launch::async, [request, this] { return scd::import_entry(request, &scd_done_, &scd_cancel_); });
  } else if (existing != nullptr) {
    // Re-verify (no program given) or attach a program: both measure everything against the entry's recorded BIOS and CUE.
    scd::EntryRecord record = existing->sega_cd;
    const fs::path chosen = program.empty() ? from_u8(record.program.path) : program;
    scd_import_job_ = std::async(std::launch::async, [record, chosen, this] { return scd::attach_program(record, chosen, &scd_done_, &scd_cancel_); });
  }
  trace(std::string("scd=verify_started target=") + (target_id.empty() ? "new" : "existing"));
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
    if (target.empty()) {
      if (result.record.bios_sha256.empty() || result.record.cue_sha256.empty()) {
        scd_form_.message = scd::message(result.refusal, result.detail);
        scd_form_.error = true;
        trace(std::string("scd=add_refused refusal=") + scd::code(result.refusal));
      } else {
        const auto added = catalog_.import_sega_cd(result.record, result.title, unix_now());
        if (added.entry == nullptr) {
          scd_form_.message = added.error;
          scd_form_.error = true;
        } else {
          save_catalog();
          request_status_refresh();
          const std::string id = added.entry->id();
          trace(std::string("scd=added program=") + (result.program_attached ? "verified" : scd::code(result.refusal)) + (added.added ? " new=1" : " new=0"));
          scd_form_ = ScdForm{};
          if (!result.program_attached && result.refusal != scd::Refusal::none)
            show_message("The game was added, but the program was not accepted", {scd::message(result.refusal, result.detail), "The game stays in the library without a program."});
          if (added.added) queue_auto_covers({id});   // like a newly added ROM: an automatic, silent Libretro lookup (never in automation)
          open_details(id);
          toast(result.program_attached ? "Added. Verified prebuilt program available." : "Added. Import a verified program to play it.");
        }
      }
    } else if (LibraryEntry *entry = catalog_.find_id(target)) {
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
          // The same automatic policies as after a native build (a policy only ever removes generated build folders, never a save; see storage.hpp).
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
  if (purpose == "scd_bios") { scd_form_.bios = path; scd_form_.message.clear(); }
  else if (purpose == "scd_cue") { scd_form_.cue = path; scd_form_.message.clear(); }
  else if (purpose == "scd_program") { scd_form_.program = path; scd_form_.message.clear(); }
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
    save_catalog();
    request_status_refresh();
    toast(bios ? "BIOS location updated." : "Disc location updated.");
  }
}

// ---------------------------------------------------------------- the Import page card

void App::draw_scd_import(float w) {
  if (!scd_form_open_ && !scd_import_job_.valid() && scd_form_.message.empty()) return;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::Dummy(ImVec2(1, ctx_.u(14.0F)));
  heading(ctx_, "SEGA CD (DISC)");
  paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, w,
            "A Sega CD game is your BIOS image plus your disc (the CUE file), added by reference: nothing is copied. After adding it, choose Build to compile a playable program from these two "
            "files on this computer (experimental; code the disc does not contain verbatim is never guessed), or import a verified prebuilt program. A program is checked "
            "against exactly these two files.");
  ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  const bool busy = scd_import_job_.valid();
  const auto row = [&](const char *id, const char *label, const std::string &path, const char *purpose, const char *hint) {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float rh = ctx_.u(40.0F), bw = ctx_.u(120.0F);
    card(ctx_, dl, a, ImVec2(a.x + w, a.y + rh));
    draw_text(dl, ctx_.bold, ctx_.u(12.5F), col::text, ImVec2(a.x + ctx_.u(14.0F), a.y + ctx_.u(7.0F)), label);
    draw_text(dl, ctx_.regular, ctx_.u(11.5F), path.empty() ? col::dim : col::text2, ImVec2(a.x + ctx_.u(14.0F), a.y + ctx_.u(22.0F)),
              ellipsize(ctx_.regular, ctx_.u(11.5F), w - bw - ctx_.u(40.0F), path.empty() ? std::string(hint) : u8s(from_u8(path).filename())).c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - bw - ctx_.u(8.0F), a.y + (rh - ctx_.u(28.0F)) * 0.5F));
    ImGui::BeginDisabled(busy);
    if (button(ctx_, id, path.empty() ? "Choose..." : "Change...", ImVec2(bw, ctx_.u(28.0F)), Style::secondary, true, ctx_.icon("folder"))) scd_browse(purpose, "");
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rh + ctx_.u(5.0F)));
    ImGui::Dummy(ImVec2(w, 1));
  };
  row("scd_bios_pick", "BIOS image", scd_form_.bios, "scd_bios", "The Sega CD BIOS file you own (up to 128 KiB)");
  row("scd_cue_pick", "Disc (CUE sheet)", scd_form_.cue, "scd_cue", "The .cue file of your disc image; the track files must be beside it");
  row("scd_prog_pick", "Verified program (optional)", scd_form_.program, "scd_program", "A viewer program built for this BIOS and disc");
  const bool ready = !scd_form_.bios.empty() && !scd_form_.cue.empty() && !busy;
  if (button(ctx_, "scd_add", "Add Sega CD game", ImVec2(ctx_.u(200.0F), ctx_.u(34.0F)), Style::primary, ready, ctx_.icon("import")))
    scd_start_import({}, from_u8(scd_form_.bios), from_u8(scd_form_.cue), scd_form_.program.empty() ? fs::path{} : from_u8(scd_form_.program));
  if (ImGui::IsItemHovered() && !ready) tooltip(ctx_, busy ? "A check is running." : "Choose the BIOS image and the CUE file first.");
  if (busy) {
    ImGui::SameLine(0.0F, ctx_.u(14.0F));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spinner(ctx_, dl, ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(17.0F)), ctx_.u(7.0F), col::info);
    const std::uint64_t done = scd_done_.load();
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text, ImVec2(p.x + ctx_.u(26.0F), p.y + ctx_.u(9.0F)),
              ("Verifying: " + format_bytes(done) + (scd_total_ > 0 ? " of " + format_bytes(scd_total_) : std::string())).c_str());
    ImGui::Dummy(ImVec2(ctx_.u(260.0F), ctx_.u(28.0F)));
    ImGui::SameLine(0.0F, ctx_.u(8.0F));
    if (button(ctx_, "scd_cancel", "Cancel", ImVec2(ctx_.u(90.0F), ctx_.u(30.0F)), Style::ghost)) scd_cancel_ = true;
  }
  if (!scd_form_.message.empty()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(4.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), scd_form_.error ? col::err : col::ok, w, scd_form_.message);
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
}

}  // namespace launcher
