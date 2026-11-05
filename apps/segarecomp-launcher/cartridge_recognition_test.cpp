// SEG-122-T009: the launcher's lock-on recognition and its persistence, without a window or the product CLI. Every image here is project-authored: pseudo-random bytes behind headers
// that carry only the public identification fields the recognition reads. No commercial content.
#include "cartridge_recognition.hpp"
#include "json_lite.hpp"
#include "library_catalog.hpp"
#include "older_build.hpp"
#include "segarecomp/sha256.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}

using Image = std::vector<std::uint8_t>;
constexpr std::size_t slot = 0x200000;
const std::string base_title = "SONIC & KNUCKLES", base_code = "GM MK-1563 -00", attached_title = "SONIC THE             HEDGEHOG 3", attached_code = "GM MK-1079 -00";

void put(Image &image, std::size_t at, std::size_t width, const std::string &text) {
  for (std::size_t i = 0; i < width; ++i) image[at + i] = i < text.size() ? static_cast<std::uint8_t>(text[i]) : ' ';
}
void put32(Image &image, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) image[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
}
void seal(Image &image, std::size_t origin, std::size_t size) {  // the header checksum of one cartridge
  std::uint16_t sum = 0;
  for (std::size_t i = origin + 0x200; i + 1 < origin + size; i += 2) sum = static_cast<std::uint16_t>(sum + ((image[i] << 8U) | image[i + 1]));
  image[origin + 0x18E] = static_cast<std::uint8_t>(sum >> 8U);
  image[origin + 0x18F] = static_cast<std::uint8_t>(sum);
}
// One cartridge's bytes at `origin` of `image`: valid header, valid checksum.
void cartridge(Image &image, std::size_t origin, const std::string &title, const std::string &code, std::uint32_t seed, std::size_t size = slot) {
  std::uint32_t x = seed;
  for (std::size_t i = origin; i < origin + size; i += 4096) { x = x * 1103515245U + 12345U; image[i] = static_cast<std::uint8_t>(x >> 16U); }
  put(image, origin + 0x100, 16, "SEGA GENESIS");
  put(image, origin + 0x120, 48, title);
  put(image, origin + 0x150, 48, title);
  put(image, origin + 0x180, 14, code);
  put32(image, origin + 0x1A0, 0);
  put32(image, origin + 0x1A4, static_cast<std::uint32_t>(size - 1U));
  seal(image, origin, size);
}
Image standalone() { Image i(slot); cartridge(i, 0, base_title, base_code, 1); return i; }
Image combined() { Image i(2 * slot); cartridge(i, 0, base_title, base_code, 1); cartridge(i, slot, attached_title, attached_code, 2); return i; }
CartridgeKind kind(const Image &image) { return recognize_genesis_cartridge(image).kind; }

void write_file(const fs::path &path, const Image &image) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
}

RomView view_of(const Image &image, const fs::path &path, const std::string &platform = "genesis") {
  RomView r;
  r.path = path;
  r.sha256 = segarecomp::sha256_hex(image);
  r.title = "Fictional";
  r.platform_id = platform;
  r.platform = platform;
  r.supported_platform = true;
  if (platform == "genesis") {
    const CartridgeFinding f = recognize_genesis_cartridge(image);
    r.mapper = f.mapper();
    r.cartridge_note = f.note;
  }
  return r;
}

void detection() {
  expect(kind(standalone()) == CartridgeKind::lock_on, "standalone 2 MiB base is recognized");
  expect(kind(combined()) == CartridgeKind::lock_on, "4 MiB base + attached cartridge is recognized");
  expect(recognize_genesis_cartridge(standalone()).mapper() == "lock_on" && recognize_genesis_cartridge(standalone()).note.empty(), "a recognized image selects lock_on with no note");

  // Ordinary cartridges, at the very sizes of the board.
  { Image i(slot); cartridge(i, 0, "FICTIONAL GAME", "GM 00000000-00", 3); expect(kind(i) == CartridgeKind::standard, "ordinary 2 MiB cartridge"); }
  { Image i(2 * slot); cartridge(i, 0, "FICTIONAL GAME", "GM 00000000-00", 3, 2 * slot); expect(kind(i) == CartridgeKind::standard, "ordinary 4 MiB cartridge"); }
  { Image i(2 * slot); cartridge(i, 0, "FICTIONAL GAME", "GM 00000000-00", 3); cartridge(i, slot, attached_title, attached_code, 4);
    expect(kind(i) == CartridgeKind::standard, "a second apparent header, even a known one, without a verified base stays standard"); }
  { Image i(2 * slot); cartridge(i, 0, "FICTIONAL GAME", "GM 00000000-00", 3); cartridge(i, slot, base_title, base_code, 4);
    expect(kind(i) == CartridgeKind::standard, "the base's own header in the upper half does not make a lock-on image"); }
  { Image i(slot); cartridge(i, 0, attached_title, attached_code, 5); expect(kind(i) == CartridgeKind::standard, "the attached cartridge alone is an ordinary cartridge"); }

  // Bytes in the wrong place.
  { Image i(slot); cartridge(i, 0, "FICTIONAL GAME", "GM 00000000-00", 3); put(i, 0x1000, 16, base_title); put(i, 0x124, 16, " "); put(i, 0x190, 16, base_title); seal(i, 0, slot);
    expect(kind(i) == CartridgeKind::standard, "the signature outside the title fields means nothing"); }
  { Image i(slot); cartridge(i, 0, "FICTIONAL GAME", "GM 00000000-00", 3); put(i, 0x130, 16, base_title);
    expect(kind(i) == CartridgeKind::standard || kind(i) == CartridgeKind::ambiguous, "a shifted title is never recognized"); }

  // Malformed and truncated.
  { Image i = standalone(); put(i, 0x100, 4, "XXXX"); expect(kind(i) == CartridgeKind::standard, "no SEGA signature: not a Genesis header at all"); }
  for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{0x100}, std::size_t{0x1FF}}) {
    Image i = standalone(); i.resize(size); expect(kind(i) == CartridgeKind::standard, "a truncated header is rejected, size " + std::to_string(size)); }

  // Contradictions and modifications: ambiguous, never lock-on.
  const auto is_ambiguous = [](const Image &i, const std::string &what) {
    const CartridgeFinding f = recognize_genesis_cartridge(i);
    expect(f.kind == CartridgeKind::ambiguous && !f.note.empty() && f.mapper().empty(), what);
  };
  { Image i = standalone(); put(i, 0x120, 48, "ANOTHER GAME"); seal(i, 0, slot); is_ambiguous(i, "the two title fields disagree"); }
  { Image i = standalone(); put(i, 0x180, 14, "GM 00000000-00"); seal(i, 0, slot); is_ambiguous(i, "the product code disagrees with the title"); }
  { Image i = standalone(); put(i, 0x120, 48, "FICTIONAL"); put(i, 0x150, 48, "FICTIONAL"); seal(i, 0, slot); is_ambiguous(i, "the product code alone is not an identity"); }
  { Image i = standalone(); put32(i, 0x1A4, 0x3FFFFF); seal(i, 0, slot); is_ambiguous(i, "the header declares another ROM range"); }
  { Image i = standalone(); i[0x4000] ^= 0x55; is_ambiguous(i, "a modified image fails its own checksum"); }
  { Image i = standalone(); put(i, 0x120, 48, "SONIC & KNUCKLES HACK"); put(i, 0x150, 48, "SONIC & KNUCKLES HACK"); seal(i, 0, slot); is_ambiguous(i, "a modified title is not the base"); }
  for (const std::size_t size : {std::size_t{0x100000}, std::size_t{0x240000}, std::size_t{0x300000}, std::size_t{0x800000}}) {
    Image i(size); cartridge(i, 0, base_title, base_code, 1, size); is_ambiguous(i, "the base's identity at an unsupported size " + std::to_string(size)); }
  { Image i = combined(); i.resize(2 * slot + 1); is_ambiguous(i, "one byte over 4 MiB"); }
  { Image i(2 * slot); cartridge(i, 0, base_title, base_code, 1); is_ambiguous(i, "the second half is empty"); }
  { Image i(2 * slot); cartridge(i, 0, base_title, base_code, 1); cartridge(i, slot, base_title, base_code, 1); is_ambiguous(i, "a doubled base image is not a composite"); }
  { Image i(2 * slot); cartridge(i, 0, base_title, base_code, 1); cartridge(i, slot, "FICTIONAL GAME", "GM 00000000-00", 3); is_ambiguous(i, "an unknown attached cartridge is not guessed"); }
  { Image i = combined(); put32(i, slot + 0x1A4, 0x0FFFFF); seal(i, slot, slot); is_ambiguous(i, "the attached header declares another range"); }
  { Image i = combined(); i[slot + 0x4000] ^= 0x55; is_ambiguous(i, "a modified attached cartridge"); }

  // Nothing about the header scan depends on more than the header.
  const Image whole = standalone();
  expect(cartridge_header_resembles_lock_on(std::span<const std::uint8_t>(whole.data(), cartridge_header_bytes)), "the first 0x200 bytes are enough to want a closer look");
}

// Same bytes, any file name: the recognition never sees one. The catalog path check is by digest only.
void persistence(const fs::path &dir) {
  const fs::path file = dir / "library" / "library.json";
  const Image image = combined(), ordinary = [] { Image i(slot); cartridge(i, 0, "FICTIONAL GAME", "GM 00000000-00", 3); return i; }();
  const fs::path rom_path = dir / "misleading-name.bin", ordinary_path = dir / "Sonic and Knuckles.bin";
  write_file(rom_path, image);
  write_file(ordinary_path, ordinary);
  const std::string id = "genesis:" + segarecomp::sha256_hex(image), ordinary_id = "genesis:" + segarecomp::sha256_hex(ordinary);

  {  // auto-detected choice survives a restart
    Catalog c(file);
    c.load();
    expect(c.import_rom(view_of(image, rom_path), 1).entry != nullptr && c.import_rom(view_of(ordinary, ordinary_path), 1).entry != nullptr, "import");
    expect(c.save(), "save");
    Catalog again(file);
    again.load();
    expect(again.find_id(id)->mapper == "lock_on" && !again.find_id(id)->mapper_explicit, "the automatic choice survives a restart");
    expect(again.find_id(ordinary_id)->mapper.empty() && !again.find_id(ordinary_id)->mapper_explicit, "an ordinary entry: no file name makes it lock-on");
    const std::string text = [&] { std::ifstream in(file, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    expect(text.find("mapper_explicit") == std::string::npos && text.find("\"cartridge") == std::string::npos, "automatic records add no member to the file");
  }
  {  // an entry imported before this change gains recognition, only when the ROM is verified
    Catalog c(file);
    c.load();
    LibraryEntry &e = *c.find_id(id);
    e.mapper.clear();
    const CartridgeCheck check = check_entry_cartridge(e);
    expect(check.examined && check.verified && check.finding.kind == CartridgeKind::lock_on, "an older entry is recognized from its original ROM");
    expect(apply_cartridge_finding(e, check.finding) && e.mapper == "lock_on", "the older entry gains the declaration and reports the change");
    expect(!apply_cartridge_finding(e, check.finding), "an unchanged declaration is no change");
    // The ordinary game: not read whole, not changed.
    const CartridgeCheck ordinary_check = check_entry_cartridge(*c.find_id(ordinary_id));
    expect(ordinary_check.examined && !ordinary_check.verified, "an ordinary game is settled from its header alone");
    // A changed file at the recorded location says nothing about the game, in either direction.
    LibraryEntry stale = e;
    stale.rom_paths = {(dir / "changed.bin").string()};
    write_file(dir / "changed.bin", ordinary);
    const CartridgeCheck changed = check_entry_cartridge(stale);
    expect(changed.examined && !changed.verified, "a different file at the location is not this game");
    stale.rom_paths = {(dir / "absent.bin").string()};
    const CartridgeCheck missing = check_entry_cartridge(stale);
    expect(!missing.examined && !missing.verified && stale.mapper == "lock_on", "a missing ROM keeps the stored declaration");
    // The second recorded location is used when the first is stale.
    stale.rom_paths = {(dir / "absent.bin").string(), rom_path.string()};
    expect(check_entry_cartridge(stale).verified, "any recorded location with the right digest counts");
  }
  {  // manual choices outrank recognition: restart, reimport, examination
    for (const int choice : {1, 2}) {
      Catalog c(file);
      c.load();
      LibraryEntry *e = c.find_id(choice == 1 ? id : ordinary_id);
      select_cartridge(*e, choice);
      expect(c.save(), "save the choice");
      Catalog again(file);
      again.load();
      const std::string wanted = choice == 1 ? "" : "lock_on";
      e = again.find_id(choice == 1 ? id : ordinary_id);
      expect(e->mapper_explicit && e->mapper == wanted, "a manual choice survives a restart");
      const Image &bytes = choice == 1 ? image : ordinary;
      const fs::path &path = choice == 1 ? rom_path : ordinary_path;
      expect(again.import_rom(view_of(bytes, path), 2).entry == e && e->mapper_explicit && e->mapper == wanted, "a manual choice survives a duplicate import");
      expect(!apply_cartridge_finding(*e, recognize_genesis_cartridge(bytes)) && e->mapper == wanted, "recognition never overrides a manual choice");
      // Back to Automatic: the next check restores the recognized result; until then nothing claims a detection.
      const unsigned generation = e->cartridge_generation;
      select_cartridge(*e, 0);
      expect(!e->mapper_explicit && !e->cartridge_examined && e->cartridge_generation != generation, "Automatic asks for a new examination");
      if (!e->mapper.empty()) expect(cartridge_status_text(*e).find("detected") == std::string::npos, "an unconfirmed declaration is not called detected");
      const CartridgeCheck check = check_entry_cartridge(*e);
      if (check.verified) apply_cartridge_finding(*e, check.finding);
      expect(e->mapper == recognize_genesis_cartridge(bytes).mapper(), "Automatic follows the ROM again");
      expect(again.save(), "save Automatic");
    }
  }
  {  // duplicate import of an older record: an upgrade, not a reset
    Catalog c(file);
    c.load();
    LibraryEntry *e = c.find_id(id);
    e->mapper.clear();
    expect(c.import_rom(view_of(image, rom_path), 3).entry->mapper == "lock_on", "reimporting gains the automatic declaration");
  }
  {  // an ambiguous image: standard behavior, explained
    Image modified = image;
    modified[0x4000] ^= 0x55;
    Catalog c(dir / "ambiguous" / "library.json");
    c.load();
    LibraryEntry *e = c.import_rom(view_of(modified, dir / "m.bin"), 1).entry;
    expect(e != nullptr && e->mapper.empty() && !e->cartridge_note.empty(), "an ambiguous image stays standard with a reason");
    expect(cartridge_status_text(*e).find("Lock-on") != std::string::npos && cartridge_status_text(*e).rfind("Standard", 0) == 0, "the reason is shown, naming the effective hardware");
  }
  {  // what the line says
    LibraryEntry e;
    e.platform_id = "genesis";
    expect(cartridge_status_text(e).empty(), "an ordinary game has nothing to say");
    e.mapper = "lock_on";
    expect(cartridge_status_text(e) == "Lock-on cartridge - automatically detected", "automatic lock-on line");
    select_cartridge(e, 2);
    expect(cartridge_status_text(e) == "Lock-on cartridge - chosen manually", "manual lock-on line");
    select_cartridge(e, 1);
    expect(cartridge_status_text(e) == "Standard cartridge - chosen manually" && e.mapper.empty(), "manual standard line");
    e.platform_id = "master-system";
    expect(cartridge_status_text(e).empty(), "no cartridge line for another platform");
  }
  {  // Master System and foreign values
    const fs::path sms_file = dir / "sms" / "library.json";
    Catalog c(sms_file);
    c.load();
    RomView r = view_of(ordinary, dir / "s.sms", "master-system");
    r.mapper = "sega";
    LibraryEntry *e = c.import_rom(r, 1).entry;
    expect(e != nullptr && e->mapper == "sega" && !e->mapper_explicit && !check_entry_cartridge(*e).examined, "a Master System record is not examined");
    expect(!apply_cartridge_finding(*e, {CartridgeKind::lock_on, {}}) && e->mapper == "sega", "a finding never touches a Master System record");
    expect(c.save(), "save sms");
    const std::string text = [&] { std::ifstream in(sms_file, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    expect(text.find("mapper_explicit") == std::string::npos, "the Master System record is unchanged");
    // A hand-edited catalog cannot put a value of one platform on the other.
    auto root = json::parse(text);
    expect(root.has_value(), "parse");
    std::string edited = text;
    const auto at = edited.find("\"sega\"");
    edited.replace(at, 6, "\"lock_on\"");
    std::ofstream(sms_file, std::ios::binary) << edited;
    Catalog back(sms_file);
    back.load();
    expect(back.entries().size() == 1 && back.entries()[0].mapper.empty(), "lock_on is not a Master System declaration");
  }
}

// The exact-reuse check, on top of the key: a record declaring other hardware is not reusable even in the requested entry.
void exact_reuse(const fs::path &dir) {
  const fs::path entry = dir / "exact";
  fs::create_directories(entry);
#if defined(_WIN32)
  std::ofstream(entry / "game.exe") << "x";
#else
  std::ofstream(entry / "game") << "x";
#endif
  std::ofstream(entry / "status.json") << "{\"status\":\"ok\"}";
  RomView r;
  r.platform_id = "genesis";
  r.aot_policy = "compatibility";
  const Layout layout;
  const auto record = [&](const std::string &members) { std::ofstream(entry / "metadata.json") << "{" << members << "\"platform\":\"genesis\"}"; };
  record("");
  expect(entry_ready_for(entry, r, layout), "a standard record serves a standard request");
  r.mapper = "lock_on";
  expect(!entry_ready_for(entry, r, layout), "a standard record never serves a lock-on request");
  record("\"mapper\":\"lock_on\",");
  expect(entry_ready_for(entry, r, layout), "a lock-on record serves a lock-on request");
  r.mapper.clear();
  expect(!entry_ready_for(entry, r, layout), "a lock-on record never serves a standard request");
  r.platform_id = "master-system";
  record("\"mapper\":\"sega\",");
  r.mapper = "sega";
  expect(entry_ready_for(entry, r, layout), "the Master System path is not affected");
}

// An ordinary game's key segment is exactly the historical string; a declaration always selects another.
void cache_keys() {
  RomView r;
  r.platform_id = "genesis";
  r.sha256 = std::string(64, 'a');
  r.aot_policy = "compatibility";
  expect(build_options_key(r) == "genesis", "standard Compatibility key is the historical key");
  r.hint_alias_discovery = true;
  expect(build_options_key(r) == "genesis:hint-alias-discovery=1", "standard discovery key is the historical key");
  r.hint_alias_discovery = false;
  r.aot_policy = "optimized";
  r.aot_model = "gen2";
  expect(build_options_key(r) == "genesis:aot=optimized:model=gen2:pid=", "standard Optimized key is the historical key");
  r.mapper = "lock_on";
  expect(build_options_key(r) == "genesis:aot=optimized:model=gen2:pid=:mapper=lock_on", "lock-on Optimized key");
  r.aot_policy = "compatibility";
  expect(build_options_key(r) == "genesis:mapper=lock_on", "lock-on Compatibility key");
  RomView sms;
  sms.platform_id = "master-system";
  sms.mapper = "sega";
  expect(build_options_key(sms) == std::string("master-system:") + sms_profile + ":mapper=sega:manifest=", "the Master System key is unchanged");
}

// A standard build is never offered as a lock-on one, and the reverse, through the older-build predicate. entry_ready_for repeats it for the exact key.
void older_builds(const fs::path &dir) {
  const fs::path games = dir / "games";
  const std::string rom(64, 'a'), key_std(32, '1'), key_lock(32, '2');
  const auto make = [&](const std::string &key, const std::string &mapper_member) {
    const fs::path b = games / key;
    fs::create_directories(b);
#if defined(_WIN32)
    std::ofstream(b / "game.exe") << "x";
#else
    std::ofstream(b / "game") << "x";
#endif
    std::ofstream(b / "status.json") << "{\"status\":\"ok\"}";
    std::ofstream(b / "metadata.json") << "{\"rom_sha256\":\"" << rom << "\",\"segarecomp_version\":\"0.5.0\",\"platform\":\"genesis\"," << mapper_member << "\"runtime_contract\":" << runtime_contract
                                       << ",\"host\":\"" << host_description() << "\",\"toolchain\":\"zig 0\",\"rom_path\":\"referenced, not copied\"}";
  };
  make(key_std, "");
  make(key_lock, "\"mapper\":\"lock_on\",\"mapper_declaration\":\"option\",");
  LibraryEntry e;
  e.platform_id = "genesis";
  e.sha256 = rom;
  BuildRecord std_build, lock_build;
  std_build.key = key_std;
  lock_build.key = key_lock;
  const Layout layout;
  e.builds = {std_build};
  e.mapper = "lock_on";
  OlderBuild found = find_playable_older_build(e, std::string(32, '9'), "compatibility", layout, games);
  expect(!found.playable && found.reason.find("cartridge") != std::string::npos, "a standard build is refused for a lock-on game: " + found.reason);
  e.builds = {lock_build};
  found = find_playable_older_build(e, std::string(32, '9'), "compatibility", layout, games);
  expect(found.reason.find("cartridge") == std::string::npos, "a lock-on build matches a lock-on game: " + found.reason);
  e.mapper.clear();
  found = find_playable_older_build(e, std::string(32, '9'), "compatibility", layout, games);
  expect(!found.playable && found.reason.find("cartridge") != std::string::npos, "a lock-on build is refused for a standard game: " + found.reason);
  e.builds = {std_build};
  found = find_playable_older_build(e, std::string(32, '9'), "compatibility", layout, games);
  expect(found.reason.find("cartridge") == std::string::npos, "a standard build matches a standard game: " + found.reason);
}
} // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "segarecomp_cartridge_recognition_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  detection();
  persistence(dir);
  older_builds(dir);
  cache_keys();
  exact_reuse(dir);
  fs::remove_all(dir);
  if (failures != 0) { std::cerr << failures << " failure(s)\n"; return 1; }
  std::cout << "cartridge_recognition_test: ok\n";
  return 0;
}
