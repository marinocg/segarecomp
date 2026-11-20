// SEG-122 (ADR 0266): the actions behind the Patches / Variants card: choose a package, verify it against the original game, install it, select a
// variant, remove a patch. Verification is patch_library.cpp (the same libs/media code `segarecomp patch ...` runs); nothing here edits a ROM.

#include "app.hpp"

#include "app_helpers.hpp"

#include <algorithm>

namespace fs = std::filesystem;
using namespace ui;

namespace launcher {

using namespace uih;

namespace {
fs::path from_u8(const std::string &text) { return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.c_str()))); }
std::string short_hash(const std::string &digest) { return digest.size() >= 16 ? digest.substr(0, 16) + "..." : digest; }
}  // namespace

// A refusal keeps the product's typed message and adds the next step; the Mods tab shows it in the error tone until the next outcome.
void App::refuse_patch(const std::string &id, const std::string &error) {
  patch_notices_[id] = error;
  patch_refused_.insert(id);
}

ModStatus App::mod_status_of(const LibraryEntry &entry, const StatusInfo &info, const InstalledPatch &patch) const {
  ModStatusInput in;
  in.selected = entry.selected_variant == patch.identity;
  for (const auto &b : info.patches) if (b.identity == patch.identity) in.built = b.built;
  std::error_code ec;
  in.package_present = fs::is_regular_file(library_dir_ / "patches" / (patch.identity + ".segapatch"), ec);
  if (in.selected && info.status == GameStatus::failed) {
    in.failed = true;
    if (const auto f = failures_.find(entry.id()); f != failures_.end()) in.failure = f->second;
  }
  return mod_status(patch, in);
}

std::string App::mod_switch_block(const LibraryEntry &entry) const {
  if (catalog_.read_only()) return "This library was written by a newer Segarecomp, so mods cannot be changed here.";
  if (is_building(entry.id()) || is_queued(entry.id()) || game_ != nullptr || maintenance_running_) return "Wait for the running preparation or game to finish before switching version or removing a mod.";
  return {};
}

void App::patch_chosen(const std::string &id, const std::string &path) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || entry->platform_id != "genesis") return;
  if (catalog_.read_only()) { show_message("The library is read-only", {"This library was written by a newer Segarecomp, so patches cannot be installed or changed here."}); return; }
  const fs::path rom = usable_rom_path(*entry);
  const PatchPreview preview = preview_patch(*entry, from_u8(path), rom);
  if (!preview.ok) {
    refuse_patch(id, preview.error);
    trace("patch=refused");
    show_message("This mod cannot be installed", {preview.error, "Next step: " + mod_refusal_next_step(preview.error), "Nothing was installed or approved."});
    return;
  }
  patch_pending_ = from_u8(path);
  patch_expected_identity_ = preview.info.identity;
  modal_ = Modal{};
  modal_.kind = Modal::Kind::install_patch;
  modal_.title = "Install " + preview.info.name + " " + preview.info.version + "?";
  modal_.ids = {id};
  modal_.target = preview.info.identity;
  const std::string kind = preview.schema3 ? (preview.mod_trust == "native" ? "Schema 3 mod with native C source" : preview.mod_trust == "luau" ? "Schema 3 mod with a Luau script" : "Schema 3 mod with ROM data edits")
                                            : (preview.info.has_native ? (preview.info.has_rom ? "Patch package with a patched image and native C source" : "Patch package with native C source") : "Patch package with a patched image");
  const std::string range = preview.info.widescreen_width_max < 0 ? std::string("not stated by the author") : "up to " + std::to_string(preview.info.widescreen_width_max) + " on the Widescreen Width scale";
  // 1. the verification result comes BEFORE any consent: what was checked, and that nothing is installed yet
  modal_.lines = {"Verified: this package is made for the exact original image of this game (SHA-256 match) and every payload checked out. Nothing is installed yet.",
                  "Package: " + preview.info.name + ", version " + preview.info.version + ", author " + (preview.info.author.empty() ? std::string("not stated") : preview.info.author) + (preview.info.license.empty() ? std::string() : " (" + preview.info.license + ")") + ".",
                  "Target: " + entry->title() + ", original image " + (entry->sha256.size() >= 12 ? entry->sha256.substr(0, 12) + "..." : entry->sha256) + ". Kind: " + kind + ".",
                  std::string(preview.already_installed ? "This exact package is already installed on this game." : "Purpose: " + (preview.info.purpose.empty() ? std::string("not stated") : preview.info.purpose) + (preview.schema3 ? std::string() : ". Validated width: " + range) + ".")};
  if (preview.info.has_native) {   // SEG-124 (ADR 0267): this package CARRIES EXECUTABLE SOURCE; the sheet is the user's approval of its exact identity
    std::string caps;
    for (const auto &c : {std::pair<unsigned, const char *>{segarecomp::patch::kCapFrameWorkRam, "work RAM snapshot"}, {segarecomp::patch::kCapFrameVdp, "VDP state"},
                          {segarecomp::patch::kCapSceneryWords, "scenery"}, {segarecomp::patch::kCapOverlayRgba, "overlay"}})
      if ((preview.info.native_caps & c.first) != 0U) caps += (caps.empty() ? "" : ", ") + std::string(c.second);
    modal_.code_bearing = true;
    if (preview.schema3) {   // SEG-143-T008: a schema 3 mod names the capabilities it requires; the sheet lists exactly those
      caps.clear();
      for (const std::string &name : preview.info.requires_capabilities) caps += (caps.empty() ? "" : ", ") + name;
    }
    modal_.lines.push_back(std::string("Executable code, host-process permissions: this ") + (preview.schema3 ? "mod" : "patch") + " contains C source by " + preview.info.author + ". Segarecomp compiles it into a separate modded program that runs with the full permissions of your user account. It is NOT sandboxed, and a verified file hash does not prove the code is trustworthy. Approve it only if you trust its source.");
    modal_.lines.push_back("Reads: " + (caps.empty() ? std::string("nothing stated") : caps) + (preview.schema3 ? ". Native mod ABI " : ". Native patch ABI ") + std::to_string(preview.info.native_abi) + (preview.schema3 ? ". It plays as its own modded program, never combined with another mod." : "."));
    modal_.lines.push_back("Exact package identity (the approval is bound to this): " + preview.info.identity);
    modal_.lines.push_back("Native source SHA-256: " + preview.info.native_sha256);
    modal_.lines.push_back("Approving applies to this exact package only; a changed package must be approved again. There is no blanket or per-author approval. Nothing is compiled or run until you prepare the selected mod. Vanilla is never affected.");
  } else {
    modal_.lines.push_back("Package identity " + short_hash(preview.info.identity) + ". This package carries no executable code.");
  }
  trace("patch=preview_ok");
}

void App::patch_install_confirmed(const std::string &id, const std::string &package, bool trust_native) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || catalog_.read_only()) return;
  const PatchInstall result = install_patch(*entry, from_u8(package), usable_rom_path(*entry), library_dir_, now_seconds(), trust_native);
  if (result.ok && !patch_expected_identity_.empty() && result.identity != patch_expected_identity_) {
    // the file changed between the preview and the confirmation: what would be installed is not what was shown
    if (result.added) { std::string ignored; (void)remove_patch(*entry, result.identity, library_dir_, &ignored); }
    refuse_patch(id, "The package changed after it was verified; it was not installed. Choose it again.");
    show_message("This mod cannot be installed", {patch_notices_[id], "Next step: " + mod_refusal_next_step(patch_notices_[id]), "Nothing was installed or approved."});
    return;
  }
  if (!result.ok) {
    refuse_patch(id, result.error);
    trace("patch=install_failed");
    show_message("This mod cannot be installed", {result.error, "Next step: " + mod_refusal_next_step(result.error), "Nothing was installed or approved."});
    return;
  }
  patch_refused_.erase(id);
  patch_notices_[id] = result.added ? "Mod installed. Choose Use this mod to select it, then Prepare selected mod." : "That exact mod was already installed.";
  save_catalog();
  request_status_refresh();
  trace(result.added ? "patch=installed" : "patch=already_installed");
  toast(result.added ? "Mod installed." : "That mod was already installed.");
}

void App::select_variant(const std::string &id, const std::string &identity) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || entry->selected_variant == identity || catalog_.read_only()) return;
  if (!identity.empty() && entry->find_patch(identity) == nullptr) return;   // only an installed patch can be selected
  if (is_building(id) || is_queued(id) || (game_ && game_id_ == id)) { toast("Wait for the running build or game to finish before switching version.", true); return; }
  const InstalledPatch *before = entry->selected_patch();
  const std::string from = before == nullptr ? std::string() : patch_label(*before);
  entry->selected_variant = identity;
  patch_refused_.erase(id);
  if (identity.empty()) patch_notices_[id] = from.empty() ? std::string() : "Original game selected. " + from + " stays installed; select it again whenever you like.";
  else if (!from.empty()) patch_notices_[id] = "Switched from " + from + " to " + patch_label(*entry->find_patch(identity)) + ". Only one mod is used at a time; mods are never combined.";
  else patch_notices_[id].clear();
  save_catalog();
  request_status_refresh();
  trace(identity.empty() ? "variant=vanilla" : "variant=patched");
}

void App::ask_remove_patch(const LibraryEntry &entry, const std::string &identity) {
  const InstalledPatch *patch = entry.find_patch(identity);
  if (patch == nullptr) return;
  modal_ = Modal{};
  modal_.kind = Modal::Kind::remove_patch;
  modal_.title = "Remove " + patch_label(*patch) + "?";
  modal_.ids = {entry.id()};
  modal_.target = identity;
  modal_.delete_cache = false;
}

void App::patch_remove_confirmed(const Modal &sheet) {
  if (sheet.ids.empty()) return;
  LibraryEntry *entry = catalog_.find_id(sheet.ids.front());
  if (entry == nullptr || catalog_.read_only()) return;
  if (is_building(entry->id()) || is_queued(entry->id()) || (game_ && game_id_ == entry->id())) { toast("Wait for the running build or game to finish first.", true); return; }
  // The build folder of the patched variant, remembered before the record disappears: only this one folder can be offered for deletion.
  fs::path built_dir;
  if (sheet.delete_cache) {
    const RomView variant = view_for_variant(*entry, sheet.target);
    built_dir = entry_dir(variant, layout_);
  }
  std::string error;
  if (!remove_patch(*entry, sheet.target, library_dir_, &error)) { show_message("The mod could not be removed", {error}); return; }
  patch_refused_.erase(entry->id());
  patch_notices_[entry->id()] = "Mod removed. The original game and its Vanilla version are unchanged.";
  if (!built_dir.empty()) {
    const std::string key = u8s(built_dir.filename());
    entry->builds.erase(std::remove_if(entry->builds.begin(), entry->builds.end(), [&](const BuildRecord &b) { return b.key == key && !b.variant.empty(); }), entry->builds.end());
    Plan plan;
    plan.kind = PlanKind::superseded;
    plan.entry_ids = {entry->id()};
    plan.items.push_back(PlanItem{built_dir, 0, true});
    run_plan(plan);   // the same validated executor as Storage: it refuses a folder that is busy, a link or anything outside the managed build folders
  }
  save_catalog();
  request_status_refresh();
  request_scan();
  trace("patch=removed");
  toast("Mod removed.");
}

void App::patch_install_from_sheet(const Modal &sheet) {
  patch_install_confirmed(sheet.ids.empty() ? std::string() : sheet.ids.front(), u8s(patch_pending_), sheet.code_bearing);
}

void App::patch_modal_confirm() {
  const Modal m = modal_;
  if (m.kind == Modal::Kind::install_patch) {
    modal_ = Modal{};
    patch_install_from_sheet(m);
  } else if (m.kind == Modal::Kind::remove_patch) {
    modal_ = Modal{};
    patch_remove_confirmed(m);
  }
}

}  // namespace launcher
