# Generation-time Master System composition (ADR 0063/0064): cartridge ingestion, mapper identity, the mapper
# contract table's ImageSet side and the emission entry point over codegen_c11_z80. It depends on no other machine's CPU or device code.
add_library(segarecomp_machine_master_system
  src/cartridge.cpp
  src/image_set.cpp
  src/emit.cpp
)
add_library(segarecomp::machine_master_system ALIAS segarecomp_machine_master_system)
target_link_libraries(segarecomp_machine_master_system PUBLIC
  segarecomp::media segarecomp::codegen_c11_z80 segarecomp::runtime_master_system)
target_include_directories(segarecomp_machine_master_system PUBLIC include)
segarecomp_enable_warnings(segarecomp_machine_master_system)
