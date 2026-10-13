#include "app.hpp"

#include "covers.hpp"
#include "pixel_assets.hpp"

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
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
  if (trace_path_ != nullptr) std::ofstream(trace_path_, std::ios::app) << line << '\n';
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

void App::viewer_settings_changed() {
  sv_settings_sanitize(&viewer_settings_);
  std::error_code ec;
  fs::create_directories(library_dir_, ec);
  if (sv_settings_save(&viewer_settings_, u8s(viewer_settings_file_).c_str()) != 0) toast("Viewer settings could not be saved.", true);
}

// ---------------------------------------------------------------- window / UI setup

bool App::init_window() {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); return false; }
  }
  const float base_w = 1080.0F, base_h = 740.0F;
  window_ = SDL_CreateWindow("Segarecomp", static_cast<int>(base_w), static_cast<int>(base_h), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window_ == nullptr) { std::fprintf(stderr, "window failed: %s\n", SDL_GetError()); return false; }
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
  ImFont *builtin = io.Fonts->AddFontDefault();
  const auto load_font = [&](const char *relative) -> ImFont * {
    const fs::path path = asset_path(relative);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return nullptr;
    return io.Fonts->AddFontFromFileTTF(path.string().c_str(), 0.0F);
  };
  ctx_.bold = load_font("fonts/Silkscreen-Bold.ttf");
  ctx_.regular = load_font("fonts/Silkscreen-Regular.ttf");
  if (ctx_.bold == nullptr) ctx_.bold = builtin;
  if (ctx_.regular == nullptr) ctx_.regular = builtin;
  ctx_.renderer = renderer_;
}

void App::shutdown() {
  if (job_) job_->wait();
  game_.reset();
  scan_cancel_ = true;
  if (scan_job_.valid()) scan_job_.wait();
  if (status_job_.valid()) status_job_.wait();
  if (import_job_.valid()) import_job_.wait();
  if (maint_job_.valid()) maint_job_.wait();
  if (sms_probe_.valid()) sms_probe_.wait();
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
  switch (info.status) {
    case GameStatus::ready: return "Built";
    case GameStatus::metadata_only: return "Metadata only";
    case GameStatus::stale: return "Stale build";
    case GameStatus::failed: return "Build failed";
    case GameStatus::not_built: return "Not built";
  }
  return {};
}

ImU32 App::status_color(const StatusInfo &info) const {
  switch (info.status) {
    case GameStatus::ready: return ui::col::ok;
    case GameStatus::failed: return ui::col::err;
    case GameStatus::stale: return ui::col::warn;
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
  if (scan_again_) { scan_again_ = false; request_scan(); return; }
  if (policy_eval_pending_) { policy_eval_pending_ = false; evaluate_policy_after_scan(); }
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
  static const SDL_DialogFileFilter filters[] = {{"Genesis / Mega Drive ROMs", "md;bin;gen;smd"}, {"Master System ROMs", "sms"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(dialog_trampoline, new DialogCtx{this, "import", ""}, window_, filters, 3, nullptr, true);
}

void App::browse_locate(const std::string &id) {
  static const SDL_DialogFileFilter filters[] = {{"ROMs", "md;bin;gen;smd;sms"}, {"All files", "*"}};
  SDL_ShowOpenFileDialog(dialog_trampoline, new DialogCtx{this, "locate", id}, window_, filters, 2, nullptr, false);
}

void App::browse_cover(const std::string &id) {
  static const SDL_DialogFileFilter filters[] = {{"Images (PNG, JPEG)", "png;jpg;jpeg"}};
  SDL_ShowOpenFileDialog(dialog_trampoline, new DialogCtx{this, "cover", id}, window_, filters, 1, nullptr, false);
}

void App::start_import(std::vector<std::string> paths, bool build_after) {
  if (import_job_.valid()) { for (auto &p : paths) deferred_paths_.push_back(std::move(p)); return; }
  if (paths.empty()) return;
  import_total_ = static_cast<int>(paths.size());
  import_done_ = 0;
  import_build_after_ = build_after;
  import_results_.clear();
  import_job_ = std::async(std::launch::async, [paths = std::move(paths), lay = layout_, done = &import_done_] {
    std::vector<std::pair<std::string, RomView>> out;
    for (const auto &p : paths) {
      out.emplace_back(p, inspect_rom_file(from_u8(p), lay));
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
  if (import_build_after_) for (const auto &id : buildable) build_queue_.push_back(id);
  const int ok = static_cast<int>(std::count_if(import_results_.begin(), import_results_.end(), [](const ImportItem &i) { return !i.entry_id.empty(); }));
  if (import_results_.size() == 1 && ok == 1) { open_details(import_results_[0].entry_id); toast(import_results_[0].message); }
  else { go(Page::import_page); toast(std::to_string(added_ids.size()) + " added, " + std::to_string(import_results_.size() - added_ids.size()) + " skipped or already present."); }
  trace("import=done added=" + std::to_string(added_ids.size()) + " total=" + std::to_string(import_results_.size()));
  if (!deferred_paths_.empty()) { auto next = std::move(deferred_paths_); deferred_paths_.clear(); start_import(std::move(next), false); }
}

void App::open_details(const std::string &id) {
  detail_id_ = id;
  editing_title_ = false;
  show_log_ = false;
  show_fallback_details_ = false;
  if (page_ != Page::details) previous_page_ = page_;
  page_ = Page::details;
}

void App::go(Page page) {
  if (page == page_) return;
  if (page_ != Page::details) previous_page_ = page_;
  page_ = page;
  editing_title_ = false;
  if (page == Page::storage) request_scan();
}

// ---------------------------------------------------------------- build / run

bool App::start_build(const std::string &id, bool restart_after) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr) return false;
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
  view.sms_optimized_available = sms_optimized_available_;
  if (view.platform_id == "master-system") { if (!entry->mapper.empty()) view.mapper = entry->mapper; if (view.sms_optimized_available) view.aot_policy = entry->aot_policy; }
  if (view.supports_optimized() && view.platform_id == "genesis") { view.aot_policy = entry->aot_policy; view.aot_model = entry->aot_model; }
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

void App::finish_build() {
  job_->wait();
  const bool ok = job_->succeeded();
  const std::string message = job_->message();
  const fs::path entry_path = job_->entry();
  busy_.end_build(job_key_);
  const std::string id = job_id_;
  const bool restart = job_restart_after_;
  job_.reset();
  job_id_.clear();
  LibraryEntry *entry = catalog_.find_id(id);
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
    if (settings_.cleanup.mode == "intermediates") {
      // Exact, validated plan for this build only; runs under the busy guard like every cleanup.
      policy_eval_pending_ = false;
      Catalog copy{fs::path{}};
      copy.entries() = catalog_.entries();
      const StorageReport fresh = scan_storage(copy, cache_dir_, library_dir_);
      const Plan plan = plan_intermediates(fresh, {id}, busy_);
      if (!plan.items.empty()) run_plan(plan);
    } else if (settings_.cleanup.mode == "keep_last_n" || settings_.cleanup.mode == "soft_limit") {
      policy_eval_pending_ = true;
    }
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
  RomView view = view_for(*entry);
  if (const std::string refusal = launch_refusal(view); !refusal.empty()) { failures_[id] = refusal; diagnostics_[id] = refusal; return false; }
  const fs::path dir = entry_dir(view, layout_);
  if (!entry_ready_for(dir, view, layout_)) {
    // Never start an executable that does not match the entry's current identity (stale, foreign or removed).
    show_message("This build cannot be played", {"The stored build no longer matches this game's current identity (a different release, option set or a removed build).", "Recompile the game to play it."});
    request_status_refresh();
    return false;
  }
  const std::string key = u8s(dir.filename());
  if (!busy_.try_begin_run(key)) { toast("Cleanup is running; try again in a moment.", true); return false; }
  game_ = std::make_unique<GameRun>(view, layout_, dir);
  if (!game_->started()) {
    busy_.end_run(key);
    game_.reset();
    failures_[id] = "The game could not be started.";
    diagnostics_[id] = failures_[id];
    request_status_refresh();
    return false;
  }
  game_id_ = id;
  game_key_ = key;
  game_view_ = view;
  game_report_ = read_policy_report(dir);
  restarted_this_play_ = restarted;
  failures_.erase(id);
  trace(std::string("run=started mode=") + effective_mode(game_report_) + " restarted=" + (restarted ? "1" : "0") + " requested=" + (view.optimized() ? "optimized" : "compatibility") +
        " label=" + running_label(game_report_, restarted));
  return true;
}

void App::finish_run() {
  const int exit_code = game_->exit_code();
  const std::string id = game_id_;
  const RomView ran = game_view_;
  const PolicyReport report = game_report_;
  const RestartDecision next = decide_after_exit(running_view(ran, report), exit_code, restarted_this_play_);
  game_.reset();
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
  diagnostics_[id] = read_tail(entry_executable(entry_dir(view_for(*entry), layout_)).parent_path() / "run.log", 24000);
  trace("state=failed reason=game_stopped_unexpectedly");
  request_status_refresh();
}

// ---------------------------------------------------------------- frame

void App::process_events(bool &running) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
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
        show_message("The file picker could not be opened", {"SDL: " + e.error, "On Linux this usually means no file-chooser service (XDG Desktop Portal) is available - common under WSL/WSLg.", "Drop the file onto this window instead."});
        continue;
      }
      if (e.purpose == "import") start_import(e.paths, import_build_option_);
      else if (e.purpose == "locate" && !e.paths.empty()) {
        const RomView view = inspect_rom_file(from_u8(e.paths[0]), layout_);
        LibraryEntry *entry = catalog_.find_id(e.key);
        if (entry == nullptr) continue;
        if (!view.error.empty()) show_message("The file could not be read", {view.error});
        else if (!catalog_.relocate(*entry, from_u8(e.paths[0]), view.sha256)) show_message("That is a different ROM", {"The chosen file does not match this game (its SHA-256 differs).", "Pick the original file, or import it as a separate game."});
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
  poll_maintenance();
  if (sms_probe_.valid() && sms_probe_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    sms_optimized_available_ = sms_probe_.get();
    request_status_refresh();
  }
  if (job_ && job_->finished()) finish_build();
  if (game_ && game_->poll()) finish_run();
  if (!job_ && !build_queue_.empty() && !maintenance_running_ && modal_.kind == Modal::Kind::none) {
    const std::string next = build_queue_.front();
    build_queue_.erase(build_queue_.begin());
    if (statuses_.count(next) == 0 || status_of(next).status != GameStatus::ready) start_build(next);
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

int App::run() {
  if (!init_window()) return 1;
  init_ui();
  // Library: load the persistent catalog, then (once, read-only) adopt verifiable finished builds of the product-owned cache folder.
  catalog_.load();
  catalog_notice_ = catalog_.notice();
  settings_ = load_launcher_settings(settings_file_);
  sv_settings_defaults(&viewer_settings_);
  (void)sv_settings_load(&viewer_settings_, u8s(viewer_settings_file_).c_str());
  if (settings_.adopt_cache_on_start && !catalog_.read_only()) {
    if (catalog_.migrate_cache(cache_dir_, now_seconds()) != 0) save_catalog();
  }
  if (layout_.problem.empty()) sms_probe_ = std::async(std::launch::async, [lay = layout_] { return sms_optimized_available_blocking(lay); });
  if (!catalog_notice_.empty()) toast(catalog_notice_, true);
  autoplay_ = SDL_getenv("SEGARECOMP_GUI_AUTOPLAY") != nullptr;
  autoplay_pending_ = autoplay_;
  trace_path_ = SDL_getenv("SEGARECOMP_GUI_TRACE");
  if (const char *shot = SDL_getenv("SEGARECOMP_SHOT")) shot_path_ = shot;
  if (const char *page = SDL_getenv("SEGARECOMP_SHOT_PAGE")) shot_page_ = page;
  if (const char *frames = SDL_getenv("SEGARECOMP_SHOT_FRAMES")) shot_frame_ = std::max(2, std::atoi(frames));
  request_status_refresh();
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
  if (shot_page_ == "storage") go(Page::storage);
  else if (shot_page_ == "import") go(Page::import_page);
  else if (shot_page_.rfind("settings", 0) == 0) { settings_tab_ = shot_page_.size() > 8 ? std::atoi(shot_page_.c_str() + 8) : 0; go(Page::settings); }
  else if (shot_page_.rfind("details:", 0) == 0) {
    const std::size_t index = static_cast<std::size_t>(std::atoi(shot_page_.c_str() + 8));
    const auto visible = visible_entries();
    if (index < visible.size()) open_details(visible[index]->id());
  } else if (shot_page_ == "list") { settings_.view = "list"; }
  if (SDL_getenv("SEGARECOMP_SHOT_SELECT")) { select_mode_ = true; for (const auto *e : visible_entries()) { selected_.insert(e->id()); if (selected_.size() >= 2) break; } }
  bool running = true;
  while (running) {
    ++frame_;
    process_events(running);
    update_jobs();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    int win_w_pt = 0, win_h_pt = 0, output_w = 0, output_h = 0;
    SDL_GetWindowSize(window_, &win_w_pt, &win_h_pt);
    SDL_GetCurrentRenderOutputSize(renderer_, &output_w, &output_h);
    SDL_SetRenderScale(renderer_, win_w_pt > 0 ? static_cast<float>(output_w) / static_cast<float>(win_w_pt) : 1.0F, win_h_pt > 0 ? static_cast<float>(output_h) / static_cast<float>(win_h_pt) : 1.0F);
    SDL_SetRenderDrawColor(renderer_, 8, 11, 26, 255);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (!shot_path_.empty() && frame_ >= shot_frame_ && !status_pending_ && !scanning_ && !job_) {
      if (SDL_Surface *surface = SDL_RenderReadPixels(renderer_, nullptr)) { SDL_SaveBMP(surface, shot_path_.c_str()); SDL_DestroySurface(surface); }
      running = false;
    }
    SDL_RenderPresent(renderer_);
  }
  shutdown();
  return 0;
}

} // namespace launcher

int gui_main() {
  launcher::App app;
  return app.run();
}
