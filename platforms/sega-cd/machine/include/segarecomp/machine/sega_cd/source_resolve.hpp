#pragma once

// SEG-078-T003 (ADR 0205): where did the code in this RAM window come from? A build pass that stops at a RAM program counter with no compiled
// variant hands over (a) the live bytes of the processor's executable RAM around it and (b) the machine's own delivery ledger - the disc blocks
// its CDC decoder read. This resolver finds the disc span those bytes are an exact copy of, or refuses with a typed class. Build time only; the
// result is nothing but a `--disc-image` extent: the emitter re-reads and re-checks it, and the generated program still compares every instruction
// with the live bytes before it runs (ADR 0142 decision 5), so a wrong extent can only stop the program, never change what it computes.
//
// The rule (the whole mechanism):
//   * the evidence must be complete: a ledger that overflowed is refused;
//   * only DELIVERED data of track 1 is searched (the machine read it; an undelivered span is never a source);
//   * an anchor of 32 live bytes at the program counter is looked up in the delivered data; every occurrence is extended in both directions
//     for as long as the live window equals the delivered data (never across an undelivered block);
//   * a candidate needs a run of at least kMinRun bytes; the longest run wins, and a tie between distinct disc offsets is `ambiguous_source`
//     (identical prefixes of sibling modules never decide by luck: the live window must be explained further by one source than by the others);
//   * the extent is the run, trimmed to an even base and length.
// Not evidence, and never used: a RAM snapshot alone, a byte pattern outside the delivered blocks, any decompression or transformation (a window
// that is not an exact copy of a disc span is `no_source`).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/disc.hpp"
#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kResolveAnchorBytes = 32U;
inline constexpr std::uint32_t kResolveMinRun = 64U;
inline constexpr std::uint64_t kResolveMaxDeliveredBytes = UINT64_C(256) << 20U;  // bound on the data searched
inline constexpr std::uint32_t kResolveMaxCandidates = 64U;                         // more occurrences of the anchor than this is ambiguity

struct LedgerRange {
  std::uint32_t first_lba{};
  std::uint32_t count{};
};

struct SourceEvidence {
  Processor processor{};
  std::uint32_t pc{};
  std::uint32_t region_base{};
  std::vector<std::uint8_t> region;      // the live bytes of the window, from region_base
  std::vector<LedgerRange> delivered;    // disc blocks the CDC decoder read (absolute LBAs)
  bool delivered_overflow{};
};

enum class ResolveError : std::uint8_t {
  none,
  bad_evidence,        // the window does not contain the program counter, or is too small for an anchor
  ledger_overflow,     // the delivery ledger is incomplete
  no_delivery_evidence,// nothing of track 1 was delivered
  first_track_not_data,
  delivered_too_large, // more delivered data than the bound
  disc_unreadable,
  no_source,           // no delivered span explains the window
  too_short,           // the best explanation is shorter than kMinRun
  ambiguous_source,    // several distinct disc offsets explain it equally (or too many candidates)
};
[[nodiscard]] const char *resolve_error_name(ResolveError error) noexcept;

struct ResolvedSource {
  std::uint32_t disc_offset{};  // track-1 user-data byte offset (the `--disc-image` offset)
  std::uint32_t base{};         // execution address of the first byte
  std::uint32_t length{};
  std::uint32_t candidates{};   // occurrences of the anchor examined
};

struct ResolveResult {
  std::optional<ResolvedSource> source;
  ResolveError error{ResolveError::none};
};

// The delivered ranges clipped to track 1, sorted and merged into contiguous runs (shared by every consumer of the ledger).
[[nodiscard]] std::vector<LedgerRange> merge_delivered(const DiscTrack &track, const std::vector<LedgerRange> &delivered);

[[nodiscard]] ResolveResult resolve_source(const DiscImage &disc, const SourceEvidence &evidence);

}  // namespace segarecomp::machine::sega_cd
