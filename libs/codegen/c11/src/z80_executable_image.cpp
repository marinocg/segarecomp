#include "segarecomp/codegen/c11/z80_executable_image.hpp"

namespace segarecomp::codegen::z80 {

ImageProjection project_executable_images(const ExecutableImageSet &images, std::span<const ImageKind> kinds) {
  ImageProjection out;
  const auto fail = [&out](std::string message) {
    out.set.images.clear();
    out.error = std::move(message);
    return out;
  };
  const ImageValidation validation = validate_executable_image_set(images);
  if (!validation.ok())
    return fail("executable image " + std::to_string(validation.image_index + 1U) + ": " +
                std::string(image_validation_error_name(validation.error)));
  if (images.cpu != CpuVariant::z80) return fail("executable image set is not a Z80 set");
  if (kinds.size() != images.images.size()) return fail("image kind count does not match the image count");
  for (std::size_t index = 0; index < images.images.size(); ++index) {
    const ExecutableImage &image = images.images[index];
    const std::string where = "executable image " + std::to_string(image.id.value) + ": ";
    if (image.id.value > 0xFFFFU) return fail(where + "identity exceeds the 16-bit entry-key half");
    if (image.verification == ImageVerification::byte_identity) return fail(where + "byte_identity verification is not a Z80 guard");
    if (kinds[index] != ImageKind::invariant && kinds[index] != ImageKind::banked) return fail(where + "unknown image kind");
    CodeImage code;
    code.identity = image.id.value;
    code.kind = kinds[index];
    code.live_bytes = image.verification == ImageVerification::structural;
    const auto bytes = executable_image_bytes(images, image);
    code.bytes.assign(bytes.begin(), bytes.end());
    for (const ImageMapping &mapping : image.mappings) {
      if (mapping.execution_base > 0xFFFFU) return fail(where + "window base exceeds the 16-bit logical space");
      code.windows.push_back(CodeWindow{static_cast<std::uint16_t>(mapping.execution_base), mapping.first_offset, mapping.length});
    }
    out.set.images.push_back(std::move(code));
  }
  return out;
}

}  // namespace segarecomp::codegen::z80
