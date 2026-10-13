// Library catalog contract: persistence, corruption recovery, duplicates by content digest, relocation, migration of existing cache
// records, and the rule that nothing unverifiable is ever treated as a playable game. No window, no product CLI.

#include "library_catalog.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
std::string slurp(const fs::path &p) { std::ifstream in(p, std::ios::binary); std::ostringstream o; o << in.rdbuf(); return o.str(); }
void spit(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
std::string digest(char c) { return std::string(64, c); }
std::string key32(char c) { return std::string(32, c); }

RomView rom(const std::string &platform, char sha, const std::string &path, const std::string &title = "Fictional Game") {
  RomView r;
  r.path = path;
  r.sha256 = digest(sha);
  r.title = title;
  r.platform_id = platform;
  r.platform = platform;
  r.supported_platform = true;
  return r;
}
} // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "segarecomp_catalog_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "library" / "library.json";

  {  // empty catalog: a missing file is a new library; saving then loading round-trips
    Catalog c(file);
    expect(c.load() == CatalogLoad::created && c.entries().empty(), "missing file is a new empty library");
    expect(c.save(), "save of an empty library");
    Catalog again(file);
    expect(again.load() == CatalogLoad::loaded && again.entries().empty(), "empty library reloads");
    expect(!fs::exists(fs::path(file).concat(".tmp")), "no temporary file is left behind");
  }
  {  // import, duplicates by content digest, tracked locations, persistence of user data
    Catalog c(file);
    c.load();
    const auto first = c.import_rom(rom("genesis", 'a', "/roms/one.md"), 100);
    expect(first.added && first.entry != nullptr && first.entry->added_at == 100, "first import adds");
    const auto same = c.import_rom(rom("genesis", 'a', "/roms/one.md"), 200);
    expect(!same.added && !same.path_added && c.entries().size() == 1, "same path, same content is a no-op");
    const auto moved = c.import_rom(rom("genesis", 'a', "/other/copy.bin"), 300);
    expect(!moved.added && moved.path_added && c.entries().size() == 1 && c.entries()[0].rom_paths.size() == 2, "same content elsewhere adds a location, not a game");
    const auto other_platform = c.import_rom(rom("master-system", 'a', "/roms/x.sms"), 400);
    expect(other_platform.added && c.entries().size() == 2, "platform is part of the identity");
    RomView bad = rom("genesis", 'b', "/roms/bad.md");
    bad.supported_platform = false;
    expect(!c.import_rom(bad, 1).error.empty(), "unsupported ROM is refused with a message");
    RomView unread = rom("genesis", 'c', "/roms/unread.md");
    unread.error = "cannot read";
    expect(!c.import_rom(unread, 1).error.empty() && c.entries().size() == 2, "unreadable ROM is refused");
    LibraryEntry &e = c.entries()[0];
    e.display_title = "My Title \"quoted\" \\ \xC3\xA9";
    e.favorite = true;
    e.mapper = "";
    BuildRecord b;
    b.key = key32('1'); b.built_at = 500; b.mode = "compatibility"; b.version = "1.0"; b.toolchain = "zig 0.15.2";
    c.record_build(e, b);
    c.record_run(e, 0, "clean", 600);
    expect(c.save(), "save with data");
    Catalog d(file);
    expect(d.load() == CatalogLoad::loaded && d.entries().size() == 2, "reload keeps both games");
    const LibraryEntry *r = d.find("genesis", digest('a'));
    expect(r != nullptr && r->display_title == e.display_title && r->favorite && r->current_build_key == key32('1') && r->last_played_at == 600 &&
               r->last_run == "clean" && r->builds.size() == 1 && r->rom_paths.size() == 2 && r->detected_title == "Fictional Game",
           "user title, favorite, build history and run result persist");
    expect(fs::exists(fs::path(file).concat(".bak")), "the previous catalog is kept as .bak after a second save");
  }
  {  // relocation re-binds by digest only
    Catalog c(file);
    c.load();
    LibraryEntry *e = c.find("genesis", digest('a'));
    expect(e != nullptr, "entry present");
    expect(!c.relocate(*e, "/new/place.md", digest('z')), "a file with another digest is refused");
    expect(c.relocate(*e, "/new/place.md", digest('a')) && e->rom_paths.front() == "/new/place.md", "a matching digest is rebound as the primary location");
    expect(c.save(), "relocation persists");
  }
  {  // corrupt main file: restored from .bak, damaged file kept aside
    spit(file, "{ this is not json");
    Catalog c(file);
    expect(c.load() == CatalogLoad::recovered_from_backup && c.entries().size() == 2 && !c.notice().empty(), "corrupt catalog is restored from the backup");
    expect(fs::exists(fs::path(file).concat(".corrupt")), "the damaged file is kept for inspection");
  }
  {  // corrupt main and backup: new library, nothing deleted
    spit(file, "garbage");
    spit(fs::path(file).concat(".bak"), "also garbage");
    Catalog c(file);
    expect(c.load() == CatalogLoad::recovered_empty && c.entries().empty() && !c.notice().empty(), "unrecoverable catalog starts a new library with a notice");
    expect(fs::exists(fs::path(file).concat(".corrupt")) || fs::exists(fs::path(file).concat(".corrupt1")), "damaged catalog is preserved");
    expect(c.save(), "a recovered library can be saved again");
  }
  {  // schema validation: bad records dropped, duplicates merged, hostile cover names cleared, caps enforced
    std::string games = "{\"schema\":1,\"games\":[";
    games += "{\"platform\":\"genesis\",\"sha256\":\"" + digest('d') + "\",\"detected_title\":\"A\",\"rom_paths\":[\"/a\"],\"cover\":\"../../etc/passwd\",\"current_build_key\":\"nothex\"},";
    games += "{\"platform\":\"genesis\",\"sha256\":\"" + digest('d') + "\",\"rom_paths\":[\"/b\",\"/a\"]},";
    games += "{\"platform\":\"saturn\",\"sha256\":\"" + digest('e') + "\"},";
    games += "{\"platform\":\"genesis\",\"sha256\":\"short\"},";
    games += "42]}";
    spit(file, games);
    Catalog c(file);
    expect(c.load() == CatalogLoad::loaded && c.entries().size() == 1, "invalid records are dropped, duplicates merged");
    const LibraryEntry &e = c.entries()[0];
    expect(e.cover.empty() && e.current_build_key.empty() && e.rom_paths.size() == 2, "traversal cover and malformed build key are cleared; locations merged");
  }
  {  // a newer schema is shown but never overwritten
    spit(file, "{\"schema\":99,\"games\":[{\"platform\":\"genesis\",\"sha256\":\"" + digest('f') + "\"}]}");
    const std::string before = slurp(file);
    Catalog c(file);
    expect(c.load() == CatalogLoad::read_only_newer && c.read_only() && c.entries().size() == 1, "newer schema opens read-only");
    expect(!c.save() && slurp(file) == before, "a read-only catalog is never written");
    expect(!c.import_rom(rom("genesis", 'g', "/x"), 1).error.empty(), "read-only catalog refuses imports");
  }
  {  // migration of existing cache records: only verified finished builds, no invented ROM path, idempotent
    const fs::path cache = dir / "cache";
    const auto build = [&](const std::string &name, bool ready, const std::string &meta) {
      const fs::path d = cache / "games" / name;
      fs::create_directories(d);
      if (ready) {
#if defined(_WIN32)
        spit(d / "game.exe", "x");
#else
        spit(d / "game", "x");
#endif
        spit(d / "status.json", "{\"status\":\"ok\"}");
      }
      if (!meta.empty()) spit(d / "metadata.json", meta);
    };
    const std::string good = "{\"rom_sha256\":\"" + digest('7') + "\",\"title\":\"Old Game\",\"segarecomp_version\":\"0.9\",\"platform\":\"genesis\",\"host\":\"x\",\"toolchain\":\"zig 0.15.2\",\"rom_path\":\"referenced, not copied\"}";
    build(key32('2'), true, good);
    build(key32('3'), false, good);                                  // not a finished build
    build(key32('4'), true, "{\"rom_sha256\":\"nonsense\"}");        // unverifiable metadata
    build(key32('5'), true, "not json");
    build(key32('6') + ".failed", true, good);                       // diagnostics folder, never adopted
    build("not-a-key", true, good);
    std::error_code ec;
    fs::create_directories(dir / "outside" / key32('8'));
    fs::create_directory_symlink(dir / "outside" / key32('8'), cache / "games" / key32('8'), ec);  // symlinked entry
    Catalog c(dir / "migrated.json");
    c.load();
    expect(c.migrate_cache(cache, 1000) == 1 && c.entries().size() == 1, "exactly the one verifiable cache record is adopted");
    const LibraryEntry &e = c.entries()[0];
    expect(e.rom_path_unknown && e.rom_paths.empty() && e.current_build_key == key32('2') && e.title() == "Old Game", "adopted without an invented ROM path, bound to its build key");
    expect(c.migrate_cache(cache, 2000) == 0 && c.entries().size() == 1 && c.entries()[0].builds.size() == 1, "migration is idempotent");
  }
  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "library_catalog_test: ok\n";
  return 0;
}
