#pragma once

// SEG-069-T004: the model of the launcher's built-in, controller-operable file picker (no UI here). The native OS file dialog cannot be assumed to accept
// gamepad input, so a controller-only user is offered this minimal picker for the mandatory "choose a ROM" step. It is deliberately not a file manager:
//
//   * it starts from an explicit list of ROOTS (the user's folders, mounted media, drives) and can never leave the chosen root: every directory it lists or
//     enters, and every file it hands back, must lie lexically inside that root (no `..`, no symlinked directory or file is followed or offered);
//   * it lists only directories and files of the extensions the purpose allows (ROM/ZIP files for import and locate, PNG/JPEG for a cover); hidden names are skipped;
//   * it returns plain paths to the existing import / locate / cover logic, which keeps doing all validation (ROM size limits, ZIP policy, SHA-256 match, image checks).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

struct PickerEntry {
  std::string name;              // UTF-8 file or folder name
  std::filesystem::path path;
  bool directory = false;
  std::uintmax_t size = 0;       // files only
};

struct PickerRoot {
  std::string label;             // "Home", "Downloads", "Drive D:", ...
  std::filesystem::path path;
};

struct PickerListing {
  std::vector<PickerEntry> entries;  // folders first, then files; each group sorted case-insensitively
  bool truncated = false;            // more than max_entries names were present
  std::string error;                 // non-empty when the folder could not be read or is outside the root
};

inline constexpr std::size_t picker_max_entries = 4000;

// The existing folders a user can start from. `extra` (may be empty) is added first (test seam: SEGARECOMP_PICKER_ROOT). Never contains duplicates.
[[nodiscard]] std::vector<PickerRoot> picker_roots(const std::filesystem::path &extra);
// True when `candidate` is `root` or lies below it, judged lexically on normalized absolute paths (no filesystem access, `..` never escapes).
[[nodiscard]] bool picker_contains(const std::filesystem::path &root, const std::filesystem::path &candidate);
// Lists `dir` (which must be inside `root`) for the given lower-case extensions (without the dot).
[[nodiscard]] PickerListing picker_list(const std::filesystem::path &root, const std::filesystem::path &dir, const std::vector<std::string> &extensions);
// The folder one level up, or an empty path when `dir` is the root itself (the caller then returns to the list of roots).
[[nodiscard]] std::filesystem::path picker_parent(const std::filesystem::path &root, const std::filesystem::path &dir);
// Re-checks, at the moment of choosing, that `path` is an existing plain file of an allowed extension inside `root` (a symlink is refused).
[[nodiscard]] bool picker_accept(const std::filesystem::path &root, const std::filesystem::path &path, const std::vector<std::string> &extensions, std::string *reason = nullptr);

} // namespace launcher
