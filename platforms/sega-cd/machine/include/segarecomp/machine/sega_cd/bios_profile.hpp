#pragma once

// SEG-064-T002 (ADR 0130 Amendment 1, ADR 0142): the Sega CD BIOS-image profile - verified broad AOT with bounded executable-image
// materialization. Build time only; the generated program never decodes, interprets, compiles or synthesizes guest instructions.
//
// Images and their authorities (ADR 0076):
//   - the BIOS image itself: `immutable_input`, mapped at main $000000, verification none (ROM);
//   - copy images: `static_proof`, a verbatim copy of a ROM span declared by an explicit descriptor, checked against the ROM;
//   - epoch images: `bounded_build_time_materialization`, the PRG RAM observed at the sub runnable epoch by a build pass;
//   - operand templates: `bounded_build_time_materialization` (a build-pass RAM frontier), verification `structural`.
// Every image is admitted by broad AOT (each aligned start the CPU owner decodes, lifts and lowers is a compiled entry; admission
// is not a claim that the bytes are code). RAM images are verified per instruction (exact bytes) before each instruction runs.
// Admissions form a provenance chain: each carries the identity of the program whose build pass produced it, and that identity
// must equal the identity of the program emitted from the same inputs and the admissions before it.
//
// Programs built from a real BIOS embed BIOS-derived code: they stay in ignored local storage and are never published.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"
#include "segarecomp/recompiler/executable_image.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kBiosWindowBytes = 0x20000U;   // main $000000-$01FFFF (contract section 3)
inline constexpr std::uint32_t kBiosMinimumBytes = 0x200U;    // vector table + system header
inline constexpr std::uint32_t kMaxEpochImages = 4U;          // ADR 0142 decision 8
inline constexpr std::uint32_t kMaxOperandTemplates = 64U;
inline constexpr std::uint32_t kMaxCopyImages = 8U;
inline constexpr std::uint32_t kEpochGranule = 0x100U;        // snapshot length granule (scd_prg_snapshot_length)
inline constexpr const char *kSegaCdBiosGeneratorVersion = "segarecomp.sega_cd.bios_profile.v2";

enum class BiosError : std::uint8_t { none, size_invalid, header_invalid, reset_vector_invalid };
[[nodiscard]] const char *bios_error_name(BiosError error) noexcept;

struct BiosImage {
  std::vector<std::uint8_t> bytes;
  std::string sha256;
  std::uint32_t reset_ssp{};
  std::uint32_t reset_pc{};
};

struct BiosAdmission {
  std::optional<BiosImage> bios;
  BiosError error{BiosError::none};
};

// Layout of a locally supplied BIOS image: even size in [kBiosMinimumBytes, kBiosWindowBytes], "SEGA" at $100 (the documented
// system-header convention), even reset SSP, even reset PC inside the image past $200.
[[nodiscard]] BiosAdmission admit_bios(std::span<const std::uint8_t> bytes);

struct CopyImage {
  Processor processor{Processor::sub};
  std::uint32_t base{};        // execution base
  std::uint32_t rom_offset{};  // source span in the BIOS image
  std::uint32_t length{};
};

enum class AdmissionKind : std::uint8_t { epoch, operand_template };

struct Admission {
  AdmissionKind kind{AdmissionKind::epoch};
  std::string pass_identity;            // identity of the program whose build pass observed it
  Processor processor{Processor::sub};  // operand_template: the processor; epoch: always sub
  std::uint32_t address{};              // operand_template: the instruction address; epoch: 0 (PRG RAM base)
  std::vector<std::uint8_t> bytes;      // epoch: the PRG RAM snapshot; operand_template: the observed live bytes (>= 6)
};

struct BiosProfileInputs {
  BiosImage bios;
  std::vector<CopyImage> copies;
  std::vector<Admission> admissions;  // in admission order
};

enum class BiosProfileError : std::uint8_t {
  none,
  copy_invalid,           // descriptor outside the ROM or the processor's RAM, odd, empty, or too many
  provenance_mismatch,    // an admission's pass identity is not the identity of the program before it
  epoch_invalid,          // empty, longer than PRG RAM, not a whole snapshot (granule / trailing zero rule) or too many
  template_invalid,       // not exactly one JMP (xxx).W/.L at an even RAM address of its processor, duplicate, or too many
  image_validation,
  partition_invalid,      // a partition window that is not a power of two in [kMinPartitionWindow, kMaxPartitionWindow]
};
[[nodiscard]] const char *bios_profile_error_name(BiosProfileError error) noexcept;

struct BiosProfileImageStats {
  Processor processor{Processor::main};
  std::string name;       // bios, copy<N>, epoch<N>
  std::string authority;
  std::uint32_t start{};
  std::uint32_t length{};
  std::uint32_t compiled{};   // aligned starts admitted as compiled entries
  std::uint32_t rejected{};   // aligned starts the CPU owner does not decode, lift or lower (no entry)
};

// SEG-064-T003: deterministic translation-unit partitioning. A window of 0 emits the single translation unit of SEG-064-T002
// (byte-identical). A non-zero window (a power of two in [kMinPartitionWindow, kMaxPartitionWindow]) emits one unit per processor
// and non-empty PC window [k * window, (k + 1) * window): every compiled variant, operand template and live-byte selection chain whose
// PC lies in the window, plus that window's PC switch. `c_source` is then the shared dispatch unit (entry tables, the compiled-entry
// lookups, the per-processor window switch, the disc tables and the program). A unit's text depends only on the images, variants and
// templates of its own window - never on the program identity or on other windows - so an unchanged unit stays byte-identical across
// admissions and its object can be reused. Partitioning changes no behaviour and is not part of the program identity.
inline constexpr std::uint32_t kMinPartitionWindow = 0x100U;
inline constexpr std::uint32_t kMaxPartitionWindow = 0x01000000U;

struct BiosProfileEmitOptions {
  std::uint32_t partition_window{};
};

struct BiosProfileUnit {
  std::string name;            // p<cpu>_<first PC, 6 hex digits>.c
  unsigned cpu{};
  std::uint32_t first_pc{};    // the window's first PC
  std::uint32_t functions{};   // instruction functions (variants and templates) in the unit
  std::string c_source;
};

[[nodiscard]] bool valid_partition_window(std::uint32_t window) noexcept;

struct BiosProfileEmission {
  std::string c_source;
  std::vector<BiosProfileUnit> units;  // empty for a single translation unit; otherwise in (cpu, first_pc) order
  std::string program_identity;
  std::vector<BiosProfileImageStats> images;
  std::uint32_t functions{};
  std::uint32_t entries[2]{};
  std::uint32_t variant_pcs[2]{};    // PCs with more than one distinct compiled instruction (selected by live bytes)
  std::uint32_t deduplicated{};      // identical variants merged at emission
  std::uint32_t templates{};
  BiosProfileError error{BiosProfileError::none};
  std::uint32_t error_index{};       // the failing copy/admission
  ImageValidationError validation{ImageValidationError::none};
};

// The identity of the program for `inputs` with only its first `admissions` admissions (the provenance chain's links).
[[nodiscard]] std::string bios_profile_identity(const DiscImage &disc, const BiosProfileInputs &inputs, std::size_t admissions);

[[nodiscard]] BiosProfileEmission emit_sega_cd_bios_program(const DiscImage &disc, const BiosProfileInputs &inputs,
                                                            const BiosProfileEmitOptions &options = {});

}  // namespace segarecomp::machine::sega_cd
