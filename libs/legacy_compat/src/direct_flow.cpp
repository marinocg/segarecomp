#include "segarecomp/direct_flow.hpp"

namespace segarecomp {

// Compatibility surface only.  The selected MC68000 route is implemented by
// m68k_pipeline_direct_flow.cpp alongside the shared decode/lift frontend.
const char *direct_flow_diagnostic_name(DirectFlowDiagnostic diagnostic) noexcept {
  return m68k_direct_flow_diagnostic_name(diagnostic);
}

DirectFlowAnalysisResult discover_direct_flow(std::span<const std::uint8_t> image,
                                              const DirectFlowProgram &program) {
  return discover_m68k_direct_flow(image, program);
}

DirectFlowExecutionResult execute_direct_flow(const DirectFlowAnalysis &analysis,
                                              DirectFlowState initial, std::uint64_t budget) {
  return execute_m68k_direct_flow(analysis, initial, budget);
}

std::string emit_direct_flow_c(const DirectFlowAnalysis &analysis, const DirectFlowState &initial,
                               std::uint64_t budget) {
  return emit_m68k_direct_flow_c(analysis, initial, budget);
}

std::string format_direct_flow_rejection(const RejectedDirectFlow &rejection) {
  return format_m68k_direct_flow_rejection(rejection);
}

} // namespace segarecomp
