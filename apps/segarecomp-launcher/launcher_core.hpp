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
  // SEG-049: the EXPERIMENTAL model family of Optimized ("v1" = default, "gen2" = explicit opt-in), forwarded verbatim to `segarecomp build
  // --aot-model`. It is an opaque product option: the launcher knows no model, threshold, schema or compatibility rule.
  std::string aot_model = "v1";
  [[nodiscard]] bool optimized_gen2() const { return optimized() && aot_model == "gen2"; }
  // Genesis console region (SEG-055): a RUN-TIME choice exported to the game as SEGARECOMP_GENESIS_REGION; it is not part of the cache
  // identity (one native build serves every region). "usa" (default, the historical console), "japan", or "europe" (PAL: admitted, but the
  // machine runs at 60 Hz; 50 Hz timing is not implemented and the UI says so, see region_note). The initial value is a hint from
  // the header's region field; an intrinsically ambiguous field keeps the default and is flagged (`region_ambiguous`).
  std::string region = "usa";
  bool region_ambiguous = false;
  [[nodiscard]] bool supports_region() const { return platform_id == "genesis"; }
  // Advanced, default OFF (changes the generated program, so it is part of the cache identity): run the build-time preparation passes
  // with a strict VDP H-interrupt policy so a RAM-resident H-interrupt vector stops the pass and its byte-guarded copy alias / RAM thunk
  // can be derived (ADR 0049) instead of the interrupt being counted and dropped. Forwarded as `segarecomp build --hint-alias-discovery 1`.
  bool hint_alias_discovery = false;
  [[nodiscard]] bool supports_hint_alias_discovery() const { return platform_id == "genesis"; }
  bool compat_known = false;          // ROM-hash-bound analysis metadata ships with this release
  std::string error;                  // non-empty: unreadable/too large
};

// The region hint of a Genesis header's region-support field ($1F0-$1FF, 16 bytes). Two encodings exist: old-style letters J/U/E and a
// newer single hexadecimal bitfield digit (bit0 Japan NTSC, bit2 Americas NTSC, bit3 Europe PAL). A lone "E" is valid in BOTH (Europe, or
// the hex digit 0xE = Asia + Americas + Europe), so it is reported as ambiguous and keeps the default; the explicit selection is the override.
struct RegionHint { std::string region = "usa"; bool ambiguous = false; };
[[nodiscard]] std::string clean_title(const std::string &raw);  // printable ASCII, single spaces, trimmed
// Genesis header title (domestic 0x120 / overseas 0x150, 48 bytes each) as clean ASCII: Shift-JIS full-width Latin is decoded, a field
// holding anything else (katakana, stray bytes) is skipped, and the file name (without its (...)/[...] tags) is the last resort.
[[nodiscard]] std::string genesis_header_title(const std::vector<std::uint8_t> &bytes, bool domestic_first, const std::string &file_stem);
[[nodiscard]] std::string region_note(const std::string &region);
// The region the build's Z80 observation ran under (empty for an older/Master System entry) and the note shown when the selected run-time
// region differs from it (Z80 coverage is established for the observed region only).
[[nodiscard]] std::string read_observed_region(const std::filesystem::path &entry);
[[nodiscard]] std::string region_coverage_note(const std::string &observed, const std::string &selected);
[[nodiscard]] RegionHint region_hint_from_header_field(const std::string &field);
// Region profiles the runtime accepts: usa, japan and europe (Europe PAL runs on the 60 Hz schedule; 50 Hz timing is not implemented, see region_note).
[[nodiscard]] bool region_executable(const std::string &region);
// Empty when the game may be launched; otherwise the plain-language reason it must not be (an unknown region name).
[[nodiscard]] std::string launch_refusal(const RomView &rom);
// Applies the per-launch environment of the child game to `environment` (never the launcher's own process environment).
void apply_game_environment(SDL_Environment *environment, const RomView &rom);

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
  std::string requested;        // compatibility | optimized
  std::string requested_model;  // none | v1 | gen2
  std::string effective;        // broad | ml_region | ml_region_gen2 | admission_plan (the producer that actually ran)
  std::string reason;           // stable sanitized code (none, validator_rejected, ...)
  std::string fallback_class;   // stable class (none, producer_incompatible, producer_input, proposal_rejected, ...)
  std::string model_family;     // none | v1 | gen2
  std::string validator;        // accepted | rejected | not_run
  std::string model, rank_policy, policy_identity, identity;  // product-provided identities (diagnostics / staleness only)
  long long selected_ppm = -1, admitted_ppm = -1;             // parts per million; -1 when not reported
  bool fallback = false;        // Optimized was requested but the build is the broad (Compatibility) program
};
[[nodiscard]] PolicyReport read_policy_report(const std::filesystem::path &entry);
// User-facing wording of a report (one short line; empty when there is nothing to say).
[[nodiscard]] std::string policy_summary(const PolicyReport &report);
// Advanced diagnostics text (producer, model/identity, fractions, validator, fallback class/reason); sanitized product fields only.
[[nodiscard]] std::string policy_details(const PolicyReport &report);
// The ROM-independent policy identity printed by `segarecomp aot-policy-identity` (empty when the tool cannot be run). The launcher keys its cache on
// it instead of reproducing any model/schema/contract compatibility rule. Memoized per (tool, policy, model).
// Memo only (empty when not resolved yet or the last attempt failed); never runs the tool, so it is cheap enough for every UI frame.
[[nodiscard]] std::string product_policy_identity_memo_lookup(const std::string &key);
[[nodiscard]] std::string product_policy_identity_cached(const Layout &layout, const std::string &policy, const std::string &model);
// Runs the product tool (blocking, up to a few seconds): call it from a worker, never from the UI thread.
[[nodiscard]] std::string product_policy_identity(const Layout &layout, const std::string &policy, const std::string &model);
// A finished entry is reusable only when it exists AND its recorded request/identity still equal the current request (a stale or foreign
// entry that somehow shares a directory is rebuilt, never reused).
[[nodiscard]] bool entry_ready_for(const std::filesystem::path &entry, const RomView &rom, const Layout &layout);
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
