#include "patch_library.hpp"

#include "segarecomp/luau_api.hpp"
#include "segarecomp/mod.hpp"
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

// SEG-143-T008: the schema 3 branch of the one verification routine (Luau, native ABI 2 and data-only packages; the build route per class is the launcher core's).
PatchPreview verify_mod(const LibraryEntry &entry, const segarecomp::mod::Package &mp, const fs::path &original_rom, std::vector<std::uint8_t> bytes, std::vector<std::uint8_t> *package_bytes) {
  PatchPreview out;
  const segarecomp::mod::Manifest &m = mp.manifest;
  out.schema3 = true;
  out.package_valid = true;
  out.info.schema_version = 3;
  out.info.id = m.id; out.info.version = m.version; out.info.name = m.name; out.info.platform = m.platform; out.info.author = m.author; out.info.license = m.license; out.info.identity = mp.identity; out.info.purpose = m.purpose; out.info.requires_capabilities = m.capabilities;
  out.mod_trust = segarecomp::mod::trust_class(m);
  if (entry.platform_id != m.platform) { out.error = refusal_for("it is for a different platform than this game"); return out; }
  bool names_revision = false;
  for (const auto &revision : m.revisions) names_revision = names_revision || revision.sha256 == entry.sha256;
  if (!names_revision) {
    out.error = "This mod was made for a different game or revision. It is only applied to the exact original image it was made for, never to a similar one.";
    return out;
  }
  out.fits_game = true;
  if (m.platform != "genesis") { out.error = refusal_for("mods are played for Genesis games only"); return out; }
  const std::optional<segarecomp::mod::Rejection> unsupported = out.mod_trust == "data" ? std::nullopt : out.mod_trust == "native" ? segarecomp::luau::native_scheduled_support(mp) : (segarecomp::luau::live_support(mp) ? segarecomp::luau::scheduled_support(mp) : std::nullopt);
  if (unsupported) { out.error = refusal_for(unsupported->message); return out; }
  std::vector<std::uint8_t> rom;
  std::string error;
  if (original_rom.empty() || !read_bounded(original_rom, segarecomp::patch::kMaxImageBytes, rom, error)) {
    out.error = "The original game file is needed to verify the mod, and it cannot be read" + (error.empty() ? std::string(".") : " (" + error + ").");
    return out;
  }
  const segarecomp::mod::EffectiveImage effective = segarecomp::mod::effective_image(mp, rom);
  if (!effective.ok) { out.error = refusal_for(effective.rejection.message); return out; }
  if (effective.original_sha256 != entry.sha256) { out.error = "The game file at its recorded location is no longer the original game in the library; the mod was not verified."; return out; }
  out.effective_sha256 = effective.effective_sha256;
  if (out.mod_trust == "native") {
    out.info.has_native = true;
    out.info.native_abi = m.abi_native;
    for (const auto &payload : m.payloads) if (payload.kind == "native-c11") { out.info.native_file = payload.path; out.info.native_sha256 = payload.sha256; break; }   // the first provider source names the executable code on the sheet
    if (out.info.native_sha256.empty()) { out.error = refusal_for("the package has no native source"); return out; }
  }
  // Presentation defaults: one shared rule (explicit values win, else the verified-widescreen-geometry fallback), also applied to already-installed records at launch.
  const PresentationDefaults defaults = mod_presentation_defaults(m);
  out.info.default_widescreen = defaults.widescreen;
  out.info.default_side_style = defaults.side_style;
  out.already_installed = entry.find_patch(mp.identity) != nullptr;
  out.ok = true;
  if (package_bytes != nullptr) *package_bytes = std::move(bytes);
  return out;
}

// The single verification routine; `package_bytes` are kept so the published copy is byte-for-byte what was verified.
PatchPreview verify(const LibraryEntry &entry, const fs::path &package, const fs::path &original_rom, std::vector<std::uint8_t> *package_bytes) {
  PatchPreview out;
  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!read_bounded(package, segarecomp::patch::kMaxPackageBytes, bytes, error)) { out.error = refusal_for("the package " + error); return out; }
  segarecomp::patch::Package pkg = segarecomp::patch::read_package(bytes);
  if (!pkg.ok) {   // SEG-143-T008: not a schema 1/2 package: a schema 3 mod package goes through the shared libs/media validation, never a launcher rule
    const segarecomp::mod::Package mp = segarecomp::mod::read_package(bytes);
    if (mp.ok) { return verify_mod(entry, mp, original_rom, std::move(bytes), package_bytes); }
    out.error = refusal_for(mp.rejection.code == "E_SCHEMA_VERSION" ? pkg.error : mp.rejection.message);
    return out;
  }
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

ModSet refused_set(std::string error) {
  ModSet out;
  out.error = std::move(error);
  return out;
}

// The one verification of a launcher mod set: installed members, unchanged stored packages, the exact original, then the shared composition.
ModSet verify_mod_set(const LibraryEntry &entry, const segarecomp::mod::SetDefinition &definition, const fs::path &original_rom, const fs::path &library_root) {
  if (const std::string unavailable = mods_unavailable_reason(entry.platform_id); !unavailable.empty()) return refused_set(unavailable);
  if (definition.platform != entry.platform_id) return refused_set("This mod set is for a different platform than this game.");
  std::vector<segarecomp::mod::Package> packages;
  for (const std::string &identity : definition.members) {
    const InstalledPatch *record = entry.find_patch(identity);
    if (record == nullptr) return refused_set("A mod of this set is not installed for this game (" + identity.substr(0, 12) + ").");
    if (!record->schema3) return refused_set(patch_label(*record) + " is a patch package of an older format and cannot be part of a mod set.");
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!read_bounded(library_root / "patches" / (identity + ".segapatch"), segarecomp::mod::kMaxArchiveBytes, bytes, error)) return refused_set("The installed package of " + patch_label(*record) + " cannot be read: " + error + ".");
    packages.push_back(segarecomp::mod::read_package(bytes));
    if (!packages.back().ok || packages.back().identity != identity) return refused_set("The installed package of " + patch_label(*record) + " changed after it was installed.");
  }
  std::vector<std::uint8_t> rom;
  std::string error;
  if (original_rom.empty() || !read_bounded(original_rom, segarecomp::patch::kMaxImageBytes, rom, error) || segarecomp::sha256_hex(rom) != entry.sha256)
    return refused_set("The original game file is needed to verify the mod set, and the recorded file cannot be read or is no longer the original game.");
  std::vector<const segarecomp::mod::Package *> members;
  for (const segarecomp::mod::Package &p : packages) members.push_back(&p);
  const segarecomp::mod::SetComposition composition = segarecomp::mod::compose_set(members, rom);
  ModSet out;
  out.definition = definition;
  out.violations = composition.violations;
  out.ok = composition.ok;
  out.effective_sha256 = composition.effective_sha256;
  if (!out.ok) {
    out.error = "These mods cannot be used together:";
    for (const segarecomp::mod::SetViolation &v : composition.violations) out.error += "\n" + v.message + ".";
  }
  return out;
}

fs::path mod_set_file(const LibraryEntry &entry, const std::string &name, const fs::path &library_root) { return library_root / "mod-sets" / entry.sha256 / (name + ".modset"); }

}  // namespace

ModSet store_mod_set(const LibraryEntry &entry, const std::string &name, const std::vector<std::string> &identities, const fs::path &original_rom, const fs::path &library_root) {
  if (!segarecomp::mod::set_name_ok(name) || !is_hex_digest(entry.sha256, 64)) return refused_set("A mod set name uses lower-case letters, digits and single dashes (at most 64).");
  // The shared parser is the one rule for what a definition may hold (member count, repeats, form).
  const segarecomp::mod::SetDefinitionResult parsed = segarecomp::mod::parse_set_definition(segarecomp::mod::serialize_set_definition({entry.platform_id, identities}));
  if (!parsed.ok) return refused_set("This mod set cannot be stored: " + parsed.rejection.message + ".");
  ModSet out = verify_mod_set(entry, parsed.definition, original_rom, library_root);
  if (!out.ok) return out;
  const fs::path file = mod_set_file(entry, name, library_root);
  fs::path temp = file;
  temp += ".partial";
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  fs::remove(temp, ec);
  {
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    const std::string text = segarecomp::mod::serialize_set_definition(parsed.definition);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) { stream.close(); fs::remove(temp, ec); return refused_set("The mod set cannot be stored in the library."); }
  }
  fs::rename(temp, file, ec);
  if (ec) { fs::remove(temp, ec); return refused_set("The mod set cannot be stored in the library."); }
  return out;
}

ModSet load_mod_set(const LibraryEntry &entry, const std::string &name, const fs::path &original_rom, const fs::path &library_root) {
  if (!segarecomp::mod::set_name_ok(name) || !is_hex_digest(entry.sha256, 64)) return refused_set("That is not a mod set name.");
  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!read_bounded(mod_set_file(entry, name, library_root), segarecomp::mod::kMaxSetDefinitionBytes, bytes, error)) return refused_set("The mod set cannot be read: " + error + ".");
  const segarecomp::mod::SetDefinitionResult parsed = segarecomp::mod::parse_set_definition(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
  if (!parsed.ok) return refused_set("The stored mod set is not readable: " + parsed.rejection.message + ".");
  return verify_mod_set(entry, parsed.definition, original_rom, library_root);
}

std::string patch_label(const InstalledPatch &patch) { return patch.name + (patch.version.empty() ? std::string() : " " + patch.version); }

std::string mods_unavailable_reason(const std::string &platform_id) {
  if (platform_id == "genesis") return {};
  const char *name = platform_id == "master-system" ? "Master System" : platform_id == "sega-cd" ? "Sega CD" : "this kind of";
  return std::string("Mods are available for Genesis / Mega Drive games only. This is a ") + name + " game, so a mod package cannot be installed or played here. Nothing about this game is affected.";
}

std::string mod_kind_label(const InstalledPatch &patch) {
  if (patch.has_native()) return "Native C source (runs with host-process permissions, not sandboxed)";
  if (patch.schema3) return patch.mod_trust == "luau" ? "Luau script" : "ROM data edits";
  return "Patched game image";
}

std::string mod_capabilities_label(const InstalledPatch &patch) {
  std::string caps;
  for (const auto &c : {std::pair<unsigned, const char *>{segarecomp::patch::kCapFrameWorkRam, "work RAM snapshot"}, {segarecomp::patch::kCapFrameVdp, "VDP state"},
                        {segarecomp::patch::kCapSceneryWords, "scenery"}, {segarecomp::patch::kCapOverlayRgba, "overlay"}})
    if ((patch.native_caps & c.first) != 0U) caps += (caps.empty() ? "" : ", ") + std::string(c.second);
  return caps;
}

std::string mod_refusal_next_step(const std::string &refusal) {
  const auto has = [&](const char *part) { return refusal.find(part) != std::string::npos; };
  if (has("different game or revision")) return "Choose the package made for this exact game and revision. A mod is never applied to a similar image.";
  if (has("different platform") || has("Genesis games only") || has("Genesis / Mega Drive games only")) return "Choose a package made for this platform. Mods are available for Genesis games only.";
  if (has("original game file is needed") || has("no longer the original")) return "Find the original game file (Game > Advanced > Find game file), then choose the package again.";
  if (has("changed after it was verified")) return "Nothing was installed. Choose the package again so it is verified as it is now.";
  if (has("executable code")) return "Choose the package again and approve it, or leave it uninstalled.";
  if (has("maximum number")) return "Remove a mod you no longer use, then install this one.";
  if (has("read-only") || has("newer Segarecomp")) return "Use the newer Segarecomp that wrote this library, or leave mods unchanged.";
  return "Choose a different package, or ask its author for one made for this game. Nothing was installed.";
}

ModStatus mod_status(const InstalledPatch &patch, const ModStatusInput &in) {
  ModStatus s;
  if (!in.package_present) {
    s.code = "incompatible_package_missing"; s.label = "Incompatible"; s.tone = ModTone::err;
    s.reason = "The installed package file is missing from the library, so this mod cannot be prepared or played.";
    s.next = "Remove it and install the package again, or return to the original game.";
  } else if (patch.has_native() && !patch.native_approved) {
    s.code = "needs_approval"; s.label = "Needs approval"; s.tone = ModTone::warn;
    s.reason = "This mod contains executable code that has not been approved on this computer, so it is never prepared or played.";
    s.next = "Install the package again and approve it, or remove it.";
  } else if (in.selected && in.failed) {
    s.code = "preparation_failed"; s.label = "Selected - preparation failed"; s.tone = ModTone::err;
    s.reason = in.failure.empty() ? std::string("The last preparation of this mod did not finish.") : in.failure;
    s.next = "Retry preparing the selected mod, or return to the original game.";
  } else if (in.selected) {
    s.code = in.built ? "selected_prepared" : "selected_needs_preparation";
    s.label = in.built ? "Selected - Prepared" : "Selected - Needs preparation";
    s.tone = in.built ? ModTone::ok : ModTone::warn;
    if (!in.built) s.next = "Prepare selected mod. The original game stays playable meanwhile; nothing falls back silently.";
  } else {
    s.code = in.built ? "installed_prepared" : "installed";
    s.label = in.built ? "Installed - Prepared" : "Installed";
  }
  return s;
}

std::string prepare_variant_launch(LibraryEntry &entry, RomView &view, bool *record_changed) {
  if (record_changed != nullptr) *record_changed = false;
  PresentationDefaults verified;
  if (const std::string refusal = launch_refusal(view, &verified); !refusal.empty()) return refusal;
  if (!view.luau_mod() || (verified.widescreen == view.patch_widescreen && verified.side_style == view.patch_side_style)) return {};
  view.patch_widescreen = verified.widescreen;
  view.patch_side_style = verified.side_style;
  if (InstalledPatch *record = entry.find_patch(view.luau_identity); record != nullptr && (record->widescreen != verified.widescreen || record->side_style != verified.side_style)) {
    record->widescreen = verified.widescreen;
    record->side_style = verified.side_style;
    if (record_changed != nullptr) *record_changed = true;
  }
  return {};
}

PatchPreview preview_patch(const LibraryEntry &entry, const fs::path &package, const fs::path &original_rom) { return verify(entry, package, original_rom, nullptr); }

PatchInstall install_patch(LibraryEntry &entry, const fs::path &package, const fs::path &original_rom, const fs::path &library_root, long long now, bool trust_native) {
  PatchInstall result;
  std::vector<std::uint8_t> bytes;
  const PatchPreview checked = verify(entry, package, original_rom, &bytes);
  if (!checked.ok) { result.error = checked.error; return result; }
  result.identity = checked.info.identity;
  if (checked.info.has_native && !trust_native) { result.error = "This patch contains executable code. It is installed only after you approve it."; return result; }
  if (entry.platform_id != "genesis") { result.error = "Patches are supported for Genesis / Mega Drive games only."; return result; }
  if (InstalledPatch *existing = const_cast<InstalledPatch *>(entry.find_patch(checked.info.identity)); existing != nullptr) {   // the identical package
    if (existing->has_native() && trust_native && !existing->native_approved) { existing->native_approved = true; result.added = true; }   // an unapproved record is approved by the user's explicit action
    if (existing->schema3) { existing->widescreen = checked.info.default_widescreen; existing->side_style = checked.info.default_side_style; }   // the verified package decides its presentation defaults; a stale record is refreshed in place
    result.ok = true;
    return result;
  }
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
  p.target_sha256 = checked.schema3 ? checked.effective_sha256 : checked.info.has_rom ? checked.info.target_sha256 : entry.sha256;   // a native-only package compiles the unchanged original image
  p.schema3 = checked.schema3;
  p.mod_trust = checked.mod_trust;
  p.effective_sha256 = checked.effective_sha256;
  if (checked.info.has_native) {
    p.native_source_sha256 = checked.info.native_sha256;
    p.native_abi = checked.info.native_abi;
    p.native_caps = checked.info.native_caps;
    p.author = checked.info.author;
    p.license = checked.info.license;
    p.native_approved = trust_native;   // set only by the code-bearing install sheet (the user's approval of this exact package identity)
  }
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
