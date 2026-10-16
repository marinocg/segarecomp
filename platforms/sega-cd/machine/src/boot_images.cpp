#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

const char *processor_name(Processor processor) noexcept { return processor == Processor::main ? "main" : "sub"; }

const char *boot_image_error_name(BootImageError error) noexcept {
  switch (error) {
  case BootImageError::none: return "none";
  case BootImageError::boot_header: return "boot_header";
  case BootImageError::ip_outside_work_ram: return "ip_outside_work_ram";
  case BootImageError::sp_outside_prg_ram: return "sp_outside_prg_ram";
  case BootImageError::image_validation: return "image_validation";
  }
  return "unknown";
}

namespace {

ProcessorImages make(Processor processor, const SegaCdBootDescriptor &descriptor, const std::vector<std::uint8_t> &area,
                     std::uint32_t base, const char *producer) {
  ProcessorImages out;
  out.processor = processor;
  out.images.cpu = CpuVariant::mc68000;
  ExecutableImage image;
  image.id = ImageId{1};
  image.bytes.assign(area.begin() + descriptor.disc_address, area.begin() + descriptor.disc_address + descriptor.load_size);
  image.mappings = {ImageMapping{base, 0U, descriptor.load_size}};
  image.provenance.authority = ImageAuthority::immutable_input;
  image.provenance.producer = producer;
  image.provenance.evidence = {{"disc.extent", "track:1 system_area_byte:" + std::to_string(descriptor.disc_address) +
                                                   " length:" + std::to_string(descriptor.load_size)},
                               {"profile", "synthetic_post_bios"}};
  image.verification = ImageVerification::byte_identity;
  out.images.images.push_back(std::move(image));
  out.entry_pc = base + descriptor.entry_offset;
  out.roles.push_back(ImageRole{processor == Processor::main ? "boot_ip" : "boot_sp", false, {descriptor.entry_offset}});
  return out;
}

}  // namespace

BootImagesResult produce_boot_images(const SegaCdBootHeader &header, const std::vector<std::uint8_t> &area) {
  BootImagesResult result;
  if (area.size() < kSegaCdSystemAreaBytes) {
    result.error = BootImageError::boot_header;
    result.boot_error = SegaCdBootError::system_area_short;
    return result;
  }
  // The descriptors were range-checked against the system area by the parser; the profile adds the RAM fits.
  if (static_cast<std::uint64_t>(kIpBase) + header.ip.load_size > kMainWorkRamEnd ||
      kIpBase + header.ip.load_size > kMainInitialSsp) {
    result.error = BootImageError::ip_outside_work_ram;
    return result;
  }
  if (static_cast<std::uint64_t>(kSpBase) + header.sp.load_size > kPrgRamBytes) {
    result.error = BootImageError::sp_outside_prg_ram;
    return result;
  }
  BootImages images;
  images.header = header;
  images.main = make(Processor::main, header.ip, area, kIpBase, "sega_cd.boot_ip");
  images.sub = make(Processor::sub, header.sp, area, kSpBase, "sega_cd.boot_sp");
  for (const auto *set : {&images.main.images, &images.sub.images}) {
    const auto validation = validate_executable_image_set(*set);
    if (!validation.ok()) {
      result.error = BootImageError::image_validation;
      result.validation = validation.error;
      return result;
    }
  }
  result.images = std::move(images);
  return result;
}

BootImagesResult produce_boot_images(const DiscImage &disc) {
  std::vector<std::uint8_t> area;
  const auto boot = read_sega_cd_boot(disc, &area);
  if (!boot.header) {
    BootImagesResult result;
    result.error = BootImageError::boot_header;
    result.boot_error = boot.error;
    return result;
  }
  return produce_boot_images(*boot.header, area);
}

}  // namespace segarecomp::machine::sega_cd
