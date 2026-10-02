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

# SEG-032-T006 (ADR 0072): the Genesis sound devices as attached machine parts (the shared Sega PSG; the YM2612 and the mixer join
# in SEG-032-T007/T009). Compiled only into programs that carry sound; a consumer links segarecomp::device_psg and segarecomp::device_ym2612 next to it.
add_library(segarecomp_runtime_genesis_audio STATIC genesis_audio.c genesis_mixer.c genesis_audio_present.c)
add_library(segarecomp::runtime_genesis_audio ALIAS segarecomp_runtime_genesis_audio)
set_target_properties(segarecomp_runtime_genesis_audio PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(segarecomp_runtime_genesis_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/libs/device/sega/psg/include ${PROJECT_SOURCE_DIR}/libs/device/sega/ym2612/include)
segarecomp_enable_warnings(segarecomp_runtime_genesis_audio)

# SEG-032-T008 (ADR 0073): the sound subsystem attach point and the two hook seams `segarecomp build` links (the final program's
# hook and the build-time materialization pass hook define the same entry symbol; a program links exactly one of them, so the
# pass hook is only compiled here as an object library to keep it under the project warning set).
add_library(segarecomp_runtime_genesis_sound STATIC genesis_sound.c genesis_sound_hook.c)
add_library(segarecomp::runtime_genesis_sound ALIAS segarecomp_runtime_genesis_sound)
add_library(segarecomp_runtime_genesis_materialize_hook OBJECT genesis_materialize_hook.c)
foreach(_t segarecomp_runtime_genesis_sound segarecomp_runtime_genesis_materialize_hook)
  set_target_properties(${_t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_include_directories(${_t} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/libs/codegen/c11/include
    ${PROJECT_SOURCE_DIR}/libs/device/sega/psg/include ${PROJECT_SOURCE_DIR}/libs/device/sega/ym2612/include)
  segarecomp_enable_warnings(${_t})
endforeach()
