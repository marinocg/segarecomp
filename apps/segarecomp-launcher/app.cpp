#include "app.hpp"

#include <sstream>

#include "cover_download.hpp"
#include "covers.hpp"
#include "pixel_assets.hpp"
#include "rom_zip.hpp"

#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

namespace launcher {

namespace {

std::string u8s(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}
fs::path from_u8(const std::string &text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str()))); }

struct DialogCtx { App *app; std::string purpose, key; };

std::map<std::string, StatusInfo> compute_statuses(std::vector<LibraryEntry> entries, Layout layout, bool sms_optimized) {
  std::map<std::string, StatusInfo> out;
  for (const auto &e : entries) {
    StatusInfo info;
    if (e.platform_id == "sega-cd") {  // SEG-078-T002: no cache entry and no build; the cheap checks of sega_cd.hpp decide
      info.sega_cd = true;
      info.scd = scd::quick_status(e.sega_cd);
      info.key = "scd-" + e.sha256.substr(0, 16);
      info.rom_missing = info.scd.state == scd::State::inputs_not_configured;
      info.status = info.scd.state == scd::State::ready ? GameStatus::ready
                    : (info.scd.state == scd::State::inputs_not_configured || info.scd.state == scd::State::no_program) ? GameStatus::not_built : GameStatus::stale;
      out[e.id()] = info;
      continue;
    }
    RomView view = view_from_entry(e);
    view.sms_optimized_available = sms_optimized;
    std::error_code ec;
    info.rom_missing = true;
    for (const auto &p : e.rom_paths)
      if (fs::is_regular_file(from_u8(p), ec)) { info.rom_missing = false; break; }
    info.compat_known = fs::is_regular_file(layout.runtime / "compat" / (e.sha256 + ".json"), ec);
    info.entry = entry_dir(view, layout);
    info.key = u8s(info.entry.filename());
    info.has_failed_dir = fs::is_directory(info.entry.parent_path() / (info.key + ".failed"), ec);
    for (const auto &b : e.builds)
      if (b.key == info.key) { info.build = b; info.build_known = true; }
    const bool mapper_needed = e.platform_id == "master-system" && e.mapper.empty() && e.mapper_manifest_sha256.empty();
    if (!mapper_needed && entry_ready_for(info.entry, view, layout)) {
      info.status = GameStatus::ready;
      info.policy = read_policy_report(info.entry);
      info.observed_region = read_observed_region(info.entry);
      if (!info.build_known) {
        info.adopt = true;
        SDL_PathInfo path_info;
        if (SDL_GetPathInfo(u8s(info.entry / "status.json").c_str(), &path_info)) info.file_time = static_cast<long long>(path_info.modify_time / 1000000000LL);
      }
    } else if (e.archived) {
      info.status = GameStatus::metadata_only;
    } else if (info.has_failed_dir && !mapper_needed) {
      info.status = GameStatus::failed;
    } else if (!e.builds.empty() || !e.current_build_key.empty()) {
      info.status = GameStatus::stale;
    } else {
      info.status = GameStatus::not_built;
    }
    // SEG-069-T005: no ready build of the CURRENT identity. An executable an earlier release built may still be playable; this is a separate,
    // fail-closed question (older_build.hpp) and never relaxes the exact identity check above.
    if (!mapper_needed && !e.archived && info.status != GameStatus::ready && !e.builds.empty()) {
      info.older = find_playable_older_build(e, info.key, view.optimized() ? "optimized" : "compatibility", layout, cache_root() / "games");
      if (info.older.playable && info.status == GameStatus::stale) info.status = GameStatus::playable_older;
    }
    if (!mapper_needed && view.supports_optimized()) {
      RomView other = view;
      other.aot_policy = view.optimized() ? "compatibility" : "optimized";
      other.aot_model = other.aot_policy == "optimized" && other.platform_id == "genesis" ? "gen2" : "v1";
      info.other_mode_supported = true;
      info.other_mode_ready = entry_ready_for(entry_dir(other, layout), other, layout);
    }
    out[e.id()] = info;
  }
  return out;
}

} // namespace

void dialog_trampoline(void *userdata, const char *const *files, int) {
  std::unique_ptr<DialogCtx> ctx(static_cast<DialogCtx *>(userdata));
  App::DialogEvent event;
  event.purpose = ctx->purpose;
  event.key = ctx->key;
  if (files == nullptr) event.error = SDL_GetError();
  else
    for (int i = 0; files[i] != nullptr; ++i) event.paths.emplace_back(files[i]);
  if (files != nullptr && event.paths.empty()) return;  // the user cancelled: nothing changes
  std::lock_guard<std::mutex> lock(ctx->app->dialog_mutex_);
  ctx->app->dialog_events_.push_back(std::move(event));
}

App::App() : catalog_(fs::path{}) {
  layout_ = locate_layout();
  cache_dir_ = cache_root();
  library_dir_ = library_root();
  covers_dir_ = library_dir_ / "covers";
  catalog_file_ = library_dir_ / "library.json";
  settings_file_ = library_dir_ / "launcher-settings.json";
  viewer_settings_file_ = library_dir_ / "viewer-settings.cfg";
  catalog_ = Catalog(catalog_file_);
}

App::~App() = default;

long long App::now_seconds() { return static_cast<long long>(std::time(nullptr)); }

void App::trace(const std::string &line) {
  if (!trace_path_.empty()) std::ofstream(trace_path_, std::ios::app) << line << '\n';
}

void App::toast(const std::string &text, bool error) {
  toasts_.push_back({text, error, SDL_GetTicks() + 4200U});
  if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
}

void App::show_message(const std::string &title, const std::vector<std::string> &lines) {
  modal_ = Modal{};
  modal_.kind = Modal::Kind::message;
  modal_.title = title;
  modal_.lines = lines;
}

void App::save_catalog() {
  std::string error;
  if (catalog_.read_only()) return;
  if (!catalog_.save(&error)) toast("The library could not be saved: " + error, true);
}

void App::save_settings() {
  if (!save_launcher_settings(settings_, settings_file_)) toast("Settings could not be saved.", true);
}

bool App::apply_intermediates_choice(bool remove_after_build) {
  LauncherSettings next = settings_;
  next.cleanup.mode = remove_after_build ? "intermediates" : "never";
  next.intermediates_choice_made = true;
  if (!save_launcher_settings(next, settings_file_)) return false;  // nothing is claimed: the question stays open and can be retried
  settings_ = next;
  trace(std::string("onboarding=intermediates_chosen mode=") + settings_.cleanup.mode);
  return true;
}

void App::viewer_settings_changed() {
  sv_settings_sanitize(&viewer_settings_);
  std::error_code ec;
  fs::create_directories(library_dir_, ec);
  if (sv_settings_save(&viewer_settings_, u8s(viewer_settings_file_).c_str()) != 0) toast("Viewer settings could not be saved.", true);
}

// ---------------------------------------------------------------- window / UI setup

bool App::init_window() {
  diag_ = sv_diag_create();
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    const std::string why = SDL_GetError();
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return false; }
    sv_diag_note_init(diag_, 1, 0, why.c_str());  // visible on the Input diagnostics page and logged: the launcher continues keyboard/mouse-only
  }
  {
    // A user-supplied gamecontrollerdb.txt (library folder, or SEGARECOMP_GAMEPAD_MAPPINGS) turns a raw joystick into an explicit gamepad.
    const char *env = SDL_getenv("SEGARECOMP_GAMEPAD_MAPPINGS");
    std::error_code ec;
    if (env != nullptr && *env != '\0') (void)sv_diag_load_mapping_file(env);
    else if (fs::is_regular_file(library_dir_ / "gamecontrollerdb.txt", ec)) (void)sv_diag_load_mapping_file(u8s(library_dir_ / "gamecontrollerdb.txt").c_str());
  }
  sv_diag_scan(diag_);
  sv_diag_log_summary(diag_, "launcher start");
  const float base_w = 1080.0F, base_h = 740.0F;
  window_ = SDL_CreateWindow("Segarecomp", static_cast<int>(base_w), static_cast<int>(base_h), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window_ == nullptr) { std::fprintf(stderr, "window failed: %s\n", SDL_GetError()); return false; }
  // The application icon (title bar / taskbar / dock): the largest size first, smaller sizes as alternates the OS picks from.
  if (SDL_Surface *icon = load_asset_surface("icons/Segarecomp-256.png")) {
    for (const char *alt : {"icons/Segarecomp-128.png", "icons/Segarecomp-48.png", "icons/Segarecomp-32.png", "icons/Segarecomp-16.png"})
      if (SDL_Surface *s = load_asset_surface(alt)) { SDL_AddSurfaceAlternateImage(icon, s); SDL_DestroySurface(s); }
    SDL_SetWindowIcon(window_, icon);
    SDL_DestroySurface(icon);
  }
  // SDL3 sizes windows in points: on a DPI-aware Windows/Linux window a point is a physical pixel, so the default size follows the display scale.
  float window_scale = 1.0F;
  const float display_scale = SDL_GetWindowDisplayScale(window_), density = SDL_GetWindowPixelDensity(window_);
  if (display_scale > 0.0F && density > 0.0F) window_scale = std::clamp(display_scale / density, 1.0F, 4.0F);
  SDL_Rect usable{};
  if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window_), &usable) && usable.h > 0 && base_h * window_scale > static_cast<float>(usable.h) * 0.92F)
    window_scale = std::max(1.0F, static_cast<float>(usable.h) * 0.92F / base_h);
  int w = static_cast<int>(base_w * window_scale), h = static_cast<int>(base_h * window_scale);
  if (const char *size = SDL_getenv("SEGARECOMP_SHOT_SIZE")) { int sw = 0, sh = 0; if (std::sscanf(size, "%dx%d", &sw, &sh) == 2 && sw >= 320 && sh >= 240) { w = sw; h = sh; } }
  if (window_scale > 1.01F || SDL_getenv("SEGARECOMP_SHOT_SIZE")) { SDL_SetWindowSize(window_, w, h); SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED); }
  SDL_SetWindowMinimumSize(window_, static_cast<int>(720.0F * std::max(1.0F, window_scale * 0.75F)), static_cast<int>(520.0F * std::max(1.0F, window_scale * 0.75F)));
  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (renderer_ == nullptr) { std::fprintf(stderr, "renderer failed: %s\n", SDL_GetError()); return false; }
  SDL_SetRenderVSync(renderer_, 1);
  return true;
}

void App::init_ui() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ImGui::GetCurrentContext()->ConfigNavWindowingWithGamepad = false;  // holding West/Y must not open Dear ImGui's "(Untitled)" window switcher over a single-window UI
  ImGui::StyleColorsDark();
  ImGui::GetStyle().FontScaleDpi = 1.0F;
  ImGuiStyle &style = ImGui::GetStyle();
  style.ScrollbarRounding = 0.0F;
  style.Colors[ImGuiCol_ScrollbarBg] = ImColor(5, 12, 30, 120);
  style.Colors[ImGuiCol_ScrollbarGrab] = ImColor(35, 62, 120);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(77, 103, 167);
  style.Colors[ImGuiCol_ScrollbarGrabActive] = ImColor(64, 191, 245);
  style.Colors[ImGuiCol_NavHighlight] = ImColor(0, 0, 0, 0);  // the widgets draw their own focus ring
  ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
  ImGui_ImplSDLRenderer3_Init(renderer_);
  // Body text: the scalable embedded font (readable mixed case at any size). Titles and section headings: the repository's Silkscreen Bold.
  ctx_.regular = io.Fonts->AddFontDefaultVector();
  ctx_.bold = io.Fonts->AddFontDefaultVector();  // a second instance of the same face: draw_text draws it twice for weight
  ui::bold_font = ctx_.bold;
  {
    const fs::path path = asset_path("fonts/Silkscreen-Bold.ttf");
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) ctx_.display = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 0.0F);
    if (ctx_.display == nullptr) ctx_.display = ctx_.regular;
  }
  ctx_.renderer = renderer_;
}

void App::shutdown() {
  trace("shutdown=workers");
  if (job_) job_->wait();
  game_.reset();
  sv_diag_reclaim_exclusive_devices(diag_);
  scan_cancel_ = true;
  if (scan_job_.valid()) scan_job_.wait();
  if (status_job_.valid()) status_job_.wait();
  if (import_job_.valid()) import_job_.wait();
  if (maint_job_.valid()) maint_job_.wait();
  if (sms_probe_.valid()) sms_probe_.wait();
  if (hid_job_.valid()) hid_job_.wait();
  cover_cancel_ = true;
  if (cover_job_.valid()) cover_job_.wait();
  trace("shutdown=textures");
  for (auto &entry : cover_cache_)
    if (entry.second != nullptr) SDL_DestroyTexture(entry.second);
  cover_cache_.clear();
  ctx_.release();
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
  }
  if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
  if (window_ != nullptr) SDL_DestroyWindow(window_);
  trace("shutdown=input");
  sv_diag_destroy(diag_);
  diag_ = nullptr;
  trace("shutdown=sdl");
  SDL_Quit();
}

// ---------------------------------------------------------------- data

RomView App::view_for(const LibraryEntry &entry) const {
  RomView view = view_from_entry(entry);
  view.sms_optimized_available = sms_optimized_available_;
  return view;
}

fs::path App::usable_rom_path(const LibraryEntry &entry) const {
  std::error_code ec;
  for (const auto &p : entry.rom_paths)
    if (fs::is_regular_file(from_u8(p), ec)) return from_u8(p);
  return {};
}

const StatusInfo &App::status_of(const std::string &id) const {
  static const StatusInfo unknown;
  const auto found = statuses_.find(id);
  return found == statuses_.end() ? unknown : found->second;
}

std::string App::status_label(const StatusInfo &info) const {
  if (info.sega_cd) return scd_label(info);
  switch (info.status) {
    case GameStatus::ready: return "Ready";
    case GameStatus::metadata_only: return "Metadata only";
    case GameStatus::stale: return "Needs rebuild";
    case GameStatus::playable_older: return "Playable (older build)";
    case GameStatus::failed: return info.older.playable ? "Playable (older build)" : "Build failed";
    case GameStatus::not_built: return "Not built";
  }
  return {};
}

ImU32 App::status_color(const StatusInfo &info) const {
  if (info.sega_cd) return info.scd.state == scd::State::ready ? ui::col::ok : (info.scd.state == scd::State::mismatched ? ui::col::err : ui::col::warn);
  switch (info.status) {
    case GameStatus::ready: return ui::col::ok;
    case GameStatus::failed: return ui::col::err;
    case GameStatus::stale: return ui::col::warn;
    case GameStatus::playable_older: return ui::col::warn;
    case GameStatus::metadata_only: return ui::col::info;
    case GameStatus::not_built: return ui::col::text2;
  }
  return ui::col::text2;
}

void App::request_status_refresh() {
  if (status_pending_) { status_again_ = true; return; }
  status_pending_ = true;
  status_job_ = std::async(std::launch::async, compute_statuses, catalog_.entries(), layout_, sms_optimized_available_);
}

void App::poll_status_refresh() {
  if (!status_pending_ || status_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  statuses_ = status_job_.get();
  status_pending_ = false;
  bool changed = false;
  for (auto &[id, info] : statuses_) {
    if (!info.adopt) continue;
    if (LibraryEntry *entry = catalog_.find_id(id)) {
      BuildRecord record;
      record.key = info.key;
      record.built_at = info.file_time;
      record.mode = info.policy.present && info.policy.requested == "optimized" ? "optimized" : "compatibility";
      catalog_.record_build(*entry, record);
      info.build = record;
      info.build_known = true;
      changed = true;
    }
    info.adopt = false;
  }
  if (changed) save_catalog();
  if (status_again_) { status_again_ = false; request_status_refresh(); }
}

void App::request_scan() {
  if (scanning_) { scan_again_ = true; return; }
  scanning_ = true;
  scan_cancel_ = false;
  // The scan reads a snapshot of the catalog (it must not touch the live one from a worker).
  scan_job_ = std::async(std::launch::async, [snapshot = catalog_.entries(), cache = cache_dir_, lib = library_dir_, cancel = &scan_cancel_] {
    Catalog copy{fs::path{}};
    copy.entries() = snapshot;
    return scan_storage(copy, cache, lib, cancel);
  });
}

void App::poll_scan() {
  if (!scanning_ || scan_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  report_ = scan_job_.get();
  scanning_ = false;
  report_valid_ = report_.complete;
  if (settings_.cleanup.mode == "soft_limit") soft_limit_exceeded_ = report_.builds_total() > static_cast<std::uintmax_t>(settings_.cleanup.soft_limit_mb) * 1024U * 1024U;
  else soft_limit_exceeded_ = false;
  if (scan_again_) { scan_again_ = false; request_scan(); }
}

// Sequential post-build maintenance, one step per call: (1) the finished games' cleanup, (2) the keep-last-N policy on a fresh scan. Each step waits
// (its flag stays set) until the maintenance guard is available; nothing is dropped, and nothing blocks gameplay or starts another worker.
void App::pump_cleanup() {
  if (maintenance_running_ || busy_.any_activity()) return;
  if (!pending_cleanup_ids_.empty()) {
    const std::vector<std::string> ids(pending_cleanup_ids_.begin(), pending_cleanup_ids_.end());
    pending_cleanup_ids_.clear();
    Catalog copy{fs::path{}};
    copy.entries() = catalog_.entries();
    const StorageReport fresh = scan_storage(copy, cache_dir_, library_dir_);
    Plan plan;
    plan.kind = settings_.keep_old_artifacts ? PlanKind::intermediates : PlanKind::superseded;
    // Recompiling replaces the game's older builds (Settings > Build & Runtime > "Keep old build artifacts" opts out); the build just produced
    // is the current one and stays. Intermediates inside a folder that is deleted whole are not scheduled twice.
    if (!settings_.keep_old_artifacts) plan = plan_superseded(fresh, ids, busy_);
    if (settings_.cleanup.mode == "intermediates") plan = merge_plans_non_overlapping(std::move(plan), plan_intermediates(fresh, ids, busy_));
    if (!plan.items.empty()) run_plan(plan);
    return;  // the policy step waits for this cleanup and the rescan that follows it
  }
  if (policy_eval_pending_ && !scanning_ && !scan_again_) {
    policy_eval_pending_ = false;
    if (report_valid_) evaluate_policy_after_scan();
  }
}

void App::evaluate_policy_after_scan() {
  if (settings_.cleanup.mode != "keep_last_n" || maintenance_running_) return;
  std::set<std::string> active;
  if (!job_id_.empty()) active.insert(job_id_);
  if (!game_id_.empty()) active.insert(game_id_);
  const auto victims = policy_eviction_candidates(catalog_, report_, settings_.cleanup, active);
  if (victims.empty()) return;
  Plan plan = plan_archive(report_, victims, busy_);
  if (plan.items.empty()) return;
  toast("Cleanup policy: reducing " + std::to_string(plan.entry_ids.size()) + " older game(s) to metadata only.");
  run_plan(plan);
}

// ---------------------------------------------------------------- maintenance

void App::run_plan(const Plan &plan) {
  if (maintenance_running_) { toast("Another cleanup is already running.", true); return; }
  if (!busy_.try_begin_maintenance()) { toast("Cleanup is not possible while a build or game is running.", true); return; }
  maintenance_running_ = true;
  maint_plan_ = plan;
  maint_job_ = std::async(std::launch::async, [plan, cache = cache_dir_, this] { return execute_plan(plan, cache, busy_); });
}

void App::poll_maintenance() {
  if (!maintenance_running_ || maint_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  const ExecResult result = maint_job_.get();
  busy_.end_maintenance();
  maintenance_running_ = false;
  apply_plan_to_catalog(maint_plan_, catalog_);
  if (maint_plan_.kind == PlanKind::remove_from_library)
    for (const auto &id : maint_plan_.entry_ids) { selected_.erase(id); storage_selected_.erase(id); if (detail_id_ == id) go(Page::library); }
  save_catalog();
  request_status_refresh();
  request_scan();
  if (result.ok()) toast("Freed " + format_bytes(result.bytes_freed) + ".");
  else {
    std::vector<std::string> lines{"Freed " + format_bytes(result.bytes_freed) + ", but some items were not removed:"};
    for (std::size_t i = 0; i < result.problems.size() && i < 8; ++i) lines.push_back("- " + result.problems[i]);
    if (result.problems.size() > 8) lines.push_back("... and " + std::to_string(result.problems.size() - 8) + " more.");
    lines.push_back("Nothing outside the Segarecomp cache was touched.");
    show_message("Cleanup finished with problems", lines);
  }
}

// ---------------------------------------------------------------- import

void App::browse_import() {
  if (use_builtin_picker()) { open_picker("import", ""); return; }
  static const SDL_DialogFileFilter filters[] = {{"ROMs and zipped ROMs", "md;bin;gen;smd;sms;zip"}, {"Genesis / Mega Drive ROMs", "md;bin;gen;smd"}, {"Master System ROMs", "sms"}, {"Zipped ROMs", "zip"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(dialog_trampoline, new DialogCtx{this, "import", ""}, window_, filters, 5, nullptr, true);
}

void App::browse_locate(const std::string &id) {
  if (use_builtin_picker()) { open_picker("locate", id); return; }
  static const SDL_DialogFileFilter filters[] = {{"ROMs", "md;bin;gen;smd;sms;zip"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(dialog_trampoline, new DialogCtx{this, "locate", id}, window_, filters, 2, nullptr, false);
}

void App::scd_browse(const std::string &purpose, const std::string &id) {
  if (use_builtin_picker()) { open_picker(purpose, id); return; }
  static const SDL_DialogFileFilter bios_filters[] = {{"BIOS images", "bin;md;rom"}, {"All files", "*"}};
  static const SDL_DialogFileFilter cue_filters[] = {{"CUE sheets", "cue"}, {"All files", "*"}};
  static const SDL_DialogFileFilter program_filters[] = {{"Programs and all files", "*"}};
  const bool bios = purpose == "scd_bios" || purpose == "scd_locate_bios", cue = purpose == "scd_cue" || purpose == "scd_locate_cue";
  SDL_ShowOpenFileDialog(dialog_trampoline, new DialogCtx{this, purpose, id}, window_, bios ? bios_filters : (cue ? cue_filters : program_filters), bios || cue ? 2 : 1, nullptr, false);
}

void App::browse_cover(const std::string &id) {
  if (use_builtin_picker()) { open_picker("cover", id); return; }
  static const SDL_DialogFileFilter filters[] = {{"Images (PNG, JPEG)", "png;jpg;jpeg"}};
  SDL_ShowOpenFileDialog(dialog_trampoline, new DialogCtx{this, "cover", id}, window_, filters, 1, nullptr, false);
}

bool App::cover_download_possible(const LibraryEntry &entry) const {
  return !cover_job_.valid() && !libretro_system_dir(entry.platform_id).empty() && !entry.title().empty();
}

void App::queue_auto_covers(const std::vector<std::string> &ids) {
  // Automation (GUI scripts, screenshots, autoplay) and SEGARECOMP_NO_COVER_FETCH never touch the network.
  if (SDL_getenv("SEGARECOMP_NO_COVER_FETCH") != nullptr || !script_.empty() || autoplay_ || !shot_path_.empty()) return;
  for (const auto &id : ids)
    if (std::find(auto_cover_pending_.begin(), auto_cover_pending_.end(), id) == auto_cover_pending_.end()) auto_cover_pending_.push_back(id);
}

void App::start_cover_download(const std::vector<std::string> &ids, bool silent) {
  if (cover_job_.valid()) return;
  std::vector<std::pair<std::string, std::pair<std::string, CoverQuery>>> work;  // id -> (sha256, query)
  for (const auto &id : ids) {
    const LibraryEntry *entry = catalog_.find_id(id);
    if (entry == nullptr || !cover_download_possible(*entry)) continue;
    CoverQuery q;
    q.platform_id = entry->platform_id;
    if (!entry->display_title.empty()) q.titles.push_back(entry->display_title);
    if (!entry->detected_title.empty()) q.titles.push_back(entry->detected_title);
    for (const auto &rom : entry->rom_paths) q.rom_names.push_back(rom);  // only the file name is used
    q.preferred_regions = settings_.preferred_regions;
    work.push_back({id, {entry->sha256, std::move(q)}});
  }
  if (work.empty()) { if (!silent) toast("No game here can be looked up on Libretro."); return; }
  cover_silent_ = silent;
  cover_total_ = static_cast<int>(work.size());
  cover_done_ = 0;
  cover_cancel_ = false;
  trace("cover=started count=" + std::to_string(work.size()));
  cover_job_ = std::async(std::launch::async, [work = std::move(work), dir = cache_dir_ / "cover-downloads", covers = covers_dir_, done = &cover_done_, cancel = &cover_cancel_] {
    std::vector<CoverDownload> results;
    const HttpGet get = curl_http_get();
    const bool have_curl = !system_curl().empty();
    for (const auto &[id, item] : work) {
      if (cancel->load()) break;
      CoverDownload r;
      r.id = id;
      if (!have_curl) r.error = "curl is not available on this computer, so nothing can be downloaded. Choose a local image instead.";
      else {
        const CoverFetch fetched = fetch_libretro_cover(item.second, dir, get);
        r.matched_name = fetched.matched_name;
        r.source = fetched.source;
        if (!fetched.ok) r.error = fetched.error;
        else {  // the same validation as a local image (PNG/JPEG signature, size and dimensions), then an atomic copy
          const CoverImport imported = import_cover(fetched.image, covers, item.first);
          if (imported.ok) r.file_name = imported.file_name;
          else r.error = "The downloaded image cannot be used: " + imported.error;
          std::error_code ec;
          fs::remove(fetched.image, ec);
        }
      }
      results.push_back(std::move(r));
      ++*done;
    }
    return results;
  });
}

void App::poll_cover_download() {
  if (!cover_job_.valid() || cover_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  const std::vector<CoverDownload> results = cover_job_.get();
  int added = 0;
  std::vector<std::string> problems;
  for (const auto &r : results) {
    LibraryEntry *entry = catalog_.find_id(r.id);
    if (!r.error.empty() || entry == nullptr) {
      trace("cover=failed id=" + r.id + " reason=" + r.error);
      if (entry != nullptr) problems.push_back(entry->title() + ": " + r.error);
      continue;
    }
    if (!entry->cover.empty() && entry->cover != r.file_name) remove_cover_file(covers_dir_, entry->cover);
    entry->cover = r.file_name;
    if (auto it = cover_cache_.find(r.file_name); it != cover_cache_.end()) { if (it->second) SDL_DestroyTexture(it->second); cover_cache_.erase(it); }
    cover_missing_.erase(r.file_name);
    trace("cover=downloaded id=" + r.id + " name=" + r.matched_name + " source=" + r.source);
    ++added;
  }
  if (added > 0) save_catalog();
  if (cover_silent_) return;  // automatic lookups fail silently
  if (results.size() == 1) {
    if (added == 1) toast("Cover downloaded: \"" + results[0].matched_name + "\" from " + results[0].source + ".");
    else if (!problems.empty()) show_message("No cover was downloaded", {problems[0]});
  } else if (!results.empty()) {
    toast(std::to_string(added) + " of " + std::to_string(results.size()) + " covers downloaded.", added == 0);
    if (!problems.empty()) { problems.resize(std::min<std::size_t>(problems.size(), 8)); show_message("Some covers were not downloaded", problems); }
  }
}

void App::start_import(std::vector<std::string> paths, bool build_after) {
  if (import_job_.valid()) { for (auto &p : paths) deferred_paths_.push_back(std::move(p)); return; }
  if (paths.empty()) return;
  import_total_ = static_cast<int>(paths.size());
  import_done_ = 0;
  import_build_after_ = build_after;
  import_results_.clear();
  import_job_ = std::async(std::launch::async, [paths = std::move(paths), lay = layout_, roms = library_dir_ / "roms", done = &import_done_] {
    std::vector<std::pair<std::string, RomView>> out;
    for (const auto &p : paths) {
      out.emplace_back(p, inspect_rom_or_zip(from_u8(p), lay, roms));
      done->fetch_add(1);
    }
    return out;
  });
}

void App::poll_import() {
  if (!import_job_.valid() || import_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  auto inspected = import_job_.get();
  std::vector<std::string> added_ids, buildable;
  for (auto &[path, rom] : inspected) {
    ImportItem item;
    item.path = path;
    item.title = rom.title;
    if (!rom.error.empty()) { item.outcome = "error"; item.message = "Could not be read: " + rom.error; }
    else if (!rom.supported_platform) { item.outcome = "unsupported"; item.message = "Not a supported Genesis or Master System ROM."; }
    else {
      const bool existed = catalog_.find(rom.platform_id, rom.sha256) != nullptr;
      const auto result = catalog_.import_rom(rom, now_seconds());
      if (result.entry == nullptr) { item.outcome = "error"; item.message = result.error; }
      else {
        item.entry_id = result.entry->id();
        item.title = result.entry->title();
        item.needs_mapper = rom.platform_id == "master-system" && result.entry->mapper.empty() && rom.mapper_manifest.empty();
        if (!existed) {
          item.outcome = "added";
          item.message = item.needs_mapper ? "Added. Declare the cartridge mapper before building." : "Added to the library (not built yet).";
          if (const std::string cover = existing_cover_for(covers_dir_, rom.sha256); !cover.empty()) result.entry->cover = cover;
          added_ids.push_back(item.entry_id);
          if (!item.needs_mapper) buildable.push_back(item.entry_id);
        } else if (result.path_added) { item.outcome = "location"; item.message = "Already in the library; this location was added."; }
        else { item.outcome = "duplicate"; item.message = "Already in the library (same content)."; }
      }
    }
    import_results_.push_back(item);
  }
  save_catalog();
  request_status_refresh();
  request_scan();
  if (import_build_after_) for (const auto &id : buildable) build_queue_.push(id);
  queue_auto_covers(added_ids);
  const int ok = static_cast<int>(std::count_if(import_results_.begin(), import_results_.end(), [](const ImportItem &i) { return !i.entry_id.empty(); }));
  if (import_results_.size() == 1 && ok == 1) { open_details(import_results_[0].entry_id); toast(import_results_[0].message); }
  else { go(Page::import_page); toast(std::to_string(added_ids.size()) + " added, " + std::to_string(import_results_.size() - added_ids.size()) + " skipped or already present."); }
  trace("import=done added=" + std::to_string(added_ids.size()) + " total=" + std::to_string(import_results_.size()));
  if (!deferred_paths_.empty()) { auto next = std::move(deferred_paths_); deferred_paths_.clear(); start_import(std::move(next), false); }
}

void App::open_details(const std::string &id) {
  if (page_ == Page::details && detail_id_ == id) return;
  detail_id_ = id;
  nav_reset_ = true;
  editing_title_ = false;
  show_log_ = false;
  show_fallback_details_ = false;
  show_advanced_ = false;
  if (page_ != Page::details) previous_page_ = page_;
  page_ = Page::details;
  trace("details=open title=" + (catalog_.find_id(id) != nullptr ? catalog_.find_id(id)->title() : std::string("?")));
}

void App::close_details() {
  if (page_ != Page::details) return;
  editing_title_ = false;
  page_ = previous_page_ == Page::details ? Page::library : previous_page_;
  if (page_ == Page::library) {
    // Back to exactly where the user was: search, filters, sort and view are untouched members; scroll and focus are restored here.
    restore_library_scroll_ = true;
    return_focus_id_ = detail_id_;
    nav_reset_ = false;
  } else {
    nav_reset_ = true;
  }
  trace(std::string("details=close page=") + page_name());
}

void App::go(Page page) {
  if (page == page_) return;
  if (page_ == Page::details && page == previous_page_) { close_details(); return; }
  if (page_ != Page::details) previous_page_ = page_;
  page_ = page;
  nav_reset_ = true;
  editing_title_ = false;
  if (page == Page::storage) request_scan();
}

// ---------------------------------------------------------------- build / run

bool App::start_build(const std::string &id, bool restart_after) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr) return false;
  if (entry->platform_id == "sega-cd") {  // ADR 0204: a typed, actionable "unsupported", never a silent bypass
    show_message("Sega CD games cannot be built here yet", {scd::message(scd::Refusal::unsupported_build), "Open the game and use Import program... to attach one."});
    trace("build=refused reason=unsupported_build platform=sega-cd");
    return false;
  }
  if (job_) { toast("A build is already running.", true); return false; }
  if (!layout_.problem.empty()) { show_message("Installation incomplete", {layout_.problem}); return false; }
  const fs::path rom_path = usable_rom_path(*entry);
  if (rom_path.empty()) {
    show_message("The ROM file is missing", {"Segarecomp keeps only a reference to your ROM, and it is not at its recorded location.", "Use Locate ROM to point to it again; the game's library data is unchanged."});
    return false;
  }
  RomView view = inspect_rom_file(rom_path, layout_);
  if (!view.error.empty()) { show_message("The ROM could not be read", {view.error}); return false; }
  if (view.sha256 != entry->sha256) {
    show_message("The ROM file has changed", {"The file at this location no longer matches the game in the library (its SHA-256 differs).", "Use Locate ROM to choose the original file, or import the changed file as a different game."});
    return false;
  }
  // stored per-game options, then product-stated availability
  view.title = entry->title();
  // SEG-069-T002: a Master System game whose stored intent is Optimized must not be built as Compatibility only because the asynchronous
  // capability probe has not answered yet: wait for the product's statement (the worker is already running it; the result is memoized).
  if (view.platform_id == "master-system" && entry->aot_policy == "optimized" && sms_probe_.valid()) {
    sms_optimized_available_ = sms_probe_.get();
    request_status_refresh();
  }
  view.sms_optimized_available = sms_optimized_available_;
  if (view.platform_id == "master-system") { if (!entry->mapper.empty()) view.mapper = entry->mapper; if (view.sms_optimized_available) view.aot_policy = entry->aot_policy; }
  if (view.supports_optimized() && view.platform_id == "genesis") { view.aot_policy = entry->aot_policy; view.aot_model = "gen2"; }  // Genesis Optimized is always the gen2 model
  if (view.supports_hint_alias_discovery()) view.hint_alias_discovery = entry->hint_alias_discovery;
  if (view.supports_region()) view.region = entry->region;
  if (view.needs_mapper()) { show_message("Declare the cartridge mapper", {"The Master System header never identifies the mapper, so it is not guessed.", "Choose Sega mapper or ROM only (32 KiB) in the game's build options."}); return false; }
  const fs::path dir = entry_dir(view, layout_);
  const std::string key = u8s(dir.filename());
  if (!busy_.try_begin_build(key)) { toast("Cleanup is running; try again in a moment.", true); return false; }
  job_view_ = view;
  job_ = std::make_unique<BuildJob>(view, layout_);
  job_id_ = id;
  job_key_ = key;
  job_restart_after_ = restart_after;
  failures_.erase(id);
  diagnostics_.erase(id);
  trace(std::string("build=started mode=") + (view.optimized() ? "optimized" : "compatibility"));
  return true;
}

void App::dequeue_build(const std::string &id) {
  // A queued entry owns nothing yet (no process, no folder), so removing it touches nothing but the FIFO.
  if (!build_queue_.remove(id)) return;
  if (const LibraryEntry *e = catalog_.find_id(id)) toast("Removed from the build queue: " + e->title() + ".");
  trace("build=dequeued");
}

void App::cancel_build(const std::string &id) {
  if (!is_building(id) || job_->cancel_requested()) return;  // a second request while stopping is ignored
  trace("build=cancel_requested");
  if (job_->request_cancel()) {
    // The cancelled attempt must not auto-continue into a play or a restart.
    restart_pending_ = false;
    job_restart_after_ = false;
    toast("Stopping the build...");
  } else {
    toast("The build has just finished; it was not cancelled.");
  }
}

void App::stop_or_dequeue_build(const std::string &id) {
  if (is_building(id)) cancel_build(id);
  else if (is_queued(id)) dequeue_build(id);
}

void App::request_build(const std::string &id) {
  if (is_building(id) || is_queued(id)) return;
  if (const LibraryEntry *e = catalog_.find_id(id); e != nullptr && e->platform_id == "sega-cd") { start_build(id); return; }  // the typed refusal above
  if (job_ || !build_queue_.empty() || maintenance_running_) {
    build_queue_.push(id);
    if (const LibraryEntry *e = catalog_.find_id(id)) toast("Queued: " + e->title() + " (build " + std::to_string(build_count()) + ").");
    return;
  }
  start_build(id);
}

App::Badge App::badge_for(const LibraryEntry &entry, const StatusInfo &info) const {
  if (is_building(entry.id())) return {"Building", ui::col::warn, true};
  if (is_queued(entry.id())) return {"Queued", ui::col::info, false};
  if (info.sega_cd && game_ && game_id_ == entry.id()) return {"Running", ui::col::ok, false};
  if (info.rom_missing && !info.runnable() && !info.sega_cd) return {"ROM missing", ui::col::warn, false};
  return {status_label(info), status_color(info), false};
}

void App::finish_build() {
  job_->wait();
  const bool ok = job_->succeeded();
  const std::string message = job_->message();
  const fs::path entry_path = job_->entry();
  const BuildOutcome outcome = job_->outcome();
  busy_.end_build(job_key_);
  const std::string id = job_id_;
  const bool restart = job_restart_after_;
  job_.reset();
  job_id_.clear();
  LibraryEntry *entry = catalog_.find_id(id);
  if (outcome == BuildOutcome::cancelled) {
    // Neither a success nor a failure: no failure record, no diagnostics, no restart/autoplay continuation; the game keeps its prior state.
    restart_pending_ = false;
    failures_.erase(id);
    diagnostics_.erase(id);
    last_build_outcome_ = "cancelled";
    trace("build=cancelled");
    request_status_refresh();
    request_scan();
    toast("Build cancelled.");
    if (autoplay_) {
      autoplay_pending_ = false;
      trace("final=cancelled");
      SDL_Event quit;
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
    }
    return;
  }
  last_build_outcome_ = ok ? "ok" : "failed";
  if (ok) {
    if (entry != nullptr) {
      BuildRecord record;
      record.key = u8s(entry_path.filename());
      record.built_at = now_seconds();
      record.mode = job_view_.optimized() ? "optimized" : "compatibility";
      record.version = SEGARECOMP_LAUNCHER_VERSION;
      record.toolchain = std::string("zig ") + SEGARECOMP_ZIG_VERSION;
      catalog_.record_build(*entry, record);
      save_catalog();
    }
    request_status_refresh();
    request_scan();
    // Post-build cleanup is DEFERRED work, retained until it has run: it needs the maintenance guard, which is unavailable while any game runs
    // (the UI lets you play another game during a build). pump_cleanup() performs it, then the keep-last-N policy, strictly in that order.
    if (!settings_.keep_old_artifacts || settings_.cleanup.mode == "intermediates") pending_cleanup_ids_.insert(id);
    if (settings_.cleanup.mode == "keep_last_n" || settings_.cleanup.mode == "soft_limit") policy_eval_pending_ = true;
    if (restart) { restart_pending_ = false; launch(id, true); }
  } else {
    failures_[id] = message;
    const fs::path failed_dir = entry_path.parent_path() / (u8s(entry_path.filename()) + ".failed");
    diagnostics_[id] = (restart ? notices_[id] + "\nThe Compatibility build then failed:\n" : std::string()) + read_tail(failed_dir / "build.log", 24000);
    trace(std::string("build=failed restart_pending=") + (restart ? "1" : "0") + " message=" + message);
    restart_pending_ = false;
    request_status_refresh();
    request_scan();
    toast("Build failed: " + message, true);
  }
}

bool App::launch(const std::string &id, bool restarted) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || game_) return false;
  if (entry->platform_id == "sega-cd") { scd_play(id, 0); return true; }  // SEG-078-T002: verified program, not a cache entry
  RomView view = view_for(*entry);
  if (const std::string refusal = launch_refusal(view); !refusal.empty()) { failures_[id] = refusal; diagnostics_[id] = refusal; return false; }
  fs::path dir = entry_dir(view, layout_);
  bool older_build = false;
  if (!entry_ready_for(dir, view, layout_)) {
    // Never start an executable that does not match the entry's current identity (stale, foreign or removed) -- except an earlier release's build that
    // is verified playable RIGHT NOW (not from the possibly outdated status list): it runs exactly as recorded, never relabelled as the current request.
    const OlderBuild older = find_playable_older_build(*entry, u8s(dir.filename()), view.optimized() ? "optimized" : "compatibility", layout_, cache_root() / "games");
    if (!older.playable) {
      show_message("This build cannot be played", {"The stored build no longer matches this game's current identity (a different release, option set or a removed build).",
                                                   older.candidate_seen ? "An earlier build cannot be used: " + older.reason + "." : std::string("There is no earlier build to fall back on."), "Recompile the game to play it."});
      request_status_refresh();
      return false;
    }
    dir = older.dir;
    older_build = true;
    trace("run=older_build version=" + (older.version.empty() ? std::string("unknown") : older.version) + " mode=" + older.mode);
  }
  const std::string key = u8s(dir.filename());
  if (!busy_.try_begin_run(key)) { toast("Cleanup is running; try again in a moment.", true); return false; }
  sv_diag_yield_exclusive_devices(diag_);  // a libusb-driven controller can be opened by one process only: free it for the game
  game_ = std::make_unique<GameRun>(view, layout_, dir, std::vector<std::string>{}, older_build);
  if (!game_->started()) {
    sv_diag_reclaim_exclusive_devices(diag_);
    busy_.end_run(key);
    game_.reset();
    failures_[id] = "The game could not be started.";
    diagnostics_[id] = failures_[id];
    request_status_refresh();
    return false;
  }
  game_id_ = id;
  game_key_ = key;
  game_dir_ = dir;
  game_view_ = view;
  game_report_ = read_policy_report(dir);
  restarted_this_play_ = restarted;
  failures_.erase(id);
  trace(std::string("run=started mode=") + effective_mode(game_report_) + " restarted=" + (restarted ? "1" : "0") + " requested=" + (view.optimized() ? "optimized" : "compatibility") +
        " label=" + running_label(game_report_, restarted));
  return true;
}

void App::finish_run() {
  if (game_is_scd_) { scd_finish_run(); return; }
  const int exit_code = game_->exit_code();
  const std::string id = game_id_;
  const RomView ran = game_view_;
  const PolicyReport report = game_report_;
  const RestartDecision next = decide_after_exit(running_view(ran, report), exit_code, restarted_this_play_);
  game_.reset();
  sv_diag_reclaim_exclusive_devices(diag_);
  busy_.end_run(game_key_);
  game_id_.clear();
  trace("run=exited code=" + std::to_string(exit_code) + " mode=" + effective_mode(report));
  LibraryEntry *entry = catalog_.find_id(id);
  const std::string verdict = exit_code == 0 ? "clean" : (next.restart ? "unadmitted_pc" : "failed");
  if (entry != nullptr) { catalog_.record_run(*entry, exit_code, verdict, now_seconds()); save_catalog(); }
  if (exit_code == 0) { request_status_refresh(); return; }
  if (next.restart && entry != nullptr) {
    // The typed unadmitted_pc stop of an Optimized program: tell the user, switch this game to Compatibility, restart from reset (once).
    notices_[id] = next.notice;
    trace("restart=compatibility notice=" + next.notice);
    restarted_this_play_ = true;
    entry->aot_policy = "compatibility";
    entry->aot_model = "v1";
    save_catalog();
    const RomView compat = view_for(*entry);
    const fs::path dir = entry_dir(compat, layout_);
    restart_pending_ = true;
    if (entry_ready_for(dir, compat, layout_)) { restart_pending_ = false; launch(id, true); }
    else start_build(id, true);
    return;
  }
  failures_[id] = "The game stopped unexpectedly.";
  diagnostics_[id] = read_tail((game_dir_.empty() ? entry_dir(view_for(*entry), layout_) : game_dir_) / "run.log", 24000);
  trace("state=failed reason=game_stopped_unexpectedly");
  request_status_refresh();
}

// ---------------------------------------------------------------- frame

void App::process_events(bool &running) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    sv_diag_event(diag_, &event);
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) sv_diag_release(diag_);
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_QUIT) running = false;
    if (event.type == SDL_EVENT_DROP_BEGIN) dragging_ = true;
    if (event.type == SDL_EVENT_DROP_COMPLETE) dragging_ = false;
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
      std::vector<std::string> one{event.drop.data};
      if (modal_.kind == Modal::Kind::none) start_import(std::move(one), import_build_option_);
    }
  }
}

void App::update_jobs() {
  {
    std::vector<DialogEvent> events;
    { std::lock_guard<std::mutex> lock(dialog_mutex_); events.swap(dialog_events_); }
    for (auto &e : events) {
      if (!e.error.empty() && e.paths.empty()) {
        // No native chooser is available (no portal under WSL, a minimal desktop): the built-in picker does the same job.
        if (e.purpose == "import" || e.purpose == "locate" || e.purpose == "cover" || e.purpose.rfind("scd_", 0) == 0) { toast("The system file chooser is not available; using the built-in picker."); open_picker(e.purpose, e.key); continue; }
        show_message("The file picker could not be opened", {"SDL: " + e.error, "On Linux this usually means no file-chooser service (XDG Desktop Portal) is available - common under WSL/WSLg.", "Drop the file onto this window instead."});
        continue;
      }
      if (e.purpose == "import") start_import(e.paths, import_build_option_);
      else if (e.purpose.rfind("scd_", 0) == 0) scd_dialog_result(e.purpose, e.key, e.paths);
      else if (e.purpose == "locate" && !e.paths.empty()) {
        const RomView view = inspect_rom_or_zip(from_u8(e.paths[0]), layout_, library_dir_ / "roms");
        LibraryEntry *entry = catalog_.find_id(e.key);
        if (entry == nullptr) continue;
        if (!view.error.empty()) show_message("The file could not be read", {view.error});
        else if (!catalog_.relocate(*entry, view.path, view.sha256)) show_message("That is a different ROM", {"The chosen file does not match this game (its SHA-256 differs).", "Pick the original file, or import it as a separate game."});
        else { save_catalog(); request_status_refresh(); toast("ROM location updated."); }
      } else if (e.purpose == "cover" && !e.paths.empty()) {
        LibraryEntry *entry = catalog_.find_id(e.key);
        if (entry == nullptr) continue;
        const CoverImport result = import_cover(from_u8(e.paths[0]), covers_dir_, entry->sha256);
        if (!result.ok) show_message("That image cannot be used", {result.error});
        else {
          if (!entry->cover.empty() && entry->cover != result.file_name) remove_cover_file(covers_dir_, entry->cover);
          entry->cover = result.file_name;
          if (auto it = cover_cache_.find(result.file_name); it != cover_cache_.end()) { if (it->second) SDL_DestroyTexture(it->second); cover_cache_.erase(it); }
          cover_missing_.erase(result.file_name);
          save_catalog();
          toast("Cover updated.");
        }
      }
    }
  }
  poll_status_refresh();
  poll_scan();
  poll_import();
  scd_poll();
  poll_maintenance();
  poll_cover_download();
  if (!cover_job_.valid() && !auto_cover_pending_.empty()) {
    std::vector<std::string> ids;
    ids.swap(auto_cover_pending_);
    ids.erase(std::remove_if(ids.begin(), ids.end(), [this](const std::string &id) { const LibraryEntry *e = catalog_.find_id(id); return e == nullptr || !e->cover.empty(); }), ids.end());
    if (!ids.empty()) start_cover_download(ids, true);
  }
  if (sms_probe_.valid() && sms_probe_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    sms_optimized_available_ = sms_probe_.get();
    request_status_refresh();
  }
  if (job_ && job_->finished()) finish_build();
  if (game_ && game_->poll()) finish_run();
  pump_cleanup();
  if (!job_ && !build_queue_.empty() && !maintenance_running_ && modal_.kind == Modal::Kind::none) {
    const std::string next = build_queue_.pop_front();
    start_build(next);
  }
  // Automation: press Play once the selected game is ready (building first when there is no matching build); quit when the run is over.
  if (autoplay_ && !status_pending_ && !detail_id_.empty() && !job_ && !game_ && !maintenance_running_ && !restart_pending_) {
    const StatusInfo &info = status_of(detail_id_);
    const LibraryEntry *entry = catalog_.find_id(detail_id_);
    if (autoplay_pending_) {
      if (info.status == GameStatus::ready) {
        autoplay_pending_ = false;
        autoplay_started_ = true;
        trace(std::string("autoplay=press mode=") + (entry != nullptr && view_for(*entry).optimized() ? "optimized" : "compatibility"));
        launch(detail_id_, false);
      } else if (!autoplay_build_tried_) {
        autoplay_build_tried_ = true;
        trace(std::string("autoplay=build mode=") + (entry != nullptr && view_for(*entry).optimized() ? "optimized" : "compatibility"));
        if (!start_build(detail_id_)) { trace("state=failed reason=build_not_started"); autoplay_pending_ = false; autoplay_started_ = true; }
      } else if (failures_.count(detail_id_) != 0) {
        autoplay_pending_ = false;  // the build failed: report it as the final state
        autoplay_started_ = true;
      }
    } else if (autoplay_started_) {
      const bool failed = failures_.count(detail_id_) != 0;
      const RomView view = entry != nullptr ? view_for(*entry) : RomView{};
      const PolicyReport report = entry != nullptr ? read_policy_report(entry_dir(view, layout_)) : PolicyReport{};
      trace(std::string("final=") + (failed ? "failed" : "ready") + " mode=" + (view.optimized() ? "optimized" : "compatibility") + " restarted=" + (restarted_this_play_ ? "1" : "0") + " entry_requested=" + report.requested +
            " effective=" + effective_mode(report));
      autoplay_started_ = false;
      SDL_Event quit;
      quit.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit);
    }
  }
}

const char *App::page_name() const {
  switch (page_) {
    case Page::library: return "library";
    case Page::details: return "details";
    case Page::import_page: return "import";
    case Page::storage: return "storage";
    case Page::settings: return "settings";
  }
  return "?";
}

// Test seam: the screen rectangle of the item that has navigation focus ("x,y,w,h", window units), "-" when none.
static std::string nav_rect_text() {
  const ImGuiContext &g = *ImGui::GetCurrentContext();
  if (g.NavWindow == nullptr || g.NavId == 0) return "-";
  const ImRect r = g.NavWindow->NavRectRel[g.NavLayer];
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "%d,%d,%d,%d", static_cast<int>(g.NavWindow->Pos.x + r.Min.x), static_cast<int>(g.NavWindow->Pos.y + r.Min.y), static_cast<int>(r.GetWidth()), static_cast<int>(r.GetHeight()));
  return buffer;
}

// One command per few frames. Commands: `key <Tab|Return|Escape|Down|Up|Left|Right|Space|F>` (F with Ctrl), `text <string>`, `frames <n>`,
// `pad_attach`, `pad <south|east|dpad_up|dpad_down|dpad_left|dpad_right|start|back> [down|up]`, `expect page=<name>`, `expect visible=<n>`, `expect modal=<0|1>`, `quit`.
void App::run_script() {
  if (script_at_ >= script_.size()) return;
  if (script_wait_ > 0) { --script_wait_; return; }
  const std::string line = script_[script_at_++];
  script_wait_ = 4;
  std::string cmd = line.substr(0, line.find(' ')), arg = line.find(' ') == std::string::npos ? std::string() : line.substr(line.find(' ') + 1);
  const auto push_key = [&](SDL_Scancode sc, bool down, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.common.timestamp = SDL_GetTicksNS();
    e.key.windowID = SDL_GetWindowID(window_);
    e.key.scancode = sc;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    e.key.mod = mod;
    e.key.down = down;
    SDL_PushEvent(&e);
  };
  if (cmd == "key") {
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    SDL_Keymod mod = SDL_KMOD_NONE;
    if (arg == "Tab") sc = SDL_SCANCODE_TAB; else if (arg == "Return") sc = SDL_SCANCODE_RETURN; else if (arg == "Escape") sc = SDL_SCANCODE_ESCAPE;
    else if (arg == "Down") sc = SDL_SCANCODE_DOWN; else if (arg == "Up") sc = SDL_SCANCODE_UP; else if (arg == "Left") sc = SDL_SCANCODE_LEFT;
    else if (arg == "Right") sc = SDL_SCANCODE_RIGHT; else if (arg == "Space") sc = SDL_SCANCODE_SPACE; else if (arg == "F") { sc = SDL_SCANCODE_F; mod = ImGui::GetIO().ConfigMacOSXBehaviors ? SDL_KMOD_LGUI : SDL_KMOD_LCTRL; }  // the shortcut modifier: Cmd on macOS (ImGui swaps Ctrl/Cmd), Ctrl elsewhere
    if (sc == SDL_SCANCODE_UNKNOWN) { trace("script=unknown_key " + arg); return; }
    const SDL_Scancode mod_key = mod == SDL_KMOD_LGUI ? SDL_SCANCODE_LGUI : SDL_SCANCODE_LCTRL;
    if (mod != SDL_KMOD_NONE) push_key(mod_key, true, mod);
    push_key(sc, true, mod);
    script_key_up_.push_back({frame_ + 2, static_cast<int>(sc)});
    if (mod != SDL_KMOD_NONE) script_key_up_.push_back({frame_ + 3, static_cast<int>(mod_key)});
  } else if (cmd == "text") {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.common.timestamp = SDL_GetTicksNS();
    e.text.windowID = SDL_GetWindowID(window_);
    static std::string keep;
    keep = arg;
    e.text.text = keep.c_str();
    SDL_PushEvent(&e);
  } else if (cmd == "frames") {
    script_wait_ = std::max(1, std::atoi(arg.c_str()));
  } else if (cmd == "pad_attach") {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Script Pad";
    script_pad_ = SDL_AttachVirtualJoystick(&desc);
    script_joy_ = script_pad_ != 0 ? SDL_OpenJoystick(script_pad_) : nullptr;
    script_wait_ = 20;
  } else if (cmd == "pad_detach") {
    if (script_joy_ != nullptr) { SDL_CloseJoystick(script_joy_); script_joy_ = nullptr; }
    if (script_pad_ != 0) { SDL_DetachVirtualJoystick(script_pad_); script_pad_ = 0; }
    script_wait_ = 12;
  } else if (cmd == "pad_raw_attach") {  // a joystick SDL has no gamepad mapping for (input diagnostics state B)
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.naxes = 4;
    desc.nbuttons = 12;
    desc.vendor_id = 0x1234;
    desc.product_id = 0x0001;
    desc.name = "Script Raw Stick";
    script_pad_ = SDL_AttachVirtualJoystick(&desc);
    script_joy_ = script_pad_ != 0 ? SDL_OpenJoystick(script_pad_) : nullptr;
    script_wait_ = 20;
  } else if (cmd == "pad_lock") {  // SDL_OpenGamepad will fail for the next gamepad (input diagnostics state C)
    if (arg == "on") sv_diag_set_open_hook([](SDL_JoystickID) -> SDL_Gamepad * { SDL_SetError("simulated: the device is locked by another program"); return nullptr; });
    else sv_diag_set_open_hook(nullptr);
  } else if (cmd == "pad_axis" && script_joy_ != nullptr) {  // pad_axis <leftx|lefty|rightx|righty> <-32768..32767>
    const std::string name = arg.substr(0, arg.find(' '));
    const int value = std::atoi(arg.substr(arg.find(' ') + 1).c_str());
    const int axis = name == "leftx" ? SDL_GAMEPAD_AXIS_LEFTX : (name == "lefty" ? SDL_GAMEPAD_AXIS_LEFTY : (name == "rightx" ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_RIGHTY));
    SDL_SetJoystickVirtualAxis(script_joy_, axis, static_cast<Sint16>(value));
    script_wait_ = 2;
  } else if (cmd == "focus_card") {  // give card/row <n> of the visible list keyboard focus (then `key Return` activates it the normal way)
    const auto vis = visible_entries();
    const std::size_t n = static_cast<std::size_t>(std::atoi(arg.c_str()));
    if (n < vis.size()) focus_card_request_ = vis[n]->id();
  } else if (cmd == "mouse_card" || cmd == "mouse_fav" || cmd == "mouse_play") {  // move the pointer to card <n>'s centre / favorite star / Play button
    const auto vis = visible_entries();
    const std::size_t n = static_cast<std::size_t>(std::atoi(arg.c_str()));
    const auto &rects = cmd == "mouse_card" ? card_rects_ : (cmd == "mouse_fav" ? fav_rects_ : play_rects_);
    if (n < vis.size()) {
      if (const auto found = rects.find(vis[n]->id()); found != rects.end()) {
        script_mouse_ = found->second.GetCenter();
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.common.timestamp = SDL_GetTicksNS();
        e.motion.windowID = SDL_GetWindowID(window_);
        e.motion.x = script_mouse_.x;
        e.motion.y = script_mouse_.y;
        SDL_PushEvent(&e);
      } else trace("script=no_rect " + cmd + " " + arg);
    }
  } else if (cmd == "click") {  // a left click at the pointer: press now, release a few frames later
    for (const bool down : {true, false}) {
      SDL_Event e;
      SDL_zero(e);
      e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      e.common.timestamp = SDL_GetTicksNS();
      e.button.windowID = SDL_GetWindowID(window_);
      e.button.button = SDL_BUTTON_LEFT;
      e.button.down = down;
      e.button.clicks = 1;
      e.button.x = script_mouse_.x;
      e.button.y = script_mouse_.y;
      if (down) SDL_PushEvent(&e); else script_mouse_up_ = {frame_ + 3, e};
    }
  } else if (cmd == "build") {  // request a build of the open game (starts now or joins the FIFO)
    if (!detail_id_.empty()) request_build(detail_id_);
  } else if (cmd == "pad_seek" && script_joy_ != nullptr) {
    // pad_seek <focus-id-or-label> <dpad_down|dpad_up|dpad_left|dpad_right> [max presses]: press that D-pad direction until the control is focused. Real virtual-
    // gamepad button events only; it proves a control is reachable in that direction, and records `expect ok|FAIL seek=<target>`.
    std::istringstream words(arg);
    std::string target, dir;
    int limit = 40;
    words >> target >> dir >> limit;
    const std::string have_id = ui::focus_id, have_label = focus_label_;
    const bool here = have_id == target || have_label == target;
    static const std::pair<const char *, int> dirs[] = {{"dpad_down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"dpad_up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"dpad_left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"dpad_right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}};
    int button = -1;
    for (const auto &d : dirs) if (dir == d.first) button = d.second;
    if (seek_left_ < 0) seek_left_ = limit;
    if (seek_pressed_) {  // release what the previous step pressed, then look again
      for (const auto &d : dirs) SDL_SetJoystickVirtualButton(script_joy_, d.second, false);
      seek_pressed_ = false;
      --script_at_;
      script_wait_ = 3;
    } else if (here) {
      trace("expect ok seek=" + target + " presses=" + std::to_string(limit - seek_left_));
      seek_left_ = -1;
    } else if (seek_left_ == 0 || button < 0) {
      trace("expect FAIL seek=" + target + " actual=" + (have_id.empty() ? have_label : have_id));
      seek_left_ = -1;
    } else {
      SDL_SetJoystickVirtualButton(script_joy_, button, true);
      --seek_left_;
      seek_pressed_ = true;
      --script_at_;
      script_wait_ = 2;
    }
  } else if (cmd == "play") {  // the Play action of the open game (does nothing when it cannot be played)
    if (!detail_id_.empty() && can_play(status_of(detail_id_))) play(detail_id_);
  } else if (cmd == "shot") {
    script_shot_ = arg;
  } else if (cmd == "page") {  // go to a page: library | import | storage | settings
    go(arg == "import" ? Page::import_page : arg == "storage" ? Page::storage : arg == "settings" ? Page::settings : Page::library);
  } else if (cmd == "scd_form_open") {
    scd_form_open_ = arg != "off";
  } else if (cmd == "scd_form") {  // SEG-078-T002: fill the "Add a Sega CD game" form: <bios>|<cue>|<program or empty>
    std::vector<std::string> parts;
    std::size_t at = 0;
    for (std::size_t bar; (bar = arg.find('|', at)) != std::string::npos; at = bar + 1) parts.push_back(arg.substr(at, bar - at));
    parts.push_back(arg.substr(at));
    parts.resize(3);
    scd_form_.bios = parts[0]; scd_form_.cue = parts[1]; scd_form_.program = parts[2];
    scd_form_open_ = true;
  } else if (cmd == "scd_add") {  // press "Add Sega CD game"
    if (!scd_form_.bios.empty() && !scd_form_.cue.empty() && !scd_import_job_.valid())
      scd_start_import({}, from_u8(scd_form_.bios), from_u8(scd_form_.cue), scd_form_.program.empty() ? fs::path{} : from_u8(scd_form_.program));
  } else if (cmd == "scd_attach") {  // "Import program..." for the open game, with this path
    if (!detail_id_.empty()) scd_start_import(detail_id_, {}, {}, from_u8(arg));
  } else if (cmd == "scd_verify") {  // "Verify again" for the open game
    if (!detail_id_.empty()) scd_start_import(detail_id_, {}, {}, {});
  } else if (cmd == "scd_record") {  // the "Record this session" switch
    scd_record_ = arg == "on";
  } else if (cmd == "scd_replay") {  // replay the newest recording of the open game: `viewer` or `windowless`
    if (const LibraryEntry *e = detail_id_.empty() ? nullptr : catalog_.find_id(detail_id_)) {
      const auto recordings = scd_recordings(*e);
      if (!recordings.empty()) scd_play(detail_id_, arg == "windowless" ? 2 : 1, recordings.front());
      else trace("script=no_recording");
    }
  } else if (cmd == "wait_scd") {  // hold the script until no Sega CD check or program is running
    if (scd_import_job_.valid() || scd_launch_job_.valid() || game_) { --script_at_; script_wait_ = 2; }
  } else if (cmd == "wait_play_end") {  // hold the script until no game is running
    if (game_) { --script_at_; script_wait_ = 2; }
  } else if (cmd == "cancel_build") {  // the Cancel build / Unqueue action for the open game
    if (!detail_id_.empty()) stop_or_dequeue_build(detail_id_);
  } else if (cmd == "wait_idle") {  // hold the script until no build is running or queued
    if (job_ || !build_queue_.empty() || status_pending_) { --script_at_; script_wait_ = 2; }
  } else if (cmd == "wait_file") {  // hold the script until a file exists (at most ~20 s of frames)
    std::error_code ec;
    if (!fs::exists(fs::path(arg), ec) && ++script_wait_file_frames_ < 1200) { --script_at_; script_wait_ = 1; }
    else script_wait_file_frames_ = 0;
  } else if (cmd == "scroll_library") {
    library_scroll_ = static_cast<float>(std::atof(arg.c_str()));
    restore_library_scroll_ = true;
  } else if (cmd == "platform_filter") {
    platform_filter_ = std::atoi(arg.c_str());
  } else if (cmd == "view") {
    settings_.view = arg == "list" ? "list" : "grid";
  } else if (cmd == "settings_tab") {
    settings_tab_ = std::atoi(arg.c_str());
    go(Page::settings);
  } else if (cmd == "pad" && script_joy_ != nullptr) {
    const std::string name = arg.substr(0, arg.find(' '));
    const bool down = arg.size() < name.size() + 3 || arg.substr(name.size() + 1) != "up";
    static const std::pair<const char *, int> buttons[] = {{"south", SDL_GAMEPAD_BUTTON_SOUTH}, {"east", SDL_GAMEPAD_BUTTON_EAST}, {"dpad_up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"dpad_down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
                                                           {"dpad_left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"dpad_right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {"start", SDL_GAMEPAD_BUTTON_START}, {"back", SDL_GAMEPAD_BUTTON_BACK}, {"west", SDL_GAMEPAD_BUTTON_WEST}, {"north", SDL_GAMEPAD_BUTTON_NORTH},
                                                           {"lshoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"rshoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
    for (const auto &b : buttons)
      if (name == b.first) SDL_SetJoystickVirtualButton(script_joy_, b.second, down);
    script_wait_ = down ? 2 : 3;  // a press is one frame long: a longer hold would auto-repeat in real time
  } else if (cmd == "expect") {
    const std::string key = arg.substr(0, arg.find('=')), value = arg.substr(arg.find('=') + 1);
    std::string actual;
    if (key == "page") actual = page_name();
    else if (key == "visible") actual = std::to_string(visible_entries().size());
    else if (key == "focus") actual = focus_label_;
    else if (key == "focus_id") actual = ui::focus_id;
    else if (key == "modal") actual = modal_.kind == Modal::Kind::none ? "0" : "1";
    else if (key == "input_state") actual = sv_pad_state_code(input_state());
    else if (key == "pad_count") actual = std::to_string(sv_diag_count(diag_));
    else if (key == "pad_events") { unsigned long total = 0; for (int i = 0; i < sv_diag_count(diag_); ++i) total += sv_diag_device(diag_, i)->events; actual = std::to_string(total); }
    else if (key == "pad_error") actual = sv_diag_count(diag_) > 0 ? sv_diag_device(diag_, 0)->open_error : "";
    else if (key == "nav_taps") actual = std::to_string(nav_taps_);
    else if (key == "search") actual = search_;
    else if (key == "favorites") actual = std::to_string(std::count_if(catalog_.entries().begin(), catalog_.entries().end(), [](const LibraryEntry &e) { return e.favorite; }));
    else if (key == "view") actual = settings_.view;
    else if (key == "platform_filter") actual = std::to_string(platform_filter_);
    else if (key == "library_scroll") actual = std::to_string(static_cast<int>(std::lround(library_scroll_)));
    else if (key == "detail") actual = page_ == Page::details && catalog_.find_id(detail_id_) != nullptr ? catalog_.find_id(detail_id_)->title() : "-";
    else if (key == "nav_items") actual = nav_items_;
    else if (key == "status") actual = !detail_id_.empty() ? status_label(status_of(detail_id_)) : "-";
    else if (key == "can_play") actual = !detail_id_.empty() && can_play(status_of(detail_id_)) ? "1" : "0";
    else if (key == "playing") actual = game_ != nullptr ? "1" : "0";
    else if (key == "scd_state") actual = !detail_id_.empty() && status_of(detail_id_).sega_cd ? scd_label(status_of(detail_id_)) : "-";
    else if (key == "scd_refusal") actual = !detail_id_.empty() && status_of(detail_id_).sega_cd ? scd::code(status_of(detail_id_).scd.refusal) : "-";
    else if (key == "scd_outcome") actual = scd_last_outcome_.count(detail_id_) != 0 ? scd_last_outcome_.at(detail_id_) : "-";
    else if (key == "scd_failure") actual = failures_.count(detail_id_) != 0 ? failures_.at(detail_id_) : "-";
    else if (key == "scd_recordings") actual = !detail_id_.empty() && catalog_.find_id(detail_id_) != nullptr ? std::to_string(scd_recordings(*catalog_.find_id(detail_id_)).size()) : "-";
    else if (key == "scd_form_message") actual = scd_form_.message.empty() ? "-" : scd_form_.message;
    else if (key == "hints") actual = gamepad_is_last_input() && modal_.kind == Modal::Kind::none && !picker_.open ? "1" : "0";
    else if (key == "want_text") actual = ImGui::GetIO().WantTextInput ? "1" : "0";
    else if (key == "settings_tab") actual = std::to_string(settings_tab_);
    else if (key == "picker") actual = picker_.open ? "1" : "0";
    else if (key == "picker_chosen") actual = std::to_string(picker_.chosen.size());
    else if (key == "picker_level") actual = !picker_.open ? "-" : (picker_.root.empty() ? "locations" : (picker_.dir == picker_.root ? "root" : "sub"));
    else if (key == "library_size") actual = std::to_string(catalog_.entries().size());
    else if (key == "cleanup_mode") actual = settings_.cleanup.mode;
    else if (key == "intermediates_choice") actual = settings_.intermediates_choice_made ? "1" : "0";
    else if (key == "mode") actual = !detail_id_.empty() && catalog_.find_id(detail_id_) != nullptr ? selected_mode_label(*catalog_.find_id(detail_id_)) : "-";
    else if (key == "building") actual = !detail_id_.empty() && is_building(detail_id_) ? "1" : "0";
    else if (key == "stopping") actual = !detail_id_.empty() && is_stopping(detail_id_) ? "1" : "0";
    else if (key == "queued") actual = !detail_id_.empty() && is_queued(detail_id_) ? "1" : "0";
    else if (key == "build_count") actual = std::to_string(build_count());
    else if (key == "build_outcome") actual = last_build_outcome_;
    else if (key == "menu") actual = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ? "1" : "0";
    trace(std::string("expect ") + (actual == value ? "ok " : "FAIL ") + arg + " actual=" + actual);
  } else if (cmd == "dump") {
    const ImGuiContext &g = *ImGui::GetCurrentContext();
    trace(std::string("dump nav_window=") + (g.NavWindow != nullptr ? g.NavWindow->Name : "-") + " nav_id=" + std::to_string(g.NavId) + " nav_active=" + std::to_string(g.NavCursorVisible ? 1 : 0) +
          " want_text=" + std::to_string(ImGui::GetIO().WantTextInput ? 1 : 0) + " focus=" + focus_label_ + " focus_id=" + ui::focus_id + " nav_rect=" + nav_rect_text());
  } else if (cmd == "dump") {
    const ImGuiContext &g = *ImGui::GetCurrentContext();
    trace(std::string("dump nav_window=") + (g.NavWindow != nullptr ? g.NavWindow->Name : "-") + " nav_id=" + std::to_string(g.NavId) + " nav_cursor=" + std::to_string(g.NavCursorVisible ? 1 : 0) +
          " want_text=" + std::to_string(ImGui::GetIO().WantTextInput ? 1 : 0) + " focus=" + focus_label_ + " focus_id=" + ui::focus_id + " nav_rect=" + nav_rect_text());
  } else if (cmd == "quit") {
    SDL_Event quit;
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
  }
}

int App::run() {
  if (const char *path = SDL_getenv("SEGARECOMP_GUI_TRACE")) trace_path_ = path;
  trace("startup=window");
  if (!init_window()) return 1;
  trace("startup=ui");
  init_ui();
  trace("startup=catalog");
  // Library: load the persistent catalog, then (once, read-only) adopt verifiable finished builds of the product-owned cache folder.
  // SEG-069-T003: a genuinely new profile has no settings, library or viewer-settings file yet (checked before anything creates them).
  std::error_code profile_ec;
  bool fresh_profile = !fs::exists(settings_file_, profile_ec) && !fs::exists(catalog_.file(), profile_ec) && !fs::exists(viewer_settings_file_, profile_ec);
  catalog_.load();
  catalog_notice_ = catalog_.notice();
  settings_ = load_launcher_settings(settings_file_);
  sv_settings_defaults(&viewer_settings_);
  (void)sv_settings_load(&viewer_settings_, u8s(viewer_settings_file_).c_str());
  if (settings_.adopt_cache_on_start && !catalog_.read_only()) {
    // Adoption can add new entries or only new build records to existing entries: persist either (records were lost on exit before).
    const auto records = [&] { std::size_t n = 0; for (const auto &e : catalog_.entries()) n += e.builds.size(); return n; };
    const std::size_t before = records();
    if (catalog_.migrate_cache(cache_dir_, now_seconds()) != 0 || records() != before) save_catalog();
  }
  if (!catalog_.entries().empty()) fresh_profile = false;  // adoption of an existing product cache means this is not a new installation
  {
    // The first-run question is interactive only: scripted/automated windows (GUI scripts, screenshots, autoplay) are never blocked by it unless a
    // test asks for it explicitly with SEGARECOMP_GUI_ONBOARDING=1. Everything else counts as already answered, in memory, so a later settings
    // save cannot turn an existing or automated profile into a "new" one.
    const bool automated = SDL_getenv("SEGARECOMP_GUI_SCRIPT") != nullptr || SDL_getenv("SEGARECOMP_GUI_AUTOPLAY") != nullptr || SDL_getenv("SEGARECOMP_SHOT_PATH") != nullptr || SDL_getenv("SEGARECOMP_SHOT_ROM") != nullptr;
    // SEGARECOMP_GUI_ONBOARDING=1 lets an automated window ask on a genuinely new profile; =always asks regardless of the profile (test seam).
    const char *onboarding_env = SDL_getenv("SEGARECOMP_GUI_ONBOARDING");
    const bool forced = onboarding_env != nullptr, always = forced && std::string(onboarding_env) == "always";
    if (!settings_.intermediates_choice_made) {
      if (always || (fresh_profile && (!automated || forced))) {
        modal_ = Modal{};
        modal_.kind = Modal::Kind::first_run_intermediates;
        modal_.title = "Keep build intermediates?";
        trace("onboarding=intermediates_shown");
      } else {
        settings_.intermediates_choice_made = true;
      }
    }
  }
  if (layout_.problem.empty()) sms_probe_ = std::async(std::launch::async, [lay = layout_] { return sms_optimized_available_blocking(lay); });
  if (!catalog_notice_.empty()) toast(catalog_notice_, true);
  autoplay_ = SDL_getenv("SEGARECOMP_GUI_AUTOPLAY") != nullptr;
  autoplay_pending_ = autoplay_;
  if (const char *script = SDL_getenv("SEGARECOMP_GUI_SCRIPT")) {
    std::ifstream in(script);
    for (std::string line; std::getline(in, line);)
      if (!line.empty() && line[0] != '#') script_.push_back(line);
    script_wait_ = 10;
    ImGui::GetIO().KeyRepeatDelay = 30.0F;  // a scripted press is frame-counted: never let real elapsed time auto-repeat it on a slow runner
  }
  if (const char *shot = SDL_getenv("SEGARECOMP_SHOT")) shot_path_ = shot;
  if (const char *page = SDL_getenv("SEGARECOMP_SHOT_PAGE")) shot_page_ = page;
  if (const char *frames = SDL_getenv("SEGARECOMP_SHOT_FRAMES")) shot_frame_ = std::max(2, std::atoi(frames));
  request_status_refresh();
  request_scan();  // sizes for the library cards (bounded, read-only, background)
  // Review/automation aids (not user features): preselect a ROM, optionally with a policy/model/region, like the original window did.
  if (const char *preselect = SDL_getenv("SEGARECOMP_SHOT_ROM")) {
    if (sms_probe_.valid()) { sms_optimized_available_ = sms_probe_.get(); }
    RomView rom = inspect_rom_file(from_u8(preselect), layout_);
    const auto result = catalog_.import_rom(rom, now_seconds());
    if (result.entry != nullptr) {
      LibraryEntry &entry = *result.entry;
      // The automation hook states the session's choice explicitly (as the original window's per-session selection did): Compatibility unless asked.
      const char *p = SDL_getenv("SEGARECOMP_SHOT_POLICY");
      entry.aot_policy = (p != nullptr && std::string(p) == "optimized" && view_for(entry).supports_optimized()) ? "optimized" : "compatibility";
      const char *m = SDL_getenv("SEGARECOMP_SHOT_MODEL");
      entry.aot_model = (m != nullptr && std::string(m) == "gen2" && entry.platform_id == "genesis" && entry.aot_policy == "optimized") ? "gen2" : "v1";
      if (const char *r = SDL_getenv("SEGARECOMP_SHOT_REGION"); r != nullptr && rom.supports_region()) entry.region = r;
      if (rom.platform_id == "master-system" && !rom.mapper_manifest.empty() && entry.mapper.empty()) entry.mapper_manifest_sha256 = rom.mapper_manifest_sha256;
      save_catalog();
      open_details(entry.id());
      request_status_refresh();
      if (SDL_getenv("SEGARECOMP_SHOT_BUILD")) { while (status_pending_) { poll_status_refresh(); SDL_Delay(5); } start_build(entry.id()); }
    }
  }
  if (shot_page_ == "storage" || shot_page_ == "storage-modal") {
    go(Page::storage);
    if (const auto vis = visible_entries(); shot_page_ == "storage-modal" || SDL_getenv("SEGARECOMP_SHOT_SELECT")) for (std::size_t i = 0; i < vis.size() && i < 2; ++i) storage_selected_.insert(vis[i]->id());
  }
  else if (shot_page_ == "import") go(Page::import_page);
  else if (shot_page_.rfind("settings", 0) == 0) { settings_tab_ = shot_page_.size() > 8 ? std::atoi(shot_page_.c_str() + 8) : 0; go(Page::settings); }
  else if (shot_page_.rfind("details:", 0) == 0) {
    const std::size_t index = static_cast<std::size_t>(std::atoi(shot_page_.c_str() + 8));
    const auto visible = visible_entries();
    if (index < visible.size()) open_details(visible[index]->id());
    if (SDL_getenv("SEGARECOMP_SHOT_LOG") != nullptr) show_log_ = true;  // review aid: the log panel expanded
  } else if (shot_page_ == "list") { settings_.view = "list"; }
  if (const char *drop = SDL_getenv("SEGARECOMP_SHOT_DROP")) {  // review aid: import several files at once, as a drag-and-drop would
    std::vector<std::string> paths;
    for (std::string rest = drop; !rest.empty();) {
      const std::size_t cut = rest.find(';');
      paths.push_back(rest.substr(0, cut));
      rest = cut == std::string::npos ? std::string() : rest.substr(cut + 1);
    }
    start_import(std::move(paths), false);
    go(Page::import_page);
  }
  if (SDL_getenv("SEGARECOMP_SHOT_MENU") != nullptr && page_ == Page::library)  // review aid: the context menu of the first game
    if (const auto vis = visible_entries(); !vis.empty()) { focus_card_request_ = vis.front()->id(); menu_on_focus_ = true; }
  if (SDL_getenv("SEGARECOMP_SHOT_SELECT")) { select_mode_ = true; for (const auto *e : visible_entries()) { selected_.insert(e->id()); if (selected_.size() >= 2) break; } }
  bool running = true;
  trace("startup=ready");
  while (running) {
    ++frame_;
    const bool capture_trace = !shot_path_.empty() && (frame_ <= shot_frame_ || (frame_ & (frame_ - 1)) == 0);
    if (capture_trace) trace("frame=" + std::to_string(frame_) + " phase=events");
    process_events(running);
    update_jobs();
    if (capture_trace) trace("frame=" + std::to_string(frame_) + " phase=jobs status=" + std::to_string(status_pending_) + " scan=" + std::to_string(scanning_));
    for (auto it = script_key_up_.begin(); it != script_key_up_.end();) {
      if (frame_ >= it->first) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_KEY_UP;
        e.common.timestamp = SDL_GetTicksNS();
        e.key.windowID = SDL_GetWindowID(window_);
        e.key.scancode = static_cast<SDL_Scancode>(it->second);
        e.key.key = SDL_GetKeyFromScancode(e.key.scancode, SDL_KMOD_NONE, false);
        e.key.down = false;
        SDL_PushEvent(&e);
        it = script_key_up_.erase(it);
      } else ++it;
    }
    if (script_mouse_up_.first >= 0 && frame_ >= script_mouse_up_.first) { SDL_PushEvent(&script_mouse_up_.second); script_mouse_up_.first = -1; }
    run_script();
    if (shot_page_ == "storage-modal" && report_valid_ && !scanning_ && modal_.kind == Modal::Kind::none && !shot_modal_done_) {  // review aid: the confirmation sheet of a real plan
      shot_modal_done_ = true;
      modal_.kind = Modal::Kind::plan;
      modal_.title = "Remove build intermediates?";
      modal_.plan = plan_intermediates(report_, std::vector<std::string>(storage_selected_.begin(), storage_selected_.end()), busy_);
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    update_input_diag();
    focus_label_.clear();
    draw();
    if (capture_trace) trace("frame=" + std::to_string(frame_) + " phase=drawn");
    ImGui::Render();
    int win_w_pt = 0, win_h_pt = 0, output_w = 0, output_h = 0;
    SDL_GetWindowSize(window_, &win_w_pt, &win_h_pt);
    SDL_GetCurrentRenderOutputSize(renderer_, &output_w, &output_h);
    SDL_SetRenderScale(renderer_, win_w_pt > 0 ? static_cast<float>(output_w) / static_cast<float>(win_w_pt) : 1.0F, win_h_pt > 0 ? static_cast<float>(output_h) / static_cast<float>(win_h_pt) : 1.0F);
    SDL_SetRenderDrawColor(renderer_, 8, 11, 26, 255);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (!script_shot_.empty()) {  // script command `shot <file.bmp>`: a review aid, the window keeps running
      if (SDL_Surface *surface = SDL_RenderReadPixels(renderer_, nullptr)) { SDL_SaveBMP(surface, script_shot_.c_str()); SDL_DestroySurface(surface); }
      trace("script=shot_saved");
      script_shot_.clear();
    }
    if (!shot_path_.empty() && frame_ >= shot_frame_ && !status_pending_ && !scanning_ && (!job_ || SDL_getenv("SEGARECOMP_SHOT_DURING_BUILD") != nullptr) && !import_job_.valid()) {
      trace("capture=read_pixels page=" + shot_page_);
      if (SDL_Surface *surface = SDL_RenderReadPixels(renderer_, nullptr)) { SDL_SaveBMP(surface, shot_path_.c_str()); SDL_DestroySurface(surface); }
      trace("capture=saved");
      running = false;
    }
    SDL_RenderPresent(renderer_);
    if (capture_trace) trace("frame=" + std::to_string(frame_) + " phase=presented");
  }
  trace("shutdown=begin");
  shutdown();
  trace("shutdown=done");
  return 0;
}

} // namespace launcher

int gui_main() {
  launcher::App app;
  return app.run();
}
