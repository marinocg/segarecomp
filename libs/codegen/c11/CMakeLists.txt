# C11 code generation, split by dependency direction (SEG-018-T004):
#   codegen_c11          common C11 support (no CPU/platform dependency)
#   codegen_c11_m68k     M68k lowering (common + cpu_m68k only; no Genesis machine/device)
#   codegen_c11_genesis  Genesis wrapper: M68k lowering + Genesis machine frontend/runtime glue
# A future Z80 lowering is a sibling of codegen_c11_m68k; an SMS wrapper a sibling of codegen_c11_genesis.
add_library(segarecomp_codegen_c11 src/manifest.cpp)
add_library(segarecomp::codegen_c11 ALIAS segarecomp_codegen_c11)
target_link_libraries(segarecomp_codegen_c11 PUBLIC segarecomp::base segarecomp::media)
target_include_directories(segarecomp_codegen_c11 PUBLIC include)
segarecomp_enable_warnings(segarecomp_codegen_c11)

add_library(segarecomp_codegen_c11_m68k src/m68k.cpp src/direct_flow.cpp)
add_library(segarecomp::codegen_c11_m68k ALIAS segarecomp_codegen_c11_m68k)
target_link_libraries(segarecomp_codegen_c11_m68k PUBLIC segarecomp::codegen_c11 segarecomp::cpu_m68k)
segarecomp_enable_warnings(segarecomp_codegen_c11_m68k)

add_library(segarecomp_codegen_c11_genesis src/frontend.cpp src/genesis.cpp src/genesis_m68k_runtime.cpp)
add_library(segarecomp::codegen_c11_genesis ALIAS segarecomp_codegen_c11_genesis)
target_link_libraries(segarecomp_codegen_c11_genesis PUBLIC segarecomp::codegen_c11_m68k segarecomp::machine_genesis segarecomp::recompiler)
segarecomp_enable_warnings(segarecomp_codegen_c11_genesis)
