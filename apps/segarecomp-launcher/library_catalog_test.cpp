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

  // SEG-122: installed patches and the selected variant (ADR 0266). The original identity never changes; Vanilla's key and record are untouched by a Patched build.
  {
    const fs::path pfile = dir / "patches" / "library.json";
    Catalog pc(pfile);
    pc.load();
    const auto added = pc.import_rom(rom("genesis", 'a', "/games/g.md", "Patch Game"), 5);
    LibraryEntry &e = *added.entry;
    expect(pc.save() && slurp(pfile).find("\"schema\": 1") != std::string::npos && slurp(pfile).find("patches") == std::string::npos, "a library without patches stays schema 1, byte-free of the new members");
    InstalledPatch p1;
    p1.identity = digest('b'); p1.id = "wide"; p1.version = "1.0.0"; p1.name = "Wide"; p1.target_sha256 = digest('c'); p1.width_max = 80; p1.installed_at = 9; p1.widescreen = "enhanced_gameplay"; p1.side_style = "raw";
    e.patches.push_back(p1);
    e.selected_variant = p1.identity;
    BuildRecord vanilla; vanilla.key = key32('1'); vanilla.mode = "compatibility";
    pc.record_build(e, vanilla);
    BuildRecord patched; patched.key = key32('2'); patched.mode = "compatibility"; patched.variant = p1.identity;
    pc.record_build(e, patched);
    expect(e.current_build_key == key32('1') && e.patches[0].build_key == key32('2'), "a Patched build never replaces the Vanilla build; it becomes its patch's build");
    expect(e.is_current_key(key32('1')) && e.is_current_key(key32('2')) && !e.is_current_key(key32('3')), "both published builds are current");
    expect(pc.save() && slurp(pfile).find("\"schema\": 3") != std::string::npos, "a library with a patch is schema 3");
    Catalog again(pfile);
    expect(again.load() == CatalogLoad::loaded, "schema 3 loads");
    const LibraryEntry *r = again.find("genesis", digest('a'));
    expect(r != nullptr && r->patches.size() == 1 && r->patches[0].identity == p1.identity && r->patches[0].width_max == 80 && r->patches[0].build_key == key32('2') && r->selected_variant == p1.identity, "patch records and the selection round-trip");
    expect(r != nullptr && r->builds.size() == 2 && r->builds[1].variant == p1.identity && r->builds[0].variant.empty(), "build records keep their variant");
    const RomView pv = view_from_entry(*r), vv = view_for_variant(*r, std::string());
    expect(pv.patched() && pv.sha256 == digest('a') && pv.patch_identity == p1.identity && pv.patch_target_sha256 == digest('c') && pv.effective_sha256() == digest('c') && pv.patch_package.filename() == p1.identity + ".segapatch", "the Patched view keeps the original identity and names the verified image");
    expect(!vv.patched() && vv.effective_sha256() == digest('a') && vv.patch_widescreen.empty() && vv.patch_side_style.empty(), "the Vanilla view is untouched by an installed patch");
    expect(pv.patch_widescreen == "enhanced_gameplay" && pv.patch_side_style == "raw" && r->patches[0].widescreen == "enhanced_gameplay", "presentation defaults round-trip and reach only the Patched view");
    { std::string bad = slurp(pfile);   // a value outside the closed sets is no default at all
      const auto w = bad.find("\"widescreen\": \"enhanced_gameplay\"");
      expect(w != std::string::npos, "the defaults are stored");
      bad.replace(w, std::string("\"widescreen\": \"enhanced_gameplay\"").size(), "\"widescreen\": \"turbo\"");
      spit(dir / "patches" / "baddefault.json", bad);
      Catalog bc(dir / "patches" / "baddefault.json"); bc.load();
      expect(bc.entries().size() == 1 && bc.entries()[0].patches.size() == 1 && bc.entries()[0].patches[0].widescreen.empty() && bc.entries()[0].patches[0].side_style == "raw", "an unknown default name is dropped, the rest survives"); }
    // a selection of something that is not installed, and damaged patch records, fall back to Vanilla / are dropped
    std::string text = slurp(pfile);
    const auto at = text.find("\"selected_variant\": \"" + p1.identity + "\"");
    expect(at != std::string::npos, "the selection is stored");
    text.replace(at, std::string("\"selected_variant\": \"" + p1.identity + "\"").size(), "\"selected_variant\": \"" + digest('d') + "\"");
    spit(dir / "patches" / "badsel.json", text);
    Catalog badsel(dir / "patches" / "badsel.json");
    badsel.load();
    expect(badsel.entries().size() == 1 && badsel.entries()[0].selected_variant.empty() && !view_from_entry(badsel.entries()[0]).patched(), "a selection naming no installed patch means Vanilla");
    std::string damaged = slurp(pfile);
    const auto id_at = damaged.find("\"identity\": \"" + p1.identity + "\"");
    damaged.replace(id_at, std::string("\"identity\": \"" + p1.identity + "\"").size(), "\"identity\": \"nothex\"");
    spit(dir / "patches" / "damaged.json", damaged);
    Catalog dam(dir / "patches" / "damaged.json");
    dam.load();
    expect(dam.entries().size() == 1 && dam.entries()[0].patches.empty() && dam.entries()[0].selected_variant.empty(), "a damaged patch record is dropped and the game survives");
    // an older release (schema 2) opens the schema 3 file read-only: simulate it with the next schema number
    std::string future = slurp(pfile);
    future.replace(future.find("\"schema\": 3"), 11, "\"schema\": 4");
    spit(dir / "patches" / "future.json", future);
    Catalog fut(dir / "patches" / "future.json");
    expect(fut.load() == CatalogLoad::read_only_newer && !fut.save(), "a newer schema is read-only");
    // a Patched build record in the cache is never adopted as a game of its own
    const fs::path cache = dir / "patches" / "cache";
    spit(cache / "games" / key32('2') / "game", "x");
    spit(cache / "games" / key32('2') / "status.json", "{\"status\":\"ok\"}");
    spit(cache / "games" / key32('2') / "metadata.json", "{\"rom_sha256\":\"" + digest('c') + "\",\"platform\":\"genesis\",\"patch_identity\":\"" + p1.identity + "\"}");
    Catalog fresh(dir / "patches" / "fresh.json");
    fresh.load();
    expect(fresh.migrate_cache(cache, 1) == 0 && fresh.entries().empty(), "cache migration never turns a Patched build into a game");
    // SEG-124 (ADR 0267): a native-only package's record (capability, implementation, components, descriptor digest) round-trips, reaches only its own Patched view, and a
    // BPS-only record stays byte-free of the native members; a damaged native record drops the whole patch (it never degrades into a BPS-only one).
    {
      expect(slurp(pfile).find("native_source_sha256") == std::string::npos, "a BPS-only patch record carries no native member");
      InstalledPatch n1;
      n1.identity = digest('d'); n1.id = "nat"; n1.version = "1.0.0"; n1.name = "Native"; n1.target_sha256 = digest('a'); n1.native_source_sha256 = digest('e'); n1.author = "Tester";
      n1.native_abi = 1; n1.native_caps = 7; n1.native_approved = true; n1.installed_at = 11;
      Catalog nc(dir / "patches" / "native.json");
      nc.load();
      LibraryEntry &ne = *nc.import_rom(rom("genesis", 'a', "/games/g.md", "Patch Game"), 5).entry;
      ne.patches.push_back(p1);
      ne.patches.push_back(n1);
      ne.selected_variant = n1.identity;
      expect(nc.save(), "native library saved");
      const std::string saved = slurp(dir / "patches" / "native.json");
      expect(saved.find("native_source_sha256") != std::string::npos && saved.find(digest('e')) != std::string::npos, "the native record is persisted");
      Catalog back(dir / "patches" / "native.json");
      back.load();
      const LibraryEntry *br = back.find("genesis", digest('a'));
      expect(br != nullptr && br->patches.size() == 2 && !br->patches[0].has_native() && br->patches[1].has_native() && br->patches[1].native_abi == 1 && br->patches[1].native_caps == 7 && br->patches[1].native_approved &&
                 br->patches[1].native_source_sha256 == digest('e') && br->patches[1].target_sha256 == digest('a'),
             "native fields round-trip; the BPS-only patch stays BPS-only");
      const RomView nv = view_from_entry(*br), bv = view_for_variant(*br, p1.identity);
      expect(nv.patched() && nv.patch_native() && nv.patch_native_source_sha256 == digest('e') && nv.patch_native_abi == 1 && nv.patch_native_caps == 7 && nv.patch_native_approved &&
                 nv.effective_sha256() == digest('a') && nv.patch_identity == n1.identity,
             "a native-only variant compiles the original image digest and carries its capability");
      expect(!bv.patch_native() && bv.patch_native_source_sha256.empty() && bv.patch_identity == p1.identity, "a BPS-only variant carries no native capability");
      for (const char *broken : {"\"native_abi\": 1", "\"native_caps\": 7"}) {
        std::string t = saved;
        const auto at = t.find(broken);
        if (at != std::string::npos) t.replace(at, std::string(broken).size(), std::string(broken).substr(0, std::string(broken).find(':') + 2) + "0");
        spit(dir / "patches" / "badnative.json", t);
        Catalog bn(dir / "patches" / "badnative.json");
        bn.load();
        const LibraryEntry *r2 = bn.find("genesis", digest('a'));
        expect(r2 != nullptr && r2->patches.size() == 1 && !r2->patches[0].has_native() && r2->selected_variant.empty(), "a damaged native record drops that patch only (never degrades into a BPS-only one)");
      }
      std::string t = saved;
      t.replace(t.find(digest('e')), 64, "nothex-nothex-nothex-nothex-nothex-nothex-nothex-nothex-nothex-n");
      spit(dir / "patches" / "badnative2.json", t);
      Catalog bn2(dir / "patches" / "badnative2.json");
      bn2.load();
      expect(bn2.find("genesis", digest('a')) != nullptr && bn2.find("genesis", digest('a'))->patches.size() == 1, "a native record with a bad descriptor digest drops the patch");
    }
    // removing the record of a patch
    e.patches.clear(); e.selected_variant.clear();
    expect(!e.is_current_key(key32('2')) && e.is_current_key(key32('1')), "once the patch is gone its build is no longer current");
  }
  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "library_catalog_test: ok\n";
  return 0;
}
