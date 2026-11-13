#pragma once

// SEG-138-T003/T004 (ADR 0298): build-time classification of statically resolved M68K function-hook sites.
//
// A hook site is the entry address of a subroutine the build compiled. This module decides, from the lifted operations of the compiled program and nothing else (no ROM bytes, no runtime
// state), whether each declared site is one of the supported classes of mod-execution-phases.md section 5.2, and returns a stable rejection class (`E_HOOK_*`) with the first failing
// check otherwise. It reuses the single control-flow owner `m68k_control_successors`; it decodes nothing.
//
// Supported class: the entry of a subroutine that (1) is a compiled instruction start in immutable ROM, (2) is the target of at least one direct JSR/BSR in code reachable from the
// reset entry or an exception vector, (3) is reached by nothing else statically (no branch, no fallthrough, no vector), (4) is not reachable from any exception vector, and (5) whose
// body (the instructions reachable from the entry without entering a callee) returns only through RTS and contains no tail jump, no JMP, no exception/return-from-exception form, no
// code-address push and no MOVEA into A7.

#include <cstdint>
#include <string>
#include <vector>

#include "segarecomp/cpu/m68k/control_successors.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

enum class M68kHookMode : std::uint8_t { before, after, before_after, replace };

// Hook register view (the only state a callback can read or replace; mod-execution-phases.md 5.3). Bit n of a mask:
//  0..7 D0-D7, 8..15 A0-A7 (A7 readable only), 16 CCR (low 5 bits of SR), 17 deterministic M68K cycle counter (read only), 24..31 stack slots 0..7 at entry (long at A7+4+4k; read only).
inline constexpr std::uint32_t kM68kHookRegD0 = 0, kM68kHookRegA0 = 8, kM68kHookRegA7 = 15, kM68kHookRegCcr = 16, kM68kHookRegCycles = 17, kM68kHookRegStack0 = 24;
inline constexpr std::uint32_t kM68kHookReadableMask = 0xFF03FFFFU;
inline constexpr std::uint32_t kM68kHookWritableMask = 0x00017FFFU;  // D0-D7, A0-A6, CCR
inline constexpr std::uint32_t kM68kHookMaxSites = 64;
inline constexpr std::uint32_t kM68kHookMaxBodyInstructions = 4096;
inline constexpr std::uint32_t kM68kHookMaxReplaceCycles = 65535;

struct M68kHookSite {
  std::string id;                  // stable mod-chosen identifier (token)
  std::string cpu{"m68k"};         // the CPU the declaration targets; only "m68k" on Genesis is supported
  std::uint32_t entry{};           // resolved image address (from a verified symbol)
  M68kHookMode mode{M68kHookMode::before};
  std::uint32_t reads{};           // register-view bits the callback may read
  std::uint32_t writes{};          // register-view bits a replacement may write (replace only)
  bool replace_declared{false};    // a replacement contract is present
  bool flags_written{false};       // the contract says CCR is an output
  std::uint32_t cost{};            // guest cycles charged for a replaced function (replace only)
};

struct M68kHookCodeInsn {
  const M68kIrOperation *operation{};
  bool execution_alias{false};  // an identity that executes from work RAM
};

struct M68kHookRejection {
  std::string code;     // E_HOOK_*
  std::size_t site{};   // index into the declaration list
  std::string message;  // no bytes, no disassembly
};

struct M68kHookResolvedSite {
  std::vector<std::uint32_t> continuations;  // sorted: the return addresses of every static direct call to the entry
  std::uint32_t body_instructions{};         // instructions of the function body (diagnostics)
};

struct M68kHookResolution {
  bool ok{true};
  M68kHookRejection rejection{};
  std::vector<M68kHookResolvedSite> sites;  // parallel to the declaration list when ok
};

// `roots`: the reset entry. `vectors`: the exception/interrupt vector targets (the reset vector excluded).
[[nodiscard]] M68kHookResolution classify_m68k_function_hooks(const std::vector<M68kHookCodeInsn> &code, const std::vector<std::uint32_t> &roots,
                                                              const std::vector<std::uint32_t> &vectors, const std::vector<M68kHookSite> &sites);

[[nodiscard]] bool m68k_hook_id_ok(const std::string &id);

}  // namespace segarecomp
