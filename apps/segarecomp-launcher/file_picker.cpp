#include "file_picker.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace launcher {

namespace {

std::string utf8_name(const fs::path &path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

fs::path normalized(const fs::path &path) {
  std::error_code ec;
  fs::path absolute = fs::absolute(path, ec);
  if (ec) absolute = path;
  return absolute.lexically_normal();
}

bool allowed_extension(const fs::path &path, const std::vector<std::string> &extensions) {
  std::string ext = lower(utf8_name(path.extension()));
  if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
  return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

void add_root(std::vector<PickerRoot> &roots, const std::string &label, const fs::path &path) {
  std::error_code ec;
  if (path.empty() || !fs::is_directory(path, ec)) return;
  const fs::path norm = normalized(path);
  for (const auto &existing : roots)
    if (existing.path == norm) return;
  roots.push_back({label, norm});
}

} // namespace

std::vector<PickerRoot> picker_roots(const fs::path &extra) {
  std::vector<PickerRoot> roots;
  if (!extra.empty()) add_root(roots, "Selected folder", extra);
  const struct { SDL_Folder folder; const char *label; } folders[] = {{SDL_FOLDER_HOME, "Home"}, {SDL_FOLDER_DESKTOP, "Desktop"}, {SDL_FOLDER_DOCUMENTS, "Documents"},
                                                                       {SDL_FOLDER_DOWNLOADS, "Downloads"}};
  for (const auto &f : folders)
    if (const char *path = SDL_GetUserFolder(f.folder)) add_root(roots, f.label, fs::path(std::u8string(reinterpret_cast<const char8_t *>(path))));
#if defined(_WIN32)
  for (char letter = 'A'; letter <= 'Z'; ++letter) add_root(roots, std::string("Drive ") + letter + ":", fs::path(std::string(1, letter) + ":\\"));
#elif defined(__APPLE__)
  add_root(roots, "Volumes", "/Volumes");
#else
  for (const char *mount : {"/media", "/mnt", "/run/media"}) add_root(roots, mount, mount);
#endif
  return roots;
}

bool picker_contains(const fs::path &root, const fs::path &candidate) {
  const fs::path r = normalized(root), c = normalized(candidate);
  if (r.empty() || c.empty()) return false;
  const fs::path relative = c.lexically_relative(r);
  if (relative.empty()) return false;
  for (const auto &part : relative)
    if (part == "..") return false;
  return true;  // "." (the root itself) is inside
}

PickerListing picker_list(const fs::path &root, const fs::path &dir, const std::vector<std::string> &extensions) {
  PickerListing listing;
  if (!picker_contains(root, dir)) { listing.error = "That folder is outside the chosen location."; return listing; }
  std::error_code ec;
  const auto status = fs::symlink_status(dir, ec);
  if (ec || status.type() != fs::file_type::directory) { listing.error = "That folder cannot be opened."; return listing; }
  std::vector<PickerEntry> dirs, files;
  std::size_t seen = 0;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    if (++seen > picker_max_entries) { listing.truncated = true; break; }
    const std::string name = utf8_name(it->path().filename());
    if (name.empty() || name.front() == '.') continue;  // hidden
    std::error_code sec;
    const auto st = it->symlink_status(sec);
    if (sec) continue;
    if (st.type() == fs::file_type::directory) {
      dirs.push_back({name, it->path(), true, 0});
    } else if (st.type() == fs::file_type::regular && allowed_extension(it->path(), extensions)) {
      std::error_code zec;
      const auto size = fs::file_size(it->path(), zec);
      files.push_back({name, it->path(), false, zec ? 0 : size});
    }  // symlinks (to anything) and special files are neither followed nor offered
  }
  if (ec) listing.error = "This folder could not be fully read.";
  const auto by_name = [](const PickerEntry &a, const PickerEntry &b) { return lower(a.name) < lower(b.name); };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  listing.entries = std::move(dirs);
  listing.entries.insert(listing.entries.end(), files.begin(), files.end());
  return listing;
}

fs::path picker_parent(const fs::path &root, const fs::path &dir) {
  const fs::path r = normalized(root), d = normalized(dir);
  if (d == r) return {};
  fs::path parent = d.parent_path();
  return picker_contains(r, parent) ? parent : fs::path{};
}

bool picker_accept(const fs::path &root, const fs::path &path, const std::vector<std::string> &extensions, std::string *reason) {
  const auto fail = [&](const char *text) { if (reason != nullptr) *reason = text; return false; };
  if (!picker_contains(root, path)) return fail("That file is outside the chosen location.");
  std::error_code ec;
  const auto status = fs::symlink_status(path, ec);
  if (ec || status.type() != fs::file_type::regular) return fail("That is not a plain file.");
  if (!allowed_extension(path, extensions)) return fail("That file type cannot be used here.");
  return true;
}

} // namespace launcher
