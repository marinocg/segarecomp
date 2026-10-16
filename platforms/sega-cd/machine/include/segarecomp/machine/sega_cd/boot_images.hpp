#pragma once

// SEG-062-T002 (ADR 0126, contract section 9): the Sega CD boot-image producer of the synthetic post-BIOS fixture profile.
//
// Maps the IP to the main processor instance and the SP to the sub processor instance, one ExecutableImageSet each (both
// CpuVariant::mc68000), with immutable_input authority, byte_identity verification (the code lives in writable RAM) and the disc
// extent as producer evidence. The processor identity and the profile bases are platform-owned; nothing here enters the generic
// executable-image header.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/recompiler/executable_image.hpp"

namespace segarecomp::machine::sega_cd {

enum class Processor : std::uint8_t { main = 0, sub = 1 };
[[nodiscard]] const char *processor_name(Processor processor) noexcept;

// Synthetic post-BIOS profile constants (contract section 9; C = project convention, T = BIOS-owned target fact).
inline constexpr std::uint32_t kIpBase = 0x00FF0000U;        // C: IP in main work RAM
inline constexpr std::uint32_t kSpBase = 0x00006000U;        // T: SP in PRG RAM at $6000 (re-verified by SEG-064)
inline constexpr std::uint32_t kMainInitialSsp = 0x00FFFE00U; // C
inline constexpr std::uint32_t kMainWorkRamBegin = 0x00FF0000U;
inline constexpr std::uint32_t kMainWorkRamEnd = 0x01000000U;
inline constexpr std::uint32_t kPrgRamBytes = 0x00080000U;

enum class BootImageError : std::uint8_t { none, boot_header, ip_outside_work_ram, sp_outside_prg_ram, image_validation };
[[nodiscard]] const char *boot_image_error_name(BootImageError error) noexcept;

// SEG-063-T001 (ADR 0132): the platform-owned role of each image of a processor's set (parallel to `images.images`). The generic
// executable-image header carries no name and no entry roots; both are Sega CD producer facts used only by this machine's emission.
struct ImageRole {
  std::string name;                          // boot_ip, boot_sp, or the manifest module name ([a-z0-9_]+)
  bool module = false;                       // a disc-loaded module (an overlay candidate, ADR 0127); false for IP/SP
  std::vector<std::uint32_t> entry_offsets;  // declared entry roots, as image offsets (applied to every mapping that covers them)
};

struct ProcessorImages {
  Processor processor{Processor::main};
  ExecutableImageSet images;
  std::uint32_t entry_pc{};
  std::vector<ImageRole> roles;  // one per image, same order
};

struct BootImages {
  SegaCdBootHeader header;
  ProcessorImages main;
  ProcessorImages sub;
};

struct BootImagesResult {
  std::optional<BootImages> images;
  BootImageError error{BootImageError::none};
  SegaCdBootError boot_error{SegaCdBootError::none};
  ImageValidationError validation{ImageValidationError::none};
};

// Pure producer over a parsed header and the 32 KiB system area.
[[nodiscard]] BootImagesResult produce_boot_images(const SegaCdBootHeader &header, const std::vector<std::uint8_t> &system_area);
// Reads the boot area of `disc` and produces both sets.
[[nodiscard]] BootImagesResult produce_boot_images(const DiscImage &disc);

}  // namespace segarecomp::machine::sega_cd
