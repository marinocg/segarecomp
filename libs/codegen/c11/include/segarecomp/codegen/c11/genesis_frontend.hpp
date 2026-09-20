#pragma once

// Genesis-wrapper C11 emitters: M68k lowering composed with the Genesis machine frontend and
// runtime glue. Depends on codegen_c11_m68k; the reverse edge is forbidden (mechanically tested).
#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/machine/genesis/address_space.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"

#include <string>
#include <string_view>

namespace segarecomp {

// The Genesis machine owns address classification. The M68k lowering receives only how an
// operand access is to be emitted.
[[nodiscard]] constexpr M68kOperandAccess genesis_lowering_access(M68kAbsoluteOperandRegion region) noexcept {
  switch (region) {
  case M68kAbsoluteOperandRegion::raw_cartridge_rom: return M68kOperandAccess::resolved_constant;
  case M68kAbsoluteOperandRegion::synthetic_work_ram: return M68kOperandAccess::linear_memory;
  case M68kAbsoluteOperandRegion::controller_io:
  case M68kAbsoluteOperandRegion::vdp:
  case M68kAbsoluteOperandRegion::routed_device: return M68kOperandAccess::runtime_routed;
  }
  return M68kOperandAccess::resolved_constant;
}

// Genesis routed-access C protocol text for the M68k lowering's runtime seam.
[[nodiscard]] const M68kRuntimeCEmitter &genesis_m68k_runtime_c_emitter();

// M68k emission context pre-bound to the Genesis memory map and runtime emitter.
struct GenesisM68kEmissionContext : M68kMemoryEmissionContext {
  GenesisM68kEmissionContext() { bind(); }
  GenesisM68kEmissionContext(std::string_view ram, std::string_view address, std::string_view frame_ids,
                             std::string_view frame_continuations, std::string_view depth,
                             std::uint32_t static_continuation,
                             std::optional<M68kAbsoluteOperandRegion> operand_region, std::uint32_t operand_value,
                             const std::vector<M68kAbsoluteOperandRegion> &transfer_regions = {},
                             std::vector<std::uint32_t> transfer_values = {})
      : M68kMemoryEmissionContext(ram, address, frame_ids, frame_continuations, depth, static_continuation,
                                  std::nullopt, operand_value, {}, std::move(transfer_values)) {
    if (operand_region) test_operand_access = genesis_lowering_access(*operand_region);
    for (const auto region : transfer_regions) movem_transfer_access.push_back(genesis_lowering_access(region));
    bind();
  }

 private:
  void bind() {
    linear_memory_begin = m68k_startup_ram_begin;
    linear_memory_end = m68k_startup_ram_end;
    runtime_emitter = &genesis_m68k_runtime_c_emitter();
  }
};

[[nodiscard]] std::string emit_m68k_general_startup_runtime_block_c(const FrontendAnalysis &analysis);
[[nodiscard]] std::string emit_m68k_general_startup_runtime_c(const FrontendAnalysis &analysis);
[[nodiscard]] std::string emit_m68k_general_startup_runtime_c(const FrontendPartialProgram &partial);
[[nodiscard]] M68kC4Preflight preflight_m68k_general_startup_c4(const FrontendPartialProgram &partial);
[[nodiscard]] std::string emit_m68k_general_startup_bridge_c(const FrontendPartialProgram &partial, std::string_view rom_sha256,
                                                                    bool execution_history_hooks = false);
[[nodiscard]] std::string emit_m68k_general_startup_bridge_c(const FrontendAnalysis &analysis, std::string_view rom_sha256,
                                                                    bool execution_history_hooks = false);
[[nodiscard]] std::string emit_m68k_frontend_c(const FrontendAnalysis &analysis, const DirectFlowState &initial, std::uint64_t budget);

} // namespace segarecomp
