#pragma once

// The CLI's input-protection rules for outputs (SEG-136, SEG-141): an output must never be an input, whatever path, link or alias names it.
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace segarecomp_cli {

// True when the entry `path` would be created, removed or renamed over lies inside `folder` (links resolved; an unresolvable path counts as inside).
[[nodiscard]] bool lies_inside(const std::filesystem::path &path, const std::filesystem::path &folder);

// A build into `output` overwrites or removes fixed files and folders under it (the effective image first of all). The conflict, if any `inputs` entry (a file) is one of those files -- by the same or an
// equivalent path, a symbolic link or a hard link, in either direction -- or lies inside one of those folders. `extra_files` are further paths the build writes. Nothing is read or changed.
[[nodiscard]] std::optional<std::string> build_output_conflict(const std::filesystem::path &output, const std::vector<std::filesystem::path> &inputs, const std::vector<std::filesystem::path> &extra_files = {});

// The fixed files a build writes or removes under its output folder.
[[nodiscard]] std::vector<std::filesystem::path> build_output_files(const std::filesystem::path &output);

}  // namespace segarecomp_cli
