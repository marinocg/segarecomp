// SEG-018-T006: the small host-portability seam for tests that need a real
// temporary file path or a stdout descriptor redirect. It exists because two
// C++ tests genuinely need it; it is not a portability framework.
#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace host_io {

// Creates an empty, uniquely named file in the system temp directory and returns
// its path (empty string on failure). The caller removes it with std::remove.
inline std::string make_temp_file(const std::string &stem) {
  static unsigned counter = 0U;
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  for (unsigned attempt = 0U; attempt < 64U; ++attempt) {
    const auto path = std::filesystem::temp_directory_path() /
                      (stem + "_" + std::to_string(tick) + "_" + std::to_string(++counter));
    if (std::filesystem::exists(path)) continue;
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) return {};
    std::fclose(file);
    return path.string();
  }
  return {};
}

#if defined(_WIN32)
inline int dup_fd(int fd) { return _dup(fd); }
inline int dup2_fd(int from, int to) { return _dup2(from, to); }
inline int close_fd(int fd) { return _close(fd); }
inline int fileno_of(std::FILE *stream) { return _fileno(stream); }
#else
inline int dup_fd(int fd) { return ::dup(fd); }
inline int dup2_fd(int from, int to) { return ::dup2(from, to); }
inline int close_fd(int fd) { return ::close(fd); }
inline int fileno_of(std::FILE *stream) { return ::fileno(stream); }
#endif

}  // namespace host_io
