// Covers, Libretro candidate naming and launcher settings: hostile and offline-safe. No window, no network.

#include "covers.hpp"
#include "launcher_settings.hpp"
#include "pixel_assets.hpp"
#include "rom_zip.hpp"
#include "segarecomp/sha256.hpp"

#include "zlib.h"  // the vendored deflate subset: the test builds real deflated archives

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
void put32(std::string &s, unsigned long v) { put16(s, static_cast<unsigned>(v & 0xFFFFU)); put16(s, static_cast<unsigned>((v >> 16U) & 0xFFFFU)); }

struct ZipFile { std::string name, data; bool deflate = false; unsigned flags = 0; unsigned method_override = 0xFFFF; long crc_delta = 0; };

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
    const std::string body = f.deflate ? raw_deflate(f.data) : f.data;
    const unsigned long crc = (crc32(0, reinterpret_cast<const Bytef *>(f.data.data()), static_cast<uInt>(f.data.size())) + static_cast<unsigned long>(f.crc_delta)) & 0xFFFFFFFFUL;
    const unsigned method = f.method_override != 0xFFFF ? f.method_override : (f.deflate ? 8U : 0U);
    const unsigned long offset = zip.size();
    std::string local;
    put32(local, 0x04034b50UL); put16(local, 20); put16(local, f.flags); put16(local, method); put16(local, 0); put16(local, 0);
    put32(local, crc); put32(local, body.size()); put32(local, f.data.size()); put16(local, static_cast<unsigned>(f.name.size())); put16(local, 0);
    zip += local + f.name + body;
    put32(cd, 0x02014b50UL); put16(cd, 20); put16(cd, 20); put16(cd, f.flags); put16(cd, method); put16(cd, 0); put16(cd, 0);
    put32(cd, crc); put32(cd, body.size()); put32(cd, f.data.size()); put16(cd, static_cast<unsigned>(f.name.size())); put16(cd, 0); put16(cd, 0);
    put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, offset);
    cd += f.name;
  }
  const unsigned long cd_offset = zip.size();
  zip += cd;
  put32(zip, 0x06054b50UL); put16(zip, 0); put16(zip, 0); put16(zip, static_cast<unsigned>(files.size())); put16(zip, static_cast<unsigned>(files.size()));
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
  {  // zipped ROMs: one ROM inside, stored or deflated, extracted by digest; everything else refused with a reason
    std::string rom(0x8000, '\0');
    for (std::size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<char>((i * 37U + 11U) & 0xFFU);
    rom.replace(0x7FF0, 8, "TMR SEGA");
    const std::string digest = segarecomp::sha256_hex({reinterpret_cast<const std::uint8_t *>(rom.data()), rom.size()});
    const fs::path roms = dir / "library" / "roms";
    expect(is_zip_path("a/Game.ZIP") && !is_zip_path("a/game.sms") && !is_zip_path("zip"), "zip detection by extension, case-insensitive");
    spit(dir / "stored.zip", make_zip({{"readme.txt", "fictional", false}, {"Fictional Cart.sms", rom, false}}));
    ZipRom r = extract_zip_rom(dir / "stored.zip", roms);
    expect(r.ok && r.extracted == roms / (digest + ".sms") && slurp(r.extracted) == rom && r.inner_name == "Fictional Cart.sms", "a stored ROM is extracted by digest, byte-exact");
    spit(dir / "deflated.zip", make_zip({{"Folder/", "", false}, {"Folder/Fictional Cart.sms", rom, true}, {"__MACOSX/Folder/._Fictional Cart.sms", "x", false}}));
    const std::string deflated = slurp(dir / "deflated.zip");
    expect(deflated.size() < rom.size(), "the test archive really is compressed");
    r = extract_zip_rom(dir / "deflated.zip", roms);
    expect(r.ok && slurp(r.extracted) == rom, "a deflated ROM is inflated byte-exact (resource forks and folders ignored)");
    expect(slurp(dir / "deflated.zip") == deflated, "the archive itself is never modified");
    const auto refused = [&](const std::string &name, const std::string &bytes, const std::string &needle) {
      spit(dir / name, bytes);
      const ZipRom z = extract_zip_rom(dir / name, roms);
      expect(!z.ok && z.error.find(needle) != std::string::npos, name + " is refused (" + needle + "): " + z.error);
    };
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
    std::size_t files = 0;
    for (const auto &e : fs::directory_iterator(roms)) { (void)e; ++files; }
    expect(files == 1, "refused archives leave nothing behind; the same ROM twice is one file");
  }
  fs::remove_all(dir);
  if (failures != 0) return 1;
  std::cout << "launcher_ui_support_test: ok\n";
  return 0;
}
