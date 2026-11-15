// SEG-143-T008: the launcher's LOCAL schema 3 flow for a NATIVE ABI 2 mod, against the real product build driver and a fabricated, redistributable image (no commercial input):
// install (exact-ROM verification, schema/capability validation, native approval tied to the package identity, immutable stored copy) -> select -> build (re-verified at build time) -> play-ready
// entry, with Vanilla and its saves untouched, and the fail-closed negatives (wrong ROM, no approval, tampered stored bytes, wrong identity, uninstall restores Vanilla).
//
//   launcher_schema3_flow_test <install-root> <rom> <other-rom> <native.segapatch> <wrong-rom.segapatch>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "launcher_core.hpp"
#include "library_catalog.hpp"
#include "patch_library.hpp"
#include "segarecomp/sha256.hpp"

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}
void set_env(const char *name, const std::string &value) { setenv(name, value.c_str(), 1); }
std::string slurp(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}
std::string digest_of(const fs::path &path) {
  const std::string bytes = slurp(path);
  return segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}
BuildOutcome build(const RomView &rom, const Layout &layout, std::string *message = nullptr) {
  BuildJob job(rom, layout);
  for (int waited = 0; waited < 900000 && !job.finished(); waited += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  job.wait();
  if (message != nullptr) *message = job.message();
  return job.outcome();
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 6) {
    std::cerr << "usage: launcher_schema3_flow_test <install-root> <rom> <other-rom> <native.segapatch> <wrong-rom.segapatch>\n";
    return 2;
  }
  const fs::path root = argv[1], rom_path = argv[2], other_path = argv[3], native = argv[4], wrong = argv[5];
  const fs::path cache = root.parent_path() / "cache", library = root.parent_path() / "library";
  fs::create_directories(cache);
  fs::create_directories(library);
  set_env("SEGARECOMP_HOME", root.string());
  set_env("SEGARECOMP_CACHE_DIR", cache.string());
  set_env("SEGARECOMP_LIBRARY_DIR", library.string());
  const Layout layout = locate_layout();
  expect(layout.problem.empty(), "the install root is complete: " + layout.problem);

  RomView vanilla;
  vanilla.path = rom_path;
  vanilla.sha256 = digest_of(rom_path);
  vanilla.title = "Fixture";
  vanilla.platform_id = "genesis";
  vanilla.supported_platform = true;
  Catalog catalog(library / "library.json");
  catalog.load();
  Catalog::ImportResult imported = catalog.import_rom(vanilla, 1000);
  expect(imported.entry != nullptr, "the fabricated game is in the library: " + imported.error);
  if (imported.entry == nullptr) return 1;
  LibraryEntry *entry = imported.entry;

  // ---- Vanilla first: its executable and save folder are what must never change ----
  std::string message;
  const fs::path vanilla_entry = entry_dir(vanilla, layout);
  expect(build(vanilla, layout, &message) == BuildOutcome::succeeded && entry_ready_for(vanilla_entry, vanilla, layout), "the Vanilla program builds: " + message);
  const std::string vanilla_game = digest_of(entry_executable(vanilla_entry)), vanilla_meta = slurp(vanilla_entry / "metadata.json");
  const fs::path vanilla_saves = genesis_save_dir(vanilla);
  fs::create_directories(vanilla_saves);
  { std::ofstream(vanilla_saves / "slot0.sav") << "vanilla save"; }

  // ---- install: wrong ROM, validation, approval ----
  PatchInstall refused = install_patch(*entry, wrong, rom_path, library, 1001, true);
  expect(!refused.ok && entry->patches.empty() && refused.error.find("different game or revision") != std::string::npos, "a package made for another image is refused and nothing is installed: " + refused.error);
  const PatchPreview preview = preview_patch(*entry, native, rom_path);
  expect(preview.ok && preview.schema3 && preview.mod_trust == "native" && preview.info.has_native && preview.info.native_abi == 2 && !preview.info.author.empty() && !preview.effective_sha256.empty(),
         "the native schema 3 package verifies against this exact game and shows what the approval sheet needs: " + preview.error);
  PatchInstall unapproved = install_patch(*entry, native, rom_path, library, 1002, false);
  expect(!unapproved.ok && entry->patches.empty() && !fs::exists(library / "patches" / (preview.info.identity + ".segapatch")), "native code is not installed without approval: " + unapproved.error);
  PatchInstall installed = install_patch(*entry, native, rom_path, library, 1003, true);
  expect(installed.ok && installed.added && entry->patches.size() == 1 && entry->patches[0].schema3 && entry->patches[0].native_approved && entry->patches[0].mod_trust == "native" && entry->patches[0].identity == preview.info.identity,
         "the approved native package is installed, bound to its identity: " + installed.error);
  const fs::path stored = library / "patches" / (preview.info.identity + ".segapatch");
  expect(fs::is_regular_file(stored) && digest_of(stored) == digest_of(native), "the stored copy is byte-for-byte the verified package");
  expect(!catalog.save() ? false : true, "the library saves");
  {
    Catalog again(library / "library.json");
    again.load();
    const LibraryEntry *e2 = again.find("genesis", vanilla.sha256);
    expect(e2 != nullptr && e2->patches.size() == 1 && e2->patches[0].schema3 && e2->patches[0].mod_trust == "native" && e2->patches[0].native_approved && e2->patches[0].effective_sha256 == preview.effective_sha256, "the mod record (kind, approval, effective image) survives a library reload");
  }

  // ---- select: exactly one mod, never a patch variant ----
  entry->selected_variant = installed.identity;
  const RomView mod = view_from_entry(*entry);
  expect(mod.luau_mod() && mod.luau_native && mod.luau_approved && !mod.patched() && mod.luau_identity == installed.identity && mod.effective_sha256() == preview.effective_sha256 && mod.sha256 == vanilla.sha256,
         "selecting the installed package yields one native mod over the ORIGINAL game, not a patch");
  expect(entry_dir(mod, layout) != vanilla_entry && genesis_save_dir(mod) != vanilla_saves && !genesis_save_dir(mod).empty(), "the mod has its own cache entry and its own saves");
  expect(verify_installed_mod(mod).empty(), "the installed mod re-verifies");

  // ---- no approval, no native build ----
  {
    RomView unapproved_view = mod;
    unapproved_view.luau_approved = false;
    expect(!verify_installed_mod(unapproved_view).empty(), "an unapproved native mod is refused at the reverification");
    expect(build(unapproved_view, layout, &message) != BuildOutcome::succeeded && !entry_ready_for(entry_dir(unapproved_view, layout), unapproved_view, layout), "an unapproved native mod builds nothing: " + message);
  }

  // ---- build the exact selected package ----
  const fs::path mod_entry = entry_dir(mod, layout);
  expect(build(mod, layout, &message) == BuildOutcome::succeeded, "the approved native mod builds through the launcher pipeline: " + message);
  expect(entry_ready_for(mod_entry, mod, layout) && fs::is_regular_file(entry_executable(mod_entry)), "the published entry is a ready, executable program for exactly this package");
  const std::string meta = slurp(mod_entry / "metadata.json");
  expect(meta.find("\"mod_kind\":\"native\"") != std::string::npos && meta.find("\"luau_identity\":\"" + installed.identity + "\"") != std::string::npos && meta.find("luau_bytecode_digest") == std::string::npos, "the record names the native package and the image it was built for");
  expect(!entry_ready_for(mod_entry, vanilla, layout) && !entry_ready_for(vanilla_entry, mod, layout), "a Vanilla program is never the mod's, nor the reverse");
  RomView other_identity = mod;
  other_identity.luau_identity = std::string(64, 'a');
  expect(!entry_ready_for(mod_entry, other_identity, layout) && !verify_installed_mod(other_identity).empty(), "another identity never accepts this program and fails the reverification");
  RomView as_luau = mod;
  as_luau.luau_native = false;
  expect(!entry_ready_for(mod_entry, as_luau, layout) && !verify_installed_mod(as_luau).empty(), "a native program is never reused as a Luau variant");
  expect(digest_of(entry_executable(vanilla_entry)) == vanilla_game && slurp(vanilla_entry / "metadata.json") == vanilla_meta && slurp(vanilla_saves / "slot0.sav") == "vanilla save", "the Vanilla executable, record and saves are untouched");

  if (const char *report = std::getenv("SEGARECOMP_S3_REPORT"); report != nullptr && *report != '\0') {   // the integrated acceptance test plays the programs this flow built
    std::ofstream out(report, std::ios::trunc);
    out << "vanilla=" << entry_executable(vanilla_entry).string() << "\nmod=" << entry_executable(mod_entry).string() << "\nidentity=" << installed.identity << "\n";
  }

  // ---- tampered installed bytes are refused at build time ----
  {
    const std::string original = slurp(stored);
    std::string altered = original;
    altered[altered.size() / 2] = static_cast<char>(altered[altered.size() / 2] ^ 0x55);
    { std::ofstream out(stored, std::ios::binary | std::ios::trunc); out << altered; }
    expect(!verify_installed_mod(mod).empty(), "tampered stored bytes fail the reverification");
    RomView tampered = mod;
    expect(build(tampered, layout, &message) != BuildOutcome::succeeded, "tampered stored bytes never build: " + message);
    { std::ofstream out(stored, std::ios::binary | std::ios::trunc); out << original; }
    expect(verify_installed_mod(mod).empty(), "the unaltered package verifies again");
  }
  // a different (wrong-ROM) package under this identity is not the approved one either
  {
    const std::string original = slurp(stored);
    { std::ofstream out(stored, std::ios::binary | std::ios::trunc); out << slurp(wrong); }
    expect(!verify_installed_mod(mod).empty(), "another package stored under the selected identity is refused");
    { std::ofstream out(stored, std::ios::binary | std::ios::trunc); out << original; }
  }

  // ---- uninstall restores Vanilla ----
  expect(remove_patch(*entry, installed.identity, library) && entry->selected_variant.empty() && !fs::exists(stored), "uninstalling removes the package and selects Vanilla");
  const RomView back = view_from_entry(*entry);
  expect(!back.luau_mod() && !back.patched() && back.effective_sha256() == vanilla.sha256 && genesis_save_dir(back) == vanilla_saves && entry_ready_for(vanilla_entry, vanilla, layout) && digest_of(entry_executable(vanilla_entry)) == vanilla_game && slurp(vanilla_saves / "slot0.sav") == "vanilla save", "Vanilla is selected again and plays its untouched program and saves");
  (void)other_path;
  if (failures == 0) std::cout << "launcher_schema3_flow_test: ok\n";
  return failures == 0 ? 0 : 1;
}
