# Standalone strict-C11 Sega CD runtime linked by generated programs (ADR 0128/0129); never links host C++ code. It composes the
# Genesis runtime only for the VDP device, frame export and the C SHA-256. This target compiles the sources under the project
# warning set; generated programs compile the same sources next to their own translation unit.
add_library(segarecomp_runtime_sega_cd STATIC
  scd_bios.c
  scd_cd.c
  scd_devices.c
  scd_disc.c
  scd_gate_array.c
  scd_machine.c
  scd_main.c
  scd_overlay.c
  scd_vdp.c
  scd_word_ram.c
)
add_library(segarecomp::runtime_sega_cd ALIAS segarecomp_runtime_sega_cd)
set_target_properties(segarecomp_runtime_sega_cd PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(segarecomp_runtime_sega_cd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(segarecomp_runtime_sega_cd PUBLIC segarecomp::runtime_genesis)
segarecomp_enable_warnings(segarecomp_runtime_sega_cd)
