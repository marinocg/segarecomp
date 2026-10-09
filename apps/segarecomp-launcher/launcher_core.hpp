#pragma once

// Orchestration only: locate the packaged toolchain, derive the per-user cache entry for a ROM, run
// `segarecomp build` and launch the produced native program. No recompilation logic lives here.

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace launcher {

struct Layout {
  std::filesystem::path root;         // directory holding bin/, toolchain/, runtime/, sdl3/
  std::filesystem::path cli;          // segarecomp build driver
  std::filesystem::path cc;           // bundled zig
  std::filesystem::path runtime;      // runtime/platforms/genesis (runtime/, viewer/, compat/)
  std::filesystem::path runtime_sms;  // runtime/platforms/master-system (runtime/, headless/, viewer/); libs/ are siblings of platforms/
  std::filesystem::path sdl_include;
  std::filesystem::path sdl_lib;
  std::string problem;                // non-empty when the package is incomplete
};

struct RomView {
  std::filesystem::path path;
  std::string sha256;
  std::string title;
  std::string platform;               // "Genesis / Mega Drive" / "Master System" when supported
  std::string platform_id;            // "genesis" or "master-system" when supported, else empty
  bool supported_platform = false;
  // Master System only. The header never identifies the mapper: it is declared by the user (selection control or the
  // headless --mapper option) or by a sidecar manifest `<rom>.mapper.json`, and never defaulted.
  std::string mapper;                 // "sega" / "rom_only" (empty until declared)
  std::filesystem::path mapper_manifest;  // sidecar manifest when present
  std::string mapper_manifest_sha256;
  [[nodiscard]] bool needs_mapper() const { return platform_id == "master-system" && mapper.empty() && mapper_manifest.empty(); }
  // AOT optimization policy (ADR 0096), the same two values as `segarecomp build --aot-policy`. Compatibility is the default; Optimized
  // is a Genesis M68K policy (a Master System image has no Optimized producer). The launcher never sees a model, threshold or schema.
  std::string aot_policy = "compatibility";
  [[nodiscard]] bool supports_optimized() const { return platform_id == "genesis"; }
  [[nodiscard]] bool optimized() const { return supports_optimized() && aot_policy == "optimized"; }
  bool compat_known = false;          // ROM-hash-bound analysis metadata ships with this release
  std::string error;                  // non-empty: unreadable/too large
};

[[nodiscard]] Layout locate_layout();
[[nodiscard]] std::filesystem::path cache_root();
[[nodiscard]] RomView inspect_rom_file(const std::filesystem::path &path, const Layout &layout);
// The Master System baseline profile the launcher builds for (mirrors the machine library's one profile name).
inline constexpr const char *sms_profile = "sms2_ntsc_export";
[[nodiscard]] std::filesystem::path entry_dir(const RomView &rom, const Layout &layout);
[[nodiscard]] bool entry_ready(const std::filesystem::path &entry);
[[nodiscard]] std::filesystem::path entry_executable(const std::filesystem::path &entry);
// The machine-readable `aot_policy` member of a finished build's status.json (a thin read; no human text is parsed).
struct PolicyReport {
  bool present = false;
  std::string requested;  // compatibility | optimized
  std::string effective;  // broad | ml_region | admission_plan
  std::string reason;     // stable sanitized code (none, validator_rejected, ...)
  bool fallback = false;  // Optimized was requested but the build is the broad (Compatibility) program
};
[[nodiscard]] PolicyReport read_policy_report(const std::filesystem::path &entry);
// User-facing wording of a report (one short line; empty when there is nothing to say).
[[nodiscard]] std::string policy_summary(const PolicyReport &report);
[[nodiscard]] std::string read_tail(const std::filesystem::path &path, std::size_t max_bytes);
[[nodiscard]] std::string host_description();

enum class StageState { pending, running, done };

// One asynchronous `segarecomp build`; poll from the UI thread.
class BuildJob {
public:
  static constexpr int stage_count = 4;  // analyze, generate, compile, link
  BuildJob(RomView rom, Layout layout);
  ~BuildJob();
  BuildJob(const BuildJob &) = delete;
  BuildJob &operator=(const BuildJob &) = delete;

  [[nodiscard]] bool finished() const { return finished_.load(); }
  [[nodiscard]] bool succeeded() const { return ok_.load(); }
  [[nodiscard]] StageState stage(int index) const { return static_cast<StageState>(stages_[index].load()); }
  [[nodiscard]] std::string message() const;
  [[nodiscard]] std::filesystem::path entry() const { return entry_; }
  void wait();

private:
  void run();
  RomView rom_;
  Layout layout_;
  std::filesystem::path entry_;
  std::atomic<int> stages_[stage_count]{};
  std::atomic<bool> finished_{false};
  std::atomic<bool> ok_{false};
  mutable std::mutex mutex_;
  std::string message_;
  std::thread thread_;
};

// The launched native game; its output goes to <entry>/run.log.
class GameRun {
public:
  GameRun(const RomView &rom, const Layout &layout, const std::filesystem::path &entry,
          const std::vector<std::string> &extra_args = {});
  ~GameRun();
  GameRun(const GameRun &) = delete;
  GameRun &operator=(const GameRun &) = delete;
  [[nodiscard]] bool started() const { return process_ != nullptr; }
  // Returns true once the game exited (exit code stored).
  bool poll();
  [[nodiscard]] int exit_code() const { return exit_code_; }

private:
  SDL_Process *process_ = nullptr;
  int exit_code_ = 0;
};

} // namespace launcher
