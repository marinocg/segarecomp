#include "segarecomp/machine/master_system/image_set.hpp"

#include <algorithm>

namespace segarecomp::machine::master_system {

codegen::z80::ImageSet build_image_set(const IngestResult& cartridge) {
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

}  // namespace segarecomp::machine::master_system
