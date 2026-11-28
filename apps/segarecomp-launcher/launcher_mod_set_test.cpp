// SEG-141-T001 (ADR 0326): the launcher library's persisted mod sets: the SHARED definition format and composition engine over really installed packages of one library game. No build, no window.
// Fabricated image and packages only.   launcher_mod_set_test <work-directory>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "library_catalog.hpp"
#include "patch_library.hpp"
#include "segarecomp/mod_set.hpp"
#include "segarecomp/sha256.hpp"

namespace fs = std::filesystem;
namespace mod = segarecomp::mod;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
void spit(const fs::path &p, const std::vector<std::uint8_t> &bytes) {
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
std::string slurp(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// A data-only schema 3 package writing one byte of the all-zero fabricated image.
std::vector<std::uint8_t> package(const std::string &id, const std::string &sha, unsigned offset) {
  char line[64];
  std::snprintf(line, sizeof line, "rev-a u8 0x%08x 0x00 0x01\n", offset);
  const std::string text = line;
  const std::vector<std::uint8_t> edits(text.begin(), text.end());
  mod::Manifest m;
  m.id = id; m.version = "1.0.0"; m.name = id; m.platform = "genesis"; m.source_kind = "cartridge-image";
  m.revisions = {{"rev-a", sha, ""}};
  mod::Payload p;
  p.kind = "rom-edit-set"; p.path = "edits/main.edits"; p.size = edits.size(); p.sha256 = segarecomp::sha256_hex(edits); p.edits = 1;
  m.payloads = {p};
  return mod::pack_package(m, {{p.path, edits}});
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const fs::path work = argv[1], library = work / "library", rom_path = work / "game.bin";
  fs::remove_all(work);
  fs::create_directories(library);
  const std::vector<std::uint8_t> rom(0x1000, 0);
  spit(rom_path, rom);
  LibraryEntry entry;
  entry.platform_id = "genesis";
  entry.sha256 = segarecomp::sha256_hex(rom);

  std::vector<std::string> ids;
  for (const auto &[name, offset] : {std::pair<const char *, unsigned>{"first", 0x400U}, {"second", 0x402U}, {"clash", 0x400U}}) {
    spit(work / (std::string(name) + ".segapatch"), package(name, entry.sha256, offset));
    const PatchInstall installed = install_patch(entry, work / (std::string(name) + ".segapatch"), rom_path, library, 1);
    expect(installed.ok, std::string(name) + " installs: " + installed.error);
    ids.push_back(installed.identity);
  }
  if (ids.size() != 3) return 1;

  const fs::path file = library / "mod-sets" / entry.sha256 / "pair.modset";
  const ModSet stored = store_mod_set(entry, "pair", {ids[1], ids[0]}, rom_path, library);
  std::vector<std::uint8_t> want = rom;
  want[0x400] = want[0x402] = 1;
  expect(stored.ok && stored.effective_sha256 == segarecomp::sha256_hex(want), "a disjoint set is stored and composes: " + stored.error);
  expect(slurp(file) == mod::serialize_set_definition({"genesis", {ids[1], ids[0]}}), "the launcher persists the shared definition format, in set order");
  const ModSet loaded = load_mod_set(entry, "pair", rom_path, library);
  expect(loaded.ok && loaded.definition == stored.definition && loaded.effective_sha256 == stored.effective_sha256, "it loads and verifies again: " + loaded.error);

  const std::string before = slurp(file);
  const ModSet clash = store_mod_set(entry, "pair", {ids[0], ids[2]}, rom_path, library);
  expect(!clash.ok && clash.violations.size() == 1 && clash.violations[0].code == "E_SET_EDIT_OVERLAP" && clash.error.find("\"first\"") != std::string::npos && clash.error.find("\"clash\"") != std::string::npos,
         "an overlapping set is refused naming both mods: " + clash.error);
  expect(slurp(file) == before, "a refused store leaves the stored definition untouched");
  expect(!store_mod_set(entry, "../escape", {ids[0]}, rom_path, library).ok && !store_mod_set(entry, "none", {}, rom_path, library).ok && !store_mod_set(entry, "twice", {ids[0], ids[0]}, rom_path, library).ok &&
             !store_mod_set(entry, "stranger", {std::string(64, 'f')}, rom_path, library).ok,
         "bad names, empty or repeated members and mods that are not installed are refused");
  expect(!fs::exists(library / "mod-sets" / "escape.modset"), "nothing is written outside the game's set folder");
  LibraryEntry sms = entry;
  sms.platform_id = "master-system";
  expect(!load_mod_set(sms, "pair", rom_path, library).ok, "the launcher offers mods for Genesis games only");

  spit(library / "patches" / (ids[0] + ".segapatch"), package("first", entry.sha256, 0x404));
  expect(!load_mod_set(entry, "pair", rom_path, library).ok, "a replaced stored package is never used");
  expect(remove_patch(entry, ids[0], library) && !load_mod_set(entry, "pair", rom_path, library).ok, "a removed member refuses the set: it never becomes a smaller set");

  if (failures != 0) return 1;
  std::cout << "launcher_mod_set_test: ok\n";
  return 0;
}
