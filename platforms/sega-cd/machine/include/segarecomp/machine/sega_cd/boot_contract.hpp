#pragma once

// SEG-113 (ADR 0257): the static BIOS boot-image contract - which executable bytes the Sega CD BIOS places in RAM before the first
// application instruction, derived from the BIOS image and the disc alone (no generated program is run).
//
// Two images, both `STATIC_BOOT_CONDITIONAL` at best: the main boot area (disc bytes from $200, placed at main Work RAM $FF0000 and
// entered there) and the sub boot program (the disc's SP, placed at sub PRG RAM $6000). Neither is proven from first principles - the
// main side's Word RAM -> Work RAM copy is an immediate-operand loop of the BIOS, but the disc -> Word RAM delivery is the sub BIOS's -
// so each is offered only under a *verified profile*: an exact BIOS image (SHA-256) whose placement has been observed on authentic discs
// with an exact header form, or a project-authored fixture BIOS (a tag in its header) that implements the same contract. Every
// other BIOS or header form yields no image and the build keeps the execution-observed admission (ADR 0205). A profile only proposes
// bytes: the emitter recomputes them from the disc and the BIOS, and every executed instruction is still compared with the live bytes.

#include <cstdint>
#include <string>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/bios_profile.hpp"

namespace segarecomp::machine::sega_cd {

enum class BootProof : std::uint8_t { proven, conditional };   // STATIC_BOOT_PROVEN / STATIC_BOOT_CONDITIONAL (UNPROVEN images are never listed)
[[nodiscard]] const char *boot_proof_name(BootProof proof) noexcept;

enum class BootContractRefusal : std::uint8_t {
  none,
  bios_not_verified,   // no verified profile matches this BIOS image
  disc_boot_area,      // the disc has no parsable Sega CD boot area
  header_not_verified, // the profile was verified only for another header form
  boot_area_short,     // track 1 does not hold the placed bytes
};
[[nodiscard]] const char *boot_contract_refusal_name(BootContractRefusal refusal) noexcept;

inline constexpr std::uint32_t kBootIpTrackOffset = 0x200U;   // the BIOS places the disc bytes from here at kIpBase
inline constexpr std::uint32_t kBiosTagOffset = 0x100U;       // project-authored fixture BIOS images carry their profile tag here
inline constexpr std::size_t kBiosTagBytes = 32U;

struct BootContractImage {
  Processor processor{Processor::main};
  std::uint32_t track_offset{};   // byte offset in the logical user data of track 1
  std::uint32_t base{};           // guest execution base
  std::uint32_t length{};
  BootProof proof{BootProof::conditional};
  std::vector<std::uint8_t> bytes;
};

struct BootContract {
  std::string profile;                          // the verified profile that matched ("" = none)
  BootContractRefusal refusal{BootContractRefusal::none};
  std::vector<BootContractImage> images;        // main boot area first, then the sub boot program; empty on refusal
};

// Pure function of the BIOS image and the (verified) disc.
[[nodiscard]] BootContract derive_boot_contract(const BiosImage &bios, const DiscImage &disc);
// `static-boot:<cpu>:<track-1 offset>:<base>:<length>:<sha256 of the bytes>` - the form the emitter recomputes and compares (coordinates are never trusted).
[[nodiscard]] std::string static_boot_admission(const BootContractImage &image);

}  // namespace segarecomp::machine::sega_cd
