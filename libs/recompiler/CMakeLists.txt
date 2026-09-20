# Retained-program/frontier/C4-plan contracts above CPU semantics (header-only).
add_library(segarecomp_recompiler INTERFACE)
add_library(segarecomp::recompiler ALIAS segarecomp_recompiler)
target_link_libraries(segarecomp_recompiler INTERFACE segarecomp::base)
target_include_directories(segarecomp_recompiler INTERFACE include)
