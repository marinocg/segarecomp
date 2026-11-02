#include "patch_library.hpp"

#include "segarecomp/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace launcher {

namespace {

bool read_bounded(const fs::path &path, std::size_t limit, std::vector<std::uint8_t> &bytes, std::string &error) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) { error = "the file is not a regular file"; return false; }   // user-chosen inputs may be reached through a link; what is read is what is verified
  const std::uintmax_t size = fs::file_size(path, ec);
  if (ec || size > limit) { error = "the file is larger than allowed"; return false; }
  bytes.resize(static_cast<std::size_t>(size));
  std::ifstream in(path, std::ios::binary);
  if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) { error = "the file cannot be read"; return false; }
  return true;
}

std::string refusal_for(const std::string &error) { return "This patch cannot be installed: " + error + "."; }

// The single verification routine; `package_bytes` are kept so the published copy is byte-for-byte what was verified.
PatchPreview verify(const LibraryEntry &entry, const fs::path &package, const fs::path &original_rom, std::vector<std::uint8_t> *package_bytes) {
  PatchPreview out;
  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!read_bounded(package, segarecomp::patch::kMaxPackageBytes, bytes, error)) { out.error = refusal_for("the package " + error); return out; }
  segarecomp::patch::Package pkg = segarecomp::patch::read_package(bytes);
  if (!pkg.ok) { out.error = refusal_for(pkg.error); return out; }
  out.info = pkg.info;
  out.package_valid = true;
  if (entry.platform_id != pkg.info.platform) { out.error = refusal_for("it is for a different platform than this game"); return out; }
  if (pkg.info.source_sha256 != entry.sha256) {
    out.error = "This patch was made for a different game or revision. It is only applied to the exact original image it was made for, never to a similar one.";
    return out;
  }
  out.fits_game = true;
  std::vector<std::uint8_t> rom;
  if (original_rom.empty() || !read_bounded(original_rom, segarecomp::patch::kMaxImageBytes, rom, error)) {
    out.error = "The original game file is needed to verify the patch, and it cannot be read" + (error.empty() ? std::string(".") : " (" + error + ").");
    return out;
  }
  if (segarecomp::sha256_hex(rom) != entry.sha256) {
    out.error = "The game file at its recorded location is no longer the original game in the library; the patch was not verified.";
    return out;
  }
  const segarecomp::patch::Applied applied = segarecomp::patch::apply_package(pkg, rom);
  if (!applied.ok) { out.error = refusal_for(applied.error); return out; }
  out.already_installed = entry.find_patch(pkg.info.identity) != nullptr;
  out.ok = true;
  if (package_bytes != nullptr) *package_bytes = std::move(bytes);
  return out;
}

}  // namespace

std::string patch_label(const InstalledPatch &patch) { return patch.name + (patch.version.empty() ? std::string() : " " + patch.version); }

PatchPreview preview_patch(const LibraryEntry &entry, const fs::path &package, const fs::path &original_rom) { return verify(entry, package, original_rom, nullptr); }

PatchInstall install_patch(LibraryEntry &entry, const fs::path &package, const fs::path &original_rom, const fs::path &library_root, long long now) {
  PatchInstall result;
  std::vector<std::uint8_t> bytes;
  const PatchPreview checked = verify(entry, package, original_rom, &bytes);
  if (!checked.ok) { result.error = checked.error; return result; }
  result.identity = checked.info.identity;
  if (entry.platform_id != "genesis") { result.error = "Patches are supported for Genesis / Mega Drive games only."; return result; }
  if (entry.find_patch(checked.info.identity) != nullptr) { result.ok = true; return result; }  // the identical package: nothing to do
  if (entry.patches.size() >= Catalog::max_patches) { result.error = "This game already has the maximum number of installed patches; remove one first."; return result; }
  std::error_code ec;
  const fs::path dir = library_root / "patches";
  fs::create_directories(dir, ec);
  if (ec) { result.error = "The patch folder of the library cannot be created."; return result; }
  const fs::path final_path = dir / (checked.info.identity + ".segapatch");
  const fs::path temp = dir / (checked.info.identity + ".segapatch.partial");
  fs::remove(temp, ec);
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) { out.close(); fs::remove(temp, ec); result.error = "The patch cannot be stored in the library."; return result; }
  }
  fs::rename(temp, final_path, ec);
  if (ec) { fs::remove(temp, ec); result.error = "The patch cannot be stored in the library."; return result; }
  InstalledPatch p;
  p.identity = checked.info.identity;
  p.id = checked.info.id;
  p.version = checked.info.version;
  p.name = checked.info.name;
  p.target_sha256 = checked.info.target_sha256;
  p.width_max = checked.info.widescreen_width_max;
  p.widescreen = checked.info.default_widescreen;
  p.side_style = checked.info.default_side_style;
  p.installed_at = now;
  entry.patches.push_back(std::move(p));
  result.ok = true;
  result.added = true;
  return result;
}

bool remove_patch(LibraryEntry &entry, const std::string &identity, const fs::path &library_root, std::string *error) {
  const auto it = std::find_if(entry.patches.begin(), entry.patches.end(), [&](const InstalledPatch &p) { return p.identity == identity; });
  if (it == entry.patches.end()) { if (error) *error = "That patch is not installed."; return false; }
  if (entry.selected_variant == identity) entry.selected_variant.clear();
  entry.patches.erase(it);
  // Only the exact installed copy, only a plain file inside <library>/patches (never a link, never a path built from anything but the digest).
  if (identity.size() == 64U && is_hex_digest(identity, 64)) {
    const fs::path file = library_root / "patches" / (identity + ".segapatch");
    std::error_code ec;
    const auto status = fs::symlink_status(file, ec);
    if (!ec && status.type() == fs::file_type::regular) fs::remove(file, ec);
  }
  return true;
}

}  // namespace launcher
