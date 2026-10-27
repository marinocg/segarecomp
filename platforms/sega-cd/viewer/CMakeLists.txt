# SEG-078-T001 (ADR 0204): the Sega CD entry of the shared SDL3 viewer. Optional (SEGARECOMP_ENABLE_SDL3_VIEWER, declared by the Genesis viewer);
# never linked by headless targets. It registers an SDL3 host with the runtime's live driver (scd_live.c) over the Genesis viewer's presenter,
# pacer, controller mapping and settings - no second presentation or input stack.
if(SEGARECOMP_ENABLE_SDL3_VIEWER)
  find_package(SDL3 REQUIRED CONFIG)
  add_library(segarecomp_viewer_sega_cd_sdl3 STATIC scd_viewer.c)
  target_include_directories(segarecomp_viewer_sega_cd_sdl3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(segarecomp_viewer_sega_cd_sdl3 PUBLIC segarecomp::runtime_sega_cd segarecomp_viewer_genesis_sdl3 SDL3::SDL3)
  set_target_properties(segarecomp_viewer_sega_cd_sdl3 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
  segarecomp_enable_warnings(segarecomp_viewer_sega_cd_sdl3)
endif()
