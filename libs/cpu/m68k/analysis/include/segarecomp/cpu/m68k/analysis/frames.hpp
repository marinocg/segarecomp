#pragma once

// SEG-030-T006 (ADR 0079 decisions 5, 7 and 10; report-only): the CPU-owned MC68000 interrupt/exception status semantics of the
// `frames` domain.
//
// The status is the part of SR the frames domain needs: the supervisor bit S (SR bit 13) and the interrupt mask I2-I0 (SR bits
// 8-10), encoded as `(S << 3) | I` (0..15) and carried as a generic finite set (Unknown: the SR is not tracked). Every rule below is
// a public MC68000 fact (M68000 Programmer's Reference Manual instruction entries and the MC68000 User's Manual exception-processing
// chapter), as already recorded for the generated runtime by ADR 0043 (vector table, privileged set P1, interrupt acceptance):
//
// - reset: S = 1, T = 0, I = 7; SSP <- the long at vector 0, PC <- the long at vector 1;
// - MOVE to SR, ANDI/ORI/EORI #imm,SR, STOP #imm (privileged) replace the whole SR; MOVE to CCR and ANDI/ORI/EORI to CCR do not touch
//   the system byte; RTE restores SR from the frame; RTR restores only the CCR;
// - interrupt acceptance: a request of level L is taken at an instruction boundary when L > I, and level 7 is
//   non-maskable (taken whatever the mask; ADR 0043 records its transition rule); on acceptance S <- 1, T <- 0, I <- L;
// - every other exception (TRAP #n, TRAPV, CHK, divide by zero, illegal/line 1010/line 1111, privilege violation) sets S <- 1 and
//   T <- 0 and leaves I unchanged (ADR 0043 section 3);
// - the MC68000 group 1 and group 2 exception frame is 6 bytes: SR (word) at the new SSP and the PC (long) above it (ADR 0043);
//   group 0 (bus/address error, 14 bytes) is never delivered by the machine (ADR 0043 section 4);
// - in supervisor mode A7 is the SSP, so a frame is pushed below the current A7 only when S = 1 is proven; in user mode the frame
//   goes to the (untracked) SSP.
//
// Report-only: no production target links this library.

#include <cstdint>
#include <optional>
#include <vector>

#include "segarecomp/analysis/finite_value.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp {

// The MC68000 group 1/2 exception frame size (SR word + PC long).
inline constexpr std::uint32_t m68k_exception_frame_bytes = 6U;
// The reset status: S = 1, I = 7.
inline constexpr std::uint64_t m68k_reset_status = 0xFU;

[[nodiscard]] constexpr std::uint64_t m68k_status_of_sr(std::uint32_t sr) noexcept {
  return (static_cast<std::uint64_t>((sr >> 13U) & 1U) << 3U) | ((sr >> 8U) & 7U);
}
[[nodiscard]] constexpr bool m68k_status_supervisor(std::uint64_t status) noexcept { return (status & 8U) != 0U; }
[[nodiscard]] constexpr unsigned m68k_status_mask(std::uint64_t status) noexcept { return static_cast<unsigned>(status & 7U); }

// Every value of a precise (non-empty) status has S = 1.
[[nodiscard]] bool m68k_status_supervisor_proven(const analysis::FiniteValue &status) noexcept;
// Some execution may be in user mode (an Unknown status, or a value with S = 0).
[[nodiscard]] bool m68k_status_may_be_user(const analysis::FiniteValue &status) noexcept;

// True for the kinds that replace the system byte of SR on their normal successor (MOVE to SR, ANDI/ORI/EORI to SR, STOP).
[[nodiscard]] bool m68k_status_register_writer(M68kIrKind kind) noexcept;
// True for the privileged kinds (M68000PRM; ADR 0043 P1): a privilege violation (vector 8) when executed in user mode.
[[nodiscard]] bool m68k_privileged_kind(M68kIrKind kind) noexcept;

// The status after an SR writer. `source` is the 16-bit source of MOVE to SR (nullopt: Unknown); ANDI/ORI/EORI and STOP use their
// immediate. Any other kind returns `in` unchanged.
[[nodiscard]] analysis::FiniteValue m68k_status_after(const M68kIrOperation &operation, const analysis::FiniteValue &in,
                                                     const std::optional<std::vector<std::uint32_t>> &source);

// ADR 0043: how a machine-delivered vector is entered.
enum class M68kVectorClass : std::uint8_t {
  interrupt,             // an autovector (25-31), the spurious (24) or uninitialized (15) vector, or a user vector (64-255)
  synchronous_resuming,  // divide by zero (5), CHK (6), TRAPV (7): the handler's RTE resumes at the analysed fallthrough
  synchronous,           // every other synchronous exception: no analysed resumption (TRAP, illegal, line A/F, privilege, ...)
};
[[nodiscard]] M68kVectorClass m68k_vector_class(std::uint32_t vector) noexcept;
// SEG-030-T009 correction cycle 2: the PC a synchronous exception stacks (MC68000 User's Manual, exception processing): the next
// instruction for TRAP #n, TRAPV, CHK and divide by zero (true); the first word of the faulting instruction itself for illegal,
// line 1010, line 1111 and privilege violation (false). A handler's RTE through an unmodified frame resumes there.
[[nodiscard]] bool m68k_exception_stacks_next(std::uint32_t vector) noexcept;
// The interrupt level of an autovector (25-31 -> 1-7); nullopt for any other vector (an interrupt of unknown level).
[[nodiscard]] std::optional<unsigned> m68k_interrupt_level(std::uint32_t vector) noexcept;

// True when an interrupt of `level` (nullopt: unknown level, as for a spurious or user vector) can be accepted at a boundary whose
// status is `status`: Unknown status, level 7 or an unknown level, or some mask below the level. Bottom: never.
[[nodiscard]] bool m68k_interrupt_eligible(const analysis::FiniteValue &status, std::optional<unsigned> level) noexcept;

// The synchronous vectors an instruction may raise (nullptr: an undecodable reached point, which may be any decode-time exception:
// illegal, privilege, line 1010, line 1111). The privilege violation is included only when `status` may be user mode.
[[nodiscard]] std::vector<std::uint32_t> m68k_raised_vectors(const M68kIrOperation *operation, const analysis::FiniteValue &status);

// The status at the entry of the handler of `vector` taken from a boundary whose status is `from`: S = 1, I = the accepted level
// for an interrupt (every level 1-7 for an unknown one), I unchanged for any other exception (every mask when `from` is Unknown).
[[nodiscard]] analysis::FiniteValue m68k_handler_entry_status(std::uint32_t vector, const analysis::FiniteValue &from);

}  // namespace segarecomp
