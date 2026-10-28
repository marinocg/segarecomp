#pragma once

// SEG-062-T003 (ADR 0128): build-time emission of the two-processor generated-native Sega CD program.
//
// For each processor instance the producer's ExecutableImageSet is traversed statically from the profile entry with the CPU-owned
// decode, lift and control-successor rules (libs/cpu/m68k); every reached instruction is lowered by the CPU-owned emitter
// (emit_m68k_operation_c) with a per-processor Sega CD M68kRuntimeCEmitter whose text targets platforms/sega-cd/runtime/
// scd_runtime.h. The result is one strict-C11 translation unit: per-instruction functions, a compiled-entry table and dispatcher per
// processor, the build-time disc table (ADR 0129) and `main`. No interpreter, no runtime decoder, no runtime code generation.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

// SEG-096-T002 (ADR 0207): emitter policy. The default is the per-mapping emitter of ADR 0132, kept byte-identical as the verified reference;
// `position_independent` emits ONE source-relative body per (image, source offset) of an image with several mappings and a thin per-mapping
// dispatch to it. Every instruction whose lowering cannot be proven source-relative keeps the per-mapping emission and is reported.
struct SegaCdEmitOptions {
  bool position_independent = false;
};

struct ProcessorEmissionStats {
  Processor processor{Processor::main};
  std::uint32_t compiled{};           // instructions lowered to generated C
  std::uint32_t unsupported{};        // reached instructions emitted as typed instruction_unsupported stops
  std::uint32_t out_of_image_targets{};  // statically known targets outside the image (dispatch stops entry_not_compiled)
  std::uint32_t vector_roots{};       // handlers proven by static vector installation (MOVE.L #imm,(vector).W/L)
  // SEG-096-T002: native instruction bodies (generated functions that lower an instruction), shared source-relative bodies among them, the
  // (mapping, instruction) instances those shared bodies serve, and the instances that kept per-mapping emission with the refusal reasons.
  std::uint32_t bodies{};
  std::uint32_t shared_bodies{};
  std::uint32_t shared_instances{};
  std::uint32_t fallback_instances{};
  std::vector<std::pair<std::string, std::uint32_t>> fallback_reasons;  // sorted by reason
};

// SEG-063-T001 (ADR 0127, ADR 0132): one compilation unit per (image, mapping). Units that overlap a module unit form an overlay
// group: their entries are not reachable from the per-PC dispatch switch; the platform runtime selects one candidate by live-byte
// verification (scd_overlay_dispatch) and only then calls that candidate's own dispatcher.
struct ImageEmissionStats {
  Processor processor{Processor::main};
  std::uint32_t image_id{};
  std::string name;
  std::string producer;
  std::uint32_t mapping_index{};
  std::uint32_t start{};
  std::uint32_t length{};
  std::uint32_t roots{};
  std::uint32_t compiled{};
  std::uint32_t unsupported{};
  std::uint32_t out_of_image_targets{};
  std::uint32_t shared_instances{};    // SEG-096-T002: this mapping's instructions served by a shared source-relative body
  std::uint32_t fallback_instances{};  // and those that kept per-mapping emission although position independence was requested
  int overlay_group = -1;  // index into the program's overlay groups, or -1 (SEG-062 per-PC dispatch)
};

// Bounds of the runtime registry (contract section 15): fixed so that selection and invalidation state is static machine data.
inline constexpr std::uint32_t kSegaCdOverlayGranuleShift = 6U;  // 64-byte granules (ADR 0127)
inline constexpr std::uint32_t kSegaCdOverlayMaxGroups = 16U;
inline constexpr std::uint32_t kSegaCdOverlayMaxCandidates = 16U;
inline constexpr std::uint32_t kSegaCdOverlayMaxGranules = 8192U;  // 512 KiB: the largest executable region (PRG RAM)

enum class EmissionError : std::uint8_t {
  none,
  roles_mismatch,           // ProcessorImages::roles is not parallel to the image set
  boot_image_shape,         // a non-module image other than the single-mapping image 1
  too_many_overlay_groups,  // more than kSegaCdOverlayMaxGroups groups across both processors
  too_many_candidates,      // a group with more than kSegaCdOverlayMaxCandidates units
  group_too_large,          // a group spanning more than kSegaCdOverlayMaxGranules granules
  image_validation,         // a processor's set does not validate
};
[[nodiscard]] const char *emission_error_name(EmissionError error) noexcept;

struct ProcessorsEmission {
  std::string c_source;        // both processors: entries, lookup, instruction functions, dispatch
  std::string overlay_source;  // candidate tables and scd_overlay_groups[] (empty without overlay groups)
  std::vector<ProcessorEmissionStats> stats;
  std::vector<ImageEmissionStats> images;
  std::uint32_t overlay_groups{};
  EmissionError error{EmissionError::none};
};

struct ProgramEmission {
  std::string c_source;
  std::vector<ProcessorEmissionStats> stats;
  std::string program_identity;
  std::vector<ImageEmissionStats> images;
  std::uint32_t overlay_groups{};
  EmissionError error{EmissionError::none};
};

inline constexpr const char *kSegaCdGeneratorVersion = "segarecomp.sega_cd.generator.v1";

// The disc-independent part of the emission (used directly by the Checkpoint A tests).
[[nodiscard]] ProcessorsEmission emit_sega_cd_processors(const BootImages &images, const SegaCdEmitOptions &options = {});
// `module_identity` (the SHA-256 of the admitted module manifest, empty without modules) enters the program identity only when set,
// so a program without modules keeps its SEG-062 identity and text.
// The program identity includes the emitter policy only when it is not the default, so default programs keep their identity and text.
[[nodiscard]] ProgramEmission emit_sega_cd_program(const DiscImage &disc, const BootImages &images, std::string_view module_identity = {},
                                                   const SegaCdEmitOptions &options = {});

}  // namespace segarecomp::machine::sega_cd
