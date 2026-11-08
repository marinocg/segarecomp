# SEG-009-T009: Master System viewer. The host-neutral core is always built (strict C11, never linked by headless
# targets); the SDL3 adapter is optional and gated by SEGARECOMP_ENABLE_SDL3_VIEWER.
add_library(segarecomp_viewer_master_system STATIC sms_viewer.c sms_snapshot.c)
add_library(segarecomp::viewer_master_system ALIAS segarecomp_viewer_master_system)
set_target_properties(segarecomp_viewer_master_system PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(segarecomp_viewer_master_system PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/libs/viewer/include)
target_link_libraries(segarecomp_viewer_master_system PUBLIC segarecomp::runtime_master_system)
segarecomp_enable_warnings(segarecomp_viewer_master_system)

# The viewer `main` only includes the SDL-free adapter header: compile-check it on every host.
add_library(segarecomp_viewer_master_system_main_check OBJECT sms_viewer_main.c)
set_target_properties(segarecomp_viewer_master_system_main_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(segarecomp_viewer_master_system_main_check PRIVATE segarecomp::viewer_master_system)
target_include_directories(segarecomp_viewer_master_system_main_check PRIVATE ${PROJECT_SOURCE_DIR}/libs/viewer/include)   # sv_perf.h (SEG-125): header-only and SDL-free
segarecomp_enable_warnings(segarecomp_viewer_master_system_main_check)

if(SEGARECOMP_ENABLE_SDL3_VIEWER)
  find_package(SDL3 REQUIRED CONFIG)
  # SEG-132: the adapter of the shared save-state service (libs/viewer) belongs with the SDL3 host that provides that service.
  add_library(segarecomp_viewer_master_system_sdl3 STATIC sms_viewer_sdl3.c sms_viewer_state.c)
  target_link_libraries(segarecomp_viewer_master_system_sdl3 PUBLIC segarecomp::viewer_master_system segarecomp::viewer_host SDL3::SDL3)
  set_target_properties(segarecomp_viewer_master_system_sdl3 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
  segarecomp_enable_warnings(segarecomp_viewer_master_system_sdl3)
endif()
