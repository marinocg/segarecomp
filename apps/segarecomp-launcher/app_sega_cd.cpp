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

std::string short_digest(const std::string &digest) { return digest.size() >= 16 ? digest.substr(0, 16) + "..." : digest; }

std::string file_name_of(const std::string &path) { return path.empty() ? std::string("not chosen") : u8s(from_u8(path).filename()); }

long long unix_now() { return static_cast<long long>(std::time(nullptr)); }

}  // namespace

// ---------------------------------------------------------------- labels

std::string App::scd_label(const StatusInfo &info) { return scd::state_label(info.scd.state); }

std::string App::scd_hint(const StatusInfo &info) {
  switch (info.scd.state) {
    case scd::State::ready:
      return "Verified: the BIOS, the disc files and the imported program match each other. The program was built outside Segarecomp.";
    case scd::State::no_program:
      return scd::message(scd::Refusal::unsupported_build);
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

void App::scd_poll() {
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
          open_details(id);
          toast(result.program_attached ? "Added. Verified prebuilt program available." : "Added. Import a verified program to play it.");
        }
      }
    } else if (LibraryEntry *entry = catalog_.find_id(target)) {
      if (result.program_attached) {
        entry->sega_cd = result.record;
        save_catalog();
        request_status_refresh();
        toast("Program verified and bound to this game.");
        trace("scd=program_attached");
      } else {
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
  if (game_ || scd_launch_job_.valid() || scd_import_job_.valid()) { toast("Wait for the running game or check to finish.", true); return; }
  {
    std::error_code ec;
    fs::create_directories(scd_dir(*entry) / "recordings", ec);
    if (ec) { show_message("The game could not be prepared", {"The folder for this game's run files cannot be written: " + ec.message()}); return; }
  }
  scd::RunOptions options;
  options.report = scd_dir(*entry) / "last-report.json";
  options.backup_ram = scd_dir(*entry) / "backup-ram.bin";
  if (!scd::ensure_backup_ram(options.backup_ram)) { show_message("The game could not be prepared", {"The folder for this game's run files cannot be written."}); return; }
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
  scd_last_outcome_[id] = text;
  // A clean close and a typed execution stop are shown as the end of the run; a refusal or an unexpected status is a failure with its log.
  const bool typed_stop = exit_code == 10;
  if (exit_code != 0 && !typed_stop && exit_code != 11) {
    failures_[id] = text;
    diagnostics_[id] = read_tail(game_dir_ / "run.log", 24000);
  }
  request_status_refresh();
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
            "A Sega CD game is your BIOS image plus your disc (the CUE file), added by reference: nothing is copied. Segarecomp cannot build Sega CD games yet, "
            "so playing needs a verified viewer program built with the developer procedure (see the Sega CD notes in the documentation). The program is checked "
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

// ---------------------------------------------------------------- the Details panel

float App::draw_scd_details(LibraryEntry &entry, const StatusInfo &info, float w) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const scd::EntryRecord &r = entry.sega_cd;
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float pad = ctx_.u(16.0F), inner = w - 2 * pad, lw = std::min(ctx_.u(110.0F), inner * 0.3F), vw = inner - lw;
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + ctx_.u(14.0F)));
  heading(ctx_, "SEGA CD");
  // ---- state ----
  {
    const ImU32 color = info.scd.state == scd::State::ready ? col::ok : (info.scd.state == scd::State::mismatched ? col::err : col::warn);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
    paragraph(ctx_, ctx_.bold, ctx_.u(13.0F), color, inner, scd_label(info));
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + ctx_.u(2.0F)));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner, scd_hint(info));
    ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  }
  const auto fact = [&](const char *label, const std::string &value, ImU32 color = col::text) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
    label_value(ctx_, lw, vw, label, value, color);
  };
  fact("BIOS", file_name_of(r.bios_path) + "  (" + format_bytes(r.bios_size) + ", SHA-256 " + short_digest(r.bios_sha256) + ")", info.scd.refusal == scd::Refusal::bios_missing || info.scd.refusal == scd::Refusal::bios_changed ? col::warn : col::text);
  fact("Disc (CUE)", file_name_of(r.cue_path) + "  (SHA-256 " + short_digest(r.cue_sha256) + ")");
  if (r.disc_files.empty()) fact("Disc files", "Known once a program is imported (the program names the track files it was built from).", col::text2);
  else fact("Disc files", std::to_string(r.disc_files.size()) + " track file(s) beside the CUE; " + (r.disc_verified ? "sizes and SHA-256 verified when the program was imported" : "not verified"), r.disc_verified ? col::text : col::warn);
  if (r.program.path.empty()) fact("Program", "none imported", col::text2);
  else {
    fact("Program", file_name_of(r.program.path) + "  (" + format_bytes(r.program.size) + ", SHA-256 " + short_digest(r.program.sha256) + ")");
    fact("Program kind", std::string(r.program.viewer ? "Viewer build (playable)" : "Headless-only build (not playable in the viewer)") + ", runtime contract " + std::to_string(r.program.runtime_abi),
         r.program.viewer && r.program.runtime_abi == scd::supported_runtime_abi ? col::text : col::warn);
    fact("Verified", fmt_date(r.program.verified_at) + "  -  the program's own statement matches your BIOS and disc; it is not signed or compiled by Segarecomp", col::text2);
  }
  fact("Audio", "Not available: sound is not part of this viewer.", col::text2);
  // ---- actions ----
  ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  const bool busy = scd_import_job_.valid() || scd_launch_job_.valid() || game_ != nullptr;
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
  if (button(ctx_, "scd_attach", r.program.path.empty() ? "Import program..." : "Replace program...", ImVec2(ctx_.u(180.0F), ctx_.u(32.0F)), Style::secondary, !busy, ctx_.icon("import"))) scd_browse("scd_attach", entry.id());
  ImGui::SameLine(0.0F, ctx_.u(8.0F));
  if (button(ctx_, "scd_verify", "Verify again", ImVec2(ctx_.u(140.0F), ctx_.u(32.0F)), Style::secondary, !busy && !r.program.path.empty(), ctx_.icon("check"))) scd_start_import(entry.id(), {}, {}, {});
  if (ImGui::IsItemHovered()) tooltip(ctx_, "Hashes the program and every disc file again (takes a few seconds for a full disc).");
  if (info.scd.refusal == scd::Refusal::bios_missing) { ImGui::SameLine(0.0F, ctx_.u(8.0F)); if (button(ctx_, "scd_locate_bios", "Locate BIOS...", ImVec2(ctx_.u(150.0F), ctx_.u(32.0F)), Style::secondary)) scd_browse("scd_locate_bios", entry.id()); }
  if (info.scd.refusal == scd::Refusal::cue_missing) { ImGui::SameLine(0.0F, ctx_.u(8.0F)); if (button(ctx_, "scd_locate_cue", "Locate disc...", ImVec2(ctx_.u(150.0F), ctx_.u(32.0F)), Style::secondary)) scd_browse("scd_locate_cue", entry.id()); }
  if (scd_import_job_.valid() && scd_import_target_ == entry.id()) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + ctx_.u(4.0F)));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spinner(ctx_, dl, ImVec2(p.x + ctx_.u(8.0F), p.y + ctx_.u(9.0F)), ctx_.u(7.0F), col::info);
    draw_text(dl, ctx_.regular, ctx_.u(12.5F), col::text, ImVec2(p.x + ctx_.u(26.0F), p.y), ("Verifying: " + format_bytes(scd_done_.load()) + (scd_total_ > 0 ? " of " + format_bytes(scd_total_) : std::string())).c_str());
    ImGui::Dummy(ImVec2(w, ctx_.u(22.0F)));
  }
  // ---- recordings ----
  ImGui::Dummy(ImVec2(1, ctx_.u(8.0F)));
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
  heading(ctx_, "INPUT RECORDING");
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
  checkbox(ctx_, "scd_record", "Record this session's controller input when I press Play", &scd_record_);
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
  paragraph(ctx_, ctx_.regular, ctx_.u(11.5F), col::dim, inner,
            "A recording is a small text script of button presses at exact frame boundaries; replaying it reproduces the session exactly. It holds no game data. "
            "The viewer's Pause menu is Esc; F6 holds the machine and F7 steps one frame while it is held.");
  const auto recordings = scd_recordings(entry);
  if (recordings.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner, "No recordings yet.");
  }
  for (std::size_t i = 0; i < recordings.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    std::error_code ec;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, p.y));
    const std::string name = u8s(recordings[i].filename());
    draw_text(dl, ctx_.regular, ctx_.u(12.0F), col::text, ImVec2(a.x + pad, p.y + ctx_.u(8.0F)), (name + "  (" + format_bytes(fs::file_size(recordings[i], ec)) + ")").c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x + w - pad - ctx_.u(250.0F), p.y));
    if (button(ctx_, "replay", "Replay", ImVec2(ctx_.u(120.0F), ctx_.u(28.0F)), Style::secondary, !busy && info.scd.state == scd::State::ready)) scd_play(entry.id(), 1, recordings[i]);
    ImGui::SameLine(0.0F, ctx_.u(6.0F));
    if (button(ctx_, "replay_fast", "No window", ImVec2(ctx_.u(120.0F), ctx_.u(28.0F)), Style::ghost, !busy && info.scd.state == scd::State::ready)) scd_play(entry.id(), 2, recordings[i]);
    if (ImGui::IsItemHovered()) tooltip(ctx_, "Replays the recording as fast as possible without a window and reports the result.");
    ImGui::PopID();
  }
  // ---- last run, build note ----
  if (const auto last = scd_last_outcome_.find(entry.id()); last != scd_last_outcome_.end()) {
    ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
    heading(ctx_, "LAST RUN");
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.5F), col::text, inner, last->second);
  } else if (!entry.sega_cd.last_stop.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
    paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner, "Last run: typed execution stop " + entry.sega_cd.last_stop + ".");
  }
  ImGui::Dummy(ImVec2(1, ctx_.u(6.0F)));
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
  heading(ctx_, "BUILDING");
  ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y));
  paragraph(ctx_, ctx_.regular, ctx_.u(12.0F), col::text2, inner,
            "Unsupported: automatic Sega CD compilation is not available in Segarecomp yet (it needs a multi-pass build with verified executable regions). "
            "Import a program built with the developer procedure instead; nothing is guessed or compiled here.");
  const float h = ImGui::GetCursorScreenPos().y - a.y + ctx_.u(14.0F);
  dl->ChannelsSetCurrent(0);
  card(ctx_, dl, a, ImVec2(a.x + w, a.y + h), false, false, rgba(8, 19, 46, 240));
  dl->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
  ImGui::Dummy(ImVec2(w, 0.0F));
  return h;
}

}  // namespace launcher
