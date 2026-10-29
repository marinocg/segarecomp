#include "process_run.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>

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

namespace segarecomp_cli {

// Narrow strings are the platform's native argv encoding (the ANSI code page on Windows, as for the rest of the CLI).

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


// `timeout_seconds` 0 = wait forever; on expiry the process is killed and kProcessTimedOut returned. `environment` entries are
// added to the child's environment.
int run_process(const std::vector<std::string> &argv, const fs::path &log, const Environment &environment, unsigned timeout_seconds) {
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

int run_process(const std::vector<std::string> &argv, const fs::path &log, const Environment &environment, unsigned timeout_seconds) {
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


std::uint64_t available_memory_bytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX status;
  status.dwLength = sizeof status;
  return GlobalMemoryStatusEx(&status) ? static_cast<std::uint64_t>(status.ullAvailPhys) : 0U;
#elif defined(__linux__)
  FILE *f = std::fopen("/proc/meminfo", "r");
  if (f == nullptr) return 0U;
  char line[256];
  unsigned long long kb = 0;
  while (std::fgets(line, sizeof line, f) != nullptr)
    if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
  std::fclose(f);
  return static_cast<std::uint64_t>(kb) * 1024U;
#else
  return 0U;
#endif
}

}  // namespace segarecomp_cli
