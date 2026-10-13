# Generation-time Master System composition (ADR 0063/0064): cartridge ingestion, mapper identity, the mapper
# contract table's ImageSet side and the emission entry point over codegen_c11_z80. It depends on no other machine's CPU or device code.
add_library(segarecomp_machine_master_system
  src/cartridge.cpp
  src/image_set.cpp
  src/emit.cpp
  src/ml_region_z80_producer.cpp
)
add_library(segarecomp::machine_master_system ALIAS segarecomp_machine_master_system)
target_link_libraries(segarecomp_machine_master_system PUBLIC
  segarecomp::media segarecomp::codegen_c11_z80 segarecomp::codegen_c11_z80_image segarecomp::runtime_master_system)
target_include_directories(segarecomp_machine_master_system PUBLIC include)
segarecomp_enable_warnings(segarecomp_machine_master_system)

# SEG-057-T002 (ADR 0119): the frozen D512HP weights are embedded from the committed SGZ80W01 artifact at build time (no model file is
# managed by users, and nothing is duplicated in the source tree). The evaluator is bit-reproducible: IEEE binary32, fixed accumulation
# order, no FMA contraction.
set(_sms_ml_blob "${PROJECT_SOURCE_DIR}/tools/segarecomp_ml_region_z80_gen2.weights.sgz80w")
set(_sms_ml_inc "${CMAKE_CURRENT_BINARY_DIR}/generated/ml_region_z80_weights.inc")
add_custom_command(OUTPUT "${_sms_ml_inc}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
  COMMAND ${CMAKE_COMMAND} -DIN=${_sms_ml_blob} -DOUT=${_sms_ml_inc} -DSYMBOL=ml_z80_weights_blob -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_blob.cmake"
  DEPENDS "${_sms_ml_blob}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_blob.cmake"
  COMMENT "Embedding the SMS Z80 D512HP weights")
set_source_files_properties(src/ml_region_z80_producer.cpp PROPERTIES OBJECT_DEPENDS "${_sms_ml_inc}")
target_include_directories(segarecomp_machine_master_system PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(MSVC)
  set_source_files_properties(src/ml_region_z80_producer.cpp PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
  set_source_files_properties(src/ml_region_z80_producer.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
endif()
find_package(Threads REQUIRED)
target_link_libraries(segarecomp_machine_master_system PRIVATE Threads::Threads)
