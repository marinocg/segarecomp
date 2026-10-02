# SEG-029-T004 (ADR 0078): synthetic, report-only Z80 second-CPU adapter of the generic abstract-analysis core.
# Only tests link it (tests/analysis_core_boundary_test.py). The CPU library itself never links the analysis.
add_library(segarecomp_cpu_z80_analysis
  src/adapter.cpp
)
add_library(segarecomp::cpu_z80_analysis ALIAS segarecomp_cpu_z80_analysis)
target_link_libraries(segarecomp_cpu_z80_analysis PUBLIC segarecomp::cpu_z80 segarecomp::analysis)
target_include_directories(segarecomp_cpu_z80_analysis PUBLIC include)
segarecomp_enable_warnings(segarecomp_cpu_z80_analysis)
