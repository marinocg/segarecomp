#pragma once

// SEG-064-T002 (ADR 0130 Amendment 1, ADR 0142): the Sega CD BIOS-image profile - verified broad AOT with bounded executable-image
// materialization. Build time only; the generated program never decodes, interprets, compiles or synthesizes guest instructions.
//
// Images and their authorities (ADR 0076):
//   - the BIOS image itself: `immutable_input`, mapped at main $000000, verification none (ROM);
//   - copy images: `static_proof`, a verbatim copy of a ROM span declared by an explicit descriptor, checked against the ROM;
//   - disc images (SEG-064-T004): `immutable_input`, an extent of the external disc's track 1 that is placed in a processor's RAM
//     (the disc's identity is verified at start-up, so the bytes are immutable inputs; verification is per instruction, byte identity);
//   - discovered disc extents (SEG-078-T003, ADR 0205): the same bytes and the same per-instruction guard as a disc image, but admitted in chain order with
//     the identity of the build pass whose frontier they explain (`resolve_source`: an exact copy of blocks the machine itself read);
//   - epoch images: `bounded_build_time_materialization`, the PRG RAM observed at the sub runnable epoch by a build pass; or (SEG-079,
//     ADR 0179) `static_proof`, an image reconstructed from the BIOS image by a recipe (epoch_recipe.hpp), with no pass identity. Both are
//     verified whole against the live PRG RAM at the epoch;
//   - operand templates: `bounded_build_time_materialization` (a build-pass RAM frontier), verification `structural`; SEG-079-T002 (ADR 0180):
//     one record may be a bounded run of contiguous JMP (xxx).L slots (same authority and verification, every slot guarded individually).
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
#include "segarecomp/machine/sega_cd/dynamic_images.hpp"
#include "segarecomp/machine/sega_cd/epoch_recipe.hpp"
#include "segarecomp/machine/sega_cd/selective_admission.hpp"
#include "segarecomp/recompiler/executable_image.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kBiosWindowBytes = 0x20000U;   // main $000000-$01FFFF (contract section 3)
inline constexpr std::uint32_t kBiosMinimumBytes = 0x200U;    // vector table + system header
inline constexpr std::uint32_t kMaxEpochImages = 4U;          // ADR 0142 decision 8
inline constexpr std::uint32_t kMaxOperandTemplates = 256U;   // template SLOTS (SEG-079-T002, ADR 0180: was 64 single-instruction records, ADR 0142 decision 8)
inline constexpr std::uint32_t kMaxTemplateRunSlots = 128U;   // slots in one run record (SEG-079-T002)
inline constexpr std::uint32_t kTemplateSlotBytes = 6U;       // JMP (xxx).L
inline constexpr std::uint32_t kMaxCopyImages = 8U;
inline constexpr std::uint32_t kMaxProducedImages = 8U;        // SEG-111 (ADR 0255): produced-code images per program
inline constexpr std::uint32_t kMaxProducedBytes = 4096U;
inline constexpr std::uint32_t kMaxStaticFragments = 8U;        // SEG-112: static producer fragments per program       // SEG-111: one produced image is a bounded stream, not a program
inline constexpr std::uint32_t kMaxDiscImages = 64U;           // SEG-064-T004 (16); SEG-098-T004 (ADR 0213): 64, measured
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

// SEG-064-T004: a disc extent that is placed in a processor's RAM (the boot area in main work RAM, entered by the BIOS at its base; the
// disc's SP in sub PRG RAM, replacing transient BIOS code; a module the SP loads). Any extent of the verified disc is an immutable input;
// soundness is the per-instruction byte guard, so an extent the program never finds in RAM simply never runs.
struct DiscImageSpec {
  Processor processor{Processor::main};
  std::uint32_t track_offset{};  // byte offset in the logical user data of track 1 (the 32 KiB system area is offsets 0-0x7FFF)
  std::uint32_t base{};          // execution base (main work RAM for the IP side, sub PRG RAM for the SP side or a loaded module)
  std::uint32_t length{};
  std::vector<std::uint8_t> bytes;  // the extent as read from the verified disc at build time (length bytes)
};

enum class AdmissionKind : std::uint8_t { epoch, operand_template, epoch_recipe, disc_extent, static_disc_extent, produced_image, static_fragment };  // epoch_recipe: SEG-079 (ADR 0179), a static producer;
                                                                                                 // disc_extent: SEG-078-T003 (ADR 0205), a discovered extent;
                                                                                                 // static_disc_extent: SEG-098-T002 (ADR 0211), an extent proven by the static loader scan (no pass identity)
                                                                                                 // static_fragment: SEG-112 (ADR 0256), bytes a generation-time producer proof evaluated from the boot images (no pass identity, no disc extent)
                                                                                                 // produced_image: SEG-111 (ADR 0255), bytes a verified producer computed in a build pass (no disc offset)

struct Admission {
  AdmissionKind kind{AdmissionKind::epoch};
  std::string pass_identity;            // identity of the program whose build pass observed it
  Processor processor{Processor::sub};  // operand_template: the processor; epoch: always sub
  std::uint32_t address{};              // operand_template: the instruction address; epoch: 0 (PRG RAM base)
  std::vector<std::uint8_t> bytes;      // epoch: the PRG RAM snapshot; operand_template: the observed live bytes (>= 6; a run: 6 * slots)
  std::uint32_t slots{1};               // operand_template: 1 = one instruction at `address`; >1 (SEG-079-T002, ADR 0180) = a run of that many
                                        // contiguous JMP (xxx).L slots at a 6-byte stride from `address`, `bytes` exactly 6 * slots
  std::uint32_t track_offset{};         // disc_extent: the track-1 user-data byte offset (`address` is the execution base, `bytes` the extent read from the disc)
  EpochRecipe recipe;                   // epoch_recipe: the image is reconstructed from the BIOS image (no pass identity, bytes ignored)
};

struct BiosProfileInputs {
  BiosImage bios;
  std::vector<CopyImage> copies;
  std::vector<DiscImageSpec> disc_images;     // SEG-064-T004
  std::uint32_t application_entry{};          // SEG-064-T004: the main address whose first execution ends the run (0 = none)
  std::vector<Admission> admissions;  // in admission order
  // SEG-100 (ADR 0221): file-first dynamic images - immutable disc files compiled to base-less native bodies and bound at run time. Needs a
  // partitioned emission (a non-zero partition window). `dynamic_policy` names the build-time candidate policy and enters the program identity.
  std::vector<DynamicImageSpec> dynamic_images;
  std::string dynamic_policy;
  std::string aot_policy;                     // SEG-097-T003: the policy identity of an experimental request ("" = Compatibility); enters the program identity
  bool main_z80 = false;                      // SEG-114 (ADR 0258): the generated main registers the main-side Z80 (scd_z80_ops); the build supplies the Z80 units. Not part of the program identity
};

enum class BiosProfileError : std::uint8_t {
  none,
  copy_invalid,           // descriptor outside the ROM or the processor's RAM, odd, empty, or too many
  disc_image_invalid,     // SEG-064-T004: extent outside the processor's RAM (main: work RAM or, SEG-071-T001, the 2M Word RAM view), odd, empty, no system area, or too many
  provenance_mismatch,    // an admission's pass identity is not the identity of the program before it
  epoch_recipe_invalid,   // SEG-079: the recipe does not reconstruct a whole-snapshot image from the BIOS image (index = admission)
  epoch_invalid,          // empty, longer than PRG RAM, not a whole snapshot (granule / trailing zero rule) or too many
  static_fragment_invalid,// SEG-112: a static fragment that is empty, odd, over the bound, outside main Work RAM / sub PRG RAM, or too many
  produced_invalid,       // SEG-111: a produced image that is empty, odd, over kMaxProducedBytes, outside main Work RAM / sub PRG RAM, or too many
  template_invalid,       // not exactly one JMP (xxx).W/.L at an even RAM address of its processor, duplicate, or too many; a run: any slot not
                          // JMP (xxx).L with an even operand, a run over kMaxTemplateRunSlots, or an overlap
  image_validation,
  partition_invalid,      // a partition window that is not a power of two in [kMinPartitionWindow, kMaxPartitionWindow]
  dynamic_invalid,        // SEG-100: the dynamic image set does not validate (see BiosProfileEmission::dynamic_error), or no partition window was given
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
  std::uint32_t omitted{};    // SEG-097-T003: aligned starts outside the (validated) selective proposal, never decoded
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
  unsigned jobs{};                   // SEG-100: worker threads for the dynamic images' derivation (0 = hardware concurrency, at most 16)
  // SEG-097-T003 (ADR 0208): the experimental selective admission. The producer scores each image's bytes once; aligned starts outside its validated
  // proposal are not compiled. Any failure emits the broad program (this call) and reports the reason in `BiosProfileEmission::admission`. The caller
  // must also set `BiosProfileInputs::aot_policy` to the same policy identity so the program identity (and the admission provenance chain) carries it.
  const SelectiveAdmissionRequest *selective = nullptr;
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
  SelectiveAdmissionOutcome admission;
  DynamicEmission dynamic;           // SEG-100: statistics and identity of the dynamic images (their units are in `units`)
  BiosProfileError error{BiosProfileError::none};
  std::uint32_t error_index{};       // the failing copy/admission
  ImageValidationError validation{ImageValidationError::none};
};

// The identity of the program for `inputs` with only its first `admissions` admissions (the provenance chain's links).
[[nodiscard]] std::string bios_profile_identity(const DiscImage &disc, const BiosProfileInputs &inputs, std::size_t admissions);

[[nodiscard]] BiosProfileEmission emit_sega_cd_bios_program(const DiscImage &disc, const BiosProfileInputs &inputs,
                                                            const BiosProfileEmitOptions &options = {});

}  // namespace segarecomp::machine::sega_cd
