#pragma once

// SEG-112 (ADR 0256): generation-time recognition of executable-image producers.
//
// Over the admitted boot images (the disc's IP and SP, immutable inputs) this bounded flat-lattice data-flow walk, built on the CPU-owned
// decode / lift / effect / control-successor APIs, recognises the counted-copy idiom and the byte-transform idiom of MC68000 code:
//
//     loop: <one load from (Ax)+> [<byte ops on a data register with immediate operands>] <one store to (Ay)+>   DBF Dn,loop
//
// from the instruction semantics alone (no opcode signature, no register or address constant of any title). When the pointers and the
// count are constants on the walked path, the loop is summarised (output[i] = f(source[i]); pointers advanced; Dn.w = $FFFF) instead of
// being iterated, the destination is classified, and the produced bytes are evaluated from the immutable source. A candidate is PROVEN
// only if the destination is main Work RAM / sub PRG RAM, the whole source lies in the processor's boot image, source and destination
// do not alias, nothing in the walk provably writes the source or the produced bytes, and a control transfer with a constant target on
// the walked path lands in the produced range ("executable use"); its bytes are then walked too (bounded closure), so a produced trampoline
// that itself produces more is analysed before any code is generated.
//
// The result is a PROPOSAL checked again by the emitter (which recomputes this function) and, at run time, by the per-instruction byte
// guard. Unknown-pointer stores are not provable and are COUNTED, not assumed away: they are the documented assumption of the class.
// Executes no guest instruction; never runs a generated program.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "segarecomp/machine/sega_cd/boot_images.hpp"

namespace segarecomp::machine::sega_cd {

inline constexpr std::uint32_t kProducerMaxSteps = 100000U;     // instruction visits (the loader scan's step bound)
inline constexpr std::uint32_t kProducerMaxPcs = 16384U;        // distinct program counters with a state
inline constexpr std::uint32_t kProducerMaxBytes = 4096U;       // one produced fragment
inline constexpr std::uint32_t kProducerMaxFragments = 8U;
inline constexpr std::uint32_t kProducerMaxLoopBody = 6U;       // instructions between the loop head and its DBcc

enum class ProducerRefusal : std::uint8_t {
  loop_operand_unknown,     // source pointer, destination pointer or count was not a constant at the loop
  loop_body_unsupported,    // the body is not a load / byte ops / store idiom
  destination_unsupported,  // not main Work RAM / sub PRG RAM, or odd
  source_not_immutable,     // the source is not wholly inside the processor's boot image
  too_large,                // more than kProducerMaxBytes
  aliasing,                 // source and destination ranges overlap
  source_written,           // the walk provably writes the source range
  destination_overwritten,  // the walk provably writes the produced range
  not_executed,             // no constant control transfer lands in the produced range
  overlapping_fragments,    // two proofs claim the same bytes differently
  too_many_fragments,
  budget_exhausted,
};
[[nodiscard]] const char *producer_refusal_name(ProducerRefusal refusal) noexcept;

struct StaticFragment {
  Processor processor{};            // the processor that executes it (and owns the destination RAM)
  bool copy{};                      // an exact span of the boot image (else a transform of one)
  std::uint32_t base{};             // execution address
  std::uint32_t length{};
  std::uint32_t disc_offset{};      // copy: track-1 byte offset of the source span
  std::uint32_t entry{};            // the constant control-transfer target that proves executable use
  std::string transform;            // transform: canonical operation chain, e.g. "eor.b:90" (parameters derived from the code)
  std::vector<std::uint8_t> bytes;  // the produced bytes
};

struct ProducerScan {
  std::vector<StaticFragment> fragments;                  // proven, deterministic order (processor, base)
  std::map<ProducerRefusal, std::uint32_t> refusals;      // refused candidates by class
  std::uint32_t candidates{};                              // loop idioms recognised (proven + refused)
  std::uint32_t unknown_writes{};                          // stores through a pointer the walk does not know (the documented assumption)
  std::uint32_t steps{};
  bool complete{true};                                     // false when a bound was reached
};

// The main processor's boot image is the Sega CD BIOS's documented boot contract (the system area from byte $200 on is copied to main Work RAM $FF0000
// and entered there), not the descriptor of the synthetic post-BIOS profile; the sub processor's is the SP descriptor (the BIOS loads it as declared).
// Both are assumptions about the BIOS, recorded as such: the emitter recomputes them and the run-time byte guard checks every executed instruction.
inline constexpr std::uint32_t kMainBootDiscOffset = 0x200U;
inline constexpr std::uint32_t kMainBootBytes = 0x7E00U;   // to the end of the 32 KiB system area
[[nodiscard]] ProducerScan scan_static_producers(const std::vector<std::uint8_t> &system_area, const BootImages &images);

}  // namespace segarecomp::machine::sega_cd
