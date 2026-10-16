#pragma once

// SEG-068-T008 (ADR 0135): research spike. D0 = static Z80 code-write discovery over a KNOWN immutable 8 KiB image.
// It executes no guest instruction. It only combines the CPU-owned decoder (decode_at / classify_all), the CPU-owned effect
// projection (project_effect) and a small bounded finite-value propagation. Unknown or unsupported never becomes a guessed value.
// Not a product facility: it lives under tests/tools and is linked by no compiler, runtime or application target.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace segarecomp::experiments::z80_mutation {

inline constexpr std::size_t kRamBytes = 8192;     // the Genesis Z80 sound RAM (mirrored twice in the 16 KiB code window)
inline constexpr std::uint32_t kCodeWindow = 0x4000;

struct Limits {  // pre-registered in ADR 0135; exceeding any one yields UNKNOWN / incomplete
  unsigned max_values = 16;
  unsigned max_starts = 16384;
  unsigned max_revisits = 4;
  unsigned max_steps = 262144;
};

struct Root {
  std::uint16_t pc{};
  bool reset_state{};  // true: architectural reset register state; false: every register unknown
};

enum class Certainty : std::uint8_t { exact, finite, unknown };
const char* certainty_name(Certainty c);

struct StoreSite {
  std::uint16_t pc{};          // the storing instruction (hardware pushes: 0)
  std::uint8_t width{1};
  bool hardware_push{};
  Certainty address{Certainty::unknown};
  Certainty value{Certainty::unknown};
  std::vector<std::uint16_t> addresses;  // sorted; empty when unknown
  std::vector<std::uint16_t> values;     // sorted; empty when unknown
  bool hits_reachable_code{};            // some possible destination byte lies in a reachable decoded instruction
};

enum class ChangeKind : std::uint8_t { payload_only, form_changed, length_changed, becomes_undecodable, becomes_decodable };
const char* change_kind_name(ChangeKind k);

struct StartChange {
  std::uint16_t start{};
  ChangeKind kind{};
  std::string original;   // form name or "undecodable"
  std::string modified;
  unsigned original_length{};
  unsigned modified_length{};
  bool reachable_start{};  // the start is in the reachable set of the unmodified image
};

// One (destination, written value) pair of a store site whose destination lies in reachable code, with the decoded effect of
// applying exactly that write to the image through the existing generation-time decoder.
struct Candidate {
  std::uint16_t site_pc{};
  std::uint16_t destination{};
  std::uint16_t value{};
  std::uint8_t width{1};
  std::vector<StartChange> changes;  // starts (logical < $4000) whose decoded classification differs (first 64 by address)
  unsigned changed_starts{};         // total number of such starts
  unsigned structural_changed_starts{};  // of those, not payload_only
  unsigned reachable_structural_changed_starts{};
  unsigned reachable_changed_starts{};  // any change (payload included) to a start of the reachable set
  bool structural() const;           // some change is not payload_only
};

struct Report {
  // reachability
  unsigned reachable_starts{};
  unsigned steps{};
  bool reachability_complete{true};
  bool effects_complete{true};
  bool store_addresses_complete{true};
  bool store_values_complete{true};  // every store that may hit reachable code has a finite value set
  bool budget_exceeded{};
  bool candidate_set_complete() const { return reachability_complete && effects_complete && store_addresses_complete && store_values_complete && !budget_exceeded; }
  // First-order scope: the set covers writes made by code reachable in the UNMODIFIED image. A write that changes a reachable start can
  // expose new code with further stores that this analysis never saw, so closure under mutation also requires no such candidate.
  bool closed_under_mutation() const {
    if (!candidate_set_complete()) return false;
    for (const Candidate& c : candidates)
      if (c.reachable_changed_starts != 0) return false;  // an operand change can retarget control flow or alter later values/addresses
    return true;
  }
  std::map<std::string, unsigned> incomplete_reasons;  // reason -> number of reachable instructions / events
  // stores
  std::vector<StoreSite> stores;
  std::vector<Candidate> candidates;
  unsigned unknown_address_stores{};
  unsigned unknown_value_stores{};
};

// `ram`: exactly kRamBytes. `hardware_roots()` is the reset vector only; the INT vector $0038 is added when an EI is reachable.
Report analyze(const std::vector<std::uint8_t>& ram, const std::vector<Root>& roots, const Limits& limits = {});
std::vector<Root> hardware_roots();  // reset only: NMI is not connected on the Genesis (contract section 8); the INT vector is added when an EI is reachable

// Deterministic aggregate text (no address, byte or snippet unless `detail`).
std::string format_report(const Report& report, bool detail);

}  // namespace segarecomp::experiments::z80_mutation
