#pragma once

// SEG-030-T006 (ADR 0079 decision 7, report-only): the named Genesis main-68000 interrupt-source premise of the analysis driver.
//
// The CPU analysis library is machine-agnostic: it takes the potential interrupt sources (installed interrupt vectors the
// generated-native machine model does not deliver) as configuration (`M68kFrameConfig::potential_interrupts`). This platform-owned
// premise selects them. On the Genesis board the main 68000's interrupt requests are levels 2 (external: VDP register 11 IE2 plus
// the I/O-port TH interrupt), 4 (H-int: VDP register 0 IE1) and 6 (V-int: VDP register 1 IE0), every acknowledge autovectored
// (vectors 26, 28, 30). Levels 1, 3, 5 and 7 are never asserted; the spurious vector (24) needs a bus error during the acknowledge
// cycle and the uninitialized vector (15) a vectored acknowledge, neither of which the board produces (the cartridge slot carries no
// IPL, /VPA or /BERR line). Mega-CD and 32X interrupts go to their own processors. ADR 0079 decision 7 records the public sources.
//
// The unconfigured premise is the conservative MC68000 default: every installed interrupt vector (the spurious vector and the
// autovectors of levels 1-7) is a potential source.

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"
#include "segarecomp/cpu/m68k/analysis/frames.hpp"

namespace segarecomp {

enum class GenesisInterruptPremise : std::uint8_t {
  genesis_board,  // the Genesis main 68000: autovectored levels 2, 4 and 6 only
  unconfigured,   // the conservative MC68000 default: every installed interrupt vector
};

// The interrupt request levels the Genesis board asserts on the main 68000 (every one autovectored).
inline constexpr std::array<unsigned, 3> genesis_main_cpu_interrupt_levels{2U, 4U, 6U};

[[nodiscard]] inline const char *genesis_interrupt_premise_name(GenesisInterruptPremise premise) noexcept {
  return premise == GenesisInterruptPremise::genesis_board ? "genesis_board" : "unconfigured";
}

// True when an installed handler at `vector` is a potential interrupt source under `premise`.
[[nodiscard]] inline bool genesis_interrupt_source(std::uint32_t vector, GenesisInterruptPremise premise) noexcept {
  if (m68k_vector_class(vector) != M68kVectorClass::interrupt) return false;
  if (premise == GenesisInterruptPremise::unconfigured) return true;
  const auto level = m68k_interrupt_level(vector);  // nullopt: the spurious, uninitialized or a user vector (never on the board)
  return level && std::find(genesis_main_cpu_interrupt_levels.begin(), genesis_main_cpu_interrupt_levels.end(), *level) !=
                      genesis_main_cpu_interrupt_levels.end();
}

// The potential sources among installed (vector, handler) pairs, in input order, handlers masked to the 24-bit bus.
[[nodiscard]] inline std::vector<M68kHandlerVector> genesis_potential_interrupt_sources(
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &installed, GenesisInterruptPremise premise) {
  std::vector<M68kHandlerVector> out;
  for (const auto &[vector, handler] : installed)
    if (genesis_interrupt_source(vector, premise)) out.push_back({vector, handler & UINT32_C(0xFFFFFF)});
  return out;
}

}  // namespace segarecomp
