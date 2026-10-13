# SEG-009-T009: Master System viewer. The host-neutral core is always built (strict C11, never linked by headless
# targets); the SDL3 adapter is optional and gated by SEGARECOMP_ENABLE_SDL3_VIEWER.
add_library(segarecomp_viewer_master_system STATIC sms_viewer.c)
add_library(segarecomp::viewer_master_system ALIAS segarecomp_viewer_master_system)
set_target_properties(segarecomp_viewer_master_system PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(segarecomp_viewer_master_system PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(segarecomp_viewer_master_system PUBLIC segarecomp::runtime_master_system)
segarecomp_enable_warnings(segarecomp_viewer_master_system)

# The viewer `main` only includes the SDL-free adapter header: compile-check it on every host.
add_library(segarecomp_viewer_master_system_main_check OBJECT sms_viewer_main.c)
set_target_properties(segarecomp_viewer_master_system_main_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(segarecomp_viewer_master_system_main_check PRIVATE segarecomp::viewer_master_system)
segarecomp_enable_warnings(segarecomp_viewer_master_system_main_check)

if(SEGARECOMP_ENABLE_SDL3_VIEWER)
  find_package(SDL3 REQUIRED CONFIG)
  add_library(segarecomp_viewer_master_system_sdl3 STATIC sms_viewer_sdl3.c)
  target_link_libraries(segarecomp_viewer_master_system_sdl3 PUBLIC segarecomp::viewer_master_system SDL3::SDL3)
  set_target_properties(segarecomp_viewer_master_system_sdl3 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
  segarecomp_enable_warnings(segarecomp_viewer_master_system_sdl3)
  if(BUILD_TESTING AND UNIX)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    # SEG-057-T002 (ADR 0119): the real viewer program speaks the typed unadmitted_pc contract (exit status 7), under SDL's dummy drivers.
    add_test(NAME sms_optimized_policy_test_viewer COMMAND ${Python3_EXECUTABLE}
      ${PROJECT_SOURCE_DIR}/tests/sms_optimized_policy_test.py $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR}
      $<TARGET_FILE:sms_admission_emit> viewer "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>" "$<TARGET_LINKER_FILE_DIR:SDL3::SDL3>")
    set_property(TEST sms_optimized_policy_test_viewer APPEND PROPERTY LABELS full)
    set_property(TEST sms_optimized_policy_test_viewer APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
    set_property(TEST sms_optimized_policy_test_viewer PROPERTY TIMEOUT 3600)
  endif()
endif()
