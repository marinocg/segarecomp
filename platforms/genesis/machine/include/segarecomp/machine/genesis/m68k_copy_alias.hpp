#pragma once

// Consumer-route ADR 0049 alias preparation (SEG-028-T005; ADR 0077 "Consumer-route ADR 0049 alias producer", ADR 0076 bounded
// build-time execution rules).
//
// `segarecomp build` runs the bounded headless materialization pass of the program it builds. When that run ends with an M68K
// guest stop, the pass writes a private stop record (stop PC, stop class, the 64 KiB work RAM). If the stop is a fail-closed
// work-RAM execution target whose bytes are a verbatim copy of the immutable cartridge image, the record yields one ADR 0049
// immutable-copy alias PROPOSAL (execution base, source base, length). The emitter re-validates every proposal against the image
// and every alias body keeps its per-instruction runtime byte-identity guard: the descriptor is static_proof, never trusted
// runtime state. Nothing here decodes an instruction or knows a title; it compares bytes only.
//
// This is the C++ form of tools/genesis_startup_bridge.py `derive_copy_alias` / `merge_copy_aliases` and the termination
// vocabulary of `discover_copy_aliases` (parity: tests/genesis_m68k_copy_alias_parity_test.py).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace segarecomp::machine::genesis::m68k_alias {

inline constexpr std::uint32_t kWorkRamBegin = 0xFF0000;
inline constexpr std::uint32_t kWorkRamBytes = 0x10000;
inline constexpr std::uint32_t kMinRun = 16;        // shortest verbatim run accepted as a copy
inline constexpr std::size_t kMaxRounds = 64;       // preparation rounds (re-emit + fixed point) per build
inline constexpr std::uint32_t kStopRecordBytes = 8 + kWorkRamBytes;  // 4-byte BE pc, 4-byte BE stop class, work RAM
// Genesis runtime stop classes a copy alias can explain (runtime.h GenesisStopClass).
inline constexpr std::uint32_t kStopKnownButUnemittedTarget = 5;
inline constexpr std::uint32_t kStopInternalDispatchInconsistency = 7;

struct CopyAlias {
  std::uint32_t execution = 0;  // work-RAM execution base
  std::uint32_t source = 0;     // cartridge source base
  std::uint32_t length = 0;     // bytes (even)
  friend bool operator==(const CopyAlias&, const CopyAlias&) = default;
  friend auto operator<=>(const CopyAlias&, const CopyAlias&) = default;
};

// The private record of one M68K guest stop of the headless pass (never persisted beyond the build's work directory).
struct GuestStopRecord {
  std::uint32_t pc = 0;
  std::uint32_t stop_class = 0;
  std::vector<std::uint8_t> work_ram;  // exactly kWorkRamBytes
  friend bool operator==(const GuestStopRecord&, const GuestStopRecord&) = default;
};

// Parses the stop-record file layout. nullopt: wrong size.
[[nodiscard]] std::optional<GuestStopRecord> parse_stop_record(std::span<const std::uint8_t> bytes);

// The maximal verbatim run of `rom` around work-RAM `pc` (even ROM occurrence; constant delta; extended forward bytewise and
// backward by words; the lowest even occurrence wins ties). nullopt: odd / non-work-RAM pc or no run of at least kMinRun bytes.
[[nodiscard]] std::optional<CopyAlias> derive_copy_alias(std::span<const std::uint8_t> rom, std::span<const std::uint8_t> work_ram,
                                                         std::uint32_t pc);

// Sorted union of overlapping/adjacent aliases that share one execution-to-source delta.
[[nodiscard]] std::vector<CopyAlias> merge_copy_aliases(std::vector<CopyAlias> aliases);

enum class Termination : std::uint8_t {
  route_advanced,              // the pass ended without an M68K guest stop
  non_alias_frontier,          // a guest stop of a class no copy alias can explain
  no_work_ram_frontier,        // the stop PC is not an even work-RAM address
  frontier_not_verbatim_copy,  // work-RAM stop without a verbatim ROM run of at least kMinRun bytes
  repeated_alias_no_progress,  // the proposal is already covered by the current aliases
  max_rounds,                  // still discovering after kMaxRounds rounds: preparation INCOMPLETE
  tool_failure,                // emit/compile/link/run or stop-record failure: preparation INCOMPLETE
};

[[nodiscard]] const char* termination_name(Termination termination) noexcept;
[[nodiscard]] constexpr bool incomplete(Termination termination) noexcept {
  return termination == Termination::max_rounds || termination == Termination::tool_failure;
}

// The outcome of one fixed-point run as the preparation sees it.
struct RoundObservation {
  bool tool_failure = false;               // the round could not be prepared or run
  bool guest_stop = false;                 // the run ended with an M68K guest stop
  std::optional<GuestStopRecord> record;   // present for a guest stop (absent: tool_failure)
};

// One preparation decision: either a termination, or the next (merged, sorted) alias set to build.
struct Step {
  std::optional<Termination> termination;
  std::vector<CopyAlias> aliases;
};
[[nodiscard]] Step next_step(std::span<const std::uint8_t> rom, const RoundObservation& observed, const std::vector<CopyAlias>& aliases);

class RoundRunner {
 public:
  virtual ~RoundRunner() = default;
  // Builds and runs the program with `aliases` (re-emit, recompile changed units, relink, fixed point). `abort` set: the round
  // failed with a typed failure the caller owns (the loop stops without a termination of its own).
  virtual RoundObservation run_round(const std::vector<CopyAlias>& aliases, bool& abort) = 0;
};

struct Summary {
  Termination termination = Termination::route_advanced;
  bool aborted = false;              // a round failed with the runner's own typed failure
  std::size_t rounds = 0;            // extra rounds (re-emit + fixed point) after the initial one
  std::vector<CopyAlias> aliases;    // the alias set of the last built program
  [[nodiscard]] std::uint64_t alias_bytes() const noexcept;
};

// The bounded loop. `initial` is the observation of the already-built alias-less program.
[[nodiscard]] Summary prepare(std::span<const std::uint8_t> rom, const RoundObservation& initial, RoundRunner& runner,
                              std::size_t max_rounds = kMaxRounds);

}  // namespace segarecomp::machine::genesis::m68k_alias
