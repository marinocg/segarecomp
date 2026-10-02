#pragma once

// SEG-029-T004 (ADR 0078, abstract-analysis-core-contract.md section 5): the bounded, synthetic Z80 second-CPU adapter.
//
// It instantiates the generic solver (segarecomp/analysis/solver.hpp) unchanged. It is architectural validation of the CPU seam,
// report-only, and is linked by tests only; production Z80 stays broad AOT (boundary test).
//
// Program point: the 16-bit logical execution address (as uint64). One image view supplies both code and immutable data.
//
// State: `ValueVector<13>`, one location per architectural register A B C D E H L BC DE HL SP IX IY (index = `Reg`). A 16-bit pair
// and its two 8-bit halves are all tracked, each an independent sound over-approximation, kept consistent by every write: writing a
// pair sets both halves to its exact projections; writing a half sets the half and recomputes the pair, pointwise (exact, keeping
// each pair value's other half) when the old pair is precise and the written value is a constant or derived from the same pair,
// otherwise as the product with the old pair (or, if that is not precise, with the other half's location; Unknown(set_bound) past
// the set bound). The pair location keeps a 16-bit value's identity through joins (joining HL=0x1234 with HL=0x5678 is
// {0x1234,0x5678}, not the product of the halves), while the half locations keep `LD H,n` / `LD L,m` precise from an unknown
// entry. F, the alternate set, I and R are not tracked.
//
// Memory: a load whose every possible address (and byte of a 16-bit load) lies inside the view's immutable range yields the exact
// byte set; anything else is Unknown(non_immutable_read). Abstract memory is a staged capability and is not admitted: a store never
// creates a precise value, and since every load outside the immutable range is Unknown, no store can make a read stale.
// Premise: the caller claims the immutable range is never written. The adapter checks the claim where it is provable: a store
// whose precise address set touches the range contradicts the premise and is treated as an unsupported transfer (no successor
// edges; the site is reported with Unknown(unsupported_transfer)). A store with a non-precise address is accepted under the claim.
//
// Unsupported forms (`project_effect(...).supported == false`) and undecodable points: no successor edge is invented; the site is
// reported in `Solution::unresolved_computed` with Unknown(unsupported_transfer) (Unknown(non_immutable_read) when the code bytes
// are not immutable image bytes). Successors are therefore relative to the discovered edge set (solver.hpp premise).
// A register a supported form writes with an opaque value becomes Unknown(unsupported_transfer).
//
// Calls: the call edge carries the post-call state to the callee. No call summary exists (staged), so the return continuation is a
// `return_edge` whose state is entirely Unknown(unsupported_transfer). RET has no static successor.
// Computed JP (HL)/(IX)/(IY): one `computed` edge per value of a precise register set; otherwise the site is unresolved with the
// register's Unknown reason.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "segarecomp/analysis/finite_value.hpp"
#include "segarecomp/analysis/solver.hpp"
#include "segarecomp/cpu/z80/decode.hpp"
#include "segarecomp/cpu/z80/effects.hpp"

namespace segarecomp::cpu::z80::analysis {

// Location i is the register `static_cast<Reg>(i)`: A B C D E H L BC DE HL SP IX IY.
inline constexpr std::size_t kLocationCount = 13;

using State = segarecomp::analysis::ValueVector<kLocationCount>;

// A project-authored byte image mapped at `base` (base + size <= 65,536). Every byte is code-fetchable and immutable.
class ImageView final : public LogicalFetch {
 public:
  ImageView(std::uint16_t base, std::span<const std::uint8_t> bytes, std::uint32_t image_id = 1);
  FetchedByte fetch(std::uint16_t address) const override;
  bool contains(std::uint16_t address) const noexcept;
  std::uint8_t byte(std::uint16_t address) const noexcept;  // valid iff contains(address)

 private:
  std::uint16_t base_;
  std::span<const std::uint8_t> bytes_;
  std::uint32_t image_id_;
};

class Adapter {
 public:
  using State = analysis::State;

  explicit Adapter(const ImageView& image, std::size_t set_bound = segarecomp::analysis::default_set_bound)
      : image_(image), set_bound_(set_bound) {}

  segarecomp::analysis::TransferResult<State> transfer(std::uint64_t point, const State& in) const;

  // A reachable state in which every location is Unknown(unknown_input).
  static State entry_state();
  // The value of any architectural register (an 8-bit half is projected from its pair).
  static segarecomp::analysis::FiniteValue read(const State& state, Reg reg);
  // Overwrite a register (an 8-bit half combines with the pair's other half). Used to seed entry states in tests.
  State with(const State& state, Reg reg, const segarecomp::analysis::FiniteValue& value) const;

 private:
  const ImageView& image_;
  std::size_t set_bound_;
};

// Deterministic text of a solution: completeness, per-point location values, computed targets and unresolved sites.
std::string describe(const segarecomp::analysis::Solution<State>& solution);

}  // namespace segarecomp::cpu::z80::analysis
