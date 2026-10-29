#pragma once

// SEG-100-T002 (ADR 0221): the source-offset instruction body, derived for a guest base that is not known when the body is compiled.
//
// A body is identified by (processor, immutable source identity, source offset); it reads the actual guest PC at run time (`scd_pc0`) and
// verifies the live instruction bytes at that PC with the unchanged `scd_code_guard`. The text is the CPU-owned lowering of the instruction
// (decode, lift, lower and timing are libs/cpu/m68k and libs/codegen/c11; nothing here is an MC68000 semantic) taken at three distinct
// sentinel PCs and reduced by `derive_source_relative_text` (ADR 0207) so that only the PC-derived literals move with the actual PC.
//
// What this layer decides, and never assumes: a PC-dependent instruction that cannot be proven affine in the PC is a typed refusal
// (`DynamicBodyStatus::source_relative_refused`, with the derivation status) and is never emitted as position independent. Nothing in
// this layer is reachable at run time: it runs in the build, the generated program only calls the bodies it emitted.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "segarecomp/codegen/c11/source_relative.hpp"

namespace segarecomp::machine::sega_cd {

enum class DynamicBodyStatus : std::uint8_t {
  ok,
  not_decoded,              // the CPU-owned decoder rejected the bytes at this offset (data, an unsupported form, a truncated instruction)
  not_lifted,               // decoded but the lifter has no operation for it
  not_lowered,              // the CPU-owned lowering produced no body or no timing
  redecode,                 // the instruction decoded differently at another sentinel PC (an address-sensitive decode; never silently shared)
  source_relative_refused,  // the lowering is not provably affine in the PC (see `derive`)
};
[[nodiscard]] const char *dynamic_body_status_name(DynamicBodyStatus status) noexcept;

struct DynamicBody {
  DynamicBodyStatus status{DynamicBodyStatus::not_decoded};
  SourceRelativeStatus derive{SourceRelativeStatus::ok};  // meaningful when status == source_relative_refused
  std::vector<std::uint8_t> raw;                          // the instruction bytes the body verifies (empty unless decoded)
  std::uint32_t relative_literals{};                      // PC-derived literals replaced by `scd_pc0 + constant`
  bool prefetch_established{};                            // the open-bus prefetch address is `scd_pc0 + raw.size()` (otherwise 0: not established)
  std::string slots, body, timing, refresh_skip;          // with `scd_pc0` as the PC symbol
};

// `image` holds the source bytes; the decoder sees `image[0, visible)` (an instruction may not straddle `visible`), and the instruction is
// the one at `offset` (even). The body is processor-neutral: it names the processor it runs for through the local `scd_k` (initialised by the
// generated function from the dispatcher) and its computed-control authority is `scd_neutral_lookup`, so one body serves a file whichever
// processor loads it. A body is a function of the instruction's bytes alone: equal bytes give equal text wherever they occur.
[[nodiscard]] DynamicBody derive_dynamic_body(std::span<const std::uint8_t> image, std::uint32_t visible, std::uint32_t offset);

}  // namespace segarecomp::machine::sega_cd
