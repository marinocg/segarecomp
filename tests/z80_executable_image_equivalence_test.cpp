// SEG-028-T003 (ADR 0077): the Z80 executable-image producers projected onto the emitter input are field-for-field the historical
// ImageSet builders, and the projection fails closed.
//
// `legacy_sms_build_image_set` and `legacy_genesis_build_image_set` are verbatim copies of the pre-SEG-028 bodies of
// platforms/master-system/machine/src/image_set.cpp build_image_set and platforms/genesis/machine/src/z80_images.cpp build_image_set.
// Every fixture is synthetic: pseudo-random ROM bytes with a project-authored header, and synthetic sound-RAM epochs.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "segarecomp/codegen/c11/z80_executable_image.hpp"
#include "segarecomp/machine/genesis/z80_images.hpp"
#include "segarecomp/machine/master_system/cartridge.hpp"
#include "segarecomp/machine/master_system/image_set.hpp"

namespace {

using namespace segarecomp;
namespace sms = segarecomp::machine::master_system;
namespace gz80 = segarecomp::machine::genesis::z80;
namespace cz80 = segarecomp::codegen::z80;

int failures = 0;

void check(bool ok, const std::string& label) {
  std::printf("%-5s %s\n", ok ? "ok" : "FAIL", label.c_str());
  if (!ok) ++failures;
}

// ---- historical builders (verbatim pre-SEG-028 bodies) ---------------------------------------------------------------------------

cz80::ImageSet legacy_sms_build_image_set(const sms::IngestResult& cartridge) {
  using codegen::z80::CodeImage;
  using codegen::z80::CodeWindow;
  using codegen::z80::ImageKind;
  codegen::z80::ImageSet set;
  if (!cartridge.ok()) return set;
  const auto& rom = cartridge.rom;
  if (cartridge.identity.mapper == SMS_MAPPER_ROM_ONLY) {
    CodeImage image;
    image.identity = SMS_IMAGE_FIXED;
    image.kind = ImageKind::invariant;
    image.bytes = rom;
    image.windows.push_back(CodeWindow{0, 0, SMS_ROM_ONLY_SIZE});
    set.images.push_back(std::move(image));
    return set;
  }
  CodeImage fixed;
  fixed.identity = SMS_IMAGE_FIXED;
  fixed.kind = ImageKind::invariant;
  fixed.bytes.assign(rom.begin(), rom.begin() + SMS_FIXED_SIZE);
  fixed.windows.push_back(CodeWindow{0, 0, SMS_FIXED_SIZE});
  set.images.push_back(std::move(fixed));
  for (std::uint32_t bank = 0; bank < cartridge.identity.bank_count; ++bank) {
    CodeImage image;
    image.identity = SMS_IMAGE_BANK_FIRST + bank;
    image.kind = ImageKind::banked;
    const auto first = rom.begin() + static_cast<std::ptrdiff_t>(bank * SMS_BANK_SIZE);
    image.bytes.assign(first, first + SMS_BANK_SIZE);
    for (const SmsSlot& slot : sms_sega_slots)
      image.windows.push_back(CodeWindow{slot.window_base, slot.first_offset, slot.length});
    set.images.push_back(std::move(image));
  }
  return set;
}

cz80::ImageSet legacy_genesis_build_image_set(const gz80::Registry& registry) {
  using namespace gz80;
  codegen::z80::ImageSet set;
  for (const Image& image : registry.images()) {
    codegen::z80::CodeImage code;
    code.identity = image.ordinal;
    code.kind = codegen::z80::ImageKind::banked;
    code.live_bytes = true;
    code.bytes.assign(image.ram.begin(), image.ram.end());
    code.bytes.insert(code.bytes.end(), image.ram.begin(), image.ram.end());  // the 8 KiB RAM is mirrored at $2000
    code.windows.push_back({0x0000, 0, static_cast<std::uint32_t>(kWindowBytes)});
    set.images.push_back(std::move(code));
  }
  return set;
}

// ---- comparison helpers -----------------------------------------------------------------------------------------------------------

bool same_set(const cz80::ImageSet& a, const cz80::ImageSet& b, std::string& why) {
  if (a.images.size() != b.images.size()) return why = "image count", false;
  for (std::size_t i = 0; i < a.images.size(); ++i) {
    const auto& x = a.images[i];
    const auto& y = b.images[i];
    const std::string at = "image " + std::to_string(i) + ": ";
    if (x.identity != y.identity) return why = at + "identity", false;
    if (x.kind != y.kind) return why = at + "kind", false;
    if (x.live_bytes != y.live_bytes) return why = at + "live_bytes", false;
    if (x.bytes != y.bytes) return why = at + "bytes", false;
    if (x.windows.size() != y.windows.size()) return why = at + "window count", false;
    for (std::size_t w = 0; w < x.windows.size(); ++w)
      if (x.windows[w].base != y.windows[w].base || x.windows[w].first_offset != y.windows[w].first_offset ||
          x.windows[w].length != y.windows[w].length)
        return why = at + "window " + std::to_string(w), false;
  }
  return true;
}

void expect_same(const cz80::ImageSet& legacy, const cz80::ImageSet& projected, const std::string& label) {
  std::string why;
  const bool ok = same_set(legacy, projected, why);
  check(ok, label + (ok ? "" : " (differs: " + why + ")"));
}

struct Prng {
  std::uint64_t state;
  std::uint32_t next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint32_t>(state >> 33);
  }
};

// Pseudo-random ROM with a TMR SEGA export-region header at 0x7FF0 (the same synthetic shape as tests/sms_machine_test.cpp).
std::vector<std::uint8_t> make_rom(std::size_t size) {
  Prng prng{0x28A0 + size};
  std::vector<std::uint8_t> rom(size);
  for (auto& b : rom) b = static_cast<std::uint8_t>(prng.next());
  for (std::size_t bank = 0; bank < size / 0x4000; ++bank) rom[bank * 0x4000 + 0x2000] = static_cast<std::uint8_t>(0x80 + bank);
  std::memcpy(&rom[0x7FF0], "TMR SEGA", 8);
  rom[0x7FF8] = rom[0x7FF9] = 0;
  rom[0x7FFF] = static_cast<std::uint8_t>((4U << 4) | 0x0C);
  return rom;
}

sms::IngestOptions declared(const char* family) {
  sms::IngestOptions o;
  o.declarations.push_back({family, sms::DeclarationSource::build_option});
  return o;
}

gz80::Epoch epoch(unsigned seed, std::size_t written_offset = 0, std::size_t length = 64) {
  gz80::Epoch e;
  for (std::size_t i = 0; i < gz80::kRamBytes; ++i) e.ram[i] = static_cast<std::uint8_t>((i * 31 + seed * 7 + 1) & 0xFF);
  for (std::size_t i = written_offset; i < written_offset + length; ++i)
    e.written[i >> 3] = static_cast<std::uint8_t>(e.written[i >> 3] | (1U << (i & 7)));
  return e;
}

gz80::Registry registry_of(std::size_t images) {
  gz80::Registry registry;
  std::uint32_t bound = 0;
  for (std::size_t i = 0; i < images; ++i) (void)registry.add(epoch(static_cast<unsigned>(i + 1), i * 128, 64 + i * 8), bound);
  return registry;
}

std::map<std::string, std::string> read_tree(const std::filesystem::path& dir) {
  std::map<std::string, std::string> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    files[std::filesystem::relative(entry.path(), dir).generic_string()] = text.str();
  }
  return files;
}

std::filesystem::path scratch(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / ("seg028_z80_equiv_" + std::to_string(std::random_device{}()) + "_" + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

std::map<std::string, std::string> emit_tree(const cz80::ImageSet& set, const std::string& name, std::string& error) {
  cz80::EmitOptions options;
  options.directory = scratch(name);
  options.stem = "equiv";
  const cz80::EmitResult result = cz80::emit_image_set(set, options);
  error = result.error;
  auto files = read_tree(options.directory);
  std::filesystem::remove_all(options.directory);
  return files;
}

// ---- SMS --------------------------------------------------------------------------------------------------------------------------

void sms_equivalence() {
  {
    const auto rom = make_rom(0x8000);
    const auto cartridge = sms::ingest_cartridge(rom, declared("rom_only"));
    check(cartridge.ok(), "sms rom_only fixture ingests");
    expect_same(legacy_sms_build_image_set(cartridge), sms::build_image_set(cartridge), "sms rom_only 32 KiB: projection == legacy");
    const auto images = sms::executable_images(cartridge);
    check(images.cpu == CpuVariant::z80 && !images.platform_selector && images.images.size() == 1 &&
              images.images[0].provenance.authority == ImageAuthority::immutable_input &&
              images.images[0].provenance.producer == sms::kCartridgeProducer &&
              images.images[0].verification == ImageVerification::none && validate_executable_image_set(images).ok(),
          "sms rom_only executable image shape");
  }
  for (const std::size_t size : {std::size_t{0x8000}, std::size_t{0x10000}, std::size_t{0x20000}, std::size_t{0x40000},
                                 std::size_t{0x80000}}) {
    const auto rom = make_rom(size);
    const auto cartridge = sms::ingest_cartridge(rom, declared("sega"));
    const std::string label = "sms sega " + std::to_string(size / 0x4000) + " banks";
    check(cartridge.ok(), label + " fixture ingests");
    const auto legacy = legacy_sms_build_image_set(cartridge);
    expect_same(legacy, sms::build_image_set(cartridge), label + ": projection == legacy");
    const auto images = sms::executable_images(cartridge);
    bool shape = images.platform_selector && validate_executable_image_set(images).ok() &&
                 images.images.size() == 1 + size / 0x4000;
    for (const auto& image : images.images)
      shape = shape && image.provenance.authority == ImageAuthority::immutable_input &&
              image.provenance.producer == sms::kCartridgeProducer && image.verification == ImageVerification::none &&
              !image.source;
    check(shape, label + ": executable image shape (immutable_input, sms.cartridge_bank, none, selector)");
    const auto kinds = sms::image_kinds(images);
    check(kinds.size() == images.images.size() && kinds[0] == cz80::ImageKind::invariant &&
              std::all_of(kinds.begin() + 1, kinds.end(), [](cz80::ImageKind k) { return k == cz80::ImageKind::banked; }),
          label + ": image kinds (fixed invariant, banks banked)");
  }
  {  // a rejected cartridge yields no images on both paths
    const auto rom = make_rom(0x20000);
    const auto rejected = sms::ingest_cartridge(rom, sms::IngestOptions{});
    check(!rejected.ok(), "undeclared mapper is rejected");
    expect_same(legacy_sms_build_image_set(rejected), sms::build_image_set(rejected), "rejected cartridge: both empty");
    check(sms::build_image_set(rejected).images.empty() && sms::executable_images(rejected).images.empty() &&
              sms::executable_images(rejected).cpu == CpuVariant::z80,
          "rejected cartridge: empty Z80 executable-image set");
  }
}

// ---- Genesis ----------------------------------------------------------------------------------------------------------------------

void genesis_equivalence() {
  for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{5}, gz80::kMaxImages}) {
    const gz80::Registry registry = registry_of(count);
    const std::string label = "genesis registry of " + std::to_string(count);
    check(registry.images().size() == count, label + " fixture has the expected image count");
    expect_same(legacy_genesis_build_image_set(registry), gz80::build_image_set(registry), label + ": projection == legacy");
    const auto images = gz80::executable_images(registry);
    bool shape = images.cpu == CpuVariant::z80 && images.platform_selector && validate_executable_image_set(images).ok();
    for (const auto& image : images.images)
      shape = shape && image.provenance.authority == ImageAuthority::bounded_build_time_materialization &&
              image.provenance.producer == "genesis.z80_materializer" && image.verification == ImageVerification::structural &&
              image.mappings.size() == 1 && image.mappings[0] == ImageMapping{0, 0, 0x4000} && image.bytes.size() == 0x4000;
    check(shape, label + ": executable image shape (bbtm, genesis.z80_materializer, structural, one $0000 window)");
  }
  {  // a re-activated signature and a plain restart add no image
    gz80::Registry registry;
    std::uint32_t bound = 0;
    (void)registry.add(epoch(1), bound);
    (void)registry.add(epoch(2, 256), bound);
    (void)registry.add(epoch(1), bound);
    gz80::Epoch restart = epoch(3);
    restart.written.fill(0);
    (void)registry.add(restart, bound);
    check(registry.images().size() == 2, "re-activation and restart register nothing new");
    expect_same(legacy_genesis_build_image_set(registry), gz80::build_image_set(registry), "genesis re-activation: projection == legacy");
  }
}

// ---- projection fail-closed cases -------------------------------------------------------------------------------------------------

ExecutableImageSet one_z80_image() {
  ExecutableImageSet set;
  set.cpu = CpuVariant::z80;
  ExecutableImage image;
  image.id = ImageId{1};
  image.bytes.assign(0x100, 0x00);
  image.mappings.push_back({0x8000, 0, 0x100});
  image.provenance.producer = "test.z80";
  set.images.push_back(image);
  return set;
}

void projection_fail_closed() {
  const std::vector<cz80::ImageKind> one{cz80::ImageKind::banked};
  {
    const auto ok = cz80::project_executable_images(one_z80_image(), one);
    check(ok.ok() && ok.set.images.size() == 1 && ok.set.images[0].windows.size() == 1 && ok.set.images[0].windows[0].base == 0x8000 &&
              !ok.set.images[0].live_bytes,
          "valid single image projects");
    auto structural = one_z80_image();
    structural.images[0].verification = ImageVerification::structural;
    const auto live = cz80::project_executable_images(structural, one);
    check(live.ok() && live.set.images[0].live_bytes, "structural verification projects to live_bytes");
  }
  const auto rejects = [&](ExecutableImageSet set, std::vector<cz80::ImageKind> kinds, const std::string& needle,
                           const std::string& label) {
    const auto out = cz80::project_executable_images(set, kinds);
    check(!out.ok() && out.set.images.empty() && out.error.find(needle) != std::string::npos,
          "projection rejects " + label + " (" + out.error + ")");
  };
  {
    auto set = one_z80_image();
    set.cpu = CpuVariant::mc68000;
    rejects(set, one, "not a Z80 set", "a non-Z80 set");
  }
  rejects(one_z80_image(), {}, "kind count", "too few kinds");
  rejects(one_z80_image(), {cz80::ImageKind::banked, cz80::ImageKind::banked}, "kind count", "too many kinds");
  rejects(one_z80_image(), {static_cast<cz80::ImageKind>(7)}, "unknown image kind", "an unknown image kind");
  {  // an identity above 0xFFFF: a dense set of 0x10000 one-byte images under a selector
    ExecutableImageSet set;
    set.cpu = CpuVariant::z80;
    set.platform_selector = true;
    for (std::uint32_t id = 1; id <= 0x10000U; ++id) {
      ExecutableImage image;
      image.id = ImageId{id};
      image.bytes = {0};
      image.mappings.push_back({0, 0, 1});
      image.provenance.producer = "test.z80";
      set.images.push_back(std::move(image));
    }
    // Intra-set overlap is allowed under the selector, so the set validates and the 0x10000th identity is refused.
    rejects(set, std::vector<cz80::ImageKind>(set.images.size(), cz80::ImageKind::banked), "16-bit entry-key", "an identity > 0xFFFF");
  }
  {
    auto set = one_z80_image();
    set.images[0].mappings[0].execution_base = 0x10000;
    rejects(set, one, "16-bit logical space", "a mapping base > 0xFFFF");
  }
  {
    auto set = one_z80_image();
    set.images[0].verification = ImageVerification::byte_identity;
    rejects(set, one, "byte_identity", "byte_identity verification");
  }
  {
    auto set = one_z80_image();
    set.images[0].id = ImageId{2};
    rejects(set, one, "id_not_dense", "an invalid set");
    set = one_z80_image();
    set.images[0].mappings.clear();
    rejects(set, one, "no_mappings", "an image without mappings");
  }
}

// ---- emission -------------------------------------------------------------------------------------------------------------------

void emission_identity() {
  {
    const gz80::Registry registry = registry_of(2);
    std::string e1, e2;
    const auto legacy = emit_tree(legacy_genesis_build_image_set(registry), "genesis_legacy", e1);
    const auto projected = emit_tree(gz80::build_image_set(registry), "genesis_projected", e2);
    check(e1.empty() && e2.empty() && !legacy.empty() && legacy == projected,
          "genesis 2-image registry: legacy and projected ImageSets emit byte-identical files (" + std::to_string(legacy.size()) +
              " files)");
  }
  {
    const auto rom = make_rom(0x8000);
    const auto cartridge = sms::ingest_cartridge(rom, declared("rom_only"));
    std::string e1, e2;
    const auto legacy = emit_tree(legacy_sms_build_image_set(cartridge), "sms_legacy", e1);
    const auto projected = emit_tree(sms::build_image_set(cartridge), "sms_projected", e2);
    check(e1.empty() && e2.empty() && !legacy.empty() && legacy == projected,
          "sms rom_only: legacy and projected ImageSets emit byte-identical files (" + std::to_string(legacy.size()) + " files)");
  }
}

}  // namespace

int main() {
  sms_equivalence();
  genesis_equivalence();
  projection_fail_closed();
  emission_identity();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("z80_executable_image_equivalence_test: all checks passed\n");
  return 0;
}
