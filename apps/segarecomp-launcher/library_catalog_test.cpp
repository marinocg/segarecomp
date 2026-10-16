// Library catalog contract: persistence, corruption recovery, duplicates by content digest, relocation, migration of existing cache
// records, and the rule that nothing unverifiable is ever treated as a playable game. No window, no product CLI.

#include "library_catalog.hpp"
#include "json_lite.hpp"
#include <limits>

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
  {
    for (const char *number : {"1e999", "-1e999", "9223372036854775808", "-9223372036854775809"})
      expect(!json::parse(std::string("{\"schema\":") + number + "}"), "nonfinite or out-of-range JSON number rejected");
    for (const char *number : {"1.5", "9.223372036854776e18", "-9.223372036854778e18"}) {
      const auto value = json::parse(std::string("{\"schema\":") + number + "}");
      expect(value && value->num("schema", -1) == -1, "invalid integer field safely returns fallback");
    }
    const auto endpoints = json::parse("{\"min\":-9223372036854775808,\"max\":9223372036854775807,\"whole\":1e0}");
    expect(endpoints && endpoints->num("min") == std::numeric_limits<long long>::min() && endpoints->num("max") == std::numeric_limits<long long>::max() && endpoints->num("whole") == 1,
           "exact signed integer endpoints and integral exponent survive");
    json::Value nonfinite = json::Value::make_object(), real;
    real.kind = json::Value::Kind::real;
    for (double n : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      real.real = n; nonfinite.set("schema", real);
      expect(nonfinite.num("schema", -1) == -1, "programmatically supplied nonfinite real never reaches integer cast");
    }
  }

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
    {  // an adopted build whose file time is unknown (0) must not erase the recorded build time
      BuildRecord adopted;
      adopted.key = key32('2'); adopted.built_at = 0; adopted.mode = "compatibility";
      LibraryEntry copy = e;
      c.record_build(copy, adopted);
      expect(copy.last_built_at == 500 && copy.current_build_key == key32('2'), "an unknown build time keeps the known last-built time");
    }
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
  {
    const fs::path invalid = dir / "invalid-schema.json";
    for (const char *number : {"1e999", "1.5", "9.223372036854776e18"}) {
      spit(invalid, std::string("{\"schema\":") + number + ",\"games\":[]}");
      Catalog c(invalid);
      expect(c.load() == CatalogLoad::recovered_empty && !c.notice().empty(), "invalid schema follows catalog corruption contract");
    }
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
  {  // SEG-069-T002: Optimized is the default for both platforms; stored selections are honoured; legacy Genesis v1 becomes gen2
    const fs::path defaults = dir / "defaults" / "library.json";
    Catalog c(defaults);
    c.load();
    c.import_rom(rom("genesis", 'd', "/roms/d.md"), 1);
    c.import_rom(rom("master-system", 'e', "/roms/e.sms"), 1);
    const LibraryEntry *genesis_entry = c.find("genesis", digest('d')), *sms_entry = c.find("master-system", digest('e'));
    expect(genesis_entry->aot_policy == "optimized" && genesis_entry->aot_model == "gen2", "a new Genesis game defaults to Optimized (gen2)");
    expect(sms_entry->aot_policy == "optimized" && sms_entry->aot_model == "v1", "a new Master System game defaults to Optimized and never carries a Genesis model");
    const RomView genesis_view = view_from_entry(*genesis_entry);
    expect(genesis_view.aot_policy == "optimized" && genesis_view.aot_model == "gen2", "the Genesis build request is Optimized + gen2");
    expect(!genesis_view.optimized() || genesis_view.optimized_gen2(), "Genesis Optimized is the gen2 request");
    RomView compat_rom = rom("genesis", 'f', "/roms/f.md");
    c.import_rom(compat_rom, 1);
    expect(c.find("genesis", digest('f'))->aot_policy == "optimized", "the RomView's own (headless) policy never leaks into the library default");
    c.find("genesis", digest('d'))->aot_policy = "compatibility";   // an explicit selection
    c.import_rom(rom("master-system", '9', "/roms/9.sms"), 1);
    c.find("master-system", digest('9'))->aot_policy = "compatibility";
    expect(c.save(), "defaults save");
    Catalog back(defaults);
    back.load();
    const LibraryEntry *g = back.find("genesis", digest('d'));
    const LibraryEntry *m = back.find("master-system", digest('e'));
    const LibraryEntry *mc = back.find("master-system", digest('9'));
    expect(g != nullptr && g->aot_policy == "compatibility", "an explicit Compatibility selection survives a round trip");
    expect(m != nullptr && m->aot_policy == "optimized" && m->aot_model == "v1", "the SMS Optimized intent survives a round trip (it is not rewritten while the capability is unknown)");
    expect(mc != nullptr && mc->aot_policy == "compatibility", "an explicit SMS Compatibility selection survives a round trip");

    // Legacy records.
    const auto legacy = [&](const std::string &platform, char sha, const std::string &policy_member) {
      return "{\"platform\":\"" + platform + "\",\"sha256\":\"" + digest(sha) + "\",\"detected_title\":\"Legacy\"" + policy_member + "}";
    };
    spit(defaults, "{\"schema\":1,\"games\":[" +
                       legacy("genesis", '1', ",\"aot_policy\":\"optimized\",\"aot_model\":\"v1\"") + "," +
                       legacy("genesis", '2', ",\"aot_policy\":\"optimized\",\"aot_model\":\"gen2\"") + "," +
                       legacy("genesis", '3', ",\"aot_policy\":\"compatibility\",\"aot_model\":\"v1\"") + "," +
                       legacy("genesis", '4', "") + "," +
                       legacy("master-system", '5', ",\"aot_policy\":\"compatibility\"") + "," +
                       legacy("master-system", '6', ",\"aot_policy\":\"optimized\",\"aot_model\":\"gen2\"") + "]}");
    Catalog old(defaults);
    old.load();
    const auto pick = [&](const std::string &platform, char sha) { return old.find(platform, digest(sha)); };
    expect(pick("genesis", '1') && pick("genesis", '1')->aot_policy == "optimized" && pick("genesis", '1')->aot_model == "gen2", "legacy Optimized/v1 takes the new Optimized (gen2) meaning");
    expect(pick("genesis", '2') && pick("genesis", '2')->aot_policy == "optimized" && pick("genesis", '2')->aot_model == "gen2", "legacy Optimized/gen2 is unchanged");
    expect(pick("genesis", '3') && pick("genesis", '3')->aot_policy == "compatibility", "legacy Compatibility is preserved");
    expect(pick("genesis", '4') && pick("genesis", '4')->aot_policy == "optimized", "a record without a stored selection takes the default");
    expect(pick("master-system", '5') && pick("master-system", '5')->aot_policy == "compatibility", "legacy SMS Compatibility is preserved");
    expect(pick("master-system", '6') && pick("master-system", '6')->aot_model == "v1", "a Genesis model never reaches a Master System entry");
    const RomView legacy_view = view_from_entry(*pick("genesis", '1'));
    expect(legacy_view.aot_model == "gen2", "the legacy v1 selection builds gen2");
  }
  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "library_catalog_test: ok\n";
  return 0;
}
