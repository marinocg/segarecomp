#pragma once

// The launcher window: library, import, game details, storage and settings. Thin orchestration over launcher_core, library_catalog,
// storage and covers; it owns no recompilation logic and never deletes anything except through storage.hpp's validated plans.

#include "file_picker.hpp"
#include "launcher_core.hpp"
#include "launcher_settings.hpp"
#include "library_catalog.hpp"
#include "older_build.hpp"
#include "sega_cd.hpp"
#include "library_view.hpp"
#include "storage.hpp"
#include "ui_kit.hpp"

extern "C" {
#include "sv_input_diag.h"
#include "sv_settings.h"
}

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  // ImRect

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

// `playable_older`: the current identity has no build, but an executable built by an earlier release is verifiably playable (older_build.hpp).
enum class GameStatus { not_built, ready, metadata_only, stale, failed, playable_older };
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
  // SEG-069-T005: what an earlier release's build of this game is (evaluated only when the current identity has no ready build).
  OlderBuild older;
  // SEG-078-T002: a Sega CD game is judged by scd::quick_status (no cache entry, no build): `status` is only its coarse mapping (ready / needs attention).
  bool sega_cd = false;
  scd::Status scd;
  [[nodiscard]] bool runnable() const { return status == GameStatus::ready || older.playable; }
};

struct ImportItem {
  std::string path;
  std::string outcome;   // added | duplicate | location | unsupported | error
  std::string title, message;
  std::string entry_id;
  bool needs_mapper = false;
};

struct Modal {
  enum class Kind { none, message, plan, remove_entries, clear_library, restore_defaults, first_run_intermediates } kind = Kind::none;
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
  void draw_import_body(float w, float h);  // drop zone, the three-step guide, results (also the empty library's page)
  void draw_storage(float w, float h);
  void draw_settings(float w, float h);
  void draw_input_diagnostics(float w);
  void update_input_diag();
  [[nodiscard]] SvPadState input_state() const;
  [[nodiscard]] std::string input_report() const;
  void draw_modal(float w, float h);
  // ---- SEG-069-T004: the built-in controller-operable file picker (file_picker.hpp). The native OS dialog stays the default for mouse and keyboard. ----
  struct PickerState {
    bool open = false;
    std::string purpose, key, title;           // import | locate | cover
    bool multi = false;
    std::vector<std::string> extensions;
    std::vector<PickerRoot> roots;
    std::filesystem::path root, dir;           // root empty: the list of roots is shown
    PickerListing listing;
    std::set<std::string> chosen;              // UTF-8 paths of files ticked (import only)
    int page = 0;
    bool focus_first = false;                  // focus the first row after the list changed
    std::string message;
  };
  PickerState picker_;
  [[nodiscard]] bool use_builtin_picker() const;
  void open_picker(const std::string &purpose, const std::string &key);
  void picker_set_dir(const std::filesystem::path &root, const std::filesystem::path &dir);
  void picker_finish(bool accept);
  void draw_file_picker(float w, float h);
  void draw_gamepad_hints(float w, float h);
  bool gamepad_is_last_input() const;
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
  // The user-facing way to build: starts now when idle, otherwise joins the FIFO queue (one build at a time; the UI stays usable).
  void request_build(const std::string &id);
  void dequeue_build(const std::string &id);
  // SEG-069-T001: stops the running build of `id` (process tree shut down, this attempt's `.partial` removed, the earlier completed entry kept).
  // The job stays "stopping" until it is quiescent; finish_build() then clears the build and the FIFO continues.
  void cancel_build(const std::string &id);
  // The one entry point of the Cancel/Remove affordances (button, menu, controller): cancels the running build or removes the queued one.
  void stop_or_dequeue_build(const std::string &id);
  [[nodiscard]] bool is_stopping(const std::string &id) const { return job_ && job_id_ == id && job_->cancel_requested(); }
  [[nodiscard]] bool is_building(const std::string &id) const { return job_ && job_id_ == id; }
  [[nodiscard]] bool is_queued(const std::string &id) const { return build_queue_.contains(id); }
  [[nodiscard]] int build_count() const { return (job_ ? 1 : 0) + static_cast<int>(build_queue_.size()); }
  // Library badge of a game: Building / Queued while in the build flow, else the stored build status (+ spinner for Building).
  struct Badge { std::string label; ImU32 color; bool spinner; };
  [[nodiscard]] Badge badge_for(const LibraryEntry &entry, const StatusInfo &info) const;
  void finish_build();
  // SEG-069-T003: records the first-run answer (Keep -> cleanup.mode "never", Remove -> "intermediates"); false (and nothing changed) when it cannot be saved.
  bool apply_intermediates_choice(bool remove_after_build);
  // SEG-069-T004: D-pad hops between the sidebar and the page (see ui::ZoneEntry).
  ui::ZoneEntry content_entry_;                  // where Right from the sidebar lands (the first game card on the library page, else the page's first control)
  ui::ZoneEntry card_entry_;                     // first game card/row submitted this frame
  ui::ZoneEntry settings_tab_entry_[6];          // the Settings tab column (Right enters the body, Left from the body returns to the current tab)
  ui::ZoneEntry settings_body_entry_;
  ui::ZoneEntry storage_header_, storage_first_row_, storage_last_row_, storage_after_;  // Storage: header checkbox, first/last row checkbox, first action button
  ImGuiWindow *settings_body_window_ = nullptr;
  bool settings_narrow_ = false;
  ImGuiID left_pending_id_ = 0;                  // nav id when Left was pressed in the page; unchanged next frame means ImGui found nothing to the left
  bool left_pending_ = false;
  // SEG-069-T004: a sheet (modal or picker) returns keyboard/gamepad focus to the control that opened it.
  struct FocusMemory { ImGuiID id = 0; ImGuiWindow *window = nullptr; ImRect rel; bool armed = false; };
  FocusMemory modal_return_;
  void remember_focus_for_sheet(bool sheet_open);
  bool modal_focus_done_ = false;  // the first-run sheet puts keyboard/gamepad focus on its first choice once, when it opens
  // SEG-069-T002: the mode the game will really build as, for display: the stored intent, except a Master System game whose Optimized the product has
  // stated it does not offer (that is Compatibility). While the capability probe is pending the stored intent is shown.
  [[nodiscard]] std::string selected_mode_label(const LibraryEntry &entry) const;
  bool launch(const std::string &id, bool restarted);
  void finish_run();
  void run_plan(const Plan &plan);
  void poll_maintenance();
  void evaluate_policy_after_scan();
  void pump_cleanup();
  void browse_import();
  void browse_locate(const std::string &id);
  void browse_cover(const std::string &id);
  // Libretro box art, only on an explicit request: one game (menus) or every game without a cover (Settings > Metadata), on a worker.
  void start_cover_download(const std::vector<std::string> &ids, bool silent = false);
  // A newly added game gets its cover looked up on Libretro automatically; every failure is silent (no toast, no dialog).
  void queue_auto_covers(const std::vector<std::string> &ids);
  void poll_cover_download();
  [[nodiscard]] bool cover_download_possible(const LibraryEntry &entry) const;
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
  bool menu_on_focus_ = false;
  std::map<std::string, ImRect> card_rects_, fav_rects_, play_rects_;  // last drawn rectangles (script aids: `mouse_card`, `mouse_fav`, `mouse_play`)
  ImVec2 script_mouse_{0, 0};
  std::pair<int, SDL_Event> script_mouse_up_{-1, SDL_Event{}};         // review aid: open the context menu of the card that receives the requested focus
  ImVec2 context_anchor_{0, 0};        // where the context menu opens: the mouse for a right click, beside the card for keyboard/gamepad
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
  struct CoverDownload { std::string id, file_name, matched_name, source, error; };
  std::future<std::vector<CoverDownload>> cover_job_;
  bool cover_silent_ = false;                   // the running download is an automatic one: report nothing
  std::vector<std::string> auto_cover_pending_;  // new games waiting for the (single) download worker
  std::atomic<int> cover_done_{0};
  std::atomic<bool> cover_cancel_{false};
  int cover_total_ = 0;
  // ---- sms probe (experimental Optimized availability, asked of the product CLI once) ----
  bool sms_optimized_available_ = false;
  std::future<bool> sms_probe_;
  // ---- build / run ----
  std::unique_ptr<BuildJob> job_;
  std::string job_id_, job_key_;
  RomView job_view_;
  bool job_restart_after_ = false;
  std::string last_build_outcome_ = "none";  // none | ok | failed | cancelled (observed by the GUI automation script)
  int script_wait_file_frames_ = 0;
  BuildQueue build_queue_;
  std::map<std::string, std::string> failures_;      // id -> message
  std::map<std::string, std::string> diagnostics_;   // id -> log text
  std::map<std::string, std::string> notices_;       // id -> restart notice
  std::unique_ptr<GameRun> game_;
  std::string game_id_, game_key_;
  std::filesystem::path game_dir_;  // the build folder of the running game (an earlier release's folder for an older build)
  RomView game_view_;
  PolicyReport game_report_;
  bool restarted_this_play_ = false;
  std::string live_log_;
  Uint64 live_log_at_ = 0;
  // ---- SEG-078-T002 (ADR 0204): Sega CD ----
  // The "Add a Sega CD game" form of the Import page: BIOS, CUE and an optional verified program, each chosen with the file picker.
  struct ScdForm { std::string bios, cue, program, message; bool error = false; };
  ScdForm scd_form_;
  bool scd_form_open_ = false;                         // the form is shown on the Import page
  std::future<scd::ImportResult> scd_import_job_;      // import / attach / re-verify on a worker (hashing a disc takes seconds)
  bool scd_import_reverify_ = false;                  // the running check is "Verify again" of the existing program
  std::string scd_import_target_;                      // entry id for attach / re-verify, empty for a new game
  std::atomic<std::uint64_t> scd_done_{0};
  std::atomic<bool> scd_cancel_{false};
  std::uint64_t scd_total_ = 0;
  std::future<scd::Plan> scd_launch_job_;              // the full check before a launch (program hash + statement + disc sizes)
  std::string scd_launch_id_;
  scd::RunOptions scd_launch_options_;
  bool scd_launch_windowless_ = false;
  bool scd_record_ = false;                            // "Record this session": the next Play writes an input script
  bool game_is_scd_ = false;
  scd::RunOptions game_scd_options_;
  std::string game_scd_note_;
  std::unique_ptr<ScdBuildJob> scd_build_;             // SEG-078-T003: the automatic Sega CD build of one game (independent of the Genesis/SMS queue)
  std::string scd_build_id_;
  std::map<std::string, std::string> scd_build_notes_; // id -> what the last successful build of this session covers (plain language)
  std::map<std::string, bool> scd_built_experimental_;   // SEG-096-T003: what a finished build's effective policy was, until its program is bound to the entry
  void scd_start_build(const std::string &id);
  void scd_cancel_build();
  void scd_finish_build();
  [[nodiscard]] bool scd_can_build(const LibraryEntry &entry, const StatusInfo &info) const;
  std::map<std::string, std::string> scd_last_outcome_;  // id -> the plain-language end of the last run of this session
  [[nodiscard]] std::filesystem::path scd_dir(const LibraryEntry &entry) const;
  [[nodiscard]] std::vector<std::filesystem::path> scd_recordings(const LibraryEntry &entry) const;
  void draw_scd_import(float w);
  float draw_scd_details(LibraryEntry &entry, const StatusInfo &info, float w);
  void scd_browse(const std::string &purpose, const std::string &id);
  void scd_dialog_result(const std::string &purpose, const std::string &id, const std::vector<std::string> &paths);
  void scd_start_import(const std::string &target_id, const std::filesystem::path &bios, const std::filesystem::path &cue, const std::filesystem::path &program);
  void scd_poll();
  // mode: 0 live (records when "Record this session" is on), 1 replay <path> in the viewer, 2 replay <path> without a window (fast)
  void scd_play(const std::string &id, int mode = 0, const std::filesystem::path &script = {});
  bool scd_spawn(const std::string &id, const scd::Plan &plan);
  void scd_finish_run();
  [[nodiscard]] static std::string scd_label(const StatusInfo &info);
  [[nodiscard]] std::string scd_hint(const StatusInfo &info) const;
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
  std::set<std::string> pending_cleanup_ids_;   // finished builds whose older builds / intermediates still have to be removed
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
  std::string trace_path_;  // owned copy: SDL_getenv's pointer does not survive SDL_Quit, and the last trace lines are written after it
  std::string shot_path_;
  std::string script_shot_;  // script command `shot <file>`: save the next rendered frame
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
  // SEG-069-T004: keyboard/gamepad focus follows the build controls on Details. Build -> Cancel build or Unqueue (the same place), and either -> Build again,
  // instead of the focused item vanishing and ImGui's spatial scoring picking a neighbour.
  std::string details_refocus_;                  // "" | "after_build" | "build"
  int details_refocus_ttl_ = 0;                  // frames the request stays valid (a refused build must not leave it pending)
  void take_details_focus(const char *id);
  int seek_left_ = -1;       // script `pad_seek`: presses left (-1 idle)
  bool seek_pressed_ = false;
  std::string focus_label_;  // the sidebar entry holding keyboard/gamepad focus (script `expect focus=`)
  void run_script();
  [[nodiscard]] const char *page_name() const;
};

} // namespace launcher
