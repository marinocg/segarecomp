#pragma once

// SEG-078-T002 (ADR 0204): Sega CD in the launcher, without a compiler. A Sega CD library entry is the user's BIOS image plus the user's disc
// (a CUE sheet), both referenced by path - never copied, never stored beyond path, size and SHA-256 - and, optionally, an IMPORTED generated-native
// viewer program: either BUILT on this computer by `segarecomp sega-cd build` (SEG-078-T003, ADR 0205: the launcher runs the product CLI with the bundled
// compiler; see ScdBuildJob in launcher_core.hpp) or IMPORTED from a developer build (platforms/sega-cd/README.md). Both paths end in the same
// verification below: a built program is imported through `attach_program` like any other, so it is bound to exactly these two files.
//
// What the launcher can verify - and nothing more is claimed:
//   * the BIOS image: size (<= 128 KiB) and SHA-256, measured here;
//   * the disc: the program's own table names every disc file with its size and SHA-256; those files are looked up NEXT TO the CUE sheet (the CUE is
//     never parsed here) and measured. Sizes are checked at every status refresh, digests when a program is imported or re-verified; the generated
//     program itself re-verifies every file before the first guest instruction and answers with a typed refusal;
//   * the program: its file is hashed at import (SHA-256, size, modification time) and again before every launch; what it says about itself comes
//     from `<program> --describe` (runtime ABI, viewer or headless-only, the BIOS and disc it was built from). The claims are compared with the
//     measured files. A program whose claims cannot be obtained is `unverifiable`, never trusted. There is no manifest to edit.
//   * NOT verifiable here: that the program faithfully implements the disc - that is what the build-time evidence of the developer procedure is for.
//     "Verified" therefore means: unchanged since import, built for exactly these two files, and speaking this launcher's contract.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace launcher::scd {

// The contract version of the generated program (platforms/sega-cd/runtime/scd_runtime.h SCD_RUNTIME_ABI) this launcher speaks.
inline constexpr int supported_runtime_abi = 1;
inline constexpr std::uintmax_t max_bios_bytes = 0x20000U;           // the program refuses a larger image
inline constexpr std::uintmax_t max_cue_bytes = 64U * 1024U;         // a CUE sheet is a short text file; only its digest is taken
inline constexpr std::uintmax_t max_program_bytes = 3ULL << 30U;     // a generated program is far smaller; larger is refused before hashing
inline constexpr std::size_t max_disc_files = 8;                     // the runtime's limit
inline constexpr int identity_version = 1;                           // the entry identity layout below

struct FileFact {
  std::string name;         // disc files: the name in the program's table (a plain file name, no directory)
  std::uint64_t size = 0;
  std::string sha256;
};

// The entry identity: SHA-256 of "segarecomp-sega-cd-entry-1\nbios <bios sha256>\ncue <cue sha256>\n". It is the `sha256` of the library entry
// (platform "sega-cd"), so a different BIOS or a different CUE sheet is a different game; the disc files and the program are bound to the entry, not
// part of its identity.
[[nodiscard]] std::string entry_identity(const std::string &bios_sha256, const std::string &cue_sha256);

// Why a launch or an import is refused. Every class has a stable code (`code()`) and a plain-language line (`message()`).
enum class Refusal {
  none,
  inputs_missing,          // no BIOS and/or no disc reference configured
  bios_missing,            // the BIOS file is not at its recorded place
  bios_unreadable,
  bios_too_large,
  bios_changed,            // size/SHA-256 differ from the recorded ones
  cue_missing,
  cue_changed,
  program_missing,         // no program imported
  program_file_missing,    // the imported program is not at its recorded place
  program_changed,         // size/time/SHA-256 differ from the import
  program_unverifiable,    // `--describe` could not be run or did not answer a well-formed statement
  program_abi_unsupported, // built for another runtime contract
  program_headless_only,   // not a viewer variant
  program_bios_mismatch,   // built for another BIOS image
  program_disc_mismatch,   // built for another disc (no table file matches)
  disc_file_missing,
  disc_file_size_mismatch,
  disc_file_changed,       // SHA-256 differs
  unsupported_build,       // this installation cannot build Sega CD games (its Sega CD runtime sources are missing)
};
[[nodiscard]] const char *code(Refusal r);
[[nodiscard]] std::string message(Refusal r, const std::string &detail = {});

// What a program claims about itself (`<program> --describe`).
struct Claims {
  Refusal refusal = Refusal::none;    // program_unverifiable when the statement was not obtained
  std::string detail;                 // sanitized reason
  int runtime_abi = 0;
  bool viewer = false;
  std::string profile;                // "bios_image" | "synthetic"
  std::string program_identity;       // the program's build identity
  std::string disc_identity;
  std::uint64_t bios_size = 0;
  std::string bios_sha256;
  std::vector<FileFact> disc_files;
};
// Parses the one-line statement. Strict: unknown shapes, missing members, oversized text, non-hex digests, path-like file names, more than
// max_disc_files files, or a duplicate name are `program_unverifiable`.
[[nodiscard]] Claims parse_claims(const std::string &text);
// Runs `program --describe` (blocking; worker only), at most `timeout_ms`, and parses the statement.
[[nodiscard]] Claims describe_program(const std::filesystem::path &program, unsigned timeout_ms = 15000);

struct Digest {
  bool ok = false;
  std::string error;                  // "missing" | "unreadable" | "too_large" | "cancelled"
  std::uint64_t size = 0;
  std::string sha256;
};
// Streams a file through SHA-256. `limit` refuses a larger file before reading. `done` (optional) accumulates bytes read, `cancel` stops early.
[[nodiscard]] Digest hash_file(const std::filesystem::path &path, std::uintmax_t limit, std::atomic<std::uint64_t> *done = nullptr,
                               const std::atomic<bool> *cancel = nullptr);

// The imported program, as recorded in the library.
struct ProgramRecord {
  std::string path;                   // UTF-8, a reference; the program stays where the user (or the developer procedure) put it
  std::uint64_t size = 0;
  long long mtime = 0;                // unix seconds at import
  std::string sha256;                 // of the file at import
  std::string program_identity, disc_identity;
  int runtime_abi = 0;
  bool viewer = false;
  long long verified_at = 0;
};

// Everything the library stores about a Sega CD entry.
struct EntryRecord {
  int identity_version = scd::identity_version;
  std::string bios_path;              // UTF-8 reference
  std::uint64_t bios_size = 0;
  std::string bios_sha256;
  std::string cue_path;               // UTF-8 reference
  std::uint64_t cue_size = 0;
  std::string cue_sha256;
  std::vector<FileFact> disc_files;   // from the imported program's table, measured at import (empty before)
  bool disc_verified = false;         // the digests above were measured and matched
  ProgramRecord program;              // path empty = none imported
  std::string last_stop;              // the typed stop class of the last run ("" none)
  // SEG-097-T003 (ADR 0208): the EXPERIMENTAL Sega CD build option. `experimental_m3` is the user's choice for the next build (off by default);
  // `program_experimental` is what the stored program was actually built with (the product's own effective policy). Both are written only when true.
  bool experimental_m3 = false;
  bool program_experimental = false;
  // SEG-103-T007: the file-first (structural) build choice. SELECTED (what the next Build requests) is `experimental_m3` (Optimized), `file_first`, or neither
  // (Compatibility); never both: use select_build_mode. BUILT (what the attached program was actually produced with, from the product's structured status.json,
  // never from the selection) is `program_built_mode`: "" when unknown (an imported or older program: nothing is invented), else "compatibility" | "file_first" |
  // "optimized"; `program_file_first_inventory` is "complete" | "incomplete" for a file-first program. Written only when set, so older records stay byte-identical.
  bool file_first = false;
  std::string program_built_mode;
  std::string program_file_first_inventory;
};
// The selected mode as a stable code ("compatibility" | "file_first" | "optimized"), and its setter (an unknown code selects Compatibility). A record that
// somehow holds both flags (hand-edited) reads as Optimized, the choice that existed first.
[[nodiscard]] const char *selected_build_mode(const EntryRecord &record);
void select_build_mode(EntryRecord &record, std::string_view code);

// How the entry stands. Order matters for the library badge: the first failing check names the state.
enum class State {
  inputs_not_configured,   // BIOS and/or disc reference missing
  no_program,              // inputs fine, no program yet: build one (Segarecomp) or import a verified one
  headless_only,           // the imported program has no viewer
  stale,                   // changed since import, or an older runtime contract
  mismatched,              // built for other inputs than the ones configured
  ready,                   // checks passed
};
[[nodiscard]] const char *state_label(State s);
// The typed execution stops that mean "the program reached code it was not built to run" - for an experimental build, the code the selective policy
// omitted. Product-defined class names (platforms/sega-cd/runtime/scd_runtime.h); the launcher decides nothing else from them.
[[nodiscard]] bool is_uncompiled_entry_stop(std::string_view stop_class);

struct Status {
  State state = State::inputs_not_configured;
  Refusal refusal = Refusal::none;
  std::string detail;
  bool bios_ok = false, disc_ok = false, program_ok = false;
};

// Cheap, no program execution: BIOS full hash (<= 128 KiB), CUE existence, disc file sizes (stat), program file size and modification time against
// the import stamp. Safe on every status refresh (worker).
[[nodiscard]] Status quick_status(const EntryRecord &record);

// What a launch needs, established in full: BIOS and CUE digests, the program file hash and `--describe`, the claims against the measured files,
// the disc files' sizes (and digests when `check_disc_digests`). Blocking (seconds on a disc); run it on a worker.
struct Plan {
  Status status;
  std::filesystem::path program, bios, disc_dir;
  Claims claims;
  long long program_mtime = 0;        // the program file's current modification time: the hash matched, so the caller refreshes the import stamp
};
[[nodiscard]] Plan prepare_launch(const EntryRecord &record, bool check_disc_digests, std::atomic<std::uint64_t> *done = nullptr,
                                  const std::atomic<bool> *cancel = nullptr);

// ---- import ----
struct ImportRequest {
  std::filesystem::path bios, cue, program;   // program may be empty (the entry is created without one)
};
struct ImportResult {
  Refusal refusal = Refusal::none;
  std::string detail;
  EntryRecord record;
  std::string title;                          // from the CUE file name
  bool program_attached = false;
};
// Measures the BIOS and CUE, and when a program is given verifies it against them (describe, claims, disc digests). A BIOS or CUE problem
// refuses the whole import; a program problem refuses only the program (the entry is still created, with the reason in `refusal`/`detail`).
[[nodiscard]] ImportResult import_entry(const ImportRequest &request, std::atomic<std::uint64_t> *done = nullptr, const std::atomic<bool> *cancel = nullptr);
// Attaches (or re-verifies) a program for an existing record. `record` is changed only on success.
[[nodiscard]] ImportResult attach_program(const EntryRecord &record, const std::filesystem::path &program, std::atomic<std::uint64_t> *done = nullptr,
                                          const std::atomic<bool> *cancel = nullptr);

// ---- running ----
// The command line of a verified program: --disc-dir, --bios, --instruction-budget, --report and the recording / replay choice. The budget is
// large but finite (a viewer session is bounded by the user; the program never runs unbounded).
struct RunOptions {
  std::filesystem::path backup_ram;    // --backup-ram: the formatted, empty internal backup RAM image (see ensure_backup_ram)
  std::filesystem::path backup_ram_out; // --backup-ram-out: the candidate the program exports at the end (empty = none; never the canonical save)
  std::filesystem::path report;
  std::filesystem::path record_to;     // --record-input
  std::filesystem::path replay_from;   // --input-script (replay in the viewer)
  std::uint64_t instruction_budget = 1ULL << 50U;
  std::uint64_t until_tick = 0;        // --until-tick (0 = none): the end of a replay without a window, which has no one to close it
};
// The machine tick at which a replay of `script` ends: its last event plus `margin_frames` frames (the first onset plus the margin for an empty script).
[[nodiscard]] std::uint64_t replay_end_tick(const std::filesystem::path &script, unsigned margin_frames = 120);
[[nodiscard]] std::vector<std::string> run_arguments(const Plan &plan, const RunOptions &options);
// The BIOS reads the console's internal backup RAM, and the program stops with a typed stop when no image is supplied. A new game starts from the state
// of a new console: 8 KiB, zero except for the public 64-byte format trailer (ADR 0147; identical to tools/sega_cd_backup_ram.py). It holds no
// software-derived byte. Saves persist (ADR 0252): `<game folder>/backup-ram.bin` is the player's data and is never reformatted.
[[nodiscard]] std::vector<std::uint8_t> formatted_backup_ram();
enum class BackupStatus { ok, created, wrong_size, unreadable, not_writable };
// An existing regular file of exactly 8 KiB is accepted unchanged whatever it contains; a missing one is created formatted; a wrong-size or unreadable
// one is reported and never replaced.
[[nodiscard]] BackupStatus ensure_backup_ram(const std::filesystem::path &path);
// Writes `bytes` through a temporary file and one rename, so the previous file survives any failure. False when it could not be written.
[[nodiscard]] bool write_file_atomic(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes);
// The scratch image of a build pass or replay: always the formatted one, rewritten each time (not user data).
[[nodiscard]] bool write_pristine_backup_ram(const std::filesystem::path &path);
// Fills backup_ram / backup_ram_out for a run in `dir`. persistent: the canonical save is the input and a candidate the output; otherwise (replay,
// recording) a pristine scratch image is the input and nothing is exported. A status other than ok/created means the game must not be started.
[[nodiscard]] BackupStatus prepare_backup_ram(RunOptions &options, const std::filesystem::path &dir, bool persistent);
[[nodiscard]] std::string describe_backup(BackupStatus status);
enum class CommitStatus { committed, no_candidate, invalid_candidate, replace_failed };
// After a successful run: validates the candidate (a regular file of exactly 8 KiB) and replaces the canonical save by rename, keeping one `.prev`
// generation. On any failure the canonical save is untouched. The candidate is consumed in every case.
[[nodiscard]] CommitStatus commit_backup_ram(const std::filesystem::path &canonical, const std::filesystem::path &candidate);
// What to do with the candidate when a run has ended: only a clean exit (status 0) is committed; any other end (typed stop, budget, refusal, crash, kill)
// discards the candidate and keeps the previous save. `not_requested` for a replay or recording.
enum class SaveOutcome { not_requested, stored, kept_run_failed, kept_commit_failed };
[[nodiscard]] SaveOutcome finish_backup_ram(const RunOptions &options, int exit_code);

// The outcome of a finished run, from the program's exit status and its --report file (a stable, sanitized subset).
struct Outcome {
  bool report_read = false;
  std::string result;                  // idle | stopped | budget_exhausted | deadline | ...
  std::string stop_class;              // typed stop class when result == stopped
  std::string live_outcome;            // quit | machine_stop | host_error | recording_capacity | ...
  std::uint64_t frames = 0, presented = 0;
  std::uint64_t input_events = 0;
  int exit_code = 0;
};
[[nodiscard]] Outcome read_outcome(const std::filesystem::path &report, int exit_code);
// One line for the user: "Closed", "Stopped by a typed execution stop: entry_not_compiled", "The BIOS does not match this program", ...
[[nodiscard]] std::string describe_outcome(const Outcome &outcome);
// True when the exit status is one of the program's typed host/refusal statuses (20 disc, 21 BIOS, 22 backup RAM, 23 input script, 24 host, 25 recording).
[[nodiscard]] bool is_refusal_exit(int exit_code);

} // namespace launcher::scd
