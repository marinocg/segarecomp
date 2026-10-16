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
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

struct ProcessorEmissionStats {
  Processor processor{Processor::main};
  std::uint32_t compiled{};           // instructions lowered to generated C
  std::uint32_t unsupported{};        // reached instructions emitted as typed instruction_unsupported stops
  std::uint32_t out_of_image_targets{};  // statically known targets outside the image (dispatch stops entry_not_compiled)
  std::uint32_t vector_roots{};       // handlers proven by static vector installation (MOVE.L #imm,(vector).W/L)
};

struct ProgramEmission {
  std::string c_source;
  std::vector<ProcessorEmissionStats> stats;
  std::string program_identity;
};

inline constexpr const char *kSegaCdGeneratorVersion = "segarecomp.sega_cd.generator.v1";

[[nodiscard]] ProgramEmission emit_sega_cd_program(const DiscImage &disc, const BootImages &images);

}  // namespace segarecomp::machine::sega_cd
