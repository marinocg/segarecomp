# Standalone strict-C11 ABI/runtime linked by generated programs; never links host C++ code.
add_library(segarecomp_runtime_genesis STATIC
  runtime.c
  vdp_render.c
  frame_export.c
)
add_library(segarecomp::runtime_genesis ALIAS segarecomp_runtime_genesis)
set_target_properties(segarecomp_runtime_genesis PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
)
target_include_directories(segarecomp_runtime_genesis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
segarecomp_enable_warnings(segarecomp_runtime_genesis)
