#include "build_command.hpp"

#include "segarecomp/machine/master_system/cartridge.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"
#include "segarecomp/machine/genesis/hybrid_admission.hpp"
#include "segarecomp/machine/genesis/m68k_copy_alias.hpp"
#include "segarecomp/machine/genesis/z80_materialization.hpp"
#include "segarecomp/machine/master_system/emit.hpp"
#include "segarecomp/rom.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;
namespace sms = segarecomp::machine::master_system;
namespace gz80 = segarecomp::machine::genesis::z80;
namespace m68k_alias = segarecomp::machine::genesis::m68k_alias;

namespace {

// Narrow strings are the platform's native argv encoding (the ANSI code page on Windows, as for the rest of the CLI).
// ---- process helper: run argv, stdout+stderr -> log file, return the exit code (-1: could not start) ----

#if defined(_WIN32)
std::wstring widen(const std::string &text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

std::wstring quote_argument(const std::wstring &arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') { ++backslashes; continue; }
    if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
    else out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

using Environment = std::vector<std::pair<std::string, std::string>>;
constexpr int kProcessTimedOut = -2;

// `timeout_seconds` 0 = wait forever; on expiry the process is killed and kProcessTimedOut returned. `environment` entries are
// added to the child's environment.
int run_process(const std::vector<std::string> &argv, const fs::path &log, const Environment &environment = {},
                unsigned timeout_seconds = 0) {
  for (const auto &entry : environment) SetEnvironmentVariableA(entry.first.c_str(), entry.second.c_str());
  std::wstring command;
  for (const auto &arg : argv) {
    if (!command.empty()) command.push_back(L' ');
    command += quote_argument(widen(arg));
  }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE sink = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sink == INVALID_HANDLE_VALUE) return -1;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = sink;
  startup.hStdError = sink;
  PROCESS_INFORMATION info{};
  const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                      nullptr, nullptr, &startup, &info);
  CloseHandle(sink);
  if (!started) return -1;
  int timed_out = 0;
  if (WaitForSingleObject(info.hProcess, timeout_seconds == 0 ? INFINITE : timeout_seconds * 1000U) == WAIT_TIMEOUT) {
    TerminateProcess(info.hProcess, 1);
    WaitForSingleObject(info.hProcess, INFINITE);
    timed_out = 1;
  }
  for (const auto &entry : environment) SetEnvironmentVariableA(entry.first.c_str(), nullptr);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  CloseHandle(info.hThread);
  return timed_out ? kProcessTimedOut : static_cast<int>(code);
}
#else
using Environment = std::vector<std::pair<std::string, std::string>>;
constexpr int kProcessTimedOut = -2;

int run_process(const std::vector<std::string> &argv, const fs::path &log, const Environment &environment = {},
                unsigned timeout_seconds = 0) {
  std::vector<std::string> env_storage;
  for (char **entry = environ; *entry != nullptr; ++entry) {
    const std::string text = *entry;
    const std::string name = text.substr(0, text.find('='));
    if (std::none_of(environment.begin(), environment.end(), [&](const auto &e) { return e.first == name; })) env_storage.push_back(text);
  }
  for (const auto &entry : environment) env_storage.push_back(entry.first + "=" + entry.second);
  std::vector<char *> envp;
  for (auto &entry : env_storage) envp.push_back(entry.data());
  envp.push_back(nullptr);
  std::vector<char *> args;
  for (const auto &arg : argv) args.push_back(const_cast<char *>(arg.c_str()));
  args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  pid_t pid{};
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) return -1;
  int status = 0;
  if (timeout_seconds == 0) {
    while (waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) return -1;
    }
  } else {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    for (;;) {
      const pid_t done = waitpid(pid, &status, WNOHANG);
      if (done == pid) break;
      if (done < 0 && errno != EINTR) return -1;
      if (std::chrono::steady_clock::now() >= deadline) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return kProcessTimedOut;
      }
      const timespec pause{0, 5 * 1000 * 1000};
      nanosleep(&pause, nullptr);
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}
#endif

// ---- build ----

struct Options {
  fs::path rom;
  fs::path output;
  std::string cc;
  std::vector<std::string> cc_args;
  std::string cxx;  // C++ compiler for the vendored YM2612 core (SEG-032-T008); derived from --cc when absent
  std::vector<std::string> cxx_args;
  bool cxx_args_given = false;
  bool keep_work = false;  // Genesis: retain obj/ (objects, pass program, pass report) for falsification tooling
  std::vector<std::string> link_args;
  fs::path runtime_dir;  // contains runtime/, viewer/ and (optional) compat/
  std::optional<fs::path> sdl3_include;
  std::optional<fs::path> sdl3_lib;
  // 0, 1 or 2 for every unit. -O2 stays the default: after the Z80 owner grouping and shared bodies (SEG-033) -O1 saved at most
  // 4% of compile time on real images for a 4% slower generated program, so no split generated/handwritten policy exists.
  std::string optimize = "2";
  // Genesis only: optimization of the stable set (generated M68K units, handwritten runtime, PSG, vendored YM2612) when it should differ
  // from the generated Z80 image units. The Z80 image units are megabytes of machine-formatted C whose compile time dominates a build,
  // while the stable set is small but is what the build-time materialization pass spends its run time in; `--optimize 0
  // --runtime-optimize 1` keeps the first cheap and speeds the second up. Empty: same as --optimize. Output C is unaffected.
  std::string runtime_optimize;
  unsigned jobs = 0;
  // Master System routing (SEG-009-T010). `platform` empty: classify the image; the mapper is never inferred.
  std::string platform;                 // "", "genesis" or "master-system"
  std::string mapper;                   // explicit mapper family declaration (build option), e.g. "sega"
  std::optional<fs::path> mapper_manifest;
  // SEG-031 (ADR 0080): explicit Genesis M68K hybrid admission candidate (a plan from an exact-map plan builder). Parsed once,
  // strictly; each M68K emission applies it only when its alias set equals the emission's (otherwise that emission is broad).
  std::optional<fs::path> admission_plan;
  std::optional<segarecomp::GenesisHybridAdmissionPlan> admission;
  // SEG-047 (ADR 0096): the user-visible AOT optimization policy. `compatibility` (default): the broad immutable-ROM universe.
  // `optimized`: Genesis M68K selective admission proposed by the native frozen region model (never a user-configurable model);
  // any producer failure falls back to the broad universe (visible in status.json), and an explicit --admission-plan (exact map)
  // outranks it.
  std::string aot_policy = "compatibility";
};

enum class Target { genesis, master_system };

struct Log {
  std::ofstream file;
  bool plan_applied = false;   // the last emission applied the explicit --admission-plan (alias set matched)
  std::string admission_line;  // SEG-047: the last emission's sanitized `m68k admission: requested=optimized ...` report
  void line(const std::string &text) { file << text << '\n'; file.flush(); }
  void append_file(const fs::path &path) {
    // Not `file << in.rdbuf()`: that sets failbit on an empty source and silently drops every later line.
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    if (in) text << in.rdbuf();
    file << text.str();
    file.flush();
  }
};

std::string json_escape(const std::string &text) {
  std::string out;
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(ch); }
    else if (c == '\n') out += "\\n";
    else if (c < 0x20U) out += ' ';
    else out.push_back(static_cast<char>(c));
  }
  return out;
}

// `extra` is a pre-formatted JSON member list (leading comma included) appended after rom_sha256: Master System
// provenance, the executable-image provenance counts and the typed diagnostic.
void write_status(const Options &options, const std::string &sha, const std::string &status,
                  const std::string &stage, const std::string &message, const std::string &extra = "") {
  std::ofstream out(options.output / "status.json", std::ios::binary | std::ios::trunc);
  out << "{\"status\":\"" << status << "\",\"stage\":\"" << stage << "\",\"message\":\"" << json_escape(message)
      << "\",\"rom_sha256\":\"" << sha << "\"" << extra << "}\n";
}

int fail(const Options &options, Log &log, const std::string &sha, const std::string &stage, int code,
         const std::string &message, const std::string &diagnostic = "") {
  log.line("FAILED (" + stage + "): " + message);
  write_status(options, sha, "failed", stage, message, diagnostic.empty() ? "" : ",\"diagnostic\":\"" + json_escape(diagnostic) + "\"");
  std::cout << "@result failed stage=" << stage << " message=" << message << std::endl;
  return code;
}

void stage(const char *name, const char *state) { std::cout << "@stage " << name << ' ' << state << std::endl; }

std::optional<Options> parse_options(int argc, char **argv) {
  Options options;
  bool have_rom = false, have_output = false, have_runtime = false;
  for (int i = 2; i < argc; i += 2) {
    const std::string_view key = argv[i];
    if (i + 1 >= argc) return std::nullopt;
    const std::string value = argv[i + 1];
    if (key == "--rom") { options.rom = fs::path(value); have_rom = true; }
    else if (key == "--output") { options.output = fs::path(value); have_output = true; }
    else if (key == "--cc") options.cc = value;
    else if (key == "--cc-arg") options.cc_args.push_back(value);
    else if (key == "--keep-work") options.keep_work = value == "1";
    else if (key == "--cxx") options.cxx = value;
    else if (key == "--cxx-arg") { options.cxx_args.push_back(value); options.cxx_args_given = true; }
    else if (key == "--link-arg") options.link_args.push_back(value);
    else if (key == "--runtime-dir") { options.runtime_dir = fs::path(value); have_runtime = true; }
    else if (key == "--sdl3-include") options.sdl3_include = fs::path(value);
    else if (key == "--sdl3-lib") options.sdl3_lib = fs::path(value);
    else if (key == "--platform") options.platform = value;
    else if (key == "--mapper") options.mapper = value;
    else if (key == "--mapper-manifest") options.mapper_manifest = fs::path(value);
    else if (key == "--optimize") options.optimize = value;
    else if (key == "--runtime-optimize") options.runtime_optimize = value;
    else if (key == "--jobs") options.jobs = static_cast<unsigned>(std::max(0, std::atoi(value.c_str())));
    else if (key == "--admission-plan" && !options.admission_plan) options.admission_plan = fs::path(value);
    else if (key == "--aot-policy" && (value == "compatibility" || value == "optimized")) options.aot_policy = value;
    else return std::nullopt;
  }
  if (!have_rom || !have_output || !have_runtime || options.cc.empty()) return std::nullopt;
  if (options.optimize != "0" && options.optimize != "1" && options.optimize != "2") return std::nullopt;
  if (!options.runtime_optimize.empty() && options.runtime_optimize != "0" && options.runtime_optimize != "1" && options.runtime_optimize != "2") return std::nullopt;
  if (options.sdl3_include.has_value() != options.sdl3_lib.has_value()) return std::nullopt;
  if (!options.platform.empty() && options.platform != "genesis" && options.platform != "master-system") return std::nullopt;
  return options;
}

// Runs every compile job with a small worker pool; returns the first failing job index or -1.
int run_compiles(const std::vector<std::vector<std::string>> &commands, const std::vector<fs::path> &logs,
                 unsigned jobs, std::vector<int> &codes) {
  codes.assign(commands.size(), 0);
  std::atomic<std::size_t> next{0};
  std::atomic<bool> stop{false};
  const auto worker = [&] {
    for (;;) {
      const std::size_t index = next.fetch_add(1);
      if (index >= commands.size() || stop.load()) return;
      codes[index] = run_process(commands[index], logs[index]);
      if (codes[index] != 0) stop.store(true);
    }
  };
  std::vector<std::thread> pool;
  for (unsigned n = 1; n < jobs; ++n) pool.emplace_back(worker);
  worker();
  for (auto &thread : pool) thread.join();
  for (std::size_t i = 0; i < codes.size(); ++i)
    if (codes[i] != 0) return static_cast<int>(i);
  return -1;
}

// ---- Genesis native program with sound (SEG-032-T008; ADR 0073) ----
//
// One link set: the generated M68K units, the Genesis runtime, the Z80 machine and the sound devices (shared PSG, vendored ymfm
// YM2612 + its C++-runtime shim), the generated Z80 image units and registry, and ONE hook object. The build-time materialization
// pass links the pass hook, the final program the sound hook (or the viewer hook); everything else is the same objects.

using Clock = std::chrono::steady_clock;

std::uint64_t millis_since(Clock::time_point start) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

struct CompileJob {
  fs::path source;
  fs::path object;
  std::vector<std::string> extra;
  bool cxx = false;
};

// Derives the C++ driver from the C driver when `--cxx` was not given: `<zig> cc ...` -> `<zig> c++ ...`; `cc`, `gcc`, `clang`
// (optionally versioned or `.exe`) -> `c++`, `g++`, `clang++`; any other driver is asked to compile the C++ sources with `-x c++`.
void resolve_cxx(Options &options) {
  if (!options.cxx.empty()) return;
  if (!options.cxx_args_given) options.cxx_args = options.cc_args;
  if (!options.cc_args.empty() && options.cc_args.front() == "cc" && !options.cxx_args_given) {
    options.cxx = options.cc;
    options.cxx_args.front() = "c++";
    return;
  }
  const fs::path cc(options.cc);
  std::string name = cc.filename().string();
  std::string suffix;
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) { suffix = ".exe"; name.resize(name.size() - 4); }
  std::string derived;
  if (name == "cc") derived = "c++";
  else if (name == "gcc") derived = "g++";
  else if (name == "clang") derived = "clang++";
  else if (name.size() > 4 && name.compare(0, 4, "gcc-") == 0) derived = "g++" + name.substr(3);
  else if (name.size() > 6 && name.compare(0, 6, "clang-") == 0) derived = "clang++" + name.substr(5);
  else if (name.size() > 4 && name.compare(name.size() - 4, 4, "-gcc") == 0) derived = name.substr(0, name.size() - 3) + "g++";
  else if (name.size() > 3 && name.compare(name.size() - 3, 3, "-cc") == 0) derived = name.substr(0, name.size() - 2) + "c++";
  if (derived.empty()) {  // an unknown driver name: ask the C driver itself to compile the sources as C++
    options.cxx = options.cc;
    if (!options.cxx_args_given) options.cxx_args.insert(options.cxx_args.end(), {"-x", "c++"});
    return;
  }
  options.cxx = (cc.has_parent_path() ? (cc.parent_path() / (derived + suffix)).string() : derived + suffix);
}

std::optional<std::vector<std::uint8_t>> read_exact(const fs::path &path, std::size_t size) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<std::uint8_t> bytes(size + 1);
  in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (static_cast<std::size_t>(in.gcount()) != size) return std::nullopt;
  bytes.resize(size);
  return bytes;
}

// GCC's -Wmisleading-indentation is super-linear in the size of a translation unit: on a generated ~800 KB Z80 owner unit it
// costs 4-5x the whole -O0 compile (measured under GCC 13). The generated C is machine-formatted, so the warning cannot say
// anything about it; every other -Wall/-Wextra/-pedantic diagnostic stays enabled. Unknown -Wno- options are ignored by GCC/Clang.
inline const char *const kGeneratedUnitFlag = "-Wno-misleading-indentation";

struct GenesisSoundBuild {
  GenesisSoundBuild(const Options &opts, Log &lg) : options(opts), log(lg) {}
  const Options &options;
  Log &log;
  fs::path root, runtime, viewer, psg, ym, object_dir, pass_dir, z80_dir;
  std::vector<std::string> c_base, cxx_base;
  unsigned jobs = 1;
  bool play = false;
  // Aggregates (sanitized): the Z80 part of the build.
  std::uint64_t z80_emit_ms = 0, z80_compile_ms = 0, z80_units = 0, z80_generated_bytes = 0, z80_object_bytes = 0;
  std::uint64_t z80_compiled_units = 0, z80_reused_units = 0;
  std::map<std::string, fs::path> z80_objects;  // unit content hash -> object (reused across iterations)
  std::vector<fs::path> z80_current;            // objects of the most recent preparation
  fs::path pass_executable;
  // The generated M68K units (SEG-028-T005): compiled by content key so an ADR 0049 alias preparation round recompiles only the
  // units its re-emission changed (the same reuse as the Z80 units). The key covers the compile flags, the non-unit files of the
  // shard directory (the shared header and manifest every unit may include) and the unit text.
  std::string runtime_optimize_flag;              // "-O<n>" from --runtime-optimize, or empty
  std::map<std::string, fs::path> m68k_objects;   // content key -> object
  std::vector<fs::path> m68k_pass, m68k_final;    // objects of the current emission: the pass program's and the final program's
  std::uint64_t m68k_compiled_units = 0, m68k_reused_units = 0;
  std::vector<fs::path> pass_runtime;             // pass link set without any generated unit (runtime, devices, pass hook)
  std::string failure_detail;
  std::string failure_stage;       // operation of prepare() that failed: emit, unit-compile, pass-link
  std::string failure_diagnostic;  // bounded tail of the failing compiler/link log

  // The last bounded part of a tool log (diagnostics trail the first error lines; the full text stays in build.log).
  static std::string bounded_log_text(const fs::path &path) {
    constexpr std::size_t kLimit = 600;
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    if (in) text << in.rdbuf();
    std::string out = text.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    if (out.size() > kLimit) out = "..." + out.substr(out.size() - kLimit);
    return out;
  }

  // Runs a batch of compile jobs with the worker pool; logs every command; returns the first failing index or -1.
  int compile(const std::vector<CompileJob> &batch) {
    std::vector<std::vector<std::string>> commands;
    std::vector<fs::path> logs;
    for (const CompileJob &job : batch) {
      auto command = job.cxx ? cxx_base : c_base;
      command.insert(command.end(), job.extra.begin(), job.extra.end());
      command.insert(command.end(), {"-c", "-o", job.object.string(), job.source.string()});
      commands.push_back(std::move(command));
      logs.push_back(job.object.string() + ".log");
    }
    std::vector<int> codes;
    const int failed = run_compiles(commands, logs, jobs, codes);
    for (std::size_t i = 0; i < commands.size(); ++i) {
      std::string joined;
      for (const auto &arg : commands[i]) joined += arg + ' ';
      log.line("$ " + joined);
      log.append_file(logs[i]);
    }
    if (failed >= 0) failure_diagnostic = bounded_log_text(logs[static_cast<std::size_t>(failed)]);
    if (failed >= 0) failure_detail = codes[static_cast<std::size_t>(failed)] < 0 ? "The bundled C compiler could not be started." : "The bundled C compiler reported errors.";
    return failed;
  }

  bool link(const fs::path &executable, const std::vector<fs::path> &objects, bool with_sdl) {
    std::vector<std::string> command{options.cc};
    command.insert(command.end(), options.cc_args.begin(), options.cc_args.end());
    command.insert(command.end(), {"-o", executable.string()});
    for (const auto &object : objects) command.push_back(object.string());
    if (with_sdl) { command.push_back("-L" + options.sdl3_lib->string()); command.push_back("-lSDL3"); }
    command.insert(command.end(), options.link_args.begin(), options.link_args.end());
    const fs::path link_log = object_dir / (executable.filename().string() + ".link.log");
    const int rc = run_process(command, link_log);
    log.line("$ (link)");
    log.line("# link target=" + executable.filename().string());
    log.append_file(link_log);
    if (rc != 0) failure_diagnostic = bounded_log_text(link_log);
    return rc == 0;
  }

  static std::string read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    if (in) text << in.rdbuf();
    return text.str();
  }

  // Plans the compile of the generated M68K units of one emission: fills m68k_pass / m68k_final and returns the jobs of the units
  // whose content key is not compiled yet (`fresh` receives their keys). units[0] is the main TU (the only one whose main() calls
  // genesis_runtime_run); the pass program links it with the sound-hook entry, a viewer build's final program with the viewer one.
  std::vector<CompileJob> plan_m68k(const std::vector<fs::path> &units, const fs::path &shard_dir,
                                    std::vector<std::pair<std::string, fs::path>> &fresh) {
    std::error_code ec;
    std::vector<std::string> shard_include;
    if (units.size() > 1 || fs::is_directory(shard_dir, ec)) shard_include = {"-I", shard_dir.string()};
    std::string context;
    if (fs::is_directory(shard_dir, ec)) {
      std::vector<fs::path> shared;
      for (const auto &entry : fs::directory_iterator(shard_dir, ec))
        if (entry.is_regular_file() && entry.path().extension() != ".c") shared.push_back(entry.path());
      std::sort(shared.begin(), shared.end());
      for (const fs::path &file : shared) context += file.filename().string() + '\0' + read_text(file) + '\0';
    }
    m68k_pass.clear();
    m68k_final.clear();
    std::vector<CompileJob> batch;
    const auto plan = [&](const fs::path &source, std::vector<std::string> extra, bool in_pass, bool in_final) {
      extra.push_back(kGeneratedUnitFlag);
      extra.insert(extra.end(), shard_include.begin(), shard_include.end());
      if (!runtime_optimize_flag.empty()) extra.push_back(runtime_optimize_flag);  // the last -O wins over the base flags
      std::string material;
      for (const auto &arg : extra) material += arg + '\0';
      material += context + '\0' + read_text(source);
      const std::string key = segarecomp::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(material.data()), material.size()));
      const fs::path object = object_dir / ("m68k-" + key.substr(0, 20) + ".o");
      const bool pending = std::any_of(fresh.begin(), fresh.end(), [&](const auto &entry) { return entry.first == key; });
      if (m68k_objects.count(key) != 0 || pending) {
        ++m68k_reused_units;
      } else {
        CompileJob job;
        job.source = source;
        job.object = object;
        job.extra = std::move(extra);
        batch.push_back(std::move(job));
        fresh.emplace_back(key, object);
        ++m68k_compiled_units;
      }
      if (in_pass) m68k_pass.push_back(object);
      if (in_final) m68k_final.push_back(object);
    };
    plan(units[0], {"-Dgenesis_runtime_run=genesis_sound_hook_run"}, true, !play);
    if (play) plan(units[0], {"-Dgenesis_runtime_run=genesis_viewer_hook_run"}, false, true);
    for (std::size_t i = 1; i < units.size(); ++i) plan(units[i], {"-I", units[0].parent_path().string()}, true, true);
    return batch;
  }

  // Removes the M68K objects of superseded emissions (kept-work object directories hold only the final program's objects).
  void prune_m68k() {
    std::error_code ec;
    for (auto it = m68k_objects.begin(); it != m68k_objects.end();) {
      const bool used = std::find(m68k_pass.begin(), m68k_pass.end(), it->second) != m68k_pass.end() ||
                        std::find(m68k_final.begin(), m68k_final.end(), it->second) != m68k_final.end();
      if (used) { ++it; continue; }
      fs::remove(it->second, ec);
      fs::remove(it->second.string() + ".log", ec);
      it = m68k_objects.erase(it);
    }
  }

  // Emits the registry's Z80 C into a fresh directory, compiles only the units not compiled before, relinks the pass program.
  bool prepare(const gz80::Registry &registry) {
    std::error_code ec;
    fs::remove_all(z80_dir, ec);
    gz80::EmitRequest request;
    request.directory = z80_dir;
    const auto emit_started = Clock::now();
    const gz80::EmitOutcome emitted = gz80::emit_registry(registry, request);
    z80_emit_ms += millis_since(emit_started);
    if (!emitted.ok()) { log.line("z80 emit diagnostic: " + emitted.error); failure_detail = "The Z80 image code could not be generated."; failure_stage = "emit"; failure_diagnostic = emitted.error; return false; }
    std::vector<fs::path> sources;
    {
      std::ifstream list(z80_dir / "genesis_z80.units");
      for (std::string line; std::getline(list, line);)
        if (!line.empty()) sources.push_back(z80_dir / line);
    }
    z80_current.clear();
    z80_units = sources.size();
    z80_generated_bytes = 0;
    std::vector<CompileJob> batch;
    std::vector<std::pair<std::string, fs::path>> fresh;
    for (const fs::path &source : sources) {
      std::ifstream in(source, std::ios::binary);
      std::ostringstream text;
      text << in.rdbuf();
      const std::string bytes = text.str();
      z80_generated_bytes += bytes.size();
      const std::string key = segarecomp::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
      const auto known = z80_objects.find(key);
      if (known != z80_objects.end()) { z80_current.push_back(known->second); ++z80_reused_units; continue; }
      const fs::path object = object_dir / ("z80-" + key.substr(0, 20) + ".o");
      CompileJob job;
      job.source = source;
      job.object = object;
      job.extra = {"-I", z80_dir.string(), kGeneratedUnitFlag};
      batch.push_back(job);
      fresh.emplace_back(key, object);
      z80_current.push_back(object);
    }
    const auto compile_started = Clock::now();
    if (!batch.empty() && compile(batch) >= 0) { failure_stage = "unit-compile"; return false; }
    z80_compile_ms += millis_since(compile_started);
    z80_compiled_units += batch.size();
    for (auto &entry : fresh) z80_objects.emplace(entry.first, entry.second);
    z80_object_bytes = 0;
    for (const fs::path &object : z80_current) z80_object_bytes += fs::file_size(object, ec);
    std::vector<fs::path> objects = m68k_pass;
    objects.insert(objects.end(), pass_runtime.begin(), pass_runtime.end());
    objects.insert(objects.end(), z80_current.begin(), z80_current.end());
    if (!link(pass_executable, objects, false)) { failure_detail = "The native program could not be linked."; failure_stage = "pass-link"; return false; }
    return true;
  }
};

class ProcessPassRunner final : public gz80::PassRunner {
 public:
  explicit ProcessPassRunner(GenesisSoundBuild &build) : build_(build) {}
  bool prepare(const gz80::Registry &registry) override {
    build_.failure_stage.clear();
    build_.failure_diagnostic.clear();
    return build_.prepare(registry);
  }
  gz80::PrepareFailure prepare_failure() const override { return {build_.failure_stage, build_.failure_diagnostic}; }

  gz80::PassObservation run() override {
    gz80::PassObservation observed;
    std::error_code ec;
    for (const char *name : {"pass.report", "unknown.ram", "unknown.written", "stop.ram"}) fs::remove(build_.pass_dir / name, ec);
    const Environment environment{{"SEGARECOMP_MATERIALIZE_DIR", build_.pass_dir.string()},
                                  {"SEGARECOMP_MATERIALIZE_FRAMES", std::to_string(gz80::kObservationFrames)}};
    const fs::path pass_log = build_.pass_dir / "pass.log";
    const int rc = run_process({build_.pass_executable.string(), "--instruction-budget", std::to_string(gz80::kPassInstructionBudget)},
                               pass_log, environment, gz80::kPassWallSeconds);
    if (rc == kProcessTimedOut) { observed.outcome = gz80::PassOutcome::wall_timeout; return observed; }
    std::ifstream report(build_.pass_dir / "pass.report");
    if (!report) return observed;  // failed: no report
    std::string outcome;
    bool well_formed = true;
    for (std::string line; std::getline(report, line);) {
      std::istringstream in(line);
      std::string key;
      in >> key;
      if (key == "outcome") in >> outcome;
      else if (key == "frames") in >> observed.frames;
      else if (key == "sound_fault") {
        std::string kind;
        gz80::SoundFault fault;
        in >> kind >> fault.epochs >> fault.master_ticks;
        if (!in || kind != "z80_code_mismatch" || observed.sound_fault) { well_formed = false; break; }
        observed.sound_fault = fault;
      }
      else if (key == "epoch") {
        unsigned ordinal = 0;
        std::string hex;
        gz80::PassEpoch epoch;
        in >> ordinal >> hex >> epoch.extents;
        if (!in || hex.size() != 64 || ordinal != observed.epochs.size() + 1) { well_formed = false; break; }
        for (std::size_t i = 0; i < 32; ++i) epoch.signature[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        observed.epochs.push_back(epoch);
      }
    }
    if (!well_formed) { observed.epochs.clear(); return observed; }
    static const std::pair<const char *, gz80::PassOutcome> kNames[] = {
        {"window_complete", gz80::PassOutcome::window_complete}, {"guest_stop", gz80::PassOutcome::guest_stop},
        {"guest_complete", gz80::PassOutcome::guest_complete}, {"unknown_image", gz80::PassOutcome::unknown_image},
        {"budget_exhausted", gz80::PassOutcome::budget_exhausted}, {"z80_code_mismatch", gz80::PassOutcome::z80_code_mismatch},
        {"z80_stop", gz80::PassOutcome::z80_stop}};
    for (const auto &entry : kNames)
      if (outcome == entry.first) observed.outcome = entry.second;
    if (observed.outcome == gz80::PassOutcome::unknown_image) {
      const auto ram = read_exact(build_.pass_dir / "unknown.ram", gz80::kRamBytes);
      const auto written = read_exact(build_.pass_dir / "unknown.written", gz80::kBitmapBytes);
      if (!ram || !written) { observed.outcome = gz80::PassOutcome::failed; return observed; }
      gz80::Epoch snapshot;
      std::copy(ram->begin(), ram->end(), snapshot.ram.begin());
      std::copy(written->begin(), written->end(), snapshot.written.begin());
      observed.unknown = snapshot;
    }
    if (observed.outcome == gz80::PassOutcome::guest_stop) {
      // SEG-028-T005: the private M68K stop record (absent or malformed: the alias preparation reports tool_failure).
      if (const auto record = read_exact(build_.pass_dir / "stop.ram", m68k_alias::kStopRecordBytes))
        observed.guest_stop = m68k_alias::parse_stop_record(*record);
      fs::remove(build_.pass_dir / "stop.ram", ec);
    }
    return observed;
  }

 private:
  GenesisSoundBuild &build_;
};

// The Genesis M68K side of one build: the image and its reset analysis, and where the generated M68K C is emitted.
struct GenesisM68kRoute {
  const std::vector<std::uint8_t> *rom = nullptr;
  const segarecomp::GenesisResetImageReport *reset = nullptr;
  fs::path source;     // generated.c (single-file emission)
  fs::path shard_dir;  // generated/ (sharded emission)
};

// SEG-031 (ADR 0080): a hybrid plan applies to an emission only when it names exactly that emission's ADR 0049 alias set.
bool admission_applies(const segarecomp::GenesisHybridAdmissionPlan &plan, const std::vector<m68k_alias::CopyAlias> &aliases) {
  if (plan.aliases.size() != aliases.size()) return false;
  for (std::size_t i = 0; i < aliases.size(); ++i)
    if (plan.aliases[i].execution_base != aliases[i].execution || plan.aliases[i].source_base != aliases[i].source ||
        plan.aliases[i].length != aliases[i].length)
      return false;
  return true;
}

// SEG-047 (ADR 0096): the machine-readable `aot_policy` status member: requested policy, effective producer, fallback and a stable
// sanitized reason, plus (when the native producer ran) the model identity and aggregate candidate/admitted metrics. No address, byte or
// per-window datum. Always present, so a consumer never parses text.
std::string aot_policy_json(const Options &options, const Log &log, const std::string &rom_sha256, bool genesis) {
  std::map<std::string, std::string> kv;
  {
    std::istringstream tokens(log.admission_line);
    for (std::string token; tokens >> token;) {
      const auto eq = token.find('=');
      if (eq != std::string::npos) kv[token.substr(0, eq)] = token.substr(eq + 1);
    }
  }
  std::string effective = "broad", reason = "none";
  bool fallback = false;
  if (options.aot_policy == "optimized") {
    if (!genesis) { fallback = true; reason = "platform_not_applicable"; }
    else if (options.admission) {
      effective = log.plan_applied ? "admission_plan" : "broad";
      reason = log.plan_applied ? "exact_plan_precedence" : "plan_alias_set_mismatch";
      fallback = !log.plan_applied;
    }
    else if (kv.count("producer") == 0U) { fallback = true; reason = "no_report"; }
    else { effective = kv["producer"]; fallback = kv["fallback"] == "1"; reason = kv["reason"]; }
  }
  const auto safe = [](const std::string &value) {
    std::string out;
    for (const char c : value) out.push_back((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '_');
    return out;
  };
  std::string json = ",\"aot_policy\":{\"requested\":\"" + options.aot_policy + "\",\"effective\":\"" + safe(effective) +
                     "\",\"fallback\":" + (fallback ? "true" : "false") + ",\"reason\":\"" + safe(reason) + "\"";
  if (effective == "ml_region") {
    const auto number = [&kv, &safe](const char *key) { return kv.count(key) != 0U ? safe(kv[key]) : std::string("0"); };
    json += ",\"model\":\"" + safe(kv["model"]) + "\",\"schema_sha256\":\"" + safe(kv["schema"]) + "\"";
    for (const char *key : {"windows", "ml_selected", "seed_windows", "universe", "k0", "k", "pruned", "rounds"})
      json += std::string(",\"") + key + "\":" + number(key);
    json += ",\"k_sha256\":\"" + safe(kv["k_sha256"]) + "\",\"validator\":\"" + safe(kv["validator"]) + "\"";
  }
  // Stable cache identity: a pure function of the image, the requested policy, the effective producer and (for the native producer)
  // the frozen model/schema and the admitted-set digest. Compatibility and Optimized builds therefore never share an identity, and a
  // fallback build never aliases an ML build. Sanitized: a digest only.
  const std::string identity_text = "segarecomp.aot_policy.v1|rom=" + rom_sha256 + "|requested=" + options.aot_policy + "|effective=" + safe(effective) +
                                    "|model=" + (effective == "ml_region" ? safe(kv["model"]) : std::string("-")) + "|schema=" +
                                    (effective == "ml_region" ? safe(kv["schema"]) : std::string("-")) + "|k=" +
                                    (effective == "ml_region" ? safe(kv["k_sha256"]) : std::string("-"));
  json += ",\"identity\":\"" + segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(identity_text.data()), identity_text.size()}) + "\"";
  return json + "}";
}

// The existing M68K emit route, in-process (`emit-general-startup-bridge-c --reset-entry --immutable-rom-aot`), plus one
// `--immutable-copy-alias` per ADR 0049 descriptor. Replaces the previous emission; fills `units` from the emitted manifest. False:
// the emitter rejected the request.
bool emit_genesis_m68k(const Options &options, Log &log, const std::string &sha, const GenesisM68kRoute &route,
                       const std::vector<m68k_alias::CopyAlias> &aliases, std::vector<fs::path> &units) {
  std::error_code ec;
  fs::remove(route.source, ec);
  fs::remove_all(route.shard_dir, ec);
  units.clear();
  const fs::path hints = options.runtime_dir / "compat" / (sha + ".json");
  std::vector<std::string> emit_args{"segarecomp", "emit-general-startup-bridge-c", "--rom", options.rom.string(),
                                     "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"};
  for (const m68k_alias::CopyAlias &alias : aliases) {
    char text[32];
    std::snprintf(text, sizeof(text), "%08x:%08x:%08x", static_cast<unsigned>(alias.execution), static_cast<unsigned>(alias.source),
                  static_cast<unsigned>(alias.length));
    emit_args.insert(emit_args.end(), {"--immutable-copy-alias", text});
  }
  if (fs::is_regular_file(hints, ec)) { emit_args.push_back("--external-hints"); emit_args.push_back(hints.string()); }
  log.plan_applied = false;
  if (options.admission) {
    if (admission_applies(*options.admission, aliases)) {
      log.plan_applied = true;
      emit_args.insert(emit_args.end(), {"--immutable-rom-aot-admission", options.admission_plan->string()});
    } else {
      log.line("m68k admission: broad for this emission (the plan's alias set differs)");
    }
  }
  if (options.aot_policy == "optimized" && !options.admission) emit_args.push_back("--immutable-rom-aot-ml-admission");
  log.admission_line.clear();
  emit_args.insert(emit_args.end(), {"--generated-c-output", route.source.string(), "--generated-c-shard-dir", route.shard_dir.string()});
  std::vector<char *> emit_argv;
  for (auto &arg : emit_args) emit_argv.push_back(arg.data());
  std::ostringstream captured_out, captured_err;
  auto *old_out = std::cout.rdbuf(captured_out.rdbuf());
  auto *old_err = std::cerr.rdbuf(captured_err.rdbuf());
  const int emit_rc = run_cli(static_cast<int>(emit_argv.size()), emit_argv.data());
  std::cout.rdbuf(old_out);
  std::cerr.rdbuf(old_err);
  log.line(captured_out.str().substr(0, 4096));
  log.line(captured_err.str());
  {
    std::istringstream lines(captured_err.str());
    for (std::string line; std::getline(lines, line);)
      if (line.rfind("segarecomp: m68k admission: requested=optimized", 0) == 0) log.admission_line = line;
  }
  if (emit_rc != 0) {
    fs::remove(route.source, ec);
    fs::remove_all(route.shard_dir, ec);
    return false;
  }
  const fs::path manifest = route.shard_dir / "bridge_generated.units";
  if (fs::is_regular_file(manifest, ec)) {
    std::ifstream in(manifest);
    for (std::string line; std::getline(in, line);)
      if (!line.empty()) units.push_back(route.shard_dir / line);
  } else if (fs::is_regular_file(route.source, ec)) {
    units.push_back(route.source);
  }
  return true;
}

bool units_complete(const std::vector<fs::path> &units) {
  return !units.empty() && std::all_of(units.begin(), units.end(), [](const fs::path &p) { return fs::is_regular_file(p); });
}

// Sanitized provenance JSON of the M68K executable images of the program built with `aliases` (counts only). nullopt: the program
// or one descriptor is not accepted.
std::optional<std::string> genesis_m68k_images_json(const GenesisM68kRoute &route, const std::vector<m68k_alias::CopyAlias> &aliases) {
  auto program = segarecomp::make_genesis_reset_bridge_startup_program(*route.rom, *route.reset, std::nullopt);
  if (!program || !segarecomp::apply_genesis_immutable_rom_aot(*program)) return std::nullopt;
  for (const m68k_alias::CopyAlias &alias : aliases)
    if (!segarecomp::apply_genesis_immutable_copy_alias(*program, alias.execution, alias.source, alias.length)) return std::nullopt;
  const auto images = segarecomp::genesis_m68k_executable_images(*program);
  if (!images) return std::nullopt;
  return segarecomp::format_image_provenance_json(segarecomp::count_image_provenance(images->set));
}

std::string alias_preparation_json(const m68k_alias::Summary &prep) {
  return "{\"rounds\":" + std::to_string(prep.rounds) + ",\"aliases\":" + std::to_string(prep.aliases.size()) + ",\"alias_bytes\":" +
         std::to_string(prep.alias_bytes()) + ",\"termination\":\"" + m68k_alias::termination_name(prep.termination) + "\"}";
}

// Builds the Genesis program. Returns 0 and fills `executable`, or the process exit code after recording the failure.
// `units` is the generate stage's (alias-less) M68K emission; `m68k_images` the sanitized provenance JSON of its executable images.
//
// SEG-028-T005 (ADR 0077, ADR 0049, ADR 0076): after the Z80 fixed point converges, the build-time ADR 0049 alias preparation runs.
// When the confirmed run ended with an M68K guest stop that a verbatim cartridge copy in work RAM explains, the M68K C is re-emitted
// with the merged immutable-copy aliases, only the changed M68K units are recompiled, the pass program is relinked and the fixed
// point runs again (the Z80 registry is kept: its images are activation-keyed facts already proven by an observed run, and the new
// fixed point re-confirms the program it builds), at most m68k_alias::kMaxRounds times. The final program links the last round's
// M68K units; the last fixed point is its confirming run. Without such a stop no extra round runs and nothing changes.
int build_genesis_program(Options &options, Log &log, const std::string &sha, const GenesisM68kRoute &route, std::vector<fs::path> units,
                          std::string m68k_images, fs::path &executable, std::string &status_extra, bool &sound_degraded) {
  std::error_code ec;
  fs::path platform_dir = options.runtime_dir;
  if (!platform_dir.has_filename()) platform_dir = platform_dir.parent_path();  // tolerate a trailing separator
  GenesisSoundBuild build(options, log);
  build.root = platform_dir.parent_path().parent_path();
  build.runtime = options.runtime_dir / "runtime";
  build.viewer = options.runtime_dir / "viewer";
  build.psg = build.root / "libs" / "device" / "sega" / "psg";
  build.ym = build.root / "libs" / "device" / "sega" / "ym2612";
  build.play = options.sdl3_include.has_value();
  build.object_dir = options.output / "obj";
  build.pass_dir = build.object_dir / "pass";
  build.z80_dir = options.output / "generated-z80";
  fs::remove_all(build.object_dir, ec);
  fs::create_directories(build.pass_dir, ec);
  build.jobs = options.jobs != 0 ? options.jobs : std::min(8U, std::max(1U, std::thread::hardware_concurrency()));
  log.line("# compile jobs: " + std::to_string(build.jobs));
  resolve_cxx(options);
  if (!options.runtime_optimize.empty()) build.runtime_optimize_flag = "-O" + options.runtime_optimize;

  const std::string opt = "-O" + options.optimize;
  build.c_base = {options.cc};
  build.c_base.insert(build.c_base.end(), options.cc_args.begin(), options.cc_args.end());
  build.c_base.insert(build.c_base.end(), {"-std=c11", "-Wall", "-Wextra", "-pedantic", opt, "-I", build.runtime.string(),
                                           "-I", (build.root / "libs" / "codegen" / "c11" / "include").string(),
                                           "-I", (build.psg / "include").string(), "-I", (build.ym / "include").string()});
  if (build.play) build.c_base.insert(build.c_base.end(), {"-I", build.viewer.string(), "-I", options.sdl3_include->string()});
  build.cxx_base = {options.cxx};
  build.cxx_base.insert(build.cxx_base.end(), options.cxx_args.begin(), options.cxx_args.end());
  build.cxx_base.insert(build.cxx_base.end(), {"-std=c++14", "-fno-exceptions", "-fno-rtti", opt, "-I", (build.ym / "include").string()});

  // ---- the stable compile set (compiled once) and the generated M68K units of the generate stage's emission ----
  const fs::path vendor = build.ym / "third_party" / "ymfm";
  std::vector<CompileJob> stable;
  std::vector<fs::path> common, final_only, pass_only;
  const auto add = [&](std::vector<fs::path> &set, const fs::path &source, const std::string &name, std::vector<std::string> extra = {},
                       bool cxx = false) {
    CompileJob job;
    job.source = source;
    job.object = build.object_dir / (name + ".o");
    job.extra = std::move(extra);
    if (!build.runtime_optimize_flag.empty()) job.extra.push_back(build.runtime_optimize_flag);  // the last -O wins over the base flags
    job.cxx = cxx;
    stable.push_back(job);
    set.push_back(job.object);
  };
  for (const char *name : {"runtime.c", "z80_machine.c", "genesis_audio.c", "genesis_mixer.c", "genesis_audio_present.c", "genesis_sound.c"})
    add(common, build.runtime / name, std::string("rt-") + std::string(name).substr(0, std::string(name).size() - 2));
  add(common, build.psg / "src" / "sn76489.c", "psg");
  add(common, build.ym / "src" / "cxx_runtime_shim.c", "ym-shim");
  for (const char *name : {"ymfm_opn.cpp", "ymfm_adpcm.cpp", "ymfm_ssg.cpp"})
    add(common, vendor / name, std::string("ymfm-") + std::string(name).substr(5, std::string(name).size() - 9),
        {"-w", "-I", vendor.string()}, true);
  add(common, build.ym / "src" / "ym2612.cpp", "ym2612", {"-isystem", vendor.string()}, true);
  add(pass_only, build.runtime / "genesis_materialize_hook.c", "hook-materialize");
  if (build.play) {
    add(final_only, build.runtime / "vdp_render.c", "vdp-render");
    for (const char *name : {"viewer.c", "viewer_sdl3.c"}) add(final_only, build.viewer / name, std::string("viewer-") + name);
    add(final_only, build.viewer / "viewer_main_hook.c", "viewer-main-hook", {"-DSEGARECOMP_GENESIS_SOUND"});
  } else {
    add(final_only, build.runtime / "genesis_sound_hook.c", "hook-sound");
  }
  for (const auto &job : stable)
    if (!fs::is_regular_file(job.source, ec))
      return fail(options, log, sha, "compile", 2, "The Segarecomp runtime files are missing: " + job.source.string());
  std::vector<std::pair<std::string, fs::path>> fresh;
  std::vector<CompileJob> first = build.plan_m68k(units, route.shard_dir, fresh);
  first.insert(first.end(), stable.begin(), stable.end());  // one batch: the generated units first, as before
  if (build.compile(first) >= 0) return fail(options, log, sha, "compile", 3, build.failure_detail);
  for (auto &entry : fresh) build.m68k_objects.emplace(entry.first, entry.second);

  // ---- build-time Z80 image materialization (fixed point) ----
  stage("z80-materialize", "begin");
  build.pass_runtime = common;
  build.pass_runtime.insert(build.pass_runtime.end(), pass_only.begin(), pass_only.end());
  build.pass_executable = build.object_dir / "materialize-pass";
  gz80::Registry registry;
  ProcessPassRunner runner(build);
  std::uint64_t materialize_ms = 0;
  const auto fixed_point = [&] {
    const auto started = Clock::now();
    gz80::MaterializationSummary result = gz80::materialize(registry, runner);
    const std::uint64_t ms = millis_since(started);
    materialize_ms += ms;
    log.line("z80 materialization: outcome=" + std::string(gz80::failure_name(result.failure)) + " images=" +
             std::to_string(registry.images().size()) + " discovery_runs=" + std::to_string(result.discovery_runs) + " runs=" +
             std::to_string(result.total_runs) + " ms=" + std::to_string(ms) +
             (result.ok() ? " end=" + std::string(gz80::pass_outcome_name(result.end)) : std::string()));
    return result;
  };
  // A failed fixed point (of the generate stage's program or of an alias preparation round): typed z80-materialize failure.
  const auto materialize_failed = [&](const gz80::MaterializationSummary &failed, const std::string &prep_extra) {
    const std::string extra = ",\"z80\":{\"outcome\":\"" + std::string(gz80::failure_name(failed.failure)) + "\",\"images\":" +
                              std::to_string(registry.images().size()) + ",\"discovery_runs\":" + std::to_string(failed.discovery_runs) +
                              ",\"runs\":" + std::to_string(failed.total_runs) + "}" + prep_extra;
    const bool compile_failed = failed.failure == gz80::Failure::z80_image_compile_failed;
    const std::string operation = compile_failed && !failed.prepare_failure.stage.empty() ? " stage=" + failed.prepare_failure.stage : "";
    log.line("FAILED (z80-materialize): " + std::string(gz80::failure_name(failed.failure)) + operation);
    if (compile_failed) log.line("z80 prepare failure: stage=" + failed.prepare_failure.stage + " detail=" + failed.prepare_failure.detail);
    write_status(options, sha, "failed", "z80-materialize",
                 compile_failed ? build.failure_detail + (operation.empty() ? "" : " (" + failed.prepare_failure.stage + ")") : "The Z80 sound program could not be materialized.",
                 extra + ",\"diagnostic\":\"" + gz80::failure_name(failed.failure) + "\"");
    std::cout << "@result failed stage=z80-materialize message=" << gz80::failure_name(failed.failure) << operation << std::endl;
    return 4;
  };
  gz80::MaterializationSummary summary = fixed_point();
  if (!summary.ok()) return materialize_failed(summary, "");
  stage("z80-materialize", "done");

  // ---- build-time ADR 0049 alias preparation (SEG-028-T005) ----
  stage("m68k-alias-prepare", "begin");
  const auto observe = [](const gz80::MaterializationSummary &converged) {
    m68k_alias::RoundObservation observed;
    observed.guest_stop = converged.end == gz80::PassOutcome::guest_stop;
    observed.record = converged.guest_stop;
    return observed;
  };
  class Rounds final : public m68k_alias::RoundRunner {
   public:
    std::function<m68k_alias::RoundObservation(const std::vector<m68k_alias::CopyAlias> &, bool &)> body;
    m68k_alias::RoundObservation run_round(const std::vector<m68k_alias::CopyAlias> &aliases, bool &abort) override { return body(aliases, abort); }
  } rounds;
  rounds.body = [&](const std::vector<m68k_alias::CopyAlias> &aliases, bool &abort) {
    m68k_alias::RoundObservation observed;
    log.line("m68k alias preparation round: aliases=" + std::to_string(aliases.size()));
    if (!emit_genesis_m68k(options, log, sha, route, aliases, units) || !units_complete(units)) { observed.tool_failure = true; return observed; }
    std::vector<std::pair<std::string, fs::path>> changed;
    const std::vector<CompileJob> batch = build.plan_m68k(units, route.shard_dir, changed);
    if (!batch.empty() && build.compile(batch) >= 0) { observed.tool_failure = true; return observed; }
    for (auto &entry : changed) build.m68k_objects.emplace(entry.first, entry.second);
    gz80::MaterializationSummary next = fixed_point();
    if (!next.ok()) { summary = std::move(next); abort = true; return observed; }
    summary = std::move(next);
    return observe(summary);
  };
  const m68k_alias::Summary prep = m68k_alias::prepare(*route.rom, observe(summary), rounds);
  const std::string prep_json = alias_preparation_json(prep);
  const std::string prep_extra = ",\"m68k_alias_preparation\":" + prep_json;
  log.line("m68k alias preparation: rounds=" + std::to_string(prep.rounds) + " aliases=" + std::to_string(prep.aliases.size()) +
           " alias_bytes=" + std::to_string(prep.alias_bytes()) + " termination=" + m68k_alias::termination_name(prep.termination) +
           (prep.aborted ? " (round fixed point failed)" : ""));
  if (prep.aborted) return materialize_failed(summary, prep_extra);
  std::optional<std::string> alias_images;
  if (!m68k_alias::incomplete(prep.termination) && !prep.aliases.empty()) {
    alias_images = genesis_m68k_images_json(route, prep.aliases);
    if (!alias_images) log.line("m68k alias preparation: the accepted aliases do not form a valid executable-image set");
  }
  if (m68k_alias::incomplete(prep.termination) || (!prep.aliases.empty() && !alias_images)) {
    log.line("FAILED (m68k-alias-prepare): alias_preparation_incomplete");
    write_status(options, sha, "failed", "m68k-alias-prepare", "The work-RAM code of this game could not be prepared.",
                 prep_extra + ",\"diagnostic\":\"alias_preparation_incomplete\"");
    std::cout << "@result failed stage=m68k-alias-prepare message=alias_preparation_incomplete" << std::endl;
    return 5;
  }
  if (alias_images) m68k_images = *alias_images;
  build.prune_m68k();
  stage("m68k-alias-prepare", "done");

  // ---- link the final program: the same generated units and registry, the production hook ----
  std::vector<fs::path> objects = build.m68k_final;
  objects.insert(objects.end(), common.begin(), common.end());
  objects.insert(objects.end(), final_only.begin(), final_only.end());
  objects.insert(objects.end(), build.z80_current.begin(), build.z80_current.end());
  stage("compile", "done");
  stage("link", "begin");
#if defined(_WIN32)
  executable = options.output / "game.exe";
#else
  executable = options.output / "game";
#endif
  if (!build.link(executable, objects, build.play)) return fail(options, log, sha, "link", 3, "The native program could not be linked.");
  const std::uint64_t exe_bytes = fs::file_size(executable, ec);
  // Machine-readable audio capability: a structural Z80 code mutation isolates the sound CPU (contract section 18) and the build
  // still succeeds; every other Z80 failure already failed the build above.
  status_extra = std::string(",\"genesis_audio\":\"") + (summary.sound_fault ? "degraded" : "supported") + "\"";
  if (summary.sound_fault) status_extra += ",\"z80_audio_outcome\":\"structural_code_mismatch\"";
  status_extra += ",\"z80\":{\"outcome\":\"converged\",\"end\":\"" + std::string(gz80::pass_outcome_name(summary.end)) +
                 "\",\"images\":" + std::to_string(summary.images) + ",\"discovery_runs\":" + std::to_string(summary.discovery_runs) +
                 ",\"runs\":" + std::to_string(summary.total_runs) + ",\"epochs\":" + std::to_string(summary.epochs) +
                 ",\"window_frames\":" + std::to_string(gz80::kObservationFrames) + ",\"frames_reached\":" + std::to_string(summary.frames) +
                 ",\"units\":" + std::to_string(build.z80_units) + ",\"units_compiled\":" + std::to_string(build.z80_compiled_units) +
                 ",\"units_reused\":" + std::to_string(build.z80_reused_units) + ",\"generated_bytes\":" + std::to_string(build.z80_generated_bytes) +
                 ",\"object_bytes\":" + std::to_string(build.z80_object_bytes) + ",\"emit_ms\":" + std::to_string(build.z80_emit_ms) +
                 ",\"compile_ms\":" + std::to_string(build.z80_compile_ms) + ",\"materialize_ms\":" + std::to_string(materialize_ms) +
                 ",\"executable_bytes\":" + std::to_string(exe_bytes) +
                 (summary.sound_fault ? ",\"sound_fault_epochs\":" + std::to_string(summary.sound_fault->epochs) +
                                            ",\"sound_fault_frame\":" + std::to_string(summary.sound_fault->master_ticks / (3420U * 262U /* NTSC master ticks per frame */))
                                      : std::string()) + "}";
  status_extra += prep_extra;  // SEG-028-T005: sanitized aggregates of the alias preparation (counts and the termination only)
  if (options.admission) {
    // SEG-031 (ADR 0080): which admission the final M68K emission used (sanitized: the strategy only).
    const bool hybrid = options.admission->strategy == segarecomp::GenesisAdmissionStrategy::hybrid &&
                        admission_applies(*options.admission, prep.aliases);
    status_extra += std::string(",\"m68k_admission\":\"") + (hybrid ? "hybrid" : "broad") + "\"";
    log.line(std::string("m68k admission: final=") + (hybrid ? "hybrid" : "broad"));
  }
  status_extra += aot_policy_json(options, log, sha, true);
  // SEG-028 (ADR 0077): the producer boundary, recorded as sanitized provenance counts only (no address, byte or hash).
  const std::string images = "{\"m68k\":" + m68k_images + ",\"z80\":" +
                             segarecomp::format_image_provenance_json(segarecomp::count_image_provenance(gz80::executable_images(registry))) + "}";
  log.line("executable_images: " + images);
  status_extra += ",\"executable_images\":" + images;
  sound_degraded = summary.sound_fault.has_value();
  return 0;
}

} // namespace

int segarecomp_build_command(int argc, char **argv) {
  const auto parsed = parse_options(argc, argv);
  if (!parsed) { std::cerr << "usage:\n" << segarecomp_build_usage; return 2; }
  Options options = *parsed;
  std::error_code ec;
  fs::create_directories(options.output, ec);
  if (ec) { std::cerr << "segarecomp: cannot create output directory\n"; return 2; }
  Log log;
  log.file.open(options.output / "build.log", std::ios::binary | std::ios::trunc);
  if (options.admission_plan) {
    std::ifstream in(*options.admission_plan, std::ios::binary);
    std::string text;
    if (in) {
      text.resize(segarecomp::genesis_hybrid_admission_plan_max_bytes + 1U);
      in.read(text.data(), static_cast<std::streamsize>(text.size()));
      text.resize(static_cast<std::size_t>(in.gcount()));
    }
    std::string error = "plan_unreadable";
    if (in || in.eof()) options.admission = segarecomp::parse_genesis_hybrid_admission_plan(text, &error);
    if (!options.admission) {
      std::cerr << "segarecomp: hybrid admission plan rejected: " << error << '\n';
      return 2;
    }
  }
  std::string sha;
  std::string sms_provenance;  // status.json members recording the SMS identity (empty for Genesis)
  std::string sms_images;      // SMS: the status.json executable_images member
  std::string m68k_images;     // Genesis: sanitized provenance JSON of the M68K executable images
  std::optional<segarecomp::GenesisResetImageReport> genesis_reset;  // Genesis: the accepted reset analysis
  try {
    // ---- analyze ----
    stage("analyze", "begin");
    const auto bytes = segarecomp::read_binary(options.rom);
    sha = segarecomp::sha256_hex(bytes);
    log.line("rom_sha256=" + sha);
    log.line("cc=" + options.cc);
    // Platform routing: an explicit --platform wins; otherwise only a recognized Master System header selects the SMS
    // route. Header absence or ambiguity never selects it implicitly (an unclassified image keeps the Genesis route and
    // its existing rejection), and a Game Gear header is rejected rather than guessed.
    Target target = Target::genesis;
    if (options.platform == "master-system") {
      target = Target::master_system;
    } else if (options.platform.empty()) {
      const auto info = segarecomp::inspect_rom(bytes);
      if (info.outcome == segarecomp::ClassificationOutcome::recognized && info.platform == segarecomp::Platform::master_system)
        target = Target::master_system;
      else if (info.outcome == segarecomp::ClassificationOutcome::recognized && info.platform == segarecomp::Platform::game_gear)
        return fail(options, log, sha, "analyze", 1, "Game Gear images are not supported.", "PLATFORM_UNSUPPORTED");
    }
    sms::IngestOptions ingest_options;
    if (target == Target::master_system && options.admission)
      return fail(options, log, sha, "analyze", 1, "A hybrid admission plan applies only to Genesis images.", "ADMISSION_NOT_APPLICABLE");
    if (target == Target::master_system) {
      log.line("platform=master-system");
      ingest_options.explicit_profile = options.platform == "master-system";
      if (!options.mapper.empty()) ingest_options.declarations.push_back({options.mapper, sms::DeclarationSource::build_option});
      if (options.mapper_manifest) {
        std::ifstream manifest(*options.mapper_manifest, std::ios::binary);
        std::ostringstream text;
        if (manifest) text << manifest.rdbuf();
        sms::ManifestResult parsed;
        if (manifest) parsed = sms::parse_mapper_manifest(text.str(), sha);
        else { parsed.error = SMS_ERROR_MAPPER_UNDECLARED; parsed.detail = "cannot read the mapper manifest"; }
        if (parsed.error != SMS_OK) {
          log.line("analyze diagnostic: " + parsed.detail);
          return fail(options, log, sha, "analyze", 1, "The Master System mapper manifest is not usable: " + parsed.detail,
                      sms_error_name(parsed.error));
        }
        ingest_options.declarations.push_back(parsed.declaration);
      }
      const sms::IngestResult cartridge = sms::ingest_cartridge(bytes, ingest_options);
      if (!cartridge.ok()) {
        log.line("analyze diagnostic: " + cartridge.detail);
        const std::string message = cartridge.error == SMS_ERROR_MAPPER_UNDECLARED
            ? "The Master System cartridge mapper must be declared (--mapper sega|rom_only or --mapper-manifest); Segarecomp never guesses it: " + cartridge.detail
            : "This Master System image is not supported: " + cartridge.detail;
        return fail(options, log, sha, "analyze", 1, message, sms_error_name(cartridge.error));
      }
      const auto &id = cartridge.identity;
      log.line("profile=" + id.profile);
      log.line(std::string("mapper=") + sms::mapper_family_name(id.mapper));
      log.line(std::string("mapper_declaration_source=") + sms::declaration_source_name(id.declaration_source));
      sms_provenance = ",\"platform\":\"master-system\",\"profile\":\"" + json_escape(id.profile) + "\",\"mapper\":\"" +
                       sms::mapper_family_name(id.mapper) + "\",\"mapper_source\":\"" +
                       sms::declaration_source_name(id.declaration_source) + "\",\"toolchain\":\"" + json_escape(options.cc) + "\"";
    } else {
      if (!options.mapper.empty() || options.mapper_manifest)
        return fail(options, log, sha, "analyze", 1, "A mapper declaration applies only to Master System images.", "MAPPER_NOT_APPLICABLE");
      genesis_reset = segarecomp::analyze_genesis_reset_image(bytes);
      const auto &reset = *genesis_reset;
      if (reset.outcome != segarecomp::ResetOutcome::accepted) {
        log.line(std::string("analyze diagnostic: ") + segarecomp::reset_diagnostic_name(reset.diagnostic));
        return fail(options, log, sha, "analyze", 1, "This file is not a supported Genesis / Mega Drive ROM.");
      }
      // The M68K executable images of the program the generate stage builds (--reset-entry --immutable-rom-aot, no aliases).
      auto program = segarecomp::make_genesis_reset_bridge_startup_program(bytes, reset, std::nullopt);
      if (!program || !segarecomp::apply_genesis_immutable_rom_aot(*program))
        return fail(options, log, sha, "analyze", 1, "This file is not a supported Genesis / Mega Drive ROM.");
      const auto images = segarecomp::genesis_m68k_executable_images(*program);
      if (!images) return fail(options, log, sha, "analyze", 1, "This file is not a supported Genesis / Mega Drive ROM.");
      m68k_images = segarecomp::format_image_provenance_json(segarecomp::count_image_provenance(images->set));
    }
    stage("analyze", "done");

    // ---- generate: the existing emit route, in-process ----
    stage("generate", "begin");
    const fs::path source = options.output / "generated.c";
    const fs::path shard_dir = options.output / "generated";
    fs::remove(source, ec);
    fs::remove_all(shard_dir, ec);
    std::vector<fs::path> units;
    if (target == Target::master_system) {
      // The one SMS generation route (ingest -> ImageSet -> Z80 emitter, plus the embedded cartridge), in-process.
      sms::EmitRequest request;
      request.directory = shard_dir;
      request.stem = "sms";
      const auto outcome = sms::emit_cartridge(bytes, ingest_options, request);
      if (!outcome.ok()) {
        log.line("emit diagnostic: " + outcome.error);
        fs::remove_all(shard_dir, ec);
        return fail(options, log, sha, "generate", 1,
                    "Segarecomp could not translate this Master System game yet (compatibility is experimental).",
                    outcome.sms_error != SMS_OK ? sms_error_name(outcome.sms_error) : "");
      }
      log.line("emitted units=" + std::to_string(outcome.stats.translation_units) + " full_owners=" +
               std::to_string(outcome.stats.full_owners));
      // SEG-028 (ADR 0077): sanitized executable-image provenance counts only (no address, byte or hash).
      const std::string images = "{\"z80\":" + segarecomp::format_image_provenance_json(outcome.provenance) + "}";
      log.line("executable_images: " + images);
      sms_images = ",\"executable_images\":" + images;
      std::ifstream in(shard_dir / "sms.units");
      for (std::string line; std::getline(in, line);)
        if (!line.empty()) units.push_back(shard_dir / line);
    } else {
      // The existing emit route, in-process; no ADR 0049 alias yet (the build-time preparation adds them, SEG-028-T005).
      if (!emit_genesis_m68k(options, log, sha, GenesisM68kRoute{&bytes, &*genesis_reset, source, shard_dir}, {}, units))
        return fail(options, log, sha, "generate", 1,
                    "Segarecomp could not translate this game yet (compatibility is experimental).");
    }
    if (!units_complete(units)) return fail(options, log, sha, "generate", 1, "Generated code is incomplete.");
    stage("generate", "done");

    // ---- compile ----
    stage("compile", "begin");
    if (target == Target::genesis) {
      // Genesis: M68K units + Z80 image materialization fixed point + sound devices + final link (SEG-032-T008).
      fs::path executable;
      std::string status_extra;
      bool sound_degraded = false;
      const int rc = build_genesis_program(options, log, sha, GenesisM68kRoute{&bytes, &*genesis_reset, source, shard_dir}, units,
                                           m68k_images, executable, status_extra, sound_degraded);
      if (rc != 0) return rc;
      if (!options.keep_work) fs::remove_all(options.output / "obj", ec);
      stage("link", "done");
      write_status(options, sha, "ok", "done", "", status_extra);
      std::cout << "@result ok executable=" << executable.string()
                << (sound_degraded ? " genesis_audio=degraded z80_audio_outcome=structural_code_mismatch" : "") << std::endl;
      return 0;
    }
    const fs::path runtime = options.runtime_dir / "runtime";
    const fs::path viewer = options.runtime_dir / "viewer";
    const bool play = options.sdl3_include.has_value();
    std::vector<std::string> base{options.cc};
    base.insert(base.end(), options.cc_args.begin(), options.cc_args.end());
    struct Unit { fs::path source; std::vector<std::string> extra; };
    std::vector<Unit> compile;
    if (target == Target::master_system) {
      // SMS link set (T008 headless / T009 viewer recipes): generated units + runtime + PSG device, then either the
      // headless driver and its VDP/audio wiring or the viewer core, its SDL3 adapter and its `main`. The platform
      // directory is <root>/platforms/master-system; the shared libraries sit at the same <root>.
      fs::path platform_dir = options.runtime_dir;
      if (!platform_dir.has_filename()) platform_dir = platform_dir.parent_path();  // tolerate a trailing separator
      const fs::path root = platform_dir.parent_path().parent_path();
      const fs::path headless = options.runtime_dir / "headless";
      const fs::path psg = root / "libs" / "device" / "sega" / "psg";
      base.insert(base.end(), {"-std=c11", "-Wall", "-Wextra", "-pedantic", "-O" + options.optimize, "-D_CRT_SECURE_NO_WARNINGS",
                               "-I", (root / "libs" / "codegen" / "c11" / "include").string(), "-I", runtime.string(),
                               "-I", (psg / "include").string(), "-I", shard_dir.string()});
      for (const auto &unit : units) compile.push_back({unit, {kGeneratedUnitFlag}});
      for (const char *name : {"sms_memory.c", "sms_sha256.c", "sms_input.c", "sms_machine.c", "sms_psg.c", "sms_pad.c",
                               "sms_vdp.c", "sms_render.c"})
        compile.push_back({runtime / name, {}});
      compile.push_back({psg / "src" / "sn76489.c", {}});
      if (play) {
        base.insert(base.end(), {"-I", viewer.string(), "-I", options.sdl3_include->string()});
        for (const char *name : {"sms_viewer.c", "sms_viewer_sdl3.c", "sms_viewer_main.c"}) compile.push_back({viewer / name, {}});
      } else {
        base.insert(base.end(), {"-I", headless.string()});
        for (const char *name : {"sms_audio.c", "sms_headless.c", "sms_devices_vdp.c"}) compile.push_back({headless / name, {}});
      }
    }
    for (const auto &unit : compile)
      if (!fs::is_regular_file(unit.source, ec))
        return fail(options, log, sha, "compile", 2, "The Segarecomp runtime files are missing: " + unit.source.string());
    const fs::path object_dir = options.output / "obj";
    fs::remove_all(object_dir, ec);
    fs::create_directories(object_dir, ec);
    std::vector<std::vector<std::string>> commands;
    std::vector<fs::path> logs, objects;
    for (std::size_t i = 0; i < compile.size(); ++i) {
      const fs::path object = object_dir / (std::to_string(i) + ".o");
      auto command = base;
      command.insert(command.end(), compile[i].extra.begin(), compile[i].extra.end());
      command.insert(command.end(), {"-c", "-o", object.string(), compile[i].source.string()});
      commands.push_back(command);
      logs.push_back(object_dir / (std::to_string(i) + ".log"));
      objects.push_back(object);
    }
    // One bounded default (SEG-033-T004): the host's concurrency capped at 8; --jobs overrides it.
    unsigned jobs = options.jobs != 0 ? options.jobs : std::min(8U, std::max(1U, std::thread::hardware_concurrency()));
    log.line("# compile jobs: " + std::to_string(jobs));
    std::vector<int> codes;
    const int failed = run_compiles(commands, logs, jobs, codes);
    for (std::size_t i = 0; i < commands.size(); ++i) {
      std::string joined;
      for (const auto &arg : commands[i]) joined += arg + ' ';
      log.line("$ " + joined);
      log.append_file(logs[i]);
    }
    if (failed >= 0)
      return fail(options, log, sha, "compile", 3,
                  codes[static_cast<std::size_t>(failed)] < 0 ? "The bundled C compiler could not be started."
                                                              : "The bundled C compiler reported errors.");
    stage("compile", "done");

    // ---- link ----
    stage("link", "begin");
#if defined(_WIN32)
    const fs::path executable = options.output / "game.exe";
#else
    const fs::path executable = options.output / "game";
#endif
    std::vector<std::string> link{options.cc};
    link.insert(link.end(), options.cc_args.begin(), options.cc_args.end());
    link.insert(link.end(), {"-o", executable.string()});
    for (const auto &object : objects) link.push_back(object.string());
    if (play) { link.push_back("-L" + options.sdl3_lib->string()); link.push_back("-lSDL3"); }
    link.insert(link.end(), options.link_args.begin(), options.link_args.end());
    const fs::path link_log = object_dir / "link.log";
    const int link_rc = run_process(link, link_log);
    log.line("$ (link)");
    log.line("# link target=" + executable.filename().string());
    log.append_file(link_log);
    if (link_rc != 0) return fail(options, log, sha, "link", 3, "The native program could not be linked.");
    fs::remove_all(object_dir, ec);
    stage("link", "done");
    write_status(options, sha, "ok", "done", "", sms_provenance + sms_images + aot_policy_json(options, log, sha, false));
    std::cout << "@result ok executable=" << executable.string() << std::endl;
    return 0;
  } catch (const std::exception &error) {
    return fail(options, log, sha, "analyze", 1, std::string("Cannot read the ROM: ") + error.what());
  }
}
