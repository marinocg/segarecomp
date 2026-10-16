#pragma once

// SEG-063-T001 (ADR 0127, ADR 0132): the explicit, digest-checked module list - the only authority that admits a disc-loaded
// module as an executable image. A filename, extension or ISO9660 path alone admits nothing: every module names the disc identity
// it belongs to, its bytes (an ISO9660 path and/or a data-track extent, which must agree), their SHA-256, the owning processor
// instance, every execution base, its entry roots and its verification policy. Nothing is inferred from names or contents.
//
// Manifest text (one directive per line; '#' starts a comment; numbers are decimal or 0x-hexadecimal):
//
//   segarecomp.sega_cd.modules.v1
//   disc <disc identity, 64 lowercase hex>
//   module <name>                 [a-z0-9_]+, unique
//   processor main|sub
//   file </ISO/PATH;1>            optional; resolved through the data track's ISO9660 volume
//   extent <block> <bytes>        optional; logical block of track 1 and byte size (required when there is no file)
//   sha256 <64 lowercase hex>
//   map <execution base>          one or more; the whole module at that base
//   entry <image offset>          one or more; declared entry roots (applied to every base)
//   verify byte_identity          the only policy for RAM-resident code
//   end
//
// Producers this milestone supports for the Sega CD are the boot IP/SP (boot_images.hpp) and these disc modules. One module at
// several addresses is several mappings of one image (ADR 0126 row 3), verified by live-byte comparison like every overlay. The
// static-proof copy and bounded-materialization producers (ADR 0049/0073/0122) are cartridge-ROM authorities with no disc-to-RAM
// proof in the existing contracts; the manifest has no directive for them, so they cannot be requested.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/iso9660.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr const char *kModuleManifestVersion = "segarecomp.sega_cd.modules.v1";
inline constexpr std::uint32_t kModuleMaxCount = 32U;
inline constexpr std::uint32_t kModuleMaxBytes = 0x80000U;  // PRG RAM
inline constexpr std::uintmax_t kModuleManifestMaxBytes = 64U * 1024U;

enum class ModuleError : std::uint8_t {
  none,
  manifest_unreadable,
  manifest_syntax,            // unknown directive, bad number, directive outside a module, missing `end`, repeated directive
  manifest_version,
  disc_identity_mismatch,     // the manifest names another disc
  name_invalid,
  duplicate_name,
  processor_invalid,
  source_missing,             // neither file nor extent
  iso9660,                    // the path does not resolve (see Iso9660Error)
  extent_mismatch,            // the file's extent differs from the stated extent
  extent_outside_track,       // the extent leaves the data track
  size_invalid,               // zero, or larger than kModuleMaxBytes
  digest_mismatch,            // the disc bytes do not hash to the stated SHA-256
  mapping_missing,
  mapping_invalid,            // an odd base
  mapping_outside_executable_ram,  // the mapped range is not inside one executable RAM region of the processor
  entry_missing,
  entry_invalid,              // an odd offset or one outside the module
  verification_unsupported,   // anything but byte_identity
  duplicate_module,           // the same bytes mapped over the same addresses twice (indistinguishable candidates)
  too_many_modules,
  image_validation,           // the resulting set does not validate (see ImageValidationError)
};
[[nodiscard]] const char *module_error_name(ModuleError error) noexcept;

struct ModuleSpec {
  std::string name;
  Processor processor{Processor::sub};
  std::optional<std::string> path;
  std::optional<Iso9660Extent> extent;
  std::string sha256;
  std::vector<std::uint32_t> bases;
  std::vector<std::uint32_t> entries;
  std::string verification;
  unsigned line{};
};

struct ModuleManifest {
  std::string disc_identity;
  std::vector<ModuleSpec> modules;
};

struct ModuleManifestResult {
  std::optional<ModuleManifest> manifest;
  ModuleError error{ModuleError::none};
  unsigned line{};
};

[[nodiscard]] ModuleManifestResult parse_module_manifest(std::string_view text);

struct AdmittedModule {
  std::string name;
  Processor processor{Processor::sub};
  std::string path;            // empty when named by extent only
  Iso9660Extent extent;        // the verified extent
  std::string sha256;
  std::vector<std::uint32_t> bases;
  std::vector<std::uint32_t> entries;
  std::uint32_t image_id{};    // ImageId in the processor's set
};

struct ModuleAdmission {
  std::vector<AdmittedModule> modules;
  std::string identity;  // SHA-256 of the canonical manifest text (enters the program identity)
  ModuleError error{ModuleError::none};
  Iso9660Error iso_error{Iso9660Error::none};
  ImageValidationError validation{ImageValidationError::none};
  std::string module;    // the offending module's name (content-free)
};

// Verifies every module against `disc` (ISO9660 extent, digest) and appends it to its processor's set in `images` (platform_selector
// set). On any error `images` is left unchanged.
[[nodiscard]] ModuleAdmission admit_modules(const DiscImage &disc, const ModuleManifest &manifest, BootImages &images);

}  // namespace segarecomp::machine::sega_cd
