#pragma once

// The launcher window: library, import, game details, storage and settings. Thin orchestration over launcher_core, library_catalog,
// storage and covers; it owns no recompilation logic and never deletes anything except through storage.hpp's validated plans.

#include "launcher_core.hpp"
#include "launcher_settings.hpp"
#include "library_catalog.hpp"
#include "storage.hpp"
#include "ui_kit.hpp"

extern "C" {
#include "sv_input_diag.h"
#include "sv_settings.h"
}

#include <SDL3/SDL.h>

#include "imgui.h"

#include <atomic>
#include <ctime>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace launcher {

enum class GameStatus { not_built, ready, metadata_only, stale, failed };
enum class Page { library, details, import_page, storage, settings };

struct StatusInfo {
  GameStatus status = GameStatus::not_built;
  bool rom_missing = false;       // no tracked ROM path currently exists
  bool compat_known = false;      // ROM-hash-bound analysis metadata ships with this release
  std::string key;                // entry_dir name of the current identity
  std::filesystem::path entry;    // entry_dir
  bool has_failed_dir = false;
  BuildRecord build;              // the record of `key` when the catalog knows it (built_at == 0 && version empty when unknown)
  bool build_known = false;
  PolicyReport policy;            // of a ready build
  std::string observed_region;    // of a ready build
  bool adopt = false;             // a ready build the catalog does not know yet (recorded by the UI thread)
  long long file_time = 0;        // modification time of its status.json (unix seconds), 0 when unknown
  // The build of the OTHER build mode (Compatibility <-> Optimized) for the same ROM and options: a mode switch is non-destructive, and when
  // that build is still on disk and valid, switching back makes the game playable again without recompiling.
  bool other_mode_supported = false;
  bool other_mode_ready = false;
};

struct ImportItem {
  std::string path;
  std::string outcome;   // added | duplicate | location | unsupported | error
  std::string title, message;
  std::string entry_id;
  bool needs_mapper = false;
};

struct Modal {
  enum class Kind { none, message, plan, remove_entries, clear_library, restore_defaults } kind = Kind::none;
  std::string title;
  std::vector<std::string> lines;
  Plan plan;
  std::vector<std::string> ids;   // remove_entries
  bool delete_cache = false;      // remove_entries: also delete the owned build folders
  bool keep_logs = false;
};

class App {
public:
  App();
  ~App();
  App(const App &) = delete;
  App &operator=(const App &) = delete;
  int run();

private:
  // ---- startup / shutdown ----
  bool init_window();
  void init_ui();
  void shutdown();
  // ---- frame ----
  void process_events(bool &running);
  void update_jobs();
  void draw();
  void draw_background(float w, float h);
  [[nodiscard]] float hero_height(float w, float h) const;
  // A real cover when the user chose one, else the neutral hand-drawn cartridge (initials of the title, no game art).
  void draw_cover(ImDrawList *dl, const LibraryEntry &entry, ImVec2 a, ImVec2 b, float rounding, ImU32 tint = IM_COL32_WHITE);
  void draw_sidebar(float x, float y, float w, float h);
  void draw_topbar(float x, float y, float w, float h);
  void draw_library(float w, float h);
  void draw_drop_zone(float w, float h, bool compact_box);
  void draw_card(const LibraryEntry &entry, float x, float y, float w, float h);
  void draw_list_row(const LibraryEntry &entry, float w, float h);
  void draw_details(float w, float h);
  void draw_import(float w, float h);
  void draw_storage(float w, float h);
  void draw_settings(float w, float h);
  void draw_input_diagnostics(float w);
  void update_input_diag();
  [[nodiscard]] SvPadState input_state() const;
  [[nodiscard]] std::string input_report() const;
  void draw_modal(float w, float h);
  void draw_toasts(float w, float h);
  // ---- data / actions ----
  [[nodiscard]] static long long now_seconds();
  void save_catalog();
  void save_settings();
  void request_status_refresh();
  void poll_status_refresh();
  void request_scan();
  void poll_scan();
  void start_import(std::vector<std::string> paths, bool build_after);
  void poll_import();
  void open_details(const std::string &id);
  // Leaves the contextual Details page for the page it was opened from; the library keeps its search, filters, sort, view, scroll and focus.
  void close_details();
  void go(Page page);
  // The Details page's build-identity facts and sections.
  void draw_details_identity(LibraryEntry &entry, const StatusInfo &info, float w, float cover_w);
  float draw_build_config(LibraryEntry &entry, const StatusInfo &info, float w);
  void draw_provenance(LibraryEntry &entry, const StatusInfo &info, float w);
  float draw_storage_rail(const LibraryEntry &entry, const StatusInfo &info, ImVec2 a, float w);
  void draw_game_menu(const std::string &popup_id, const std::string &id, bool include_details);
  [[nodiscard]] std::string build_action_label(const StatusInfo &info) const;
  [[nodiscard]] bool can_build(const LibraryEntry &entry, const StatusInfo &info) const;
  [[nodiscard]] bool can_play(const StatusInfo &info) const;
  void play(const std::string &id);
  void ask_remove(const LibraryEntry &entry);
  void toast(const std::string &text, bool error = false);
  void show_message(const std::string &title, const std::vector<std::string> &lines);
  [[nodiscard]] RomView view_for(const LibraryEntry &entry) const;
  [[nodiscard]] std::filesystem::path usable_rom_path(const LibraryEntry &entry) const;
  [[nodiscard]] const StatusInfo &status_of(const std::string &id) const;
  [[nodiscard]] std::string status_label(const StatusInfo &info) const;
  [[nodiscard]] ImU32 status_color(const StatusInfo &info) const;
  bool start_build(const std::string &id, bool restart_after = false);
  void finish_build();
  bool launch(const std::string &id, bool restarted);
  void finish_run();
  void run_plan(const Plan &plan);
  void poll_maintenance();
  void evaluate_policy_after_scan();
  void browse_import();
  void browse_locate(const std::string &id);
  void browse_cover(const std::string &id);
  void trace(const std::string &line);
  [[nodiscard]] std::vector<const LibraryEntry *> visible_entries() const;
  SDL_Texture *cover_texture(const LibraryEntry &entry);
  void viewer_settings_changed();

  // ---- window ----
  SDL_Window *window_ = nullptr;
  SDL_Renderer *renderer_ = nullptr;
  ui::Ctx ctx_;
  float win_w_ = 0, win_h_ = 0;
  int frame_ = 0;
  bool dragging_ = false;
  // ---- data ----
  Layout layout_;
  std::filesystem::path library_dir_, cache_dir_, covers_dir_, catalog_file_, settings_file_, viewer_settings_file_;
  Catalog catalog_;
  LauncherSettings settings_;
  SvSettings viewer_settings_{};
  // ---- input diagnostics (one engine shared with the viewer; see libs/viewer/include/sv_input_diag.h) ----
  SvDiag *diag_ = nullptr;
  std::vector<SvDiagHid> hid_;
  int hid_state_ = -2;            // -2 not scanned, -1 failed, otherwise the number of Nintendo HID devices
  std::string hid_error_;
  struct HidResult { std::vector<SvDiagHid> devices; int count = 0; std::string error; };
  std::future<HidResult> hid_job_;
  int nav_taps_ = 0;              // the input page's menu test: activations seen by the launcher's own navigation
  std::string nav_last_source_;
  bool input_seen_gamepad_nav_ = false;
  BusyGuard busy_;
  std::string catalog_notice_;
  // ---- navigation / library view ----
  Page page_ = Page::library;
  Page previous_page_ = Page::library;
  std::string detail_id_;
  char search_[96] = "";
  int platform_filter_ = 0;   // 0 all, 1 genesis, 2 master-system
  int status_filter_ = 0;     // 0 all, 1 ready, 2 needs build, 3 failed, 4 missing ROM
  bool select_mode_ = false;
  std::set<std::string> selected_;
  float library_scroll_ = 0.0F;        // the game grid/list scroll offset, restored when Details closes
  bool restore_library_scroll_ = false;
  std::string return_focus_id_;        // the card/row that opened Details regains keyboard/gamepad focus on return
  std::string focus_card_request_;     // script aid: give this card focus when it is drawn
  std::string context_id_;             // the game of the open context menu
  bool open_context_ = false;
  bool show_advanced_ = false;         // Details: the Advanced build options section is expanded
  int settings_tab_ = 0;
  bool focus_search_ = false;
  char title_edit_[200] = "";
  bool editing_title_ = false;
  bool show_log_ = false;
  bool show_fallback_details_ = false;
  // ---- status ----
  std::map<std::string, StatusInfo> statuses_;
  std::future<std::map<std::string, StatusInfo>> status_job_;
  bool status_pending_ = false, status_again_ = false;
  std::map<std::string, SDL_Texture *> cover_cache_;
  std::map<std::string, bool> cover_missing_;
  // ---- sms probe (experimental Optimized availability, asked of the product CLI once) ----
  bool sms_optimized_available_ = false;
  std::future<bool> sms_probe_;
  // ---- build / run ----
  std::unique_ptr<BuildJob> job_;
  std::string job_id_, job_key_;
  RomView job_view_;
  bool job_restart_after_ = false;
  std::vector<std::string> build_queue_;
  std::map<std::string, std::string> failures_;      // id -> message
  std::map<std::string, std::string> diagnostics_;   // id -> log text
  std::map<std::string, std::string> notices_;       // id -> restart notice
  std::unique_ptr<GameRun> game_;
  std::string game_id_, game_key_;
  RomView game_view_;
  PolicyReport game_report_;
  bool restarted_this_play_ = false;
  std::string live_log_;
  Uint64 live_log_at_ = 0;
  // ---- import ----
  std::future<std::vector<std::pair<std::string, RomView>>> import_job_;
  std::atomic<int> import_done_{0};
  int import_total_ = 0;
  bool import_build_after_ = false;
  std::vector<ImportItem> import_results_;
  bool import_build_option_ = false;
  // ---- dialog results ----
  struct DialogEvent { std::string purpose, key; std::vector<std::string> paths; std::string error; };
  std::mutex dialog_mutex_;
  std::vector<DialogEvent> dialog_events_;
  friend void dialog_trampoline(void *, const char *const *, int);
  // ---- storage ----
  StorageReport report_;
  bool report_valid_ = false;
  std::future<StorageReport> scan_job_;
  std::atomic<bool> scan_cancel_{false};
  bool scanning_ = false, scan_again_ = false;
  std::set<std::string> storage_selected_;
  int storage_sort_ = 0;      // 0 name, 1 size
  bool keep_failed_logs_ = true;
  bool soft_limit_exceeded_ = false;
  bool policy_eval_pending_ = false;
  // maintenance (cleanup runs on a worker under the busy guard)
  std::future<ExecResult> maint_job_;
  Plan maint_plan_;
  bool maintenance_running_ = false;
  // ---- overlays ----
  Modal modal_;
  struct Toast { std::string text; bool error; Uint64 until; };
  std::vector<Toast> toasts_;
  std::vector<std::string> deferred_paths_;
  // ---- automation (review/test aids, not user features) ----
  bool autoplay_ = false, autoplay_pending_ = false, autoplay_started_ = false, autoplay_build_tried_ = false;
  const char *trace_path_ = nullptr;
  std::string shot_path_;
  std::string shot_page_;
  int shot_frame_ = 14;
  bool restart_pending_ = false;
  // GUI script driver (SEGARECOMP_GUI_SCRIPT): synthetic keys / gamepad presses for the keyboard- and gamepad-navigation tests.
  std::vector<std::string> script_;
  std::size_t script_at_ = 0;
  int script_wait_ = 0;
  SDL_JoystickID script_pad_ = 0;
  SDL_Joystick *script_joy_ = nullptr;
  std::vector<std::pair<int, int>> script_key_up_;  // (frame, scancode) key releases to deliver
  bool shot_modal_done_ = false;
  bool nav_reset_ = true;  // put keyboard/gamepad focus on the active sidebar entry (start-up and page changes)
  std::string nav_items_;    // the primary navigation entries drawn this frame (script `expect nav_items=`)
  std::string focus_label_;  // the sidebar entry holding keyboard/gamepad focus (script `expect focus=`)
  void run_script();
  [[nodiscard]] const char *page_name() const;
};

} // namespace launcher
