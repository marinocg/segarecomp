// SEG-137-T007 (ADR 0296): selecting a Luau-only schema 3 package as the version of a library game. Orchestration only: every verification is the shared libs/media and libs/luau_host code
// (`segarecomp mod ...` and `segarecomp build --luau-package` run the same functions again; a preview here is never trusted by the build).
#include "launcher_core.hpp"

#include "segarecomp/luau_api.hpp"
#include "segarecomp/mod.hpp"
#include "segarecomp/patch.hpp"

#include <fstream>
#include <iterator>

namespace launcher {

namespace {
// The original image, read with the product's maximum-image-size policy: a missing, non-regular, oversized, unreadable or short-read file is refused BEFORE anything is allocated at its declared size.
bool read_original(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0U || size > segarecomp::patch::kMaxImageBytes) return false;
  bytes.resize(static_cast<std::size_t>(size));
  std::ifstream in(path, std::ios::binary);
  return static_cast<bool>(in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) && in.gcount() == static_cast<std::streamsize>(size);
}
}  // namespace


std::string select_luau_mod(RomView &rom, const std::filesystem::path &package) {
  if (rom.platform_id != "genesis") return "Luau mods run in Genesis games only.";
  if (rom.patched()) return "A Luau mod cannot be combined with a selected patch: select one version.";
  const segarecomp::mod::Package p = segarecomp::mod::read_package_file(package);
  if (!p.ok) return "The mod package is not valid: " + p.rejection.message + ".";
  // ADR 0305: the played program runs a package that stays inside the HUD-only seam through it and anything else the scheduled path supports through the one mod scheduler; the build re-checks both.
  if (segarecomp::luau::live_support(p))
    if (const auto refusal = segarecomp::luau::scheduled_support(p)) return refusal->message + ".";
  std::vector<std::uint8_t> original;
  if (!read_original(rom.path, original)) return "The original game file is missing, unreadable or larger than allowed.";
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

// The single rule for a schema 3 package's presentation defaults (install and launch both use it): explicit manifest values win; a package that verifiably claims widescreen (a geometry
// declaration with aspect_max and a scene covering scenery or actors) but states none gets Enhanced Widescreen + Raw Extension per missing field; anything else states no default.
PresentationDefaults mod_presentation_defaults(const segarecomp::mod::Manifest &m) {
  PresentationDefaults d{m.default_widescreen, m.default_side_style};
  bool claims_widescreen = false;
  if (m.has_geometry && !m.aspect_max.empty())
    for (const auto &scene : m.geometry_scenes)
      for (const std::string &component : scene.components) claims_widescreen = claims_widescreen || component == "scenery" || component == "actors";
  if (claims_widescreen) {
    if (d.widescreen.empty()) d.widescreen = "enhanced_gameplay";
    if (d.side_style.empty()) d.side_style = "raw";
  }
  return d;
}

std::string verify_installed_mod(const RomView &rom, PresentationDefaults *defaults) {
  if (!rom.luau_mod()) return {};
  const segarecomp::mod::Package p = segarecomp::mod::read_package_file(rom.luau_package);
  if (!p.ok) return "The installed mod package is not valid any more: " + p.rejection.message + ".";
  if (p.identity != rom.luau_identity) return "The installed mod package is not the one selected (its identity changed).";
  const std::string trust = segarecomp::mod::trust_class(p.manifest);
  if (rom.luau_data != (trust == "data") || rom.luau_native != (trust == "native")) return "The installed mod package is not of the kind that was selected.";
  if (rom.luau_native && !rom.luau_approved) return "This mod contains executable code and has not been approved. Install the package again and approve it.";
  std::vector<std::uint8_t> original;
  if (!read_original(rom.path, original)) return "The original game file is missing, unreadable or larger than allowed.";
  const segarecomp::mod::EffectiveImage effective = segarecomp::mod::effective_image(p, original);
  if (!effective.ok) return "This game is not one the mod supports: " + effective.rejection.message + ".";
  if (effective.original_sha256 != rom.sha256 || effective.effective_sha256 != rom.luau_effective_sha256) return "The original game or the effective image does not match the installed mod.";
  if (defaults != nullptr) *defaults = mod_presentation_defaults(p.manifest);
  return {};
}

}  // namespace launcher
