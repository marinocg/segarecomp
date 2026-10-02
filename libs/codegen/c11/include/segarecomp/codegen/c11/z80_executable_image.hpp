#pragma once

// Z80 projection of CPU-neutral executable images onto the Z80 emitter input (SEG-028-T003; ADR 0077).
//
// A pure function: executable images (libs/recompiler executable_image.hpp) -> codegen::z80::ImageSet. Everything Z80-specific stays
// here or in the emitter: the 16-bit identity and window bases, `ImageKind` (supplied by the platform, never inferred from the generic
// artifact) and the RAM-backed live guard (`structural` verification <-> CodeImage::live_bytes). The projection is emission-neutral: a
// platform producer that describes exactly its historical ImageSet projects back onto it field for field.

#include <span>
#include <string>

#include "segarecomp/codegen/c11/z80.hpp"
#include "segarecomp/recompiler/executable_image.hpp"

namespace segarecomp::codegen::z80 {

struct ImageProjection {
  ImageSet set;
  std::string error;  // empty on success; `set` is empty on failure
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// `kinds[i]` is the platform's binding kind for image i. Fails closed on: an invalid set, a non-Z80 set, a kind count different from
// the image count, an identity above 0xFFFF, a mapping base above 0xFFFF, and `byte_identity` verification (the Z80 has no
// whole-instruction byte guard; RAM-backed Z80 code uses the structural guard).
[[nodiscard]] ImageProjection project_executable_images(const ExecutableImageSet &images, std::span<const ImageKind> kinds);

}  // namespace segarecomp::codegen::z80
