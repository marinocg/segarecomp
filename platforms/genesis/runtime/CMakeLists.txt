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

# SEG-032-T004 (ADR 0072): the Genesis Z80 as a secondary CPU (its view of the machine). Compiled only into programs that carry a
# Z80 image registry; the generated-native Z80 ABI header comes from the codegen library (header only). A consumer links this
# next to segarecomp::runtime_genesis (the ownership test forbids the runtime library from linking anything).
add_library(segarecomp_runtime_genesis_z80 STATIC z80_machine.c)
add_library(segarecomp::runtime_genesis_z80 ALIAS segarecomp_runtime_genesis_z80)
set_target_properties(segarecomp_runtime_genesis_z80 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(segarecomp_runtime_genesis_z80 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/libs/codegen/c11/include)
segarecomp_enable_warnings(segarecomp_runtime_genesis_z80)
