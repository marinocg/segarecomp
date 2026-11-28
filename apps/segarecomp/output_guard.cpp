#include "output_guard.hpp"

#include <system_error>

namespace segarecomp_cli {

namespace fs = std::filesystem;

bool lies_inside(const fs::path &path, const fs::path &folder) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(path, ec);
  if (ec) return true;
  const fs::path parent = fs::weakly_canonical(absolute.parent_path(), ec);   // where the entry itself is created, removed or renamed over
  if (ec) return true;
  const fs::path resolved = fs::weakly_canonical(absolute, ec);               // where a final link points
  if (ec) return true;
  for (const fs::path &entry : {parent / absolute.filename(), resolved})
    for (fs::path at = entry.parent_path(); !at.empty(); at = at.parent_path()) {
      std::error_code same;
      if (fs::equivalent(at, folder, same) && !same) return true;
      if (at == at.parent_path()) break;
    }
  return false;
}

std::vector<fs::path> build_output_files(const fs::path &output) {
  return {output / "input" / "effective.rom", output / "build.log", output / "status.json", output / "set-build.log", output / "generated.c", output / "compiler-version.tmp", output / "game", output / "game.exe"};
}

std::optional<std::string> build_output_conflict(const fs::path &output, const std::vector<fs::path> &inputs, const std::vector<fs::path> &extra_files) {
  std::vector<fs::path> files = build_output_files(output);
  files.insert(files.end(), extra_files.begin(), extra_files.end());
  for (const fs::path &input : inputs) {
    std::error_code ec;
    if (input.empty() || !fs::exists(input, ec)) continue;
    for (const fs::path &file : files) {
      std::error_code exists_error, same_error;
      if (fs::exists(file, exists_error) && fs::equivalent(file, input, same_error) && !same_error)
        return "the build output " + file.lexically_relative(output).generic_string() + " is the same file as an input; refusing to overwrite or remove it";
    }
    for (const char *tree : {"obj", "generated", "generated-z80", "set-mod", "sms-mod", "native-mod", "native", "luau", "hooks"}) {
      std::error_code tree_error;
      if (fs::exists(output / tree, tree_error) && lies_inside(input, output / tree))
        return std::string("an input lies inside the build folder ") + tree + ", which the build replaces; use another output folder or move the input";
    }
  }
  return std::nullopt;
}

}  // namespace segarecomp_cli
