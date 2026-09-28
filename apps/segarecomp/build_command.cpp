#include "build_command.hpp"

#include "segarecomp/rom.hpp"
#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

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

int run_process(const std::vector<std::string> &argv, const fs::path &log) {
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
  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  CloseHandle(info.hThread);
  return static_cast<int>(code);
}
#else
int run_process(const std::vector<std::string> &argv, const fs::path &log) {
  std::vector<char *> args;
  for (const auto &arg : argv) args.push_back(const_cast<char *>(arg.c_str()));
  args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  pid_t pid{};
  const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) return -1;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
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
  std::vector<std::string> link_args;
  fs::path runtime_dir;  // contains runtime/, viewer/ and (optional) compat/
  std::optional<fs::path> sdl3_include;
  std::optional<fs::path> sdl3_lib;
  std::string optimize = "2";
  unsigned jobs = 0;
};

struct Log {
  std::ofstream file;
  void line(const std::string &text) { file << text << '\n'; file.flush(); }
  void append_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (in) file << in.rdbuf();
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

void write_status(const Options &options, const std::string &sha, const std::string &status,
                  const std::string &stage, const std::string &message) {
  std::ofstream out(options.output / "status.json", std::ios::binary | std::ios::trunc);
  out << "{\"status\":\"" << status << "\",\"stage\":\"" << stage << "\",\"message\":\"" << json_escape(message)
      << "\",\"rom_sha256\":\"" << sha << "\"}\n";
}

int fail(const Options &options, Log &log, const std::string &sha, const std::string &stage, int code,
         const std::string &message) {
  log.line("FAILED (" + stage + "): " + message);
  write_status(options, sha, "failed", stage, message);
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
    else if (key == "--link-arg") options.link_args.push_back(value);
    else if (key == "--runtime-dir") { options.runtime_dir = fs::path(value); have_runtime = true; }
    else if (key == "--sdl3-include") options.sdl3_include = fs::path(value);
    else if (key == "--sdl3-lib") options.sdl3_lib = fs::path(value);
    else if (key == "--optimize") options.optimize = value;
    else if (key == "--jobs") options.jobs = static_cast<unsigned>(std::max(0, std::atoi(value.c_str())));
    else return std::nullopt;
  }
  if (!have_rom || !have_output || !have_runtime || options.cc.empty()) return std::nullopt;
  if (options.optimize != "0" && options.optimize != "2") return std::nullopt;
  if (options.sdl3_include.has_value() != options.sdl3_lib.has_value()) return std::nullopt;
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
  std::string sha;
  try {
    // ---- analyze ----
    stage("analyze", "begin");
    const auto bytes = segarecomp::read_binary(options.rom);
    sha = segarecomp::sha256_hex(bytes);
    log.line("rom_sha256=" + sha);
    log.line("cc=" + options.cc);
    const auto reset = segarecomp::analyze_genesis_reset_image(bytes);
    if (reset.outcome != segarecomp::ResetOutcome::accepted)
      return fail(options, log, sha, "analyze", 1,
                  "This file does not look like a supported Genesis / Mega Drive ROM (" +
                      std::string(segarecomp::reset_diagnostic_name(reset.diagnostic)) + ").");
    stage("analyze", "done");

    // ---- generate: the existing emit route, in-process ----
    stage("generate", "begin");
    const fs::path source = options.output / "generated.c";
    const fs::path shard_dir = options.output / "generated";
    fs::remove(source, ec);
    fs::remove_all(shard_dir, ec);
    fs::path hints = options.runtime_dir / "compat" / (sha + ".json");
    std::vector<std::string> emit_args{"segarecomp", "emit-general-startup-bridge-c", "--rom", options.rom.string(),
                                       "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot"};
    if (fs::is_regular_file(hints, ec)) { emit_args.push_back("--external-hints"); emit_args.push_back(hints.string()); }
    emit_args.insert(emit_args.end(), {"--generated-c-output", source.string(), "--generated-c-shard-dir",
                                       shard_dir.string()});
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
    if (emit_rc != 0) {
      fs::remove(source, ec);
      fs::remove_all(shard_dir, ec);
      return fail(options, log, sha, "generate", 1,
                  "Segarecomp could not translate this game yet (compatibility is experimental).");
    }
    std::vector<fs::path> units;
    const fs::path manifest = shard_dir / "bridge_generated.units";
    if (fs::is_regular_file(manifest, ec)) {
      std::ifstream in(manifest);
      for (std::string line; std::getline(in, line);)
        if (!line.empty()) units.push_back(shard_dir / line);
    } else if (fs::is_regular_file(source, ec)) {
      units.push_back(source);
    }
    if (units.empty() || !std::all_of(units.begin(), units.end(), [](const fs::path &p) { return fs::is_regular_file(p); }))
      return fail(options, log, sha, "generate", 1, "Generated code is incomplete.");
    stage("generate", "done");

    // ---- compile ----
    stage("compile", "begin");
    const fs::path runtime = options.runtime_dir / "runtime";
    const fs::path viewer = options.runtime_dir / "viewer";
    const bool play = options.sdl3_include.has_value();
    std::vector<std::string> base{options.cc};
    base.insert(base.end(), options.cc_args.begin(), options.cc_args.end());
    base.insert(base.end(), {"-std=c11", "-Wall", "-Wextra", "-pedantic", "-O" + options.optimize,
                             "-I", runtime.string()});
    struct Unit { fs::path source; std::vector<std::string> extra; };
    std::vector<Unit> compile;
    // units[0] is the main TU (the only one whose main() calls genesis_runtime_run).
    if (play) {
      base.insert(base.end(), {"-I", viewer.string(), "-I", options.sdl3_include->string()});
      compile.push_back({units[0], {"-Dgenesis_runtime_run=genesis_viewer_hook_run"}});
    } else {
      compile.push_back({units[0], {}});
    }
    for (std::size_t i = 1; i < units.size(); ++i) compile.push_back({units[i], {"-I", units[0].parent_path().string()}});
    if (units.size() > 1 || fs::is_directory(shard_dir, ec)) {
      // sharded: generated headers live beside the units
      for (auto &unit : compile) { unit.extra.push_back("-I"); unit.extra.push_back(shard_dir.string()); }
    }
    compile.push_back({runtime / "runtime.c", {}});
    if (play)
      for (const char *name : {"vdp_render.c"}) compile.push_back({runtime / name, {}});
    if (play)
      for (const char *name : {"viewer.c", "viewer_sdl3.c", "viewer_main_hook.c"}) compile.push_back({viewer / name, {}});
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
    unsigned jobs = options.jobs != 0 ? options.jobs : std::min(4U, std::max(1U, std::thread::hardware_concurrency()));
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
    log.append_file(link_log);
    if (link_rc != 0) return fail(options, log, sha, "link", 3, "The native program could not be linked.");
    fs::remove_all(object_dir, ec);
    stage("link", "done");
    write_status(options, sha, "ok", "done", "");
    std::cout << "@result ok executable=" << executable.string() << std::endl;
    return 0;
  } catch (const std::exception &error) {
    return fail(options, log, sha, "analyze", 1, std::string("Cannot read the ROM: ") + error.what());
  }
}
