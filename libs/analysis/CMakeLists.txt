# SEG-029 (ADR 0078): generic, CPU-free abstract program-analysis core. Header-only; links only segarecomp::base.
# CPU-owned adapters live in separate targets (libs/cpu/<cpu>/analysis); no production target links this library.
add_library(segarecomp_analysis INTERFACE)
add_library(segarecomp::analysis ALIAS segarecomp_analysis)
target_link_libraries(segarecomp_analysis INTERFACE segarecomp::base)
target_include_directories(segarecomp_analysis INTERFACE include)
