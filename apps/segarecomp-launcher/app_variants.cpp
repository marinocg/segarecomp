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

void App::patch_chosen(const std::string &id, const std::string &path) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || entry->platform_id != "genesis") return;
  if (catalog_.read_only()) { show_message("The library is read-only", {"This library was written by a newer Segarecomp, so patches cannot be installed or changed here."}); return; }
  const fs::path rom = usable_rom_path(*entry);
  const PatchPreview preview = preview_patch(*entry, from_u8(path), rom);
  if (!preview.ok) {
    patch_notices_[id] = preview.error;
    trace("patch=refused");
    show_message("This patch cannot be installed", {preview.error});
    return;
  }
  patch_pending_ = from_u8(path);
  patch_expected_identity_ = preview.info.identity;
  modal_ = Modal{};
  modal_.kind = Modal::Kind::install_patch;
  modal_.title = "Install " + preview.info.name + " " + preview.info.version + "?";
  modal_.ids = {id};
  modal_.target = preview.info.identity;
  modal_.lines = {};
  const std::string range = preview.info.widescreen_width_max < 0 ? std::string("not stated by the author") : "up to " + std::to_string(preview.info.widescreen_width_max) + " on the Widescreen Width scale";
  modal_.lines = {"Compatible with this game: yes. It matches the exact original image (SHA-256 verified) and every payload verified (a patch's result matches its published digest).",
                  "Purpose: " + preview.info.purpose + ". Validated width: " + range + ".",
                  std::string(preview.already_installed ? "This exact package is already installed on this game." : "Package identity " + short_hash(preview.info.identity) + ".")};
  if (preview.info.has_native)   // SEG-124: a data-only descriptor that selects an audited component of this release; nothing from the package is ever executed
    modal_.lines.push_back(std::string("Native widescreen component: ") + ((preview.info.native_components & segarecomp::patch::kNativeComponentHud) != 0U ? "HUD " : "") +
                           ((preview.info.native_components & segarecomp::patch::kNativeComponentScenery) != 0U ? "world scenery " : "") +
                           "(" + preview.info.native_capability + ", built into this release). The package only selects it; no code is loaded from it.");
  trace("patch=preview_ok");
}

void App::patch_install_confirmed(const std::string &id, const std::string &package) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || catalog_.read_only()) return;
  const PatchInstall result = install_patch(*entry, from_u8(package), usable_rom_path(*entry), library_dir_, now_seconds());
  if (result.ok && !patch_expected_identity_.empty() && result.identity != patch_expected_identity_) {
    // the file changed between the preview and the confirmation: what would be installed is not what was shown
    if (result.added) { std::string ignored; (void)remove_patch(*entry, result.identity, library_dir_, &ignored); }
    patch_notices_[id] = "The package changed after it was verified; it was not installed. Choose it again.";
    show_message("This patch cannot be installed", {patch_notices_[id]});
    return;
  }
  if (!result.ok) {
    patch_notices_[id] = result.error;
    trace("patch=install_failed");
    show_message("This patch cannot be installed", {result.error});
    return;
  }
  patch_notices_[id] = result.added ? "Patch installed. Choose Patched, then Build patched version." : "That exact patch was already installed.";
  save_catalog();
  request_status_refresh();
  trace(result.added ? "patch=installed" : "patch=already_installed");
  toast(result.added ? "Patch installed." : "That patch was already installed.");
}

void App::select_variant(const std::string &id, const std::string &identity) {
  LibraryEntry *entry = catalog_.find_id(id);
  if (entry == nullptr || entry->selected_variant == identity || catalog_.read_only()) return;
  if (!identity.empty() && entry->find_patch(identity) == nullptr) return;   // only an installed patch can be selected
  if (is_building(id) || is_queued(id) || (game_ && game_id_ == id)) { toast("Wait for the running build or game to finish before switching version.", true); return; }
  entry->selected_variant = identity;
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
  if (!remove_patch(*entry, sheet.target, library_dir_, &error)) { show_message("The patch could not be removed", {error}); return; }
  patch_notices_[entry->id()] = "Patch removed. The original game and the Vanilla version are unchanged.";
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
  toast("Patch removed.");
}

void App::patch_modal_confirm() {
  const Modal m = modal_;
  if (m.kind == Modal::Kind::install_patch) {
    modal_ = Modal{};
    patch_install_confirmed(m.ids.empty() ? std::string() : m.ids.front(), u8s(patch_pending_));
  } else if (m.kind == Modal::Kind::remove_patch) {
    modal_ = Modal{};
    patch_remove_confirmed(m);
  }
}

}  // namespace launcher
