#pragma once

// SEG-111 (ADR 0255): build-time executable image reconstruction from verified guest execution.
//
// A build pass that stops at a RAM program counter no exact delivered-disc span explains hands over the bounded RAM write streams its runtime
// observed (platforms/sega-cd/runtime/scd_reloc.c) and the live window. Two producer classes converge on the existing image admission:
//
//   copy      (Class A) a verified writer copied bytes with memory-to-memory moves. Every write is paired, per instruction instance, with the read
//             of the same instruction; the contiguous source range lies wholly inside ONE admitted disc extent of the same processor and equals the
//             disc bytes at the mapped offset, which lie in the delivered blocks. The result is an ordinary exact `extent:` admission.
//   produced  (Class B) a verified writer computed bytes (an unpaired stream). The result is the stream's bytes, admitted as a bounded build-time
//             materialization (`produced:` admission). No disc offset is invented.
//
// Common ownership rules (both classes, all checked here, nothing guessed): exactly one clean stream owns the frontier PC; its writer is the
// frontier processor; the memory is supported (main Work RAM, sub PRG RAM); the stream is complete (not evicted, too large, clobbered by any later
// write, dropped by write protect, or fed by a device register) and equals the live window; the claimed range is even and at least kMinFragmentBytes.
// Equal bytes at a unique disc location are never evidence. The generated program's per-instruction byte guard remains the execution authority.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"
#include "segarecomp/machine/sega_cd/source_resolve.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kMinFragmentBytes = 4U;
inline constexpr std::uint32_t kMaxStreamBytes = 4096U;
inline constexpr std::uint32_t kMaxStreams = 8U;

struct WriteStream {
  std::uint32_t writer_cpu{};
  bool work_ram{};                  // main Work RAM (else sub PRG RAM)
  std::uint32_t destination{};      // RAM offset of the first byte
  std::uint32_t length{};           // bytes written; may exceed bytes.size() when too_large
  std::uint32_t pc{};               // the writer instruction
  bool paired{};                    // every write was the value its own instruction instance read
  std::uint32_t source{};           // paired: the first source byte in the writer's space (mirrors normalised)
  std::uint32_t source_class{};     // SCD_RELOC_CLASS_*
  bool source_gap{}, clobbered{}, dropped{}, too_large{}, volatile_input{};
  std::vector<std::uint8_t> bytes;
};

struct StreamEvidence {
  bool present{};                   // the capture carried a `streams` member
  std::uint32_t evicted{};
  std::vector<WriteStream> items;
};

// Strictly parses the version-1 `streams` member of a frontier capture; nullopt on any malformed or out-of-bound field.
[[nodiscard]] std::optional<StreamEvidence> parse_stream_evidence(std::string_view capture_json);

struct FrontierEvidence {
  Processor processor{};
  std::uint32_t pc{};
  std::uint32_t region_base{};
  std::vector<std::uint8_t> region;       // the live window (contains the program counter)
  std::vector<LedgerRange> delivered;
  bool delivered_overflow{};
};

// The frontier capture's identity and facts (kind "frontier": pass identity, processor, pc, the region's delivery ledger); the live window is
// supplied separately (region.bin). nullopt on anything that is not exactly the runtime's capture format.
struct FrontierCapture {
  std::string pass_identity;
  FrontierEvidence frontier;
};

struct VerifiedExtent {             // an admitted exact disc extent (`<cpu>:<offset>:<base>:<length>`)
  Processor processor{};
  std::uint32_t disc_offset{};
  std::uint32_t base{};
  std::uint32_t length{};
};

enum class FragmentError : std::uint8_t {
  none,
  bad_evidence,         // malformed, or the stream does not lie in the live window
  no_stream_evidence,   // the capture has no `streams` member
  unsupported_region,   // the frontier is not in main Work RAM or sub PRG RAM
  no_stream,            // no stream owns the program counter
  stream_evicted,       // none owns it, and the bounded table evicted streams
  stream_overwritten,   // every stream containing it was written over afterwards
  stream_dropped,       // a write of the stream was dropped by the machine
  stream_too_large,     // longer than kMaxStreamBytes
  stream_not_live,      // the stream's bytes are not the live bytes
  foreign_writer,       // written by the other processor
  volatile_input,       // a device register was read while it was produced
  too_short,            // the even-aligned claim is shorter than kMinFragmentBytes, or the program counter is odd / outside it
  not_a_copy,           // copy requested for a stream that is not made of paired moves
  source_discontiguous, // a paired stream fed from several source runs
  source_not_admitted,  // the source range is not wholly inside one admitted extent of the processor
  ambiguous_source,     // several admitted extents map the source range to different disc offsets
  source_modified,      // the disc bytes of the mapped source differ from what was copied
  ledger_overflow,
  no_delivery_evidence,
  source_not_delivered, // the mapped disc blocks were not all delivered
  first_track_not_data,
  disc_unreadable,
  not_computed,         // produced requested for a stream made of paired moves (it is a copy: use the copy path)
};
[[nodiscard]] const char *fragment_error_name(FragmentError error) noexcept;

struct Fragment {
  bool copy{};                      // Class A (an exact disc extent) or Class B (produced bytes)
  Processor processor{};
  std::uint32_t disc_offset{};      // copy only: the track-1 user-data offset
  std::uint32_t base{};             // execution address of the first byte
  std::uint32_t length{};
  std::vector<std::uint8_t> bytes;  // the claimed bytes (copy: equal to the disc bytes)
};

struct FragmentResult {
  std::optional<Fragment> fragment;
  FragmentError error{FragmentError::none};
};

[[nodiscard]] FragmentResult derive_relocation(const DiscImage &disc, const FrontierEvidence &frontier, const StreamEvidence &streams,
                                               const std::vector<VerifiedExtent> &extents);
[[nodiscard]] FragmentResult derive_produced_image(const FrontierEvidence &frontier, const StreamEvidence &streams);

[[nodiscard]] std::optional<FrontierCapture> parse_frontier_capture(std::string_view capture_json, std::vector<std::uint8_t> region);

// The bytes of a track-1 span through the validated disc reader (cooked 2048-byte user data); none on any failure.
[[nodiscard]] FragmentError read_track1_span(const DiscImage &disc, std::uint32_t offset, std::uint32_t length, std::vector<std::uint8_t> &bytes);

}  // namespace segarecomp::machine::sega_cd
