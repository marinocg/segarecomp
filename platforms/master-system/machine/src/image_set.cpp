#include "segarecomp/machine/master_system/image_set.hpp"

#include <algorithm>

#include "segarecomp/codegen/c11/z80_executable_image.hpp"

namespace segarecomp::machine::master_system {
namespace {

ExecutableImage cartridge_image(std::uint32_t identity, std::vector<std::uint8_t> bytes, std::vector<ImageMapping> mappings,
                                std::string_view region) {
  ExecutableImage image;
  image.id = ImageId{identity};
  image.bytes = std::move(bytes);
  image.mappings = std::move(mappings);
  image.provenance.authority = ImageAuthority::immutable_input;
  image.provenance.producer = kCartridgeProducer;
  image.provenance.evidence.push_back({"derivation", "mapping_claim"});
  image.provenance.evidence.push_back({"region", std::string(region)});
  image.verification = ImageVerification::none;
  return image;
}

}  // namespace

ExecutableImageSet executable_images(const IngestResult& cartridge) {
  ExecutableImageSet set;
  set.cpu = CpuVariant::z80;
  if (!cartridge.ok()) return set;
  const auto& rom = cartridge.rom;
  if (cartridge.identity.mapper == SMS_MAPPER_ROM_ONLY) {
    set.images.push_back(cartridge_image(SMS_IMAGE_FIXED, rom, {ImageMapping{0, 0, SMS_ROM_ONLY_SIZE}}, "rom_only"));
    return set;
  }
  // Banks are selected at run time by the mapper registers: several banks map the same slot windows (the platform selector).
  set.platform_selector = true;
  set.images.push_back(cartridge_image(SMS_IMAGE_FIXED, std::vector<std::uint8_t>(rom.begin(), rom.begin() + SMS_FIXED_SIZE),
                                       {ImageMapping{0, 0, SMS_FIXED_SIZE}}, "fixed"));
  for (std::uint32_t bank = 0; bank < cartridge.identity.bank_count; ++bank) {
    const auto first = rom.begin() + static_cast<std::ptrdiff_t>(bank * SMS_BANK_SIZE);
    std::vector<ImageMapping> mappings;
    for (const SmsSlot& slot : sms_sega_slots) mappings.push_back(ImageMapping{slot.window_base, slot.first_offset, slot.length});
    set.images.push_back(cartridge_image(SMS_IMAGE_BANK_FIRST + bank, std::vector<std::uint8_t>(first, first + SMS_BANK_SIZE),
                                         std::move(mappings), "bank"));
  }
  return set;
}

std::vector<codegen::z80::ImageKind> image_kinds(const ExecutableImageSet& images) {
  std::vector<codegen::z80::ImageKind> kinds;
  for (const ExecutableImage& image : images.images)
    kinds.push_back(image.id.value == SMS_IMAGE_FIXED ? codegen::z80::ImageKind::invariant : codegen::z80::ImageKind::banked);
  return kinds;
}

codegen::z80::ImageSet build_image_set(const IngestResult& cartridge) {
  const ExecutableImageSet images = executable_images(cartridge);
  const auto kinds = image_kinds(images);
  codegen::z80::ImageProjection projected = codegen::z80::project_executable_images(images, kinds);
  // A validated cartridge always projects (identities, windows and tags are fixed by the contract table); an invalid one yields no
  // images, exactly like the historical builder.
  return projected.ok() ? std::move(projected.set) : codegen::z80::ImageSet{};
}

}  // namespace segarecomp::machine::master_system
