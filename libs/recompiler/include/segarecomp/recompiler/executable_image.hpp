#pragma once

// CPU-neutral executable-image artifact (SEG-028; ADR 0077, docs/architecture/executable-image-contract.md).
//
// An executable image is the build-time statement "these bytes, executed by this CPU, at these execution addresses, were produced by
// this producer, and live bytes must be verified this way". It is the boundary between producers (where executable bytes come from) and
// consumers (broad AOT today). It is not a loader, a memory model, a mapper model, a runtime object or a discovery result: the final
// executable never sees it, only the generated tables a CPU emitter derives from it.
//
// Identity is layered. This header owns only the build-local `ImageId` (a dense 1-based ordinal in deterministic producer order). The
// generated entry key is CPU/codegen-owned and runtime selection keys are platform-owned; neither appears here.
//
// This header deliberately names no CPU, console, device or platform concept (tests/executable_image_forbidden_identifiers_test.py).

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/core/address.hpp"

namespace segarecomp {

// How the compiler establishes the bytes (closed; ADR 0076 "Image authority"). A future analysis-based producer uses `static_proof`.
enum class ImageAuthority : std::uint8_t {
  immutable_input,                     // the validated input image
  static_proof,                        // static reasoning over immutable input, no guest execution
  bounded_build_time_materialization,  // observed while running generated-native code under the bounded build-time execution rules
};
inline constexpr std::uint8_t kImageAuthorityCount = 3;

// Which live bytes must equal the image bytes (closed tag). The implementation and the consequence of a mismatch are CPU/platform owned.
enum class ImageVerification : std::uint8_t {
  none,           // the bytes cannot change while mapped
  byte_identity,  // every byte of every executed instruction must equal the image bytes
  structural,     // the CPU-defined structural bytes must be equal; the CPU defines which bytes are live payload
};
inline constexpr std::uint8_t kImageVerificationCount = 3;

struct ImageId {
  std::uint32_t value = 0;  // 1-based; 0 is never a valid image
  friend bool operator==(ImageId, ImageId) = default;
};

// Image offset `o` (first_offset <= o < first_offset + length) executes at `execution_base + o`. Several mappings mean the same bytes may
// execute at several bases; which one is live at run time is platform semantics and never part of this artifact.
struct ImageMapping {
  std::uint32_t execution_base = 0;
  std::uint32_t first_offset = 0;
  std::uint32_t length = 0;
  friend bool operator==(const ImageMapping &, const ImageMapping &) = default;
};

// The image's bytes are `[offset, offset + length)` of an earlier image of the same set that owns its bytes (no copy).
struct ImageSourceReference {
  ImageId source{};
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
  friend bool operator==(const ImageSourceReference &, const ImageSourceReference &) = default;
};

// Producer-owned evidence, including the derivation relationship (there is no closed generic derivation taxonomy). It is build-artifact
// data: durable reports carry only the authority, the producer name and counts, never these facts.
struct ImageEvidenceFact {
  std::string name;
  std::string value;
  friend bool operator==(const ImageEvidenceFact &, const ImageEvidenceFact &) = default;
};

struct ImageProvenance {
  ImageAuthority authority = ImageAuthority::immutable_input;
  std::string producer;  // stable producer name: [a-z0-9_.]+
  std::vector<ImageEvidenceFact> evidence;
  friend bool operator==(const ImageProvenance &, const ImageProvenance &) = default;
};

struct ExecutableImage {
  ImageId id{};
  std::vector<std::uint8_t> bytes;              // owned bytes; empty when `source` is set
  std::optional<ImageSourceReference> source;   // borrowed bytes
  std::vector<ImageMapping> mappings;           // non-empty
  ImageProvenance provenance;
  ImageVerification verification = ImageVerification::none;
  friend bool operator==(const ExecutableImage &, const ExecutableImage &) = default;
};

struct ExecutableImageSet {
  CpuVariant cpu{};
  // Several images may map the same execution addresses only when the platform supplies a fail-closed run-time selection mechanism.
  bool platform_selector = false;
  std::vector<ExecutableImage> images;
  friend bool operator==(const ExecutableImageSet &, const ExecutableImageSet &) = default;
};

enum class ImageValidationError : std::uint8_t {
  none,
  id_not_dense,                // image i does not carry ImageId i + 1
  unknown_authority,
  unknown_verification,
  invalid_producer_name,       // empty or outside [a-z0-9_.]
  no_bytes,                    // neither owned bytes nor a source reference
  bytes_and_source,            // both owned bytes and a source reference
  unresolved_source,           // the source is not an earlier image that owns its bytes
  source_out_of_range,         // empty or outside the source image's bytes
  no_mappings,
  empty_mapping,
  mapping_outside_bytes,       // first_offset + length exceeds the image bytes
  mapping_wraps,               // the execution range leaves the 32-bit execution space
  overlapping_mappings,        // two mappings of one image, or of two images without a platform selector, share an execution address
};

[[nodiscard]] constexpr std::string_view image_validation_error_name(ImageValidationError error) noexcept {
  switch (error) {
  case ImageValidationError::none: return "none";
  case ImageValidationError::id_not_dense: return "id_not_dense";
  case ImageValidationError::unknown_authority: return "unknown_authority";
  case ImageValidationError::unknown_verification: return "unknown_verification";
  case ImageValidationError::invalid_producer_name: return "invalid_producer_name";
  case ImageValidationError::no_bytes: return "no_bytes";
  case ImageValidationError::bytes_and_source: return "bytes_and_source";
  case ImageValidationError::unresolved_source: return "unresolved_source";
  case ImageValidationError::source_out_of_range: return "source_out_of_range";
  case ImageValidationError::no_mappings: return "no_mappings";
  case ImageValidationError::empty_mapping: return "empty_mapping";
  case ImageValidationError::mapping_outside_bytes: return "mapping_outside_bytes";
  case ImageValidationError::mapping_wraps: return "mapping_wraps";
  case ImageValidationError::overlapping_mappings: return "overlapping_mappings";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view image_authority_name(ImageAuthority authority) noexcept {
  switch (authority) {
  case ImageAuthority::immutable_input: return "immutable_input";
  case ImageAuthority::static_proof: return "static_proof";
  case ImageAuthority::bounded_build_time_materialization: return "bounded_build_time_materialization";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view image_verification_name(ImageVerification verification) noexcept {
  switch (verification) {
  case ImageVerification::none: return "none";
  case ImageVerification::byte_identity: return "byte_identity";
  case ImageVerification::structural: return "structural";
  }
  return "unknown";
}

struct ImageValidation {
  ImageValidationError error = ImageValidationError::none;
  std::uint32_t image_index = 0;  // the first offending image (0-based); meaningless when ok
  [[nodiscard]] bool ok() const noexcept { return error == ImageValidationError::none; }
};

namespace detail {
[[nodiscard]] inline bool valid_producer_name(std::string_view name) noexcept {
  if (name.empty()) return false;
  for (const char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
  return true;
}
struct ExecutionRange {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint32_t image_index = 0;
};
} // namespace detail

// The byte count of an image: its owned bytes, or the length of its source reference.
[[nodiscard]] inline std::size_t executable_image_size(const ExecutableImage &image) noexcept {
  return image.source ? static_cast<std::size_t>(image.source->length) : image.bytes.size();
}

// Fail-closed structural validation. A set that validates has dense ids, known tags, resolvable bytes and unambiguous execution ranges.
// An empty set is valid (a producer may produce nothing). CPU address-width limits are checked by the CPU consumer, not here.
[[nodiscard]] inline ImageValidation validate_executable_image_set(const ExecutableImageSet &set) {
  std::vector<detail::ExecutionRange> ranges;
  for (std::size_t index = 0; index < set.images.size(); ++index) {
    const ExecutableImage &image = set.images[index];
    const auto fail = [index](ImageValidationError error) { return ImageValidation{error, static_cast<std::uint32_t>(index)}; };
    if (image.id.value != index + 1U) return fail(ImageValidationError::id_not_dense);
    if (static_cast<std::uint8_t>(image.provenance.authority) >= kImageAuthorityCount) return fail(ImageValidationError::unknown_authority);
    if (static_cast<std::uint8_t>(image.verification) >= kImageVerificationCount) return fail(ImageValidationError::unknown_verification);
    if (!detail::valid_producer_name(image.provenance.producer)) return fail(ImageValidationError::invalid_producer_name);
    if (image.source) {
      if (!image.bytes.empty()) return fail(ImageValidationError::bytes_and_source);
      const ImageSourceReference &ref = *image.source;
      if (ref.source.value == 0U || ref.source.value > index || set.images[ref.source.value - 1U].source)
        return fail(ImageValidationError::unresolved_source);
      const std::size_t source_size = set.images[ref.source.value - 1U].bytes.size();
      if (ref.length == 0U || static_cast<std::uint64_t>(ref.offset) + ref.length > source_size)
        return fail(ImageValidationError::source_out_of_range);
    } else if (image.bytes.empty()) {
      return fail(ImageValidationError::no_bytes);
    }
    if (image.mappings.empty()) return fail(ImageValidationError::no_mappings);
    const std::size_t size = executable_image_size(image);
    const std::size_t first_range = ranges.size();
    for (const ImageMapping &mapping : image.mappings) {
      if (mapping.length == 0U) return fail(ImageValidationError::empty_mapping);
      if (static_cast<std::uint64_t>(mapping.first_offset) + mapping.length > size) return fail(ImageValidationError::mapping_outside_bytes);
      const std::uint64_t begin = static_cast<std::uint64_t>(mapping.execution_base) + mapping.first_offset;
      const std::uint64_t end = begin + mapping.length;
      if (end > (std::uint64_t{1} << 32)) return fail(ImageValidationError::mapping_wraps);
      for (std::size_t other = 0; other < ranges.size(); ++other) {
        const bool same_image = other >= first_range;
        if (!same_image && set.platform_selector) continue;
        if (begin < ranges[other].end && ranges[other].begin < end) return fail(ImageValidationError::overlapping_mappings);
      }
      ranges.push_back({begin, end, static_cast<std::uint32_t>(index)});
    }
  }
  return {};
}

// The bytes of a validated image (resolving a source reference). Precondition: the set validates.
[[nodiscard]] inline std::span<const std::uint8_t> executable_image_bytes(const ExecutableImageSet &set, const ExecutableImage &image) {
  if (!image.source) return image.bytes;
  const ExecutableImage &source = set.images[image.source->source.value - 1U];
  return std::span<const std::uint8_t>(source.bytes).subspan(image.source->offset, image.source->length);
}

// Sanitized provenance: image counts by authority and by producer name only (no address, byte or hash).
struct ImageProvenanceCounts {
  std::uint32_t images = 0;
  std::uint32_t by_authority[kImageAuthorityCount]{};
  std::map<std::string, std::uint32_t> by_producer;  // ordered: deterministic output
  friend bool operator==(const ImageProvenanceCounts &, const ImageProvenanceCounts &) = default;
};

[[nodiscard]] inline ImageProvenanceCounts count_image_provenance(const ExecutableImageSet &set) {
  ImageProvenanceCounts counts;
  for (const ExecutableImage &image : set.images) {
    ++counts.images;
    const auto authority = static_cast<std::uint8_t>(image.provenance.authority);
    if (authority < kImageAuthorityCount) ++counts.by_authority[authority];
    ++counts.by_producer[image.provenance.producer];
  }
  return counts;
}

// {"images":N,"authority":{"immutable_input":a,"static_proof":b,"bounded_build_time_materialization":c},"producers":{"name":n,...}}
// Producer names are validated identifiers, so no escaping is needed.
[[nodiscard]] inline std::string format_image_provenance_json(const ImageProvenanceCounts &counts) {
  std::string out = "{\"images\":" + std::to_string(counts.images) + ",\"authority\":{";
  for (std::uint8_t a = 0; a < kImageAuthorityCount; ++a) {
    if (a != 0U) out += ',';
    out += '"';
    out += image_authority_name(static_cast<ImageAuthority>(a));
    out += "\":" + std::to_string(counts.by_authority[a]);
  }
  out += "},\"producers\":{";
  bool first = true;
  for (const auto &[producer, count] : counts.by_producer) {
    if (!first) out += ',';
    first = false;
    out += '"' + producer + "\":" + std::to_string(count);
  }
  out += "}}";
  return out;
}

} // namespace segarecomp
