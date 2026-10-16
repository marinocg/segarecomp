#include "process_tree.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#endif

namespace launcher {

namespace {
long long process_id(SDL_Process *process) {
  return process == nullptr ? 0 : static_cast<long long>(SDL_GetNumberProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_PID_NUMBER, 0));
}
} // namespace

#if defined(_WIN32)

struct ProcessTree::Impl {
  std::mutex mutex;
  HANDLE job = nullptr;
  bool terminated = false;
};

ProcessTree::ProcessTree() : impl_(std::make_unique<Impl>()) {}

ProcessTree::~ProcessTree() {
  // KILL_ON_JOB_CLOSE: closing the last handle also ends anything the job still holds.
  if (impl_->job != nullptr) CloseHandle(impl_->job);
}

void ProcessTree::attach(SDL_Process *process) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const long long pid = process_id(process);
  if (pid == 0 || impl_->job != nullptr) return;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (handle == nullptr || !AssignProcessToJobObject(job, handle)) {
    // Not assignable (for example already in a job that forbids nesting): the direct child is still terminated by terminate().
    if (handle != nullptr) CloseHandle(handle);
    CloseHandle(job);
    return;
  }
  CloseHandle(handle);
  impl_->job = job;
}

void ProcessTree::terminate() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->terminated) return;
  impl_->terminated = true;
  if (impl_->job != nullptr) TerminateJobObject(impl_->job, 1);
}

void ProcessTree::release() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->terminated = true;
}

#else

struct ProcessTree::Impl {
  std::mutex mutex;
  long long pid = 0;
  SDL_Process *process = nullptr;
  bool terminated = false;
};

namespace {

// pid -> parent pid for every visible process (empty when the table cannot be read).
std::map<long long, long long> process_table() {
  std::map<long long, long long> table;
  std::error_code ec;
  if (std::filesystem::is_directory("/proc/self", ec)) {
    for (const auto &item : std::filesystem::directory_iterator("/proc", ec)) {
      const std::string name = item.path().filename().string();
      if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
      std::ifstream stat(item.path() / "stat");
      std::string text;
      std::getline(stat, text);
      // "pid (comm) state ppid ...": comm may contain spaces and parentheses, so anchor on the LAST ')'.
      const auto close = text.rfind(')');
      if (close == std::string::npos) continue;
      std::istringstream rest(text.substr(close + 1));
      std::string state;
      long long ppid = 0;
      if (rest >> state >> ppid) table[std::stoll(name)] = ppid;
    }
    if (!table.empty()) return table;
  }
  // macOS / BSD (no procfs): `ps` prints "<pid> <ppid>" per process.
  if (FILE *pipe = popen("ps -A -o pid= -o ppid= 2>/dev/null", "r")) {
    long long pid = 0, ppid = 0;
    while (std::fscanf(pipe, "%lld %lld", &pid, &ppid) == 2) table[pid] = ppid;
    pclose(pipe);
  }
  return table;
}

std::set<long long> descendants_of(long long root, const std::map<long long, long long> &table) {
  std::set<long long> found;
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto &[pid, ppid] : table)
      if ((ppid == root || found.count(ppid) != 0) && found.insert(pid).second) grew = true;
  }
  return found;
}

} // namespace

ProcessTree::ProcessTree() : impl_(std::make_unique<Impl>()) {}
ProcessTree::~ProcessTree() = default;

void ProcessTree::attach(SDL_Process *process) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->process = process;
  impl_->pid = process_id(process);
}

void ProcessTree::terminate() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->terminated || impl_->pid <= 0) return;
  impl_->terminated = true;
  const auto root = static_cast<pid_t>(impl_->pid);
  // Freeze the root first so it cannot start new children while the tree is being collected, then freeze each newly found level.
  ::kill(root, SIGSTOP);
  std::set<long long> victims;
  for (int round = 0; round < 8; ++round) {
    const std::set<long long> now = descendants_of(impl_->pid, process_table());
    std::size_t before = victims.size();
    for (const long long pid : now)
      if (victims.insert(pid).second) ::kill(static_cast<pid_t>(pid), SIGSTOP);
    if (victims.size() == before) break;
  }
  for (const long long pid : victims) ::kill(static_cast<pid_t>(pid), SIGKILL);
  ::kill(root, SIGKILL);
}

void ProcessTree::release() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->terminated = true;
}

#endif

} // namespace launcher
