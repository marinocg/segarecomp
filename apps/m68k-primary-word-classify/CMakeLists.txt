# SEG-042-T001: tiny, bounded, non-production batch query CLI over the generation-time primary-word
# legality classifier (libs/cpu/m68k), so an external proof-path producer (SEG-042-T002's automated
# real-title harvester) never has to duplicate decoder logic in Python. Never installed; never linked by
# the segarecomp CLI, the compiler, or the runtime -- a standalone top-level diagnostic tool, exactly
# like segarecomp-genesis-analysis-report, but over the plain cpu_m68k library rather than the analysis
# core, so it deliberately lives outside platforms/genesis/analysis_report/ (analysis_core_boundary_test
# reserves that directory's CMakeLists.txt for exactly the analysis-report driver's own three targets).
add_executable(segarecomp-m68k-primary-word-classify main.cpp)
target_link_libraries(segarecomp-m68k-primary-word-classify PRIVATE segarecomp::cpu_m68k)
segarecomp_enable_warnings(segarecomp-m68k-primary-word-classify)
