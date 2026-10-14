// Covers, Libretro candidate naming and launcher settings: hostile and offline-safe. No window, no network.

#include "covers.hpp"
#include "launcher_settings.hpp"
#include "library_catalog.hpp"
#include "pixel_assets.hpp"
#include "rom_zip.hpp"
#include "segarecomp/sha256.hpp"

#include "zlib.h"  // the vendored deflate subset: the test builds real deflated archives

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace launcher;

namespace {
int failures = 0;
void expect(bool ok, const std::string &what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
void spit(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
std::string slurp(const fs::path &p) { std::ifstream in(p, std::ios::binary); std::ostringstream o; o << in.rdbuf(); return o.str(); }

void put16(std::string &s, unsigned v) { s.push_back(static_cast<char>(v & 0xFFU)); s.push_back(static_cast<char>((v >> 8U) & 0xFFU)); }
void put32(std::string &s, std::size_t v) { put16(s, static_cast<unsigned>(v & 0xFFFFU)); put16(s, static_cast<unsigned>((v >> 16U) & 0xFFFFU)); }

// One archive member. `declared` overrides the uncompressed size written in the headers (0: the real size); `cut` drops bytes from the end of
// the stored body (a truncated stream); `crc_delta` corrupts the recorded CRC-32.
struct ZipFile { std::string name, data; bool deflate = false; unsigned flags = 0; unsigned method_override = 0xFFFF; long crc_delta = 0; std::size_t declared = 0; std::size_t cut = 0; };

std::string raw_deflate(const std::string &data) {
  z_stream z{};
  deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&z, static_cast<uLong>(data.size())) + 64, '\0');
  z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
  z.avail_in = static_cast<uInt>(data.size());
  z.next_out = reinterpret_cast<Bytef *>(out.data());
  z.avail_out = static_cast<uInt>(out.size());
  deflate(&z, Z_FINISH);
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}

// A classic zip archive written byte by byte (local headers, central directory, end record).
std::string make_zip(const std::vector<ZipFile> &files) {
  std::string zip, cd;
  for (const auto &f : files) {
    std::string body = f.deflate ? raw_deflate(f.data) : f.data;
    body.resize(body.size() - std::min(f.cut, body.size()));
    const std::size_t usize = f.declared != 0 ? f.declared : f.data.size();
    const std::size_t crc = (static_cast<std::size_t>(crc32(0, reinterpret_cast<const Bytef *>(f.data.data()), static_cast<uInt>(f.data.size()))) + static_cast<std::size_t>(f.crc_delta)) & 0xFFFFFFFFU;
    const unsigned method = f.method_override != 0xFFFF ? f.method_override : (f.deflate ? 8U : 0U);
    const std::size_t offset = zip.size();
    std::string local;
    put32(local, 0x04034b50U); put16(local, 20); put16(local, f.flags); put16(local, method); put16(local, 0); put16(local, 0);
    put32(local, crc); put32(local, body.size()); put32(local, usize); put16(local, static_cast<unsigned>(f.name.size())); put16(local, 0);
    zip += local + f.name + body;
    put32(cd, 0x02014b50U); put16(cd, 20); put16(cd, 20); put16(cd, f.flags); put16(cd, method); put16(cd, 0); put16(cd, 0);
    put32(cd, crc); put32(cd, body.size()); put32(cd, usize); put16(cd, static_cast<unsigned>(f.name.size())); put16(cd, 0); put16(cd, 0);
    put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, offset);
    cd += f.name;
  }
  const std::size_t cd_offset = zip.size();
  zip += cd;
  put32(zip, 0x06054b50U); put16(zip, 0); put16(zip, 0); put16(zip, static_cast<unsigned>(files.size())); put16(zip, static_cast<unsigned>(files.size()));
  put32(zip, cd.size()); put32(zip, cd_offset); put16(zip, 0);
  return zip;
}

// A valid 1x1 PNG (too small for a cover).
const unsigned char tiny_png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                                  0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
                                  0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "segarecomp_ui_support_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path covers = dir / "library" / "covers";
  const std::string sha(64, 'a');
  const fs::path good = asset_path("icon-drop.png");  // a real PNG shipped with the launcher

  expect(fs::is_regular_file(good), "a shipped PNG asset exists");
  {  // a valid image is validated and copied; the original is untouched
    const std::string before = slurp(good);
    const CoverImport r = import_cover(good, covers, sha);
    expect(r.ok && r.file_name == sha + ".png" && fs::is_regular_file(covers / r.file_name), "a valid PNG is stored under the game's digest");
    expect(slurp(good) == before && slurp(covers / r.file_name) == before, "the original is untouched and the copy is byte-exact");
    expect(existing_cover_for(covers, sha) == sha + ".png", "the stored cover is found again by digest (re-import re-associates it)");
    expect(!fs::exists(covers / (sha + ".png.tmp")), "no temporary file is left behind");
  }
  {  // invalid images are refused with a plain reason and never stored
    spit(dir / "not_image.png", "this is not an image");
    spit(dir / "truncated.png", slurp(good).substr(0, 60));
    spit(dir / "tiny.png", std::string(reinterpret_cast<const char *>(tiny_png), sizeof tiny_png));
    spit(dir / "fake_jpeg.jpg", std::string("\xFF\xD8\xFF\xE0", 4) + std::string(100, 'x'));
    spit(dir / "huge.png", std::string("\x89PNG\r\n\x1a\n", 8) + std::string(3U << 20U, 'x'));
    const std::string other(64, 'b');
    for (const char *name : {"not_image.png", "truncated.png", "tiny.png", "fake_jpeg.jpg", "huge.png", "missing.png"}) {
      const CoverImport r = import_cover(dir / name, covers, other);
      expect(!r.ok && !r.error.empty(), std::string("refused: ") + name);
    }
    expect(existing_cover_for(covers, other).empty(), "nothing is stored for a refused image");
    expect(!import_cover(good, covers, "../../escape").ok, "a hostile identity is refused");
  }
  {  // cover removal only touches plain files with a bare name
    spit(covers / "x.png", "x");
    spit(dir / "outside.txt", "keep");
    expect(!remove_cover_file(covers, "../../outside.txt") && fs::exists(dir / "outside.txt"), "traversal is refused");
    expect(!remove_cover_file(covers, "") && !remove_cover_file(covers, "..") && !remove_cover_file(covers, "sub/x.png"), "non-bare names are refused");
    std::error_code ec;
    fs::create_symlink(dir / "outside.txt", covers / "link.png", ec);
    if (!ec) expect(!remove_cover_file(covers, "link.png") && fs::exists(dir / "outside.txt"), "a symlink is never followed or deleted");
    expect(remove_cover_file(covers, "x.png") && !fs::exists(covers / "x.png"), "a plain cover file is removed");
  }
  {  // Libretro candidate naming: conservative, preferred-region ordered, never claimed as a match
    const auto names = libretro_candidate_names("Fictional  Game: The Sequel", {"Europe", "USA"});
    expect(names.size() == 2 && names[0] == "Fictional Game_ The Sequel (Europe)" && names[1] == "Fictional Game_ The Sequel (USA)", "region order and Libretro's replacement of reserved characters");
    expect(libretro_candidate_names("", {"USA"}).empty(), "no title, no candidate");
    expect(libretro_candidate_names("Game", {}).empty(), "no region preference, no guessed region");
    expect(libretro_candidate_names("Game", {"USA", "USA"}).size() == 1, "duplicate regions collapse");
    expect(libretro_system_dir("genesis") == "Sega - Mega Drive - Genesis" && libretro_system_dir("master-system") == "Sega - Master System - Mark III" && libretro_system_dir("saturn").empty(), "system trees");
    expect(libretro_candidate_url("genesis", "Fictional Game (USA)") == "https://thumbnails.libretro.com/Sega%20-%20Mega%20Drive%20-%20Genesis/Named_Boxarts/Fictional%20Game%20%28USA%29.png", "percent-encoded https URL");
    expect(libretro_candidate_url("saturn", "x").empty() && libretro_candidate_url("genesis", "").empty(), "unusable inputs give no URL");
    expect(libretro_candidate_url("genesis", "a b").rfind("https://", 0) == 0, "only https is ever produced");
  }
  {  // launcher settings: defaults, round trip, damaged file, clamps
    const fs::path file = dir / "library" / "launcher-settings.json";
    LauncherSettings d = load_launcher_settings(file);
    expect(d.view == "grid" && d.sort == "last_played" && d.cleanup.mode == "never" && d.adopt_cache_on_start, "defaults when the file is missing");
    d.view = "list"; d.sort = "size"; d.compact = true; d.cleanup.mode = "keep_last_n"; d.cleanup.keep_last_n = 7; d.preferred_regions = {"Japan"};
    expect(save_launcher_settings(d, file), "save");
    const LauncherSettings r = load_launcher_settings(file);
    expect(r.view == "list" && r.sort == "size" && r.compact && r.cleanup.mode == "keep_last_n" && r.cleanup.keep_last_n == 7 && r.preferred_regions.size() == 1 && r.preferred_regions[0] == "Japan", "round trip");
    spit(file, "{\"view\":\"carousel\",\"sort\":\"nonsense\",\"cleanup_mode\":\"delete-everything\",\"keep_last_n\":-5,\"soft_limit_mb\":1,\"preferred_regions\":[\"\",\"x\"]}");
    const LauncherSettings h = load_launcher_settings(file);
    expect(h.view == "grid" && h.sort == "last_played" && h.cleanup.mode == "never" && h.cleanup.keep_last_n == 1 && h.cleanup.soft_limit_mb == 256 && h.preferred_regions.size() == 1, "hostile values fall back to safe ones");
    spit(file, "not json at all");
    expect(load_launcher_settings(file).view == "grid", "a damaged file loads as defaults");
  }
  {  // zipped ROMs: one ROM inside, stored or deflated, decoded into a buffer of exactly its declared size, extracted by digest
    std::string rom(0x8000, '\0');
    for (std::size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<char>((i * 37U + 11U) & 0xFFU);
    rom.replace(0x7FF0, 8, "TMR SEGA");
    rom[0x7FFF] = 0x4C;  // header region/size byte (export, 32 KiB), as the launcher fixture's synthetic cartridges have
    const std::string digest = segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(rom.data()), rom.size()});
    const fs::path roms = dir / "library" / "roms";
    const fs::path target = roms / (digest + ".sms");
    const auto count_files = [&] { std::size_t n = 0; std::error_code ec; for (const auto &e : fs::directory_iterator(roms, ec)) { (void)e; ++n; } return n; };
    expect(is_zip_path("a/Game.ZIP") && !is_zip_path("a/game.sms") && !is_zip_path("zip"), "zip detection by extension, case-insensitive");

    // valid stored and deflated archives; the deflated one expands to exactly its declared size
    spit(dir / "stored.zip", make_zip({{"readme.txt", "fictional", false}, {"Fictional Cart.sms", rom, false}}));
    ZipRom r = extract_zip_rom(dir / "stored.zip", roms);
    expect(r.ok && r.extracted == target && slurp(r.extracted) == rom && r.inner_name == "Fictional Cart.sms", "a stored ROM is extracted by digest, byte-exact");
    expect(r.output_capacity == rom.size(), "the stored ROM buffer is exactly its declared size");
    fs::remove(target);
    spit(dir / "deflated.zip", make_zip({{"Folder/", "", false}, {"Folder/Fictional Cart.sms", rom, true}, {"__MACOSX/Folder/._Fictional Cart.sms", "x", false}}));
    const std::string deflated = slurp(dir / "deflated.zip");
    expect(deflated.size() < rom.size(), "the test archive really is compressed");
    const std::size_t stb_before = stb_requested_bytes_for_tests();
    r = extract_zip_rom(dir / "deflated.zip", roms);
    expect(r.ok && slurp(r.extracted) == rom && r.output_capacity == rom.size(), "a deflated ROM expanding to exactly its declared size is inflated byte-exact");
    expect(stb_requested_bytes_for_tests() == stb_before, "the deflate decoder allocates nothing of its own (output goes to the bounded buffer)");
    expect(slurp(dir / "deflated.zip") == deflated, "the archive itself is never modified");

    const auto refused = [&](const std::string &name, const std::string &bytes, const std::string &needle) {
      spit(dir / name, bytes);
      const std::size_t files_before = count_files();
      const std::size_t stb_at = stb_requested_bytes_for_tests();
      const ZipRom z = extract_zip_rom(dir / name, roms);
      expect(!z.ok && z.error.find(needle) != std::string::npos, name + " is refused (" + needle + "): " + z.error);
      expect(stb_requested_bytes_for_tests() == stb_at, name + ": no decoder allocation");
      expect(count_files() == files_before, name + ": nothing is written to the roms folder");
      return z;
    };
    // a stream that expands beyond its declared size: decoding stops at the buffer end, nothing larger is ever allocated
    ZipFile over{"c.sms", rom, true};
    over.declared = rom.size() - 1;
    ZipRom z = refused("over.zip", make_zip({over}), "larger than its declared size");
    expect(z.output_capacity == rom.size() - 1, "the over-long stream got a buffer of its declared size only");
    // a "bomb": declares 1 KiB, deflates 8 MiB of zeros
    ZipFile bomb{"bomb.md", std::string(8U << 20U, '\0'), true};
    bomb.declared = 1024;
    const std::string bomb_zip = make_zip({bomb});
    expect(bomb_zip.size() < 64U * 1024U, "the bomb archive is small");
    z = refused("bomb.zip", bomb_zip, "larger than its declared size");
    expect(z.output_capacity == 1024, "the bomb is decoded into 1 KiB at most, never its real 8 MiB");
    // a stream that ends before its declared size, and a truncated compressed body
    ZipFile under{"c.sms", rom, true};
    under.declared = rom.size() + 4096;
    refused("under.zip", make_zip({under}), "");
    ZipFile cut{"c.sms", rom, true};
    cut.cut = 40;
    refused("cut.zip", make_zip({cut}), "");
    // declared output above the maximum: refused before any buffer exists
    ZipFile huge{"c.md", "tiny", false};
    huge.declared = static_cast<std::size_t>(kMaxZipRomBytes) + 1U;
    z = refused("huge.zip", make_zip({huge}), "implausible size");
    expect(z.output_capacity == 0, "an over-limit declaration allocates no output buffer");
    // archive-level refusals
    refused("none.zip", make_zip({{"notes.txt", "fictional", true}}), "contains no");
    refused("two.zip", make_zip({{"a.md", rom, false}, {"b.sms", rom, true}}), "contains 2 ROMs");
    ZipFile encrypted{"c.sms", rom, false};
    encrypted.flags = 1;
    refused("encrypted.zip", make_zip({encrypted}), "encrypted");
    ZipFile bzip{"c.sms", rom, false};
    bzip.method_override = 12;
    refused("bzip2.zip", make_zip({bzip}), "compression method");
    ZipFile bad_crc{"c.sms", rom, true};
    bad_crc.crc_delta = 1;
    refused("crc.zip", make_zip({bad_crc}), "CRC");
    const std::string whole = make_zip({{"c.sms", rom, true}});
    refused("truncated.zip", whole.substr(0, 40) + whole.substr(whole.size() - 22), "");
    refused("garbage.zip", std::string(300, 'z'), "not a zip");

    // an existing extracted copy is trusted only when its own SHA-256 matches
    expect(fs::is_regular_file(target), "the deflated ROM is in place");
    const auto mtime = fs::last_write_time(target);
    r = extract_zip_rom(dir / "deflated.zip", roms);
    expect(r.ok && r.extracted == target && slurp(target) == rom && fs::last_write_time(target) == mtime, "a correct existing copy is reused (idempotent re-import)");
    std::string damaged = rom;
    damaged[0x100] = static_cast<char>(damaged[0x100] ^ 0x5A);
    spit(target, damaged);
    r = extract_zip_rom(dir / "deflated.zip", roms);
    expect(r.ok && slurp(target) == rom, "a same-size damaged copy is not trusted: it is replaced by the verified contents");
    spit(target, rom.substr(0, 1000));
    r = extract_zip_rom(dir / "deflated.zip", roms);
    expect(r.ok && slurp(target) == rom, "a different-size copy is not trusted: it is replaced");
    // a failed write never exposes a partial ROM (nor a temporary file)
    fs::remove(target);
    set_zip_write_failure_for_tests(true);
    r = extract_zip_rom(dir / "deflated.zip", roms);
    set_zip_write_failure_for_tests(false);
    expect(!r.ok && !fs::exists(target) && !fs::exists(roms / (digest + ".partial")), "a failed extraction leaves neither a ROM nor a partial file");
    spit(target, damaged);
    set_zip_write_failure_for_tests(true);
    r = extract_zip_rom(dir / "deflated.zip", roms);
    set_zip_write_failure_for_tests(false);
    expect(!r.ok && slurp(target) == damaged && !fs::exists(roms / (digest + ".partial")), "a failed replacement leaves the old file untouched and nothing partial");
    r = extract_zip_rom(dir / "deflated.zip", roms);
    expect(r.ok && slurp(target) == rom, "the next import repairs it");
#if !defined(_WIN32)
    {  // a link at the content-addressed name is refused: never followed, never deleted, the file it points to is untouched
      fs::remove(target);
      spit(dir / "elsewhere.sms", "outside the roms folder");
      fs::create_symlink(dir / "elsewhere.sms", target);
      r = extract_zip_rom(dir / "deflated.zip", roms);
      expect(!r.ok && r.error.find("unexpected item") != std::string::npos, "a symlink in place of the extracted ROM is refused: " + r.error);
      expect(fs::is_symlink(target) && slurp(dir / "elsewhere.sms") == "outside the roms folder", "the link and its target are left untouched");
      fs::remove(target);
      // a roms folder that is itself a link is refused
      const fs::path linked_roms = dir / "linked-library" / "roms";
      fs::create_directories(linked_roms.parent_path());
      fs::create_directories(dir / "real-roms");
      fs::create_directory_symlink(dir / "real-roms", linked_roms);
      r = extract_zip_rom(dir / "deflated.zip", linked_roms);
      expect(!r.ok && fs::is_empty(dir / "real-roms"), "a roms folder that is a link is refused, nothing is written through it");
    }
#endif
    fs::create_directory(target);  // a folder in its place is refused too (and kept)
    r = extract_zip_rom(dir / "deflated.zip", roms);
    expect(!r.ok && fs::is_directory(target), "a folder in place of the extracted ROM is refused and left alone");
    fs::remove(target);

    {  // the same ROM imported plain and zipped is one logical game (unchanged library de-duplication)
      spit(dir / "Fictional Cart.sms", rom);
      Catalog catalog(dir / "library" / "dedup.json");
      catalog.load();
      const Layout layout{};
      const auto plain = catalog.import_rom(inspect_rom_or_zip(dir / "Fictional Cart.sms", layout, roms), 1);
      const auto zipped = catalog.import_rom(inspect_rom_or_zip(dir / "stored.zip", layout, roms), 2);
      const auto again = catalog.import_rom(inspect_rom_or_zip(dir / "deflated.zip", layout, roms), 3);
      expect(plain.error.empty() && zipped.error.empty(), "plain and zipped copies are recognized: " + plain.error + " / " + zipped.error);
      expect(plain.added && zipped.entry != nullptr && !zipped.added && again.entry != nullptr && !again.added && catalog.entries().size() == 1,
             "plain, stored-zip and deflated-zip imports of one ROM are one library entry");
      expect(catalog.entries().size() == 1 && catalog.entries()[0].sha256 == digest, "its identity is the ROM's SHA-256");
    }
    expect(count_files() == 1, "one extracted file for one ROM, however often and however it was zipped");
  }
  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "launcher_ui_support_test: ok\n";
  return 0;
}
