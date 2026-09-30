// SEG-009-T002: Master System cartridge ingestion, ImageSet declaration, run-time memory map/mapper and the agreement
// between the generation-time declaration and the run-time code_image over the enumerated mapper contract.
// Project-authored synthetic ROMs only (deterministic PRNG bytes plus a synthetic `TMR SEGA` header).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "segarecomp/machine/master_system/cartridge.hpp"
#include "segarecomp/machine/master_system/emit.hpp"
#include "segarecomp/machine/master_system/image_set.hpp"
#include "segarecomp/sha256.hpp"
#include "sms_memory.h"

using namespace segarecomp::machine::master_system;
using segarecomp::codegen::z80::ImageKind;
using segarecomp::codegen::z80::ImageSet;

namespace {

int failures = 0;
void check(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", message.c_str());
  }
}

struct Prng {
  std::uint64_t state;
  std::uint32_t next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint32_t>(state >> 33);
  }
};

// Synthetic ROM: pseudo-random bytes, every byte of bank n's first page carries a marker, a TMR SEGA header at 0x7FF0.
std::vector<std::uint8_t> make_rom(std::size_t size, unsigned region = 4, bool header = true, std::size_t header_at = 0x7FF0) {
  Prng prng{0x5EC0 + size};
  std::vector<std::uint8_t> rom(size);
  for (auto& b : rom) b = static_cast<std::uint8_t>(prng.next());
  for (std::size_t bank = 0; bank < size / 0x4000; ++bank) rom[bank * 0x4000 + 0x2000] = static_cast<std::uint8_t>(0x80 + bank);
  if (header && header_at + 16 <= size) {
    std::memcpy(&rom[header_at], "TMR SEGA", 8);
    rom[header_at + 8] = rom[header_at + 9] = 0;
    rom[header_at + 15] = static_cast<std::uint8_t>((region << 4) | 0x0C);
  }
  return rom;
}

IngestOptions sega() {
  IngestOptions o;
  o.declarations.push_back({"sega", DeclarationSource::build_option});
  return o;
}

IngestOptions declared(const char* family, DeclarationSource source = DeclarationSource::build_option) {
  IngestOptions o;
  o.declarations.push_back({family, source});
  return o;
}

// ---- ingestion --------------------------------------------------------------------------------------------------

void test_ingestion() {
  const auto rom = make_rom(0x20000);
  const auto ok = ingest_cartridge(rom, sega());
  check(ok.ok(), "a valid sega-declared ROM is accepted");
  check(ok.identity.mapper == SMS_MAPPER_SEGA && ok.identity.declaration_source == DeclarationSource::build_option,
        "identity records the declared family and its source");
  check(ok.identity.size == 0x20000 && ok.identity.bank_count == 8 && ok.identity.sha256 == segarecomp::sha256_hex(rom),
        "identity records size, bank count and sha256");
  check(ok.identity.header_class == "tmr_sega" && ok.identity.region == "sms_export" && ok.identity.header_offset == 0x7FF0,
        "identity records header class and region");
  check(ok.identity.profile == "sms2_ntsc_export", "identity records the profile");
  check(ok.rom == rom, "the validated ROM bytes are retained exactly");

  check(ingest_cartridge(rom, IngestOptions{}).error == SMS_ERROR_MAPPER_UNDECLARED,
        "a valid header with no declaration fails closed (never a Sega-mapper default)");
  check(ingest_cartridge(rom, declared("mystery")).error == SMS_ERROR_MAPPER_UNDECLARED, "an unknown mapper name is undeclared");
  for (const char* name : {"codemasters", "Korean", "msx", "janggun", "4-pak", "eeprom_93c46", "nemesis"})
    check(ingest_cartridge(rom, declared(name)).error == SMS_ERROR_MAPPER_UNSUPPORTED,
          std::string("declared non-baseline mapper ") + name + " is unsupported");
  {
    IngestOptions both = sega();
    both.declarations.push_back({"rom_only", DeclarationSource::manifest});
    check(ingest_cartridge(rom, both).error == SMS_ERROR_MAPPER_UNDECLARED, "conflicting declarations are an error");
    IngestOptions same = sega();
    same.declarations.push_back({"SEGA", DeclarationSource::fixture_builder});
    const auto r = ingest_cartridge(rom, same);
    check(r.ok() && r.identity.declaration_source == DeclarationSource::build_option, "agreeing declarations: the highest-precedence source is recorded");
  }
  // sizes
  for (std::size_t size : {0x8000u, 0x10000u, 0x20000u, 0x40000u, 0x80000u})
    check(ingest_cartridge(make_rom(size), sega()).ok(), "size " + std::to_string(size) + " accepted");
  for (std::size_t size : {0x4000u, 0x8001u, 0x18000u, 0x100000u})
    check(ingest_cartridge(make_rom(size, 4, true, size >= 0x8000 ? 0x7FF0 : 0x3FF0), sega()).error == SMS_ERROR_ROM_SIZE_UNSUPPORTED,
          "size " + std::to_string(size) + " rejected");
  check(ingest_cartridge(make_rom(0x10000), declared("rom_only")).error == SMS_ERROR_ROM_SIZE_UNSUPPORTED, "rom_only needs exactly 32 KiB");
  check(ingest_cartridge(make_rom(0x8000), declared("rom_only")).ok(), "rom_only 32 KiB accepted");
  // profile
  check(ingest_cartridge(make_rom(0x20000, 3), sega()).error == SMS_ERROR_PROFILE_UNSUPPORTED, "Japanese region is unsupported");
  check(ingest_cartridge(make_rom(0x20000, 6), sega()).error == SMS_ERROR_PROFILE_UNSUPPORTED, "a Game Gear header is unsupported");
  check(ingest_cartridge(make_rom(0x20000, 6), [] { auto o = sega(); o.explicit_profile = true; return o; }()).error ==
            SMS_ERROR_PROFILE_UNSUPPORTED,
        "explicit selection never overrides a recognized Game Gear header");
  check(ingest_cartridge(make_rom(0x20000, 4, false), sega()).error == SMS_ERROR_PROFILE_UNSUPPORTED,
        "a headerless ROM needs explicit profile selection");
  {
    auto o = sega();
    o.explicit_profile = true;
    const auto r = ingest_cartridge(make_rom(0x20000, 4, false), o);
    check(r.ok() && r.identity.header_class == "none" && r.identity.region == "unspecified", "explicit selection admits a headerless ROM");
    auto bad = make_rom(0x20000, 9);  // invalid region nibble
    check(ingest_cartridge(bad, sega()).error == SMS_ERROR_PROFILE_UNSUPPORTED, "an invalid-region header needs explicit selection");
    check(ingest_cartridge(bad, o).ok(), "explicit selection admits an invalid-region header");
  }
  // the header and checksum never decide acceptance
  auto odd = make_rom(0x20000);
  odd[0x7FFA] ^= 0xFF;
  check(ingest_cartridge(odd, sega()).ok(), "a header checksum mismatch does not reject");
  check(ingest_cartridge(odd, sega()).identity.checksum == ChecksumStatus::mismatch, "a checksum mismatch is recorded");
  auto good = make_rom(0x20000);
  {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 0x20000; ++i)
      if (i < 0x7FF0 || i >= 0x8000) sum += good[i];
    good[0x7FFF] = 0x4F;  // size code F = 128 KiB
    sum = 0;
    for (std::size_t i = 0; i < 0x20000; ++i)
      if (i < 0x7FF0 || i >= 0x8000) sum += good[i];
    good[0x7FFA] = static_cast<std::uint8_t>(sum);
    good[0x7FFB] = static_cast<std::uint8_t>(sum >> 8);
    check(ingest_cartridge(good, sega()).identity.checksum == ChecksumStatus::match, "a matching checksum is recorded");
  }
  // manifests
  const std::string digest = segarecomp::sha256_hex(rom);
  auto m = parse_mapper_manifest("{\"mapper\": \"sega\", \"sha256\": \"" + digest + "\", \"size\": 131072, \"declaration_source\": \"fixture_builder\"}", digest);
  check(m.error == SMS_OK && m.declaration.family == "sega" && m.declaration.source == DeclarationSource::fixture_builder, "manifest parses");
  check(parse_mapper_manifest("{\"mapper\": \"sega\", \"sha256\": \"00\"}", digest).error == SMS_ERROR_MAPPER_UNDECLARED, "a manifest sha256 mismatch fails");
  check(parse_mapper_manifest("{\"sha256\": \"" + digest + "\"}", digest).error == SMS_ERROR_MAPPER_UNDECLARED, "a manifest without a mapper fails");
  check(parse_mapper_manifest("{\"mapper\": \"sega\"}", digest).error == SMS_ERROR_MAPPER_UNDECLARED, "a manifest without a digest fails");
  check(parse_mapper_manifest("{\"mapper\": [\"sega\"], \"sha256\": \"" + digest + "\"}", digest).error == SMS_ERROR_MAPPER_UNDECLARED, "nested JSON fails");
  check(parse_mapper_manifest("{\"mapper\": \"sega\", \"mapper\": \"rom_only\", \"sha256\": \"" + digest + "\"}", digest).error == SMS_ERROR_MAPPER_UNDECLARED,
        "a duplicate key fails");
  check(parse_mapper_manifest("not json", digest).error == SMS_ERROR_MAPPER_UNDECLARED, "garbage fails");
  check(parse_mapper_manifest("{\"mapper\": \"sega\", \"sha256\": \"" + digest + "\"} trailing", digest).error == SMS_ERROR_MAPPER_UNDECLARED,
        "trailing bytes fail");
}

// ---- ImageSet -----------------------------------------------------------------------------------------------------

void test_image_set() {
  for (std::size_t size : {0x8000u, 0x20000u, 0x80000u}) {
    const auto rom = make_rom(size);
    const auto cart = ingest_cartridge(rom, sega());
    const ImageSet set = build_image_set(cart);
    const std::size_t banks = size / 0x4000;
    check(set.images.size() == 1 + banks, "image count is 1 + banks");
    std::set<std::uint32_t> ids;
    for (const auto& image : set.images) ids.insert(image.identity);
    check(ids.size() == set.images.size() && *ids.rbegin() <= 0xFFFF, "identities unique and <= 0xFFFF");
    const auto& fixed = set.images.at(0);
    check(fixed.identity == 1 && fixed.kind == ImageKind::invariant && fixed.windows.size() == 1 &&
              fixed.windows[0].base == 0 && fixed.windows[0].first_offset == 0 && fixed.windows[0].length == 0x400 &&
              fixed.bytes == std::vector<std::uint8_t>(rom.begin(), rom.begin() + 0x400),
          "image 1 is the invariant first 1 KiB");
    for (std::size_t b = 0; b < banks; ++b) {
      const auto& image = set.images.at(1 + b);
      check(image.identity == 2 + b && image.kind == ImageKind::banked && image.bytes.size() == 0x4000 &&
                std::equal(image.bytes.begin(), image.bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(b * 0x4000)),
            "bank image carries its 16 KiB");
      check(image.windows.size() == 3 && image.windows[0].base == 0 && image.windows[0].first_offset == 0x400 &&
                image.windows[0].length == 0x3C00 && image.windows[1].base == 0x4000 && image.windows[1].length == 0x4000 &&
                image.windows[2].base == 0x8000 && image.windows[2].first_offset == 0 && image.windows[2].length == 0x4000,
            "bank windows are slot 0 (offsets 0x400+), slot 1, slot 2");
    }
  }
  const auto rom = make_rom(0x8000);
  const ImageSet only = build_image_set(ingest_cartridge(rom, declared("rom_only")));
  check(only.images.size() == 1 && only.images[0].kind == ImageKind::invariant && only.images[0].windows.size() == 1 &&
            only.images[0].windows[0].length == 0x8000 && only.images[0].bytes == rom,
        "rom_only: one invariant 32 KiB image");
  check(build_image_set(ingest_cartridge(rom, IngestOptions{})).images.empty(), "a rejected cartridge yields no images");
}

// ---- run-time memory ----------------------------------------------------------------------------------------------

struct Machine {
  std::vector<std::uint8_t> rom;
  SmsMemory mem;
  Machine(std::size_t size, SmsMapperFamily family = SMS_MAPPER_SEGA) : rom(make_rom(size)) {
    check(sms_memory_init(&mem, rom.data(), static_cast<std::uint32_t>(rom.size()), family) == SMS_OK, "memory init");
  }
  std::uint8_t r(unsigned a) { return sms_memory_read(&mem, static_cast<std::uint16_t>(a), 0); }
  void w(unsigned a, unsigned v) { sms_memory_write(&mem, static_cast<std::uint16_t>(a), static_cast<std::uint8_t>(v), 0); }
  bool ci(unsigned a, std::uint32_t& id, std::uint16_t& base) {
    Z80CodeImage image{};
    const bool found = sms_memory_code_image(&mem, static_cast<std::uint16_t>(a), &image) != 0;
    id = image.identity;
    base = image.window_base;
    return found;
  }
};

void test_memory() {
  Machine m(0x20000);  // 8 banks
  const auto& rom = m.rom;
  // reset state
  check(m.r(0xC000) == 0xAB && m.r(0xE000) == 0xAB, "reset: $C000 = $AB and mirrored");
  check(m.mem.memory_control == 0xAB && m.mem.mapper_regs[0] == 0 && m.mem.mapper_regs[1] == 0 && m.mem.mapper_regs[2] == 1 &&
            m.mem.mapper_regs[3] == 2,
        "reset: memory control $AB, mapper 0,0,1,2");
  check(m.r(0x0000) == rom[0] && m.r(0x4000 + 5) == rom[0x4000 + 5] && m.r(0x8000 + 5) == rom[0x8000 + 5], "reset mapping: banks 0,1,2");
  // write-through and read-back
  m.w(0xFFFF, 5);
  check(m.r(0xFFFF) == 5 && m.r(0xDFFF) == 5 && m.mem.ram[0x1FFF] == 5, "write-through: $FFFF reads back the RAM copy and $DFFF");
  check(m.r(0x8005) == rom[5 * 0x4000 + 5], "slot 2 follows $FFFF");
  m.w(0xDFFF, 7);  // the RAM copy does not change the mapping
  check(m.r(0x8005) == rom[5 * 0x4000 + 5] && m.r(0xFFFF) == 7, "writing the RAM copy does not remap");
  // masking: bank 10 of 8 banks reads bank 2
  m.w(0xFFFF, 10);
  check(m.r(0x9000) == rom[2 * 0x4000 + 0x1000] && m.r(0xFFFF) == 10, "bank value 10 masks to 2; the register readback is unmasked");
  {
    Machine small(0x8000);  // 2 banks
    small.w(0xFFFE, 0x0B);
    check(small.r(0x4010) == small.rom[0x4000 + 0x10], "32 KiB ROM: bank 11 masks to 1");
    small.w(0xFFFE, 0x02);
    check(small.r(0x4010) == small.rom[0x10], "32 KiB ROM: bank 2 masks to 0");
  }
  // fixed first 1 KiB vs slot 0
  m.w(0xFFFD, 3);
  check(m.r(0x0200) == rom[0x200] && m.r(0x03FF) == rom[0x3FF], "first 1 KiB stays fixed ROM offset 0-0x3FF");
  check(m.r(0x0400) == rom[3 * 0x4000 + 0x400] && m.r(0x2000) == rom[3 * 0x4000 + 0x2000] && m.r(0x3FFF) == rom[3 * 0x4000 + 0x3FFF],
        "slot 0 shows bank offsets 0x400+");
  {
    std::uint32_t id = 0;
    std::uint16_t base = 0xFFFF;
    check(m.ci(0x03FF, id, base) && id == 1 && base == 0, "code_image: fixed image at 0x03FF");
    check(m.ci(0x0400, id, base) && id == 5 && base == 0, "code_image: slot 0 bank 3 -> identity 5, base 0");
    check(m.ci(0x4000, id, base) && id == 3 && base == 0x4000, "code_image: slot 1 bank 1 -> identity 3, base 0x4000");
    check(m.ci(0xBFFF, id, base) && id == 4 && base == 0x8000, "code_image: slot 2 (bank 10 & 7 = 2) -> identity 4, base 0x8000");
    check(!m.ci(0xC000, id, base) && !m.ci(0xDFFF, id, base) && !m.ci(0xE000, id, base) && !m.ci(0xFFFF, id, base),
          "code_image: RAM and mirror are not code (Z80_ERROR_MUTABLE_CODE)");
  }
  // ROM writes have no effect
  m.w(0x0100, 0x99);
  m.w(0x5000, 0x99);
  check(m.r(0x0100) == rom[0x100] && m.r(0x5000) == rom[0x4000 + 0x1000], "writes to ROM addresses are ignored");
  // RAM and mirror
  m.w(0xC123, 0x42);
  check(m.r(0xE123) == 0x42, "RAM write visible in the mirror");
  m.w(0xF0F0, 0x24);
  check(m.r(0xD0F0) == 0x24, "mirror write visible in RAM");
  check(m.mem.error == SMS_OK, "no error so far");
}

void test_cart_ram_control_and_rom_only() {
  Machine m(0x20000);
  m.w(0x8000, 0x11);  // slot 2 is ROM: ignored
  check(m.r(0x8000) == m.rom[0x8000], "slot 2 ROM write ignored");
  m.w(0xFFFC, 0x08);
  std::uint32_t id = 0;
  std::uint16_t base = 0;
  check(m.r(0xFFFC) == 0x08 && m.r(0xDFFC) == 0x08, "$FFFC reads back through the RAM copy");
  check(m.r(0x8000) == 0, "cartridge RAM is zero at power-on");
  m.w(0x8000, 0x5A);
  m.w(0xBFFF, 0xA5);
  check(m.r(0x8000) == 0x5A && m.r(0xBFFF) == 0xA5, "cartridge RAM read/write in slot 2");
  check(!m.ci(0x8000, id, base) && !m.ci(0xBFFF, id, base), "cartridge RAM is data only: code_image 0");
  check(m.ci(0x4000, id, base) && id == 3, "slot 1 stays ROM while slot 2 is cartridge RAM");
  m.w(0xFFFC, 0x0C);  // second 16 KiB cartridge RAM bank
  check(m.r(0x8000) == 0, "second cartridge RAM bank is distinct");
  m.w(0x8000, 0x77);
  m.w(0xFFFC, 0x08);
  check(m.r(0x8000) == 0x5A, "first cartridge RAM bank kept its data");
  m.w(0xFFFC, 0x80);  // bit 7 "ROM write enable": stored, no effect
  check(m.r(0x8000) == m.rom[2 * 0x4000] && m.ci(0x8000, id, base) && id == 4, "cartridge RAM unmapped: ROM bank 2 again");
  m.w(0x8000, 0x33);
  check(m.r(0x8000) == m.rom[2 * 0x4000], "control bit 7 does not make ROM writable");
  m.w(0xFFFC, 0x88);
  check(m.r(0x8000) == 0x5A, "cartridge RAM data persists across unmapping within the run");
  for (unsigned bad : {0x10u, 0x18u, 0x01u, 0x02u, 0x03u, 0x9Bu}) {
    Machine e(0x20000);
    e.w(0xFFFC, 0x08);
    e.w(0xFFFC, bad);
    check(e.mem.error == SMS_ERROR_CONTROL_BIT_UNSUPPORTED && e.mem.error_address == 0xFFFC && e.mem.error_value == bad,
          "control value rejected: " + std::to_string(bad));
    check(e.mem.mapper_regs[0] == 0x08 && e.r(0xC000) == 0xFF && !e.ci(0x0000, id, base), "state is frozen after the typed error");
  }
  // memory control
  Machine c(0x20000);
  sms_memory_control_write(&c.mem, 0x3E, 0xAB, 5);
  check(c.mem.error == SMS_OK && !sms_memory_io_disabled(&c.mem), "port $3E $AB accepted; I/O chip enabled");
  sms_memory_control_write(&c.mem, 0x3E, 0xAF, 6);
  check(c.mem.error == SMS_OK && sms_memory_io_disabled(&c.mem), "port $3E bit 2 disables the I/O chip (modelled)");
  sms_memory_control_write(&c.mem, 0x3E, 0x2B, 7);  // bits 7, 5 clear/set differences have no effect
  check(c.mem.error == SMS_OK, "port $3E bits 7 and 5 have no effect");
  for (unsigned bad : {0xEBu, 0xBBu, 0xA3u, 0x00u}) {
    Machine e(0x20000);
    sms_memory_control_write(&e.mem, 0x3E, static_cast<std::uint8_t>(bad), 99);
    check(e.mem.error == SMS_ERROR_CONTROL_BIT_UNSUPPORTED && e.mem.error_cycles == 99 && e.mem.error_value == bad,
          "port $3E write rejected: " + std::to_string(bad));
  }
  // rom_only
  Machine o(0x8000, SMS_MAPPER_ROM_ONLY);
  check(o.r(0x0000) == o.rom[0] && o.r(0x7FFF) == o.rom[0x7FFF] && o.r(0x4000) == o.rom[0x4000], "rom_only: ROM at $0000-$7FFF");
  o.w(0xFFFF, 1);
  o.w(0xFFFC, 0x18);  // plain RAM-mirror writes, no mapper, no control error
  check(o.mem.error == SMS_OK && o.r(0xFFFF) == 1 && o.r(0xDFFC) == 0x18, "rom_only: mapper addresses are RAM-mirror writes only");
  check(o.ci(0x7FFF, id, base) && id == 1 && base == 0 && !o.ci(0x8000, id, base) && !o.ci(0xC000, id, base), "rom_only: one invariant image");
  check(o.r(0x8000) == 0xFF && o.mem.error == SMS_ERROR_UNMAPPED_READ && o.mem.error_address == 0x8000, "rom_only: $8000 read is a typed stop");
  SmsMemory bad;
  const auto rom = make_rom(0x20000);
  check(sms_memory_init(&bad, rom.data(), 0x18000, SMS_MAPPER_SEGA) == SMS_ERROR_ROM_SIZE_UNSUPPORTED, "init rejects a bad size");
  check(sms_memory_init(&bad, rom.data(), 0x20000, SMS_MAPPER_ROM_ONLY) == SMS_ERROR_ROM_SIZE_UNSUPPORTED, "init: rom_only needs 32 KiB");
  check(sms_memory_init(&bad, rom.data(), 0x20000, SMS_MAPPER_UNDECLARED) == SMS_ERROR_MAPPER_UNDECLARED, "init rejects an undeclared family");
}

// ---- independent reference model (not derived from sms_mapper_contract.h) -----------------------------------------
// SMS Power! "Mappers" semantics restated with a per-1-KiB page table: each page maps either a ROM offset, cartridge
// RAM, work RAM or nothing. code_image is read off the page table.
struct RefModel {
  const std::vector<std::uint8_t>& rom;
  std::uint32_t banks;
  std::uint8_t ram[0x2000] = {}, cart[0x8000] = {}, ctl = 0, reg[3] = {0, 1, 2};
  explicit RefModel(const std::vector<std::uint8_t>& r) : rom(r), banks(static_cast<std::uint32_t>(r.size() / 0x4000)) { reset(); }
  void reset() {
    std::memset(ram, 0, sizeof ram);
    std::memset(cart, 0, sizeof cart);
    ctl = 0;
    reg[0] = 0; reg[1] = 1; reg[2] = 2;
    ram[0] = 0xAB;
  }
  struct Page { int kind; std::uint32_t offset; };  // kind 0 ROM, 1 cart RAM, 2 work RAM
  Page page(unsigned address) const {
    if (address >= 0xC000) return {2, address & 0x1FFF};
    if (address < 0x400) return {0, address};
    const unsigned slot = address >> 14;
    if (slot == 2 && (ctl & 8)) return {1, ((ctl & 4) ? 0x4000u : 0u) + (address & 0x3FFF)};
    return {0, (reg[slot] % banks) * 0x4000 + (address & 0x3FFF)};
  }
  std::uint8_t read(unsigned a) const {
    const Page p = page(a);
    return p.kind == 0 ? rom[p.offset] : p.kind == 1 ? cart[p.offset] : ram[p.offset];
  }
  // returns false when the write is a typed error (the machine then stops)
  bool write(unsigned a, std::uint8_t v) {
    if (a >= 0xFFFC && a == 0xFFFC && ((v & 0x10) || (v & 3))) return false;
    const Page p = page(a);
    if (p.kind == 1) cart[p.offset] = v;
    if (p.kind == 2) {
      ram[p.offset] = v;
      if (a == 0xFFFC) ctl = v;
      if (a > 0xFFFC) reg[a - 0xFFFD] = v;
    }
    return true;
  }
  bool code(unsigned a, std::uint32_t& id, std::uint16_t& base) const {
    const Page p = page(a);
    if (p.kind != 0) return false;
    if (a < 0x400) { id = 1; base = 0; return true; }
    id = 2 + p.offset / 0x4000;
    base = static_cast<std::uint16_t>(a - (p.offset & 0x3FFF));
    if (a < 0x4000) base = 0;  // slot 0 exposes offsets 0x400+ at logical 0x400+: image offset o is at logical o
    return true;
  }
};

void test_reference_differential() {
  for (std::size_t size : {0x8000u, 0x10000u, 0x20000u, 0x80000u}) {
    Machine m(size);
    RefModel ref(m.rom);
    Prng prng{size * 31 + 7};
    std::uint64_t compared = 0, mismatches = 0;
    for (int step = 0; step < 300000 && mismatches == 0; ++step) {
      const std::uint32_t r = prng.next();
      const unsigned op = r & 7;
      unsigned address = (r >> 3) & 0xFFFF;
      const std::uint8_t value = static_cast<std::uint8_t>(prng.next());
      if (op == 0) {  // mapper register write (valid control values only)
        static const std::uint8_t controls[] = {0x00, 0x04, 0x08, 0x0C, 0x80, 0x84, 0x88, 0x8C};
        const unsigned which = prng.next() % 4;
        const std::uint8_t v = which == 0 ? controls[prng.next() % 8] : value;
        m.w(0xFFFC + which, v);
        ref.write(0xFFFC + which, v);
      } else if (op == 1) {
        m.w(address, value);  // anywhere, including ROM addresses, cart RAM and mirrors
        if (address == 0xFFFC && ((value & 0x10) || (value & 3))) {
          if (m.mem.error != SMS_ERROR_CONTROL_BIT_UNSUPPORTED) ++mismatches;
          sms_memory_init(&m.mem, m.rom.data(), static_cast<std::uint32_t>(m.rom.size()), SMS_MAPPER_SEGA);  // typed stop: restart both sides
          ref.reset();
        } else {
          ref.write(address, value);
        }
      } else if (op <= 4) {
        if (m.r(address) != ref.read(address)) ++mismatches;
        ++compared;
      } else {
        std::uint32_t id = 0, rid = 0;
        std::uint16_t base = 0, rbase = 0;
        const bool a = m.ci(address, id, base), b = ref.code(address, rid, rbase);
        if (a != b || (a && (id != rid || base != rbase))) ++mismatches;
        ++compared;
      }
    }
    check(mismatches == 0, "memory/mapper differs from the independent reference model (size " + std::to_string(size) + ")");
    check(compared > 100000, "the differential compared a meaningful number of operations");
  }
}

// ---- generation-time declaration vs run-time contract -------------------------------------------------------------

using CodeImageFn = std::function<bool(const std::uint8_t regs[4], std::uint16_t address, std::uint32_t&, std::uint16_t&)>;

// Enumerates (register state, address) classes and checks that the declared ImageSet and the run-time code_image agree:
// the answer names a declared image whose declared window contains the address, the identity is the masked bank, and each
// declared window exposes exactly the addresses the run-time ever answers for that (identity, base).
std::vector<std::string> verify_contract(const ImageSet& set, const std::vector<std::uint8_t>& rom, SmsMapperFamily family,
                                         const CodeImageFn& code_image) {
  std::vector<std::string> problems;
  const auto report = [&](const std::string& what) {
    if (problems.size() < 8) problems.push_back(what);
  };
  std::map<std::uint32_t, const segarecomp::codegen::z80::CodeImage*> by_id;
  for (const auto& image : set.images) {
    by_id[image.identity] = &image;
    for (const auto& w : image.windows)
      if (static_cast<std::uint32_t>(w.base) + w.first_offset + w.length > 0xC000 && family == SMS_MAPPER_SEGA) report("a declared window reaches RAM");
  }
  const std::uint32_t banks = static_cast<std::uint32_t>(rom.size() / 0x4000);
  for (const auto& image : set.images) {  // the identity must name the ROM bytes the run-time mapping selects
    const std::size_t from = family == SMS_MAPPER_SEGA && image.identity >= 2 ? (image.identity - 2) * std::size_t{0x4000} : 0;
    const std::size_t length = family == SMS_MAPPER_SEGA && image.identity == 1 ? 0x400 : image.bytes.size();
    if (from + length > rom.size() || image.bytes.size() != length || !std::equal(image.bytes.begin(), image.bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(from)))
      report("image " + std::to_string(image.identity) + " bytes are not the ROM range its identity selects");
  }
  std::map<std::pair<std::uint32_t, std::uint16_t>, std::vector<bool>> observed;
  const auto visit = [&](const std::uint8_t regs[4], unsigned address) {
    std::uint32_t id = 0;
    std::uint16_t base = 0;
    const bool found = code_image(regs, static_cast<std::uint16_t>(address), id, base);
    if (family == SMS_MAPPER_ROM_ONLY) {
      if (found != (address < 0x8000)) report("rom_only code_image extent");
    } else {
      const bool expect_code = address < 0xC000 && !(address >= 0x8000 && (regs[0] & 8));
      if (found != expect_code) report("code_image presence at " + std::to_string(address));
    }
    if (!found) return;
    const auto it = by_id.find(id);
    if (it == by_id.end()) return report("run-time identity " + std::to_string(id) + " is not declared");
    std::uint32_t expected_id = 1;
    if (family == SMS_MAPPER_SEGA && address >= 0x400 && address < 0xC000) expected_id = 2 + (regs[(address >> 14) + 1] & (banks - 1));
    if (id != expected_id) report("identity at " + std::to_string(address) + " is not the masked bank");
    auto& bits = observed[{id, base}];
    if (bits.empty()) bits.assign(0x10000, false);
    bits[address] = true;
  };
  std::vector<unsigned> addresses;
  for (unsigned a = 0; a < 0x10000; ++a)
    if (family == SMS_MAPPER_ROM_ONLY || (a & 0x3FF) == 0 || (a & 0x3FF) == 0x3FF || (a & 0xFF) == 0x55) addresses.push_back(a);
  if (family == SMS_MAPPER_SEGA) {
    // every bank value in every slot register, each control combination, and two cross-slot permutations
    for (unsigned control_value : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x80u, 0x88u}) {
      const auto control = static_cast<std::uint8_t>(control_value);
      for (unsigned v = 0; v < 256; ++v) {
        for (unsigned variant = 0; variant < 3; ++variant) {
          const std::uint8_t regs[4] = {control, static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(variant == 0 ? v : v * 7 + 3),
                                        static_cast<std::uint8_t>(variant == 2 ? v * 13 + 5 : v)};
          for (unsigned a : addresses) visit(regs, a);
        }
      }
    }
  } else {
    const std::uint8_t regs[4] = {0, 0, 1, 2};
    for (unsigned a = 0; a < 0x10000; ++a) visit(regs, a);
  }
  // declared windows versus observed coverage
  for (const auto& image : set.images) {
    for (const auto& w : image.windows) {
      const auto it = observed.find({image.identity, w.base});
      if (it == observed.end()) { report("declared window of image " + std::to_string(image.identity) + " is never selected"); continue; }
      for (unsigned a = 0; a < 0x10000; ++a) {
        const bool declared = a >= static_cast<unsigned>(w.base) + w.first_offset && a < static_cast<unsigned>(w.base) + w.first_offset + w.length;
        // coverage is sampled on an address subset for the Sega mapper: compare on the sampled addresses only
        const bool sampled = family == SMS_MAPPER_ROM_ONLY || (a & 0x3FF) == 0 || (a & 0x3FF) == 0x3FF || (a & 0xFF) == 0x55;
        if (sampled && declared != it->second[a]) { report("window coverage differs at " + std::to_string(a)); break; }
      }
    }
  }
  for (const auto& [key, bits] : observed) {
    (void)bits;
    bool declared = false;
    for (const auto& w : by_id[key.first]->windows) declared |= w.base == key.second;
    if (!declared) report("run-time window base " + std::to_string(key.second) + " is not declared for image " + std::to_string(key.first));
  }
  return problems;
}

CodeImageFn runtime_code_image(SmsMapperFamily family, std::uint32_t rom_size) {
  return [=](const std::uint8_t regs[4], std::uint16_t address, std::uint32_t& id, std::uint16_t& base) {
    return sms_mapper_code_image(family, rom_size, regs, address, &id, &base) != 0;
  };
}

void test_contract_agreement_and_mutations() {
  for (std::size_t size : {0x8000u, 0x20000u, 0x80000u}) {
    const auto rom = make_rom(size);
    const auto cart = ingest_cartridge(rom, sega());
    const ImageSet set = build_image_set(cart);
    const auto good = verify_contract(set, rom, SMS_MAPPER_SEGA, runtime_code_image(SMS_MAPPER_SEGA, static_cast<std::uint32_t>(size)));
    check(good.empty(), "declaration and run-time agree over the enumerated contract (size " + std::to_string(size) + "): " + (good.empty() ? "" : good[0]));
    if (size != 0x20000) continue;
    const auto sz = static_cast<std::uint32_t>(size);
    // run-time mutants
    const auto wide_mask = verify_contract(set, rom, SMS_MAPPER_SEGA, runtime_code_image(SMS_MAPPER_SEGA, sz * 2));
    check(!wide_mask.empty(), "mutation: a wrong bank mask (run time) is detected");
    const auto wrong_base = verify_contract(set, rom, SMS_MAPPER_SEGA, [&](const std::uint8_t regs[4], std::uint16_t a, std::uint32_t& id, std::uint16_t& base) {
      const bool f = sms_mapper_code_image(SMS_MAPPER_SEGA, sz, regs, a, &id, &base) != 0;
      if (f && a >= 0x4000 && a < 0x8000) base = 0;
      return f;
    });
    check(!wrong_base.empty(), "mutation: a wrong window base (run time) is detected");
    const auto cart_ram_code = verify_contract(set, rom, SMS_MAPPER_SEGA, [&](const std::uint8_t regs[4], std::uint16_t a, std::uint32_t& id, std::uint16_t& base) {
      std::uint8_t copy[4] = {0, regs[1], regs[2], regs[3]};  // forgets cartridge RAM in slot 2
      return sms_mapper_code_image(SMS_MAPPER_SEGA, sz, copy, a, &id, &base) != 0;
    });
    check(!cart_ram_code.empty(), "mutation: reporting cartridge RAM as code (run time) is detected");
    const auto ram_code = verify_contract(set, rom, SMS_MAPPER_SEGA, [&](const std::uint8_t regs[4], std::uint16_t a, std::uint32_t& id, std::uint16_t& base) {
      if (a >= 0xC000 && a < 0xE000) { id = 1; base = 0; return true; }
      return sms_mapper_code_image(SMS_MAPPER_SEGA, sz, regs, a, &id, &base) != 0;
    });
    check(!ram_code.empty(), "mutation: reporting work RAM as code (run time) is detected");
    // declaration mutants
    ImageSet m1 = set;
    m1.images[3].windows[0].first_offset = 0;  // slot 0 would expose offsets 0..
    check(!verify_contract(m1, rom, SMS_MAPPER_SEGA, runtime_code_image(SMS_MAPPER_SEGA, sz)).empty(), "mutation: a wrong declared first_offset is detected");
    ImageSet m2 = set;
    m2.images[2].windows[1].length -= 1;
    check(!verify_contract(m2, rom, SMS_MAPPER_SEGA, runtime_code_image(SMS_MAPPER_SEGA, sz)).empty(), "mutation: a wrong declared window length is detected");
    ImageSet m3 = set;
    m3.images[4].windows[2].base = 0xC000;
    check(!verify_contract(m3, rom, SMS_MAPPER_SEGA, runtime_code_image(SMS_MAPPER_SEGA, sz)).empty(), "mutation: a declared window in RAM is detected");
    ImageSet m4 = set;
    std::swap(m4.images[1].identity, m4.images[2].identity);
    check(!verify_contract(m4, rom, SMS_MAPPER_SEGA, runtime_code_image(SMS_MAPPER_SEGA, sz)).empty(), "mutation: swapped declared identities are detected");
    ImageSet m5 = set;
    m5.images[5].windows.pop_back();
    check(!verify_contract(m5, rom, SMS_MAPPER_SEGA, runtime_code_image(SMS_MAPPER_SEGA, sz)).empty(), "mutation: a missing declared window is detected");
  }
  const auto rom = make_rom(0x8000);
  const ImageSet only = build_image_set(ingest_cartridge(rom, declared("rom_only")));
  const auto r = verify_contract(only, rom, SMS_MAPPER_ROM_ONLY, runtime_code_image(SMS_MAPPER_ROM_ONLY, 0x8000));
  check(r.empty(), "rom_only declaration and run time agree");
}

}  // namespace

int main() {
  test_ingestion();
  test_image_set();
  test_memory();
  test_cart_ram_control_and_rom_only();
  test_reference_differential();
  test_contract_agreement_and_mutations();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("sms machine tests: ok\n");
  return 0;
}
