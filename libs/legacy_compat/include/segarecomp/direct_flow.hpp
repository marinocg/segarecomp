#pragma once

#include "segarecomp/codegen/c11/genesis_frontend.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace segarecomp {
[[nodiscard]] DirectFlowAnalysisResult discover_direct_flow(std::span<const std::uint8_t> image, const DirectFlowProgram &program);
[[nodiscard]] DirectFlowExecutionResult execute_direct_flow(const DirectFlowAnalysis &analysis, DirectFlowState initial, std::uint64_t budget);
[[nodiscard]] std::string emit_direct_flow_c(const DirectFlowAnalysis &analysis, const DirectFlowState &initial, std::uint64_t budget);
[[nodiscard]] std::string format_direct_flow_rejection(const RejectedDirectFlow &rejection);
[[nodiscard]] const char *direct_flow_diagnostic_name(DirectFlowDiagnostic diagnostic) noexcept;
} // namespace segarecomp
