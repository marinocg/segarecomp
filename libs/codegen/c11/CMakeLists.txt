# C11 code generation, split by dependency direction (SEG-018-T004):
#   codegen_c11          common C11 support (no CPU/platform dependency)
#   codegen_c11_m68k     M68k lowering (common + cpu_m68k only; no Genesis machine/device)
#   codegen_c11_genesis  Genesis wrapper: M68k lowering + Genesis machine frontend/runtime glue
# A future Z80 lowering is a sibling of codegen_c11_m68k; an SMS wrapper a sibling of codegen_c11_genesis.
add_library(segarecomp_codegen_c11 src/manifest.cpp src/translation_units.cpp)
add_library(segarecomp::codegen_c11 ALIAS segarecomp_codegen_c11)
target_link_libraries(segarecomp_codegen_c11 PUBLIC segarecomp::base segarecomp::media)
target_include_directories(segarecomp_codegen_c11 PUBLIC include)
segarecomp_enable_warnings(segarecomp_codegen_c11)

add_library(segarecomp_codegen_c11_m68k src/m68k.cpp src/direct_flow.cpp src/provenance_diagnostics.cpp)
add_library(segarecomp::codegen_c11_m68k ALIAS segarecomp_codegen_c11_m68k)
target_link_libraries(segarecomp_codegen_c11_m68k PUBLIC segarecomp::codegen_c11 segarecomp::cpu_m68k)
segarecomp_enable_warnings(segarecomp_codegen_c11_m68k)

add_library(segarecomp_codegen_c11_genesis src/frontend.cpp src/genesis.cpp src/genesis_m68k_runtime.cpp)
add_library(segarecomp::codegen_c11_genesis ALIAS segarecomp_codegen_c11_genesis)
target_link_libraries(segarecomp_codegen_c11_genesis PUBLIC segarecomp::codegen_c11_m68k segarecomp::machine_genesis segarecomp::recompiler)
segarecomp_enable_warnings(segarecomp_codegen_c11_genesis)

# Z80 lowering (SEG-008-T003, ADR 0059): sibling of codegen_c11_m68k; common C11 support + cpu_z80 only, no M68k,
# Genesis or platform dependency. The generated-code runtime ABI is the plain C header
# include/segarecomp/codegen/c11/runtime/z80_runtime.h. Lowering rows live one family per file.
add_library(segarecomp_codegen_c11_z80
  src/z80.cpp src/z80_lowering.cpp
  src/z80_lower_data_alu.cpp src/z80_lower_control_stack.cpp src/z80_lower_cb_bit_prefix.cpp
  src/z80_lower_ed_io_interrupt.cpp)
add_library(segarecomp::codegen_c11_z80 ALIAS segarecomp_codegen_c11_z80)
target_link_libraries(segarecomp_codegen_c11_z80 PUBLIC segarecomp::codegen_c11 segarecomp::cpu_z80)
segarecomp_enable_warnings(segarecomp_codegen_c11_z80)

# SEG-028-T003 (ADR 0077): projection of CPU-neutral executable images onto the Z80 emitter input. A separate target so the Z80 lowering
# itself keeps depending on codegen_c11 and cpu_z80 only.
add_library(segarecomp_codegen_c11_z80_image src/z80_executable_image.cpp)
add_library(segarecomp::codegen_c11_z80_image ALIAS segarecomp_codegen_c11_z80_image)
target_link_libraries(segarecomp_codegen_c11_z80_image PUBLIC segarecomp::codegen_c11_z80 segarecomp::recompiler)
segarecomp_enable_warnings(segarecomp_codegen_c11_z80_image)
