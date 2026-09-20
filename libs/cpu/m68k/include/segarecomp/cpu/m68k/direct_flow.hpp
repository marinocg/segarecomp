#pragma once

// SEG-014-T002: the temporary SEG-002/SEG-003 direct-flow compatibility
// route's CPU-fact half (analysis/discovery/execution), per
// docs/architecture/seg-014-t001-symbol-migration-map.md's "Temporary
// SEG-002/SEG-003 compatibility surfaces" and `cpu/m68k/` sections. C
// emission (emit_m68k_direct_flow_c/emit_m68k_structured_direct_flow_c)
// deliberately stays out of this header/module: it belongs to codegen/c11
// (RULE 5, "codegen only renders") and is not moved by this task at all.
//
// Relocated from include/segarecomp/m68k_pipeline.hpp. No behavior changes.

#include "segarecomp/core/address.hpp"
#include "segarecomp/cpu/m68k/instruction.hpp"
#include "segarecomp/cpu/m68k/ir.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace segarecomp {

enum class DirectFlowKind { moveq_d0, subq_l_1_d0, bne_short, bra_short };
enum class DirectFlowIrKind { write_moveq_d0, subtract_quick_long_d0, branch_ne_short, branch_always_short };
enum class DirectEdgeKind { fallthrough, bne_taken, bne_fallthrough, bra_taken };
enum class DirectCondition { always, z_clear, z_set };
// `discovery_budget_exhausted` (SEG-007-T010): a configured
// discover_m68k_general_startup discovery-time budget was exhausted; carries
// which budget via RejectedDirectFlow::unresolved_reason and full available
// provenance.
enum class DirectFlowDiagnostic { odd_instruction_address, unmapped_instruction_address, truncated_instruction, illegal_instruction, valid_but_unsupported_instruction, unsupported_instruction_form, odd_direct_target, conflicting_address_mapping, invalid_address_mapping, unmapped_direct_target, mid_instruction_direct_target, reached_unresolved_direct_edge, invalid_frontend_image_source_id, frontend_image_byte_length_mismatch, invalid_mapping_claim, vector_fixture_id_mismatch, vector_image_sha256_mismatch, vector_cpu_variant_mismatch, vector_execution_entry_space_mismatch, execution_entry_inside_discovered_instruction, execution_entry_inside_discovered_block, execution_entry_not_discovered_block_start, vector_block_instruction_count_mismatch, effective_address_not_24bit, odd_effective_address, rom_write_prohibited, unmapped_data_access, unsupported_device_region_controller_io, invalid_stack_alignment, invalid_stack_range, return_context_missing, return_target_mismatch, startup_graph_mismatch, discovery_budget_exhausted, authoritative_exact_target_closure_exhausted };

struct DirectInstruction { InstructionProvenance provenance{}; DirectFlowKind kind{DirectFlowKind::moveq_d0}; std::int8_t operand{}; };
struct DirectFlowIrOperation { InstructionProvenance provenance{}; DirectFlowIrKind kind{DirectFlowIrKind::write_moveq_d0}; std::int8_t operand{}; };
struct BlockId { M68kProgramAddress entry{}; };
struct BlockProvenance { BlockId id{}; InstructionProvenance entry_instruction{}; std::vector<InstructionProvenance> instructions; };
struct DirectBlock { BlockProvenance provenance{}; std::vector<DirectFlowIrOperation> operations; };
struct DirectEdge { BlockId source_block{}; InstructionProvenance source_instruction{}; DirectEdgeKind kind{}; DirectCondition condition{}; M68kProgramAddress target{}; bool unresolved{}; std::string unresolved_reason; };
struct DirectFlowState { std::array<std::uint32_t, 8> d{}; M68kProgramAddress pc{}; std::uint16_t sr{}; };
struct BlockBoundary { std::uint64_t ordinal{}; BlockProvenance block{}; std::optional<DirectEdge> incoming_edge; bool incoming_is_reset_seed{}; DirectFlowState state{}; std::optional<DirectEdge> outgoing_edge; std::uint64_t executed_blocks{}; std::uint64_t budget{}; std::string stop_reason{"continue"}; };
struct DirectFlowProgram { std::vector<MappingClaim> mappings; M68kProgramAddress reset_entry{}; std::vector<InstructionProvenance> structural_intervals; std::vector<M68kProgramAddress> unresolved_targets; };
struct DirectFlowAnalysis { std::vector<DirectBlock> blocks; std::vector<DirectEdge> edges; std::vector<M68kDecodedInstruction> decoded; std::vector<M68kIrOperation> ir; };
struct RejectedDirectFlow { DirectFlowDiagnostic category{}; BlockId block{}; InstructionProvenance provenance{}; bool has_provenance{}; std::uint64_t available_bytes{}; std::uint32_t requested_length{}; std::uint32_t instruction_length{}; bool has_instruction_length{}; M68kProgramAddress target{}; bool has_target{}; std::vector<MappingClaim> mapping_claims; std::string unresolved_reason; };
struct DirectFlowExecution { std::vector<BlockBoundary> boundaries; DirectFlowState final_state{}; std::string stop_reason; };
using DirectFlowAnalysisResult = std::variant<DirectFlowAnalysis, RejectedDirectFlow>;
using DirectFlowExecutionResult = std::variant<DirectFlowExecution, RejectedDirectFlow>;

[[nodiscard]] DirectFlowAnalysisResult discover_m68k_direct_flow(
    std::span<const std::uint8_t> image, const DirectFlowProgram &program);
// The frontend supplies already validated typed seeds.  This is the same
// selected decode/lift/FIFO discovery route as the compatibility entry point.
[[nodiscard]] DirectFlowAnalysisResult discover_m68k_direct_flow_entries(
    std::span<const std::uint8_t> image, const DirectFlowProgram &program,
    std::span<const M68kProgramAddress> entries);
[[nodiscard]] DirectFlowExecutionResult execute_m68k_direct_flow(
    const DirectFlowAnalysis &analysis, DirectFlowState initial, std::uint64_t budget);
[[nodiscard]] std::string format_m68k_direct_flow_rejection(const RejectedDirectFlow &rejection);
[[nodiscard]] const char *m68k_direct_flow_diagnostic_name(DirectFlowDiagnostic diagnostic) noexcept;

} // namespace segarecomp
