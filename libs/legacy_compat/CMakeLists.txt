# Historical command/report compatibility shims; not a semantic umbrella.
add_library(segarecomp_legacy_compat
  src/direct_flow.cpp
  src/moveq.cpp
)
add_library(segarecomp::legacy_compat ALIAS segarecomp_legacy_compat)
target_link_libraries(segarecomp_legacy_compat PUBLIC segarecomp::cpu_m68k segarecomp::codegen_c11_genesis)
target_include_directories(segarecomp_legacy_compat PUBLIC include)
segarecomp_enable_warnings(segarecomp_legacy_compat)
