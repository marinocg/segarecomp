#pragma once

// SEG-100-T002/T004 (ADR 0221): build-time emission of file-first dynamic images - immutable disc files compiled to native instruction bodies
// before launch, with no guest base.
//
// Separation (the point of the milestone):
//   * the image is an immutable input: a track-1 extent whose digest is verified against the disc when the program is built;
//   * the body is a function of the instruction's bytes alone (processor-neutral, dynamic_body.hpp); one body serves every source offset, file and
//     processor whose bytes equal it, and it verifies those bytes at the actual guest PC before it runs;
//   * the table `map` says which body serves which source offset of which image - nothing else about the image is compiled in;
//   * the guest mapping (which RAM the original game loaded the image into) is machine state decided at run time (platforms/sega-cd/runtime/
//     scd_dynamic.c) and appears nowhere in the emitted code.
// The generated program decodes nothing and compiles nothing at run time; the CPU owner's decode and lowering ran here.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/dynamic_body.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kDynamicMaxImageBytes = 0x80000U;   // PRG RAM: the largest executable region of a processor
inline constexpr std::uint32_t kDynamicBodySliceSize = 1024U;      // bodies per translation unit
inline constexpr std::uint32_t kDynamicMaxImages = 4096U;
inline constexpr std::uint64_t kDynamicMaxCandidateOffsets = UINT64_C(32) << 20U;   // the emission holds 12 bytes per candidate offset; more is the typed too_many_candidates

struct DynamicImageSpec {
  std::uint32_t track_offset{};          // byte offset in the logical user data of track 1; a multiple of 2048
  std::vector<std::uint8_t> bytes;       // the extent as read from the disc (<= kDynamicMaxImageBytes)
  std::vector<std::uint8_t> candidate;   // per even offset (index offset / 2): 1 = compile; empty = every even offset
};

enum class DynamicError : std::uint8_t {
  none,
  too_many_images,
  image_invalid,        // empty, larger than kDynamicMaxImageBytes, unaligned, or outside the data track
  digest_mismatch,      // the disc bytes at the extent differ from the supplied bytes
  duplicate_content,    // two images with the same bytes (indistinguishable candidates)
  candidate_size,       // a candidate mask whose size is not length / 2
  too_many_candidates,  // more than kDynamicMaxCandidateOffsets requested: use the structural policy or an offset budget
};
[[nodiscard]] const char *dynamic_error_name(DynamicError error) noexcept;

struct DynamicImageStats {
  std::uint32_t id{};
  std::uint32_t track_offset{};
  std::uint32_t length{};
  std::string sha256;
  std::uint64_t candidate_offsets{};     // even offsets requested
  std::uint64_t compiled_offsets{};      // of them, offsets with a body
  std::uint64_t not_decoded{};
  std::uint64_t refused{};               // decoded but no source-offset body could be derived (typed in DynamicEmission::refused)
};

struct DynamicUnit {
  std::string name;       // d_b<NNNNN>.c (bodies) or d_m<NNNN>.c (one image's map)
  std::string c_source;
  std::uint32_t functions{};
};

struct DynamicEmission {
  std::vector<DynamicUnit> units;
  std::string program_text;              // declarations and the ScdDynamicProgram initializer for the program unit
  std::string identity;                  // SHA-256 over the image set and the candidate masks; enters the program identity
  std::vector<DynamicImageStats> images;
  std::uint64_t candidate_offsets{};
  std::uint64_t compiled_offsets{};
  std::uint64_t unique_encodings{};      // distinct instruction byte strings requested
  std::uint64_t bodies{};                // of them, with a derived body (functions emitted)
  std::uint64_t refused{};               // distinct encodings with no source-offset body
  std::vector<std::pair<std::string, std::uint64_t>> refused_reasons;   // by class (distinct encodings), sorted
  std::uint64_t body_text_bytes{};
  std::uint64_t map_text_bytes{};
  std::uint32_t body_units{};
  DynamicError error{DynamicError::none};
  std::uint32_t error_index{};
};

// The identity of an image set and its candidate masks (what enters the program identity); computed without lowering anything.
[[nodiscard]] std::string dynamic_images_identity(const std::vector<DynamicImageSpec> &images, std::string_view policy);

// `verify_only_guard` mirrors the BIOS-image profile's application-entry mode (the bodies verify and return before the effect).
[[nodiscard]] DynamicEmission emit_dynamic_images(const DiscImage &disc, const std::vector<DynamicImageSpec> &images, std::string_view policy,
                                                  bool verify_only_guard, unsigned jobs);

}  // namespace segarecomp::machine::sega_cd
