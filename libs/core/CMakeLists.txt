# SEG-014-T002 / SEG-018-T002: genuinely cross-target core/ contracts (header-only).
add_library(segarecomp_base INTERFACE)
add_library(segarecomp::base ALIAS segarecomp_base)
target_compile_features(segarecomp_base INTERFACE cxx_std_20)
target_include_directories(segarecomp_base INTERFACE include)
