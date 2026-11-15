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
  // ADR 0305: the played program runs a package that stays inside the HUD-only seam through it and anything else the scheduled path supports through the one mod scheduler; the build re-checks both.
  if (segarecomp::luau::live_support(p))
    if (const auto refusal = segarecomp::luau::scheduled_support(p)) return refusal->message + ".";
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

std::string verify_installed_mod(const RomView &rom) {
  if (!rom.luau_mod()) return {};
  const segarecomp::mod::Package p = segarecomp::mod::read_package_file(rom.luau_package);
  if (!p.ok) return "The installed mod package is not valid any more: " + p.rejection.message + ".";
  if (p.identity != rom.luau_identity) return "The installed mod package is not the one selected (its identity changed).";
  const std::string trust = segarecomp::mod::trust_class(p.manifest);
  if (rom.luau_native != (trust == "native")) return "The installed mod package is not of the kind that was selected.";
  if (rom.luau_native && !rom.luau_approved) return "This mod contains executable code and has not been approved. Install the package again and approve it.";
  std::ifstream in(rom.path, std::ios::binary);
  const std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (original.empty()) return "The original game file cannot be read.";
  const segarecomp::mod::EffectiveImage effective = segarecomp::mod::effective_image(p, original);
  if (!effective.ok) return "This game is not one the mod supports: " + effective.rejection.message + ".";
  if (effective.original_sha256 != rom.sha256 || effective.effective_sha256 != rom.luau_effective_sha256) return "The original game or the effective image does not match the installed mod.";
  return {};
}

}  // namespace launcher
