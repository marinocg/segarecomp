#pragma once

// SEG-100-T004 (ADR 0224): the build-time candidate policy of file-first broad AOT - which files of the disc, and which offsets of them, are compiled
// before launch as dynamic images (dynamic_images.hpp).
//
// Enumeration is the one ISO9660 reader (file_candidates.hpp); compilability is the CPU owner's decode and lowering; the structural filter is a property
// of the bytes under the CPU owner's decode and control rules. Nothing here consults a name, extension, path, address or title.

#include <cstdint>
#include <string>
#include <vector>

#include "segarecomp/machine/sega_cd/dynamic_images.hpp"
#include "segarecomp/machine/sega_cd/file_candidates.hpp"

namespace segarecomp::machine::sega_cd {

// The policies:
//
//   all         every even offset of every file small enough to be resident in one processor's executable RAM;
//   structural  only the offsets whose forward flow is closed and decodable (structural_closure).
// Neither policy grants executable authority: they bound the compile. Every file the policy does not compile in full is accounted for with a typed
// outcome; nothing is dropped silently, and no name, extension, path or address is consulted.
enum class FileFirstPolicy : std::uint8_t { all, structural };
[[nodiscard]] const char *file_first_policy_name(FileFirstPolicy policy) noexcept;

struct FileFirstLimits {
  std::uint32_t max_file_bytes{kDynamicMaxImageBytes};   // a larger file cannot be wholly resident in one processor's executable RAM
  std::uint64_t max_candidate_offsets{0};                // 0 = unbounded; otherwise files are admitted in the order below until the budget is full
  std::uint64_t max_total_bytes{UINT64_C(256) << 20U};   // bytes of candidate files held while selecting; further files are over_budget (typed), never silently skipped
  unsigned jobs{4};
};

struct FileFirstSelection {
  std::vector<DynamicImageSpec> images;   // admitted, in disc order
  std::string policy;                     // name with its limits: enters the program identity
  std::uint64_t files{};
  std::uint64_t file_bytes{};
  std::uint64_t oversize_files{};         // larger than max_file_bytes: not compiled (typed incomplete)
  std::uint64_t oversize_bytes{};
  std::uint64_t duplicate_files{};        // same bytes as an earlier file: one image, aliases counted
  std::uint64_t empty_files{};            // fewer than two bytes
  std::uint64_t unreadable_files{};
  std::uint64_t no_structure_files{};     // structural: no closure-valid offset
  std::uint64_t over_budget_files{};      // a file the candidate-offset budget could not hold (typed incomplete)
  std::uint64_t candidate_offsets{};      // of the admitted images
  std::uint64_t admitted_bytes{};
  bool complete{true};                    // false when any file was oversize, unreadable or over budget: the inventory is not an executable inventory
  FileCandidateError enumeration{FileCandidateError::none};
};

// Deterministic: the same disc and limits give the same selection. The budget (when set) admits files by descending structural density
// (closure-valid offsets per byte), ties by ascending logical block.
[[nodiscard]] FileFirstSelection select_file_first_images(const DiscImage &disc, FileFirstPolicy policy, const FileFirstLimits &limits);

}  // namespace segarecomp::machine::sega_cd
