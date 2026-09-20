#pragma once

// SEG-002/SEG-003 compatibility surface (temporary, not a permanent
// architecture module). Per docs/architecture/
// seg-014-t001-symbol-migration-map.md's "Temporary SEG-002/SEG-003
// compatibility surfaces" section: `TargetAddressSpace`, `M68kProgramAddress`,
// `MoveqImageOffset`, `ByteLength`, `CpuVariant`, `DecodeSource`,
// `InstructionProvenance` now live in core/ (segarecomp/core/address.hpp,
// segarecomp/core/provenance.hpp); `DataRegister` and `DecodeOutcome` now
// live in cpu/m68k/ (segarecomp/cpu/m68k/instruction.hpp,
// segarecomp/cpu/m68k/decode.hpp). This header no longer redefines any of
// them -- it reuses the moved definitions so it has no semantic ownership of
// its own, only the MOVEQ-named wrapper types/functions SEG-014-T006 is
// scoped to eventually delete once legacy CLI/report compatibility is routed
// through the shared architecture instead.

#include "segarecomp/core/address.hpp"
#include "segarecomp/core/provenance.hpp"
#include "segarecomp/cpu/m68k/decode.hpp"
#include "segarecomp/cpu/m68k/instruction.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

namespace segarecomp {

struct MoveqInstruction { InstructionProvenance provenance{}; DecodeOutcome decode_outcome{DecodeOutcome::decoded_moveq}; DataRegister destination{DataRegister::d0}; std::int8_t immediate{}; };
struct IrMoveq32 { InstructionProvenance provenance{}; DecodeOutcome decode_outcome{DecodeOutcome::decoded_moveq}; DataRegister destination{DataRegister::d0}; std::int8_t immediate{}; };
struct DecodedMoveq { MoveqInstruction instruction{}; };
struct RejectedMoveq { DecodeOutcome outcome{}; DecodeSource source{}; std::uint64_t available_bytes{}; std::uint32_t requested_length{2}; std::uint32_t instruction_length{}; bool has_instruction_length{}; };
using MoveqDecodeResult = std::variant<DecodedMoveq, RejectedMoveq>;

struct MoveqCpuState { std::array<std::uint32_t, 8> d{}; M68kProgramAddress pc{}; std::uint16_t sr{}; };
struct EmittedMoveq { IrMoveq32 instruction{}; std::string c_source; };

[[nodiscard]] MoveqDecodeResult decode_moveq(std::span<const std::uint8_t> image, DecodeSource source);
[[nodiscard]] IrMoveq32 lift_moveq(const MoveqInstruction &instruction);
[[nodiscard]] const char *decode_outcome_name(DecodeOutcome outcome) noexcept;
[[nodiscard]] std::string format_moveq_rejection(const RejectedMoveq &rejection);
[[nodiscard]] EmittedMoveq emit_moveq(const IrMoveq32 &instruction, const MoveqCpuState &initial_state);
[[nodiscard]] std::string emit_moveq_c(const IrMoveq32 &instruction, const MoveqCpuState &initial_state);

} // namespace segarecomp
