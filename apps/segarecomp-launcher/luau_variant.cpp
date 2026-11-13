// SEG-137-T007 (ADR 0296): selecting a Luau-only schema 3 package as the version of a library game. Orchestration only: every verification is the shared libs/media and libs/luau_host code
// (`segarecomp mod ...` and `segarecomp build --luau-package` run the same functions again; a preview here is never trusted by the build).
#include "launcher_core.hpp"

#include "segarecomp/luau_api.hpp"
#include "segarecomp/mod.hpp"

#include <fstream>
#include <iterator>

namespace launcher {

std::string select_luau_mod(RomView &rom, const std::filesystem::path &package) {
  if (rom.platform_id != "genesis") return "Luau mods run in Genesis games only.";
  if (rom.patched()) return "A Luau mod cannot be combined with a selected patch: select one version.";
  const segarecomp::mod::Package p = segarecomp::mod::read_package_file(package);
  if (!p.ok) return "The mod package is not valid: " + p.rejection.message + ".";
  if (const auto refusal = segarecomp::luau::live_support(p)) return refusal->message + ".";
  std::ifstream in(rom.path, std::ios::binary);
  const std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (original.empty()) return "The original game file cannot be read.";
  const segarecomp::mod::EffectiveImage effective = segarecomp::mod::effective_image(p, original);
  if (!effective.ok) return "This game is not one the mod supports: " + effective.rejection.message + ".";
  if (effective.original_sha256 != rom.sha256) return "The game file changed since it was added to the library.";
  rom.luau_identity = p.identity;
  rom.luau_package = package;
  rom.luau_effective_sha256 = effective.effective_sha256;
  rom.luau_pin = std::string(segarecomp::luau::compiler_pin());
  rom.luau_label = p.manifest.name + " " + p.manifest.version;
  return {};
}

}  // namespace launcher
