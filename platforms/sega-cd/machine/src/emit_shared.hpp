#pragma once

// Internal to the Sega CD machine library: the routed-access protocol text of one processor instance (ADR 0128) and the lowering of
// one lifted MC68000 operation into the body of a generated instruction function. Shared by the synthetic post-BIOS profile
// (program.cpp, SEG-062/063) and the BIOS-image profile (bios_profile.cpp, SEG-064, ADR 0142); both produce identical text for an
// identical operation. The CPU lowering (emit_m68k_operation_c) owns every MC68000 semantic; this layer only names the runtime ABI.

#include <cstdint>
#include <string>
#include <string_view>

#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

namespace segarecomp::machine::sega_cd::detail {

std::string hex(std::uint32_t value, int width = 8);

class SegaCdM68kRuntimeCEmitter final : public M68kRuntimeCEmitter {
 public:
  explicit SegaCdM68kRuntimeCEmitter(unsigned cpu);
  std::string instruction_source(const M68kIrOperation &op) const override;
  std::string_view default_provenance_helper() const override;
  std::string routed_read(const M68kMemoryEmissionContext &ctx, std::string_view addr, std::string_view value, std::string_view,
                          M68kMemoryAccessWidth size) const override;
  std::string routed_write(const M68kMemoryEmissionContext &ctx, std::string_view addr, std::string_view, M68kMemoryAccessWidth size,
                           std::string_view value) const override;
  std::string return_pop_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7) const override;
  std::string unresolved_indirect_stop(const M68kMemoryEmissionContext &ctx) const override;
  std::string call_push_guard(const M68kMemoryEmissionContext &ctx, std::string_view a7, bool) const override;
  std::string divide_by_zero(const M68kMemoryEmissionContext &ctx, std::uint32_t next_pc, std::uint32_t entry_cycles) const override;
  std::string exception_return(const M68kMemoryEmissionContext &ctx) const override;
  std::string privilege_violation(const M68kMemoryEmissionContext &ctx, std::uint32_t fault_pc, std::uint32_t entry_cycles) const override;
  std::string trace_deferred_stop(const M68kMemoryEmissionContext &ctx) const override;
  std::string software_exception(const M68kMemoryEmissionContext &ctx, std::uint32_t vector, std::uint32_t stacked_pc,
                                 std::uint32_t entry_cycles) const override;
  std::string condition_code_return(const M68kMemoryEmissionContext &ctx) const override;
  std::string stopped_state(const M68kMemoryEmissionContext &) const override;

 private:
  std::string k_;
  std::string lookup_;
};

// The generated body of one instruction function: per-instruction timing slots, the CPU-lowered effect, and the retirement
// timing expression. `compiled` is false when the CPU owner produced no body or no timing.
struct LoweredInstruction {
  bool compiled = false;
  std::string slots;
  std::string body;
  std::string timing;
};

[[nodiscard]] LoweredInstruction lower_instruction(const M68kIrOperation &op, std::uint32_t pc, const SegaCdM68kRuntimeCEmitter &emitter,
                                                   const std::string &lookup_symbol);

}  // namespace segarecomp::machine::sega_cd::detail
