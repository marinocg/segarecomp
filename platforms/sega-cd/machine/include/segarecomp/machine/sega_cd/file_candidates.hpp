#pragma once

// SEG-100-T001/T004 (ADR 0221): file-first candidate enumeration over the disc's ISO9660 data track, and the bounded broad lowering census
// of a candidate's bytes.
//
// Enumeration reuses the one ISO9660 reader (libs/media); it never parses a filesystem itself, never uses a name, extension or path as
// executable authority, and reports a directory it could not fully read as a typed incompleteness instead of a short list. The census asks
// the CPU-owned decoder and lowering what they can compile at every even source offset (derive_dynamic_body); that is *compilability*, never
// permission to execute: whether bytes may run is decided only by a verified runtime mapping (dynamic_binding).

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/iso9660.hpp"
#include "segarecomp/machine/sega_cd/dynamic_body.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kFileCandidateMaxEntries = 4096U;  // directory entries visited (typed `too_many_entries` beyond)

enum class FileCandidateError : std::uint8_t { none, no_data_track, iso9660, too_many_entries };
[[nodiscard]] const char *file_candidate_error_name(FileCandidateError error) noexcept;

struct FileCandidate {
  std::string path;          // "/DIR/NAME;1" as recorded (identification only)
  Iso9660Extent extent;      // logical block of track 1 and byte size
};

struct FileCandidateList {
  std::vector<FileCandidate> files;  // breadth-first, directory order: deterministic
  std::uint32_t directories{};
  FileCandidateError error{FileCandidateError::none};
  Iso9660Error iso_error{Iso9660Error::none};
  [[nodiscard]] bool complete() const noexcept { return error == FileCandidateError::none; }
};

[[nodiscard]] FileCandidateList enumerate_file_candidates(const DiscImage &disc);

// The bytes of `extent` (track 1 user data), or an empty vector when a block cannot be read.
[[nodiscard]] std::vector<std::uint8_t> read_file_candidate(const DiscImage &disc, const Iso9660Extent &extent);

// Counts over the even source offsets of one byte range.
struct BroadCensus {
  std::uint64_t even_offsets{};
  std::uint64_t not_decoded{};
  std::uint64_t not_lifted{};
  std::uint64_t not_lowered{};
  std::uint64_t redecode{};
  std::uint64_t refused_token_structure{};
  std::uint64_t refused_literal_not_linear{};
  std::uint64_t refused_literal_suffix{};
  std::uint64_t refused_constant_context{};
  std::uint64_t compiled{};              // derive_dynamic_body status ok: a source-offset body exists
  std::uint64_t pc_dependent{};          // of those, bodies with at least one PC-derived literal
  std::uint64_t generated_text_bytes{};  // body + slots + timing + refresh text (before sharing)
};

// `on_body`, when set, sees every successfully derived body (the caller may dedupe by instruction bytes). `only`, when non-empty, is a
// per-even-offset mask (index offset / 2): offsets whose entry is 0 are not lowered at all.
using BroadBodyObserver = std::function<void(std::uint32_t offset, const DynamicBody &)>;
[[nodiscard]] BroadCensus broad_census(std::span<const std::uint8_t> bytes, const BroadBodyObserver &on_body = {},
                                       std::span<const std::uint8_t> only = {});

// The structural candidate policy (ADR 0221): which even offsets of a byte range are the start of a *closed decodable flow*.
//
// An offset is closure-valid when the CPU-owned decoder decodes an instruction there and every fixed control successor of it that stays
// inside the range (the fall-through of a non-terminating instruction, a PC-relative branch or call target) is itself closure-valid. A target
// that is absolute (does not move with the PC), a computed transfer, a return, and the continuation a call stacks are boundaries, not
// dependencies: they neither validate nor invalidate. A decode failure, a PC-relative target outside the range or on an odd address, an
// instruction that always raises an exception, and a fall-through off the end of the range invalidate every offset that depends on it.
// Computed as the greatest fixed point, so loops are valid. No threshold, density or name is involved: it is a property of the bytes under the
// CPU owner's decode and control rules, and it is *only* a build-time compile filter - never executable authority.
struct StructuralClosure {
  std::vector<std::uint8_t> valid;   // index offset / 2: 1 when closure-valid
  std::vector<std::uint8_t> decoded; // index offset / 2: 1 when the CPU-owned decoder accepts an instruction there (a superset of `valid`)
  // Why an offset is not valid (index offset / 2): 0 valid, otherwise the root defect of the flow that starts there (a defect reached through successors is inherited).
  enum Defect : std::uint8_t { none = 0, not_decoded, always_faults, not_understood, target_outside_file, target_odd, fall_off_file };
  std::vector<std::uint8_t> defect;
  std::uint64_t even_offsets{};
  std::uint64_t decodable{};
  std::uint64_t valid_offsets{};
};
// Evaluation options (measured in ADR 0221; the product policy uses the defaults):
//   leave_file_is_boundary (default false): a PC-relative target outside the file (a call into resident code at a fixed offset from the module's own base) is a boundary
//     rather than an invalidating defect;
//   faults_are_terminators (default false): an instruction the CPU owner always executes as an exception entry (ILLEGAL, line A/F, other reserved words) is a valid
//     instruction that ends its flow - like TRAP - instead of a defect.
// Both are properties of the bytes alone.
struct StructuralOptions {
  bool leave_file_is_boundary = false;
  bool faults_are_terminators = false;
};
[[nodiscard]] StructuralClosure structural_closure(std::span<const std::uint8_t> bytes, StructuralOptions options = {});

}  // namespace segarecomp::machine::sega_cd
