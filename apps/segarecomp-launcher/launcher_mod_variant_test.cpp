// SEG-137-T007 (ADR 0296): the launcher's executable cache and build job with a Luau-only mod, against the REAL product build driver (the driver calls the real compiler; only the bundled-Zig
// location is a thin wrapper around the host compiler, an OS dependency) and fabricated, redistributable inputs.
//
//   launcher_mod_variant_test <install-root> <rom> <other-rom> <hud.segapatch> <hud-v2.segapatch> <native.segapatch> <wram.segapatch> <syntax.segapatch>
//
// Proves: a Luau variant resolves to its own cache entry (key = original image + package identity + effective image + compiler pin, on top of the usual components); the same complete inputs
// reuse the published entry; a modified script or another image is another entry; a Vanilla or schema 1/2 key is exactly what it was (a stray mod field without an identity changes nothing);
// an entry is reusable only for the variant it records; a failed build publishes nothing and changes no other entry or save folder; unsupported packages are refused before any build.
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

void set_env(const char *name, const std::string &value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

std::string slurp(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string digest_of_file(const fs::path &path) {
  const std::string bytes = slurp(path);
  return segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

// Builds the entry of `rom` with the real driver and waits for the job; returns the outcome.
BuildOutcome build(const RomView &rom, const Layout &layout, std::string *message = nullptr) {
  BuildJob job(rom, layout);
  for (int waited = 0; waited < 900000 && !job.finished(); waited += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  job.wait();
  if (message != nullptr) *message = job.message();
  return job.outcome();
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 9) {
    std::cerr << "usage: launcher_mod_variant_test <install-root> <rom> <other-rom> <hud> <hud-v2> <native> <wram> <syntax>\n";
    return 2;
  }
  const fs::path root = argv[1], rom_path = argv[2], other_rom_path = argv[3], hud = argv[4], hud2 = argv[5], native = argv[6], wram = argv[7], syntax = argv[8];
  const fs::path cache = root.parent_path() / "cache";
  fs::create_directories(cache);
  set_env("SEGARECOMP_HOME", root.string());
  set_env("SEGARECOMP_CACHE_DIR", cache.string());
  set_env("SEGARECOMP_LIBRARY_DIR", (root.parent_path() / "library").string());
  const Layout layout = locate_layout();
  expect(layout.problem.empty(), "the install root is complete: " + layout.problem);

  RomView vanilla;
  vanilla.path = rom_path;
  vanilla.sha256 = digest_of_file(rom_path);
  vanilla.title = "Fixture";
  vanilla.platform_id = "genesis";
  vanilla.supported_platform = true;

  // ---- keys: Vanilla and schema 1/2 are untouched, a mod adds its identity ----
  const fs::path vanilla_entry = entry_dir(vanilla, layout);
  RomView stray = vanilla;   // mod fields without an identity are not a mod
  stray.luau_package = hud;
  stray.luau_pin = "0.746";
  stray.luau_effective_sha256 = std::string(64, 'e');
  expect(entry_dir(stray, layout) == vanilla_entry, "mod fields without an identity do not change a Vanilla key");
  RomView patched = vanilla;
  patched.patch_identity = std::string(64, '1');
  patched.patch_target_sha256 = std::string(64, '2');
  RomView patched_stray = patched;
  patched_stray.luau_pin = "0.746";
  patched_stray.luau_effective_sha256 = std::string(64, 'e');
  expect(entry_dir(patched, layout) != vanilla_entry && entry_dir(patched_stray, layout) == entry_dir(patched, layout), "a schema 1/2 variant key is its own and unaffected by mod fields");

  RomView a = vanilla;
  expect(select_luau_mod(a, hud).empty() && a.luau_mod() && a.luau_effective_sha256 == vanilla.sha256 && !a.luau_label.empty(), "the example package is selected for the fixture game");
  expect(a.patch_identity.empty() && !a.patched() && a.effective_sha256() == vanilla.sha256, "a mod is not a schema 1/2 patch");
  RomView a2 = vanilla;
  expect(select_luau_mod(a2, hud).empty() && entry_dir(a2, layout) == entry_dir(a, layout), "the same complete inputs select the same entry");
  expect(entry_dir(a, layout) != vanilla_entry, "a mod never shares the Vanilla entry");
  RomView b = vanilla;
  expect(select_luau_mod(b, hud2).empty() && entry_dir(b, layout) != entry_dir(a, layout), "a modified script (another package identity) is another entry");
  RomView pin = a;
  pin.luau_pin = "0.747";
  expect(entry_dir(pin, layout) != entry_dir(a, layout), "another pinned compiler is another entry");
  RomView eff = a;
  eff.luau_effective_sha256 = std::string(64, 'f');
  expect(entry_dir(eff, layout) != entry_dir(a, layout), "another effective image is another entry");
  expect(genesis_save_dir(a) != genesis_save_dir(vanilla) && !genesis_save_dir(a).empty() && genesis_save_dir(a) != genesis_save_dir(b), "a mod has its own player data, apart from Vanilla and from another mod");

  // ---- refusals happen before any build ----
  {
    RomView r = vanilla;
    expect(!select_luau_mod(r, native).empty() && !r.luau_mod(), "a package with native code is refused and nothing is selected");
    expect(!select_luau_mod(r, wram).empty() && !r.luau_mod(), "a package requiring an unsupported capability is refused");
    expect(!select_luau_mod(r, root / "missing.segapatch").empty(), "a missing package is refused");
    RomView wrong = vanilla;
    wrong.path = other_rom_path;
    wrong.sha256 = digest_of_file(other_rom_path);
    expect(!select_luau_mod(wrong, hud).empty(), "an image the package does not declare is refused");
    RomView with_patch = patched;
    expect(!select_luau_mod(with_patch, hud).empty() && !with_patch.luau_mod(), "a mod is not combined with a selected patch");
    RomView sms = vanilla;
    sms.platform_id = "master-system";
    expect(!select_luau_mod(sms, hud).empty(), "another platform is refused");
  }

  // ---- executable entries: built by the real driver, reused, invalidated ----
  std::string message;
  expect(!entry_ready_for(vanilla_entry, vanilla, layout), "nothing is built yet");
  expect(build(vanilla, layout, &message) == BuildOutcome::succeeded && entry_ready_for(vanilla_entry, vanilla, layout), "the Vanilla program builds: " + message);
  const fs::path game = entry_executable(vanilla_entry);
  const std::string vanilla_game = digest_of_file(game), vanilla_meta = slurp(vanilla_entry / "metadata.json");
  expect(vanilla_meta.find("luau") == std::string::npos, "a Vanilla record carries no Luau member");

  const fs::path entry_a = entry_dir(a, layout);
  expect(!entry_ready_for(entry_a, a, layout), "the mod entry is not there before its build");
  expect(build(a, layout, &message) == BuildOutcome::succeeded, "the mod program builds through the launcher pipeline: " + message);
  expect(entry_ready_for(entry_a, a, layout) && fs::is_regular_file(entry_executable(entry_a)), "the published entry is a ready, executable program for exactly this package");
  const std::string meta_a = slurp(entry_a / "metadata.json");
  expect(meta_a.find("\"luau_identity\":\"" + a.luau_identity + "\"") != std::string::npos && meta_a.find("\"luau_effective_sha256\":\"" + a.luau_effective_sha256 + "\"") != std::string::npos &&
             meta_a.find("\"luau_pin\":\"0.746\"") != std::string::npos && meta_a.find("\"luau_bytecode_digest\":\"") != std::string::npos,
         "the record names the package, the image, the compiler and the bytecode digest");
  expect(slurp(entry_a / "status.json").find("\"bytecode_digest\":\"") != std::string::npos && !fs::exists(entry_a / "input"), "the build recorded the verified bytecode digest and left no derivative image");
  expect(!entry_ready_for(entry_a, vanilla, layout) && !entry_ready_for(vanilla_entry, a, layout), "an entry is reusable only for the variant it records (a Vanilla program is never a mod's, nor the reverse)");
  expect(!entry_ready_for(entry_a, b, layout) && !entry_ready_for(entry_a, pin, layout) && !entry_ready_for(entry_a, eff, layout), "another package, compiler pin or image never accepts this program");
  const std::string game_a = digest_of_file(entry_executable(entry_a));
  // the second identical request: the entry is ready for it, so the launcher reuses it and starts no build
  RomView a3 = vanilla;
  expect(select_luau_mod(a3, hud).empty() && entry_dir(a3, layout) == entry_a && entry_ready_for(entry_dir(a3, layout), a3, layout), "a second identical request finds the published entry ready (reuse, no rebuild)");
  expect(digest_of_file(entry_executable(entry_a)) == game_a && slurp(entry_a / "metadata.json") == meta_a, "reuse leaves the entry byte-identical");

  const fs::path entry_b = entry_dir(b, layout);
  expect(!entry_ready_for(entry_b, b, layout), "a modified script has no entry yet");
  expect(build(b, layout, &message) == BuildOutcome::succeeded && entry_ready_for(entry_b, b, layout) && entry_b != entry_a, "the modified script builds its own entry: " + message);
  expect(digest_of_file(entry_executable(entry_a)) == game_a && slurp(entry_a / "metadata.json") == meta_a && digest_of_file(game) == vanilla_game, "the earlier entries are untouched");

  // ---- a failed build publishes nothing and changes nothing else ----
  RomView bad = vanilla;
  expect(select_luau_mod(bad, syntax).empty(), "a package with a script syntax error is selectable (only the trusted compile step sees the script)");
  const fs::path entry_bad = entry_dir(bad, layout);
  const fs::path failed_dir = entry_bad.parent_path() / (entry_bad.filename().string() + ".failed");
  const fs::path partial_dir = entry_bad.parent_path() / (entry_bad.filename().string() + ".partial");
  expect(build(bad, layout, &message) == BuildOutcome::failed && message.find("compile") != std::string::npos, "the build of a script with a syntax error fails with a diagnostic: " + message);
  expect(!fs::exists(entry_bad) && !fs::exists(partial_dir) && fs::is_regular_file(failed_dir / "build.log") && !entry_ready_for(entry_bad, bad, layout), "no entry and no partial entry exists; the diagnostics are kept");
  expect(!fs::exists(failed_dir / "input"), "a failed build leaves no derivative image");
  expect(digest_of_file(entry_executable(entry_a)) == game_a && slurp(entry_a / "metadata.json") == meta_a && digest_of_file(game) == vanilla_game && slurp(vanilla_entry / "metadata.json") == vanilla_meta &&
             entry_ready_for(entry_a, a, layout) && entry_ready_for(entry_b, b, layout) && entry_ready_for(vanilla_entry, vanilla, layout),
         "Vanilla, the earlier mod entries and their cached executables are byte-identical and still valid");
  expect(!fs::exists(genesis_save_dir(a)) || fs::is_empty(genesis_save_dir(a)), "no save data was created or touched by the builds");

  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "launcher_mod_variant_test: ok\n";
  return 0;
}
