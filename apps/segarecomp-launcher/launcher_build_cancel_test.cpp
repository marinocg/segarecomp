// SEG-069-T001: BuildJob cancellation without a window, against the product/program doubles.
//
//   launcher_build_cancel_test <fake_segarecomp> <fake_game>
//
//   * cancel before the worker starts: no spawn, no folder;
//   * cancel while the build tool and a grandchild run: the whole tree stops (heartbeat), the attempt's `.partial` is gone, no `.failed` is
//     created, the earlier completed entry is intact, the job reports `cancelled`, repeated requests are idempotent;
//   * cancel after the build has been published / has failed: refused, the outcome stands;
//   * the FIFO: removing a queued id keeps the order of the others.
// Project-authored synthetic files and doubles only.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "launcher_core.hpp"
#include "library_view.hpp"

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void set_env(const char *name, const std::string &value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
}

#if defined(_WIN32)
constexpr const char *exe = ".exe";
#else
constexpr const char *exe = "";
#endif

std::string slurp(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::uintmax_t file_size_or_zero(const fs::path &path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  return ec ? 0 : size;
}

template <class Predicate>
bool wait_until(Predicate done, int milliseconds) {
  for (int waited = 0; waited < milliseconds; waited += 10) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}

bool wait_finished(const BuildJob &job, int milliseconds = 20000) { return wait_until([&] { return job.finished(); }, milliseconds); }

fs::path make_package(const fs::path &root, const fs::path &fake_cli) {
  for (const char *file : {"toolchain/zig", "runtime/platforms/genesis/runtime/runtime.c", "runtime/platforms/genesis/viewer/viewer_sdl3.c",
                           "runtime/platforms/master-system/viewer/sms_viewer_sdl3.c", "runtime/libs/viewer/src/sv_host.c", "runtime/libs/viewer/src/sv_input_diag.c", "sdl3/include/SDL3/SDL.h"}) {
    const fs::path path = root / (std::string(file) + (std::string(file) == "toolchain/zig" ? exe : ""));
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
  }
  fs::create_directories(root / "bin");
  fs::copy_file(fake_cli, root / "bin" / (std::string("segarecomp") + exe), fs::copy_options::overwrite_existing);
  return root;
}

RomView make_rom(const fs::path &path) {
  std::vector<char> bytes(0x8000, 'r');
  std::memcpy(&bytes[0x7FF0], "TMR SEGA", 8);
  std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  RomView rom;
  rom.path = path;
  rom.sha256 = std::string(64, 'c');
  rom.title = "Cancel Test";
  rom.platform_id = "master-system";
  rom.supported_platform = true;
  rom.mapper = "sega";
  return rom;
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: launcher_build_cancel_test <fake_segarecomp> <fake_game>\n";
    return 2;
  }
  const fs::path fake_cli = argv[1], fake_game = argv[2];
  const fs::path dir = fs::temp_directory_path() / "segarecomp-launcher-build-cancel-test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path package = make_package(dir / "package", fake_cli);
  const fs::path cache = dir / "cache";
  fs::create_directories(cache);
  set_env("SEGARECOMP_HOME", package.string());
  set_env("SEGARECOMP_CACHE_DIR", cache.string());
  set_env("FAKE_GAME_BIN", fake_game.string());
  const Layout layout = locate_layout();
  expect(layout.problem.empty(), "the synthetic package is complete: " + layout.problem);
  const RomView rom = make_rom(dir / "cancel.sms");
  const fs::path entry = entry_dir(rom, layout);
  const fs::path partial = entry.parent_path() / (entry.filename().string() + ".partial");
  const fs::path failed = entry.parent_path() / (entry.filename().string() + ".failed");
  const fs::path heartbeat = dir / "heartbeat";
  const fs::path log = dir / "builds.txt";
  set_env("FAKE_BUILD_LOG", log.string());

  // ---- 1. cancelled before the worker started: nothing spawns, nothing is created ----
  {
    BuildJob job(rom, layout, /*cancelled_at_start=*/true);
    expect(wait_finished(job), "1) a job cancelled at start finishes");
    expect(job.outcome() == BuildOutcome::cancelled && !job.succeeded(), "1) outcome is cancelled, not failed");
    expect(!fs::exists(partial) && !fs::exists(failed) && !fs::exists(entry), "1) no folder of the attempt exists");
    expect(!fs::exists(log), "1) the build tool was never spawned");
    expect(job.request_cancel(), "1) repeated cancel of a cancelled job is idempotent (accepted)");
  }

  // ---- 2. an earlier completed build exists; a rebuild is cancelled while the tool and a grandchild run ----
  {
    {
      BuildJob first(rom, layout);
      expect(wait_finished(first) && first.outcome() == BuildOutcome::succeeded, "2) the first build succeeds");
    }
    expect(entry_ready(entry), "2) the completed entry is playable");
    const std::string before = slurp(entry / "status.json");
    const std::string metadata_before = slurp(entry / "metadata.json");
    set_env("FAKE_BUILD_HOLD", heartbeat.string());
    BuildJob job(rom, layout);
    expect(wait_until([&] { return fs::exists(partial / "hold_started"); }, 20000), "2) the held build reached the compile stage");
    expect(wait_until([&] { return file_size_or_zero(heartbeat) > 3; }, 20000), "2) the grandchild is alive and beating");
    expect(!job.finished() && job.outcome() == BuildOutcome::running, "2) the build is running");
    expect(job.request_cancel(), "2) the cancel request is accepted");
    expect(job.cancel_requested(), "2) the job reports that it is stopping");
    expect(job.request_cancel(), "2) a second request is harmless");
    expect(wait_finished(job), "2) the cancelled job becomes quiescent and finishes (no hang on the output pipe)");
    expect(job.outcome() == BuildOutcome::cancelled && !job.succeeded(), "2) outcome is cancelled");
    expect(!fs::exists(partial), "2) the attempt's .partial is removed");
    expect(!fs::exists(failed), "2) a cancelled attempt leaves no .failed diagnostics");
    expect(entry_ready(entry) && slurp(entry / "status.json") == before && slurp(entry / "metadata.json") == metadata_before,
           "2) the earlier completed entry is untouched");
    // The grandchild (a compiler stand-in) must be dead: its heartbeat stops. Sample twice across a window much longer than its period.
    const auto a = file_size_or_zero(heartbeat);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto b = file_size_or_zero(heartbeat);
    expect(a == b, "2) no process of the build tree is still running (heartbeat advanced " + std::to_string(b - a) + " bytes)");
    expect(job.request_cancel() && job.outcome() == BuildOutcome::cancelled, "2) cancel after the end of a cancelled job stays idempotent");
    set_env("FAKE_BUILD_HOLD", "");
  }

  // ---- 2b. a descendant that exists before anything else (the build tool starts it immediately) is stopped too ----
  {
    fs::remove_all(cache);
    fs::create_directories(cache);
    const fs::path early = dir / "heartbeat-early";
    set_env("FAKE_BUILD_HOLD", heartbeat.string());
    set_env("FAKE_SPAWN_FIRST", early.string());
    BuildJob job(rom, layout);
    expect(wait_until([&] { return fs::exists(partial / "hold_started") && file_size_or_zero(early) > 3; }, 20000), "2b) the early descendant is beating");
    expect(job.request_cancel() && wait_finished(job) && job.outcome() == BuildOutcome::cancelled, "2b) cancelled");
    const auto a = file_size_or_zero(early);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    expect(a == file_size_or_zero(early), "2b) the early descendant is stopped as well");
    set_env("FAKE_SPAWN_FIRST", "");
    set_env("FAKE_BUILD_HOLD", "");
  }

  // ---- 3. a cancel that comes after the build was published is refused; the success stands ----
  {
    fs::remove_all(cache);
    fs::create_directories(cache);
    BuildJob job(rom, layout);
    expect(wait_finished(job) && job.outcome() == BuildOutcome::succeeded, "3) the build succeeds");
    expect(!job.request_cancel(), "3) a cancel after publication is refused");
    expect(job.outcome() == BuildOutcome::succeeded && entry_ready(entry) && !job.cancel_requested(), "3) the published entry stands");
  }

  // ---- 4. a build that failed by itself is not retroactively a cancellation ----
  {
    fs::remove_all(cache);
    fs::create_directories(cache);
    set_env("FAKE_BUILD_FAIL", "compatibility");
    BuildJob job(rom, layout);
    expect(wait_finished(job) && job.outcome() == BuildOutcome::failed, "4) the build fails on its own");
    expect(!job.request_cancel() && job.outcome() == BuildOutcome::failed, "4) cancel after a failure is refused; it stays a failure");
    expect(fs::exists(failed) && !fs::exists(partial), "4) the failure keeps its diagnostics folder");
    set_env("FAKE_BUILD_FAIL", "");
  }

  // ---- 5. a second build after a cancel starts cleanly (nothing blocks the next attempt) ----
  {
    fs::remove_all(cache);
    fs::create_directories(cache);
    set_env("FAKE_BUILD_HOLD", heartbeat.string());
    {
      BuildJob held(rom, layout);
      expect(wait_until([&] { return fs::exists(partial / "hold_started"); }, 20000), "5) held build started");
      expect(held.request_cancel() && wait_finished(held) && held.outcome() == BuildOutcome::cancelled, "5) first attempt cancelled");
    }
    set_env("FAKE_BUILD_HOLD", "");
    BuildJob next(rom, layout);
    expect(wait_finished(next) && next.outcome() == BuildOutcome::succeeded && entry_ready(entry), "5) the next attempt builds and publishes normally");
    expect(!fs::exists(partial) && !fs::exists(failed), "5) no leftover of the cancelled attempt");
  }

  // ---- 6. queue accounting: removing a queued game keeps the other entries' order ----
  {
    BuildQueue queue;
    for (const char *id : {"a", "b", "c", "d"}) queue.push(id);
    expect(queue.remove("b") && !queue.remove("b") && queue.size() == 3 && !queue.contains("b"), "6) the queued game is removed once");
    expect(queue.pop_front() == "a" && queue.pop_front() == "c" && queue.pop_front() == "d" && queue.empty(), "6) the remaining order is unchanged");
  }

  fs::remove_all(dir);
  if (failures == 0) std::cout << "launcher_build_cancel_test: ok\n";
  return failures == 0 ? 0 : 1;
}
