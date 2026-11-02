#pragma once

// SEG-122 (ADR 0266): installing and removing widescreen patch packages of one library game. Orchestration only: every verification is the
// shared libs/media implementation (`segarecomp patch ...` runs the same code). A package is data; installing it never changes the original
// game file, never selects the Patched variant and never starts a build.

#include "library_catalog.hpp"
#include "segarecomp/patch.hpp"

#include <filesystem>
#include <string>

namespace launcher {

struct PatchPreview {
  bool ok = false;               // the package verifies completely against THIS game's original image
  std::string error;             // plain-language refusal when !ok
  segarecomp::patch::PackageInfo info;   // valid when the package itself parsed (even if it does not fit this game)
  bool package_valid = false;
  bool fits_game = false;
  bool already_installed = false;
};

// Full verification without installing anything: ZIP structure, manifest, payload digest, the exact original bytes, BPS application, patched digest.
[[nodiscard]] PatchPreview preview_patch(const LibraryEntry &entry, const std::filesystem::path &package, const std::filesystem::path &original_rom);

struct PatchInstall {
  bool ok = false;
  bool added = false;            // false: the identical package was already installed
  std::string error;
  std::string identity;
};

// Verifies exactly like preview_patch, then publishes a copy of the very bytes that were verified to `<library_root>/patches/<identity>.segapatch`
// (temporary file + rename) and records it on the entry. A failure leaves neither a file nor a record behind.
PatchInstall install_patch(LibraryEntry &entry, const std::filesystem::path &package, const std::filesystem::path &original_rom,
                           const std::filesystem::path &library_root, long long now);

// Removes the record and the installed package file; selects Vanilla if this patch was selected. The original ROM, the Vanilla build, the
// user's settings and recordings are untouched. Its published build stays in the cache until storage cleanup removes it (it is no longer current).
bool remove_patch(LibraryEntry &entry, const std::string &identity, const std::filesystem::path &library_root, std::string *error = nullptr);

// "<name> <version>" label of an installed patch.
[[nodiscard]] std::string patch_label(const InstalledPatch &patch);

}  // namespace launcher
