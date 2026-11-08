# SEG-078-T001 (ADR 0204): the Sega CD entry of the shared SDL3 viewer. Optional (SEGARECOMP_ENABLE_SDL3_VIEWER, declared by the Genesis viewer);
# never linked by headless targets. It registers an SDL3 host with the runtime's live driver (scd_live.c) over the Genesis viewer's presenter,
# pacer, controller mapping and settings - no second presentation or input stack.
if(SEGARECOMP_ENABLE_SDL3_VIEWER)
  find_package(SDL3 REQUIRED CONFIG)
  # SEG-132 (ADR 0279): the Sega CD state codec, its adapter to the shared save-state service and the Genesis section codecs the codec reuses (no Z80 sections here: the CMake library carries no Z80).
add_library(segarecomp_viewer_sega_cd_sdl3 STATIC scd_viewer.c scd_snapshot.c scd_viewer_state.c ${PROJECT_SOURCE_DIR}/platforms/genesis/runtime/genesis_snapshot.c)
  target_include_directories(segarecomp_viewer_sega_cd_sdl3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/libs/viewer/include ${PROJECT_SOURCE_DIR}/platforms/genesis/runtime)
  target_link_libraries(segarecomp_viewer_sega_cd_sdl3 PUBLIC segarecomp::runtime_sega_cd segarecomp_viewer_genesis_sdl3 segarecomp::viewer_core SDL3::SDL3)
  set_target_properties(segarecomp_viewer_sega_cd_sdl3 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
  segarecomp_enable_warnings(segarecomp_viewer_sega_cd_sdl3)
endif()
