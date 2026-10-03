# SEG-029-T003 (ADR 0078): report-only MC68000 adapter for the generic abstract-analysis core. Links the CPU semantic
# owner and the CPU-free core only; no production target links this library (analysis_core_boundary_test).
add_library(segarecomp_cpu_m68k_analysis
  src/abstract_memory.cpp
  src/address_value.cpp
  src/finite_adapter.cpp
  src/frames.cpp
)
add_library(segarecomp::cpu_m68k_analysis ALIAS segarecomp_cpu_m68k_analysis)
target_link_libraries(segarecomp_cpu_m68k_analysis PUBLIC segarecomp::cpu_m68k segarecomp::analysis)
target_include_directories(segarecomp_cpu_m68k_analysis PUBLIC include)
segarecomp_enable_warnings(segarecomp_cpu_m68k_analysis)
