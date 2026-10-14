#pragma once

// Private deletion backend. Never delete through std::filesystem path traversal:
// POSIX operations are relative to pinned directory descriptors, and Windows
// holds non-delete/non-write-sharing handles for the whole ancestor chain.
struct Remover {
  ExecResult &result;
  std::size_t visited = 0;
  void problem(const fs::path &p) { result.problems.push_back("refused or denied (unsafe, changed, linked or inaccessible item): " + utf8_of(p.filename())); }
#if defined(_WIN32)
  struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(const fs::path &p, bool deleting = false) {
      value = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES | (deleting ? DELETE : 0), FILE_SHARE_READ,
                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    }
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    bool info(BY_HANDLE_FILE_INFORMATION &i) const {
      return value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(value, &i) && !(i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }
  };
  bool item(const fs::path &p, bool directory, int depth) {
    if (depth > max_depth || ++visited > max_entries_per_tree) { problem(p); return false; }
    Handle h(p, true);
    if (h.value == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) return true;
    BY_HANDLE_FILE_INFORMATION i{};
    if (!h.info(i) || ((i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) { problem(p); return false; }
    if (directory) {
      // h denies rename, deletion and reparse mutation while descendants are accessed.
      std::error_code ec;
      bool ok = true;
      for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
        const auto s = it->symlink_status(ec);
        if (ec) break;
        if (s.type() != fs::file_type::directory && s.type() != fs::file_type::regular) { problem(it->path()); ok = false; continue; }
        if (!item(it->path(), s.type() == fs::file_type::directory, depth + 1)) ok = false;
        if (visited > max_entries_per_tree) break;
      }
      if (ec || !ok) { if (ec) problem(p); return false; }
    }
    // Handle-based delete cannot be redirected by a final-name swap. Read-only
    // files fail closed; do not chmod a potentially replaced path and retry.
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(h.value, FileDispositionInfo, &disposition, sizeof disposition)) { problem(p); return false; }
    if (!directory) { result.bytes_freed += (static_cast<std::uintmax_t>(i.nFileSizeHigh) << 32U) | i.nFileSizeLow; ++result.removed; }
    return true;
  }
  void remove(const fs::path &root, const fs::path &relative, bool directory) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(root, ec).lexically_normal();
    if (ec) { problem(root); return; }
    fs::path p = absolute.root_path();
    std::vector<std::unique_ptr<Handle>> chain;
    const auto pin = [&](const fs::path &path) {
      auto h = std::make_unique<Handle>(path);
      BY_HANDLE_FILE_INFORMATION i{};
      if (!h->info(i) || !(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
      chain.push_back(std::move(h));
      return true;
    };
    if (!pin(p)) { problem(p); return; }
    for (const auto &part : absolute.relative_path()) { p /= part; if (!pin(p)) { problem(p); return; } }
    for (const auto &part : relative.parent_path()) { p /= part; if (!pin(p)) { problem(p); return; } }
    item(p / relative.filename(), directory, 0);
  }
#else
  struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
  };
  bool item(int parent, const std::string &name, bool directory, int depth) {
    if (depth > max_depth || ++visited > max_entries_per_tree) { problem(name); return false; }
    struct stat s{};
    if (fstatat(parent, name.c_str(), &s, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) return true;
      problem(name); return false;
    }
    if (directory ? !S_ISDIR(s.st_mode) : !S_ISREG(s.st_mode)) { problem(name); return false; }
    if (directory) {
      Fd child(openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      struct stat opened{};
      if (child.value < 0 || fstat(child.value, &opened) != 0 || opened.st_dev != s.st_dev || opened.st_ino != s.st_ino) { problem(name); return false; }
      DIR *stream = fdopendir(dup(child.value));
      if (stream == nullptr) { problem(name); return false; }
      bool ok = true;
      for (;;) {
        errno = 0;
        const dirent *e = readdir(stream);
        if (e == nullptr) { if (errno != 0) { problem(name); ok = false; } break; }
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat entry{};
        if (fstatat(child.value, n.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) { problem(n); ok = false; continue; }
        if (!item(child.value, n, S_ISDIR(entry.st_mode), depth + 1)) ok = false;
        if (visited > max_entries_per_tree) break;
      }
      closedir(stream);
      if (!ok) return false;
      // Refuse removal if the name no longer names the opened directory.
      struct stat current{};
      if (fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 || current.st_dev != opened.st_dev || current.st_ino != opened.st_ino) { problem(name); return false; }
    }
    // unlinkat never follows the final component, even if it races with a link swap.
    if (unlinkat(parent, name.c_str(), directory ? AT_REMOVEDIR : 0) != 0) { problem(name); return false; }
    if (!directory) { result.bytes_freed += static_cast<std::uintmax_t>(s.st_size); ++result.removed; }
    return true;
  }
  void remove(const fs::path &root, const fs::path &relative, bool directory) {
    Fd pinned(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (pinned.value < 0 || !root.is_absolute()) { problem(root); return; }
    int parent = pinned.value;
    std::vector<std::unique_ptr<Fd>> chain;
    const fs::path parents = root.relative_path() / relative.parent_path();
    for (const auto &part : parents) {
      auto fd = std::make_unique<Fd>(openat(parent, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (fd->value < 0) { problem(part); return; }
      parent = fd->value;
      chain.push_back(std::move(fd));
    }
    item(parent, relative.filename().string(), directory, 0);
  }
#endif
};
