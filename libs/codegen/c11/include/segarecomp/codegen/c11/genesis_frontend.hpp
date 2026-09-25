#pragma once

// Genesis-wrapper C11 emitters: M68k lowering composed with the Genesis machine frontend and
// runtime glue. Depends on codegen_c11_m68k; the reverse edge is forbidden (mechanically tested).
#include "segarecomp/codegen/c11/m68k.hpp"
#include "segarecomp/machine/genesis/address_space.hpp"
#include "segarecomp/machine/genesis/frontend.hpp"

#include <cstddef>
#include <ostream>
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
// SEG-022-T003: deterministic single-file-vs-sharded selection for the CLI. The compiled-unit count is
// the same quantity for both accepted result shapes: ordinary static blocks + immutable-ROM AOT entries.
inline constexpr std::size_t generated_c_shard_threshold = 1024U;
[[nodiscard]] inline std::size_t generated_program_unit_count(const FrontendPartialProgram &partial) {
  return partial.accepted_prefix.static_blocks.size() + partial.accepted_prefix.immutable_rom_aot_entries.size();
}
[[nodiscard]] inline std::size_t generated_program_unit_count(const FrontendAnalysis &analysis) {
  return analysis.static_blocks.size() + analysis.immutable_rom_aot_entries.size();
}
// shard-dir alone => shard; output file alone => single file; both => shard iff units >= threshold.
[[nodiscard]] inline bool select_sharded_generated_c(bool has_output_file, bool has_shard_dir, std::size_t program_units) {
  return has_shard_dir && (!has_output_file || program_units >= generated_c_shard_threshold);
}
// SEG-022-T002: streaming forms. The generated program is written to `sink` as it is produced and is never
// materialized as one string. The return value is empty on success; a non-empty return is the
// "/* translation rejected: ... */" text and any bytes already written to `sink` MUST be discarded.
[[nodiscard]] std::string emit_m68k_general_startup_bridge_c_to(std::ostream &sink, const FrontendPartialProgram &partial,
                                                                std::string_view rom_sha256, bool execution_history_hooks = false);
[[nodiscard]] std::string emit_m68k_general_startup_bridge_c_to(std::ostream &sink, const FrontendAnalysis &analysis,
                                                                std::string_view rom_sha256, bool execution_history_hooks = false);
[[nodiscard]] std::string emit_m68k_general_startup_bridge_c(const FrontendPartialProgram &partial, std::string_view rom_sha256,
                                                                    bool execution_history_hooks = false);
[[nodiscard]] std::string emit_m68k_general_startup_bridge_c(const FrontendAnalysis &analysis, std::string_view rom_sha256,
                                                                    bool execution_history_hooks = false);
[[nodiscard]] std::string emit_m68k_frontend_c(const FrontendAnalysis &analysis, const DirectFlowState &initial, std::uint64_t budget);

} // namespace segarecomp
