# SEG-030-T002 (ADR 0079 decisions 1-2): the report-only Genesis M68K instantiation driver of the abstract-analysis core. The only
# non-test place allowed to link the analysis (analysis_core_boundary_test): a static library (executable-image view + driver
# core) and its report executable. Never installed; the segarecomp CLI and every production target never link either.
add_library(segarecomp_genesis_analysis_report
  src/image_view.cpp
  src/report.cpp
)
add_library(segarecomp::genesis_analysis_report ALIAS segarecomp_genesis_analysis_report)
target_link_libraries(segarecomp_genesis_analysis_report PUBLIC segarecomp::machine_genesis segarecomp::cpu_m68k_analysis)
target_include_directories(segarecomp_genesis_analysis_report PUBLIC include)
segarecomp_enable_warnings(segarecomp_genesis_analysis_report)

add_executable(segarecomp-genesis-analysis-report src/main.cpp)
target_link_libraries(segarecomp-genesis-analysis-report PRIVATE segarecomp::genesis_analysis_report)
segarecomp_enable_warnings(segarecomp-genesis-analysis-report)
