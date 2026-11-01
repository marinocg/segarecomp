# SEG-058: shared in-game viewer foundation (settings, layout/CRT math and menu model are SDL-free; the host is SDL3).
# Strict C11 (the same dialect the per-game packaged build compiles with). Never linked by headless targets.
add_library(segarecomp_viewer_core STATIC src/sv_settings.c src/sv_layout.c src/sv_menu.c src/sv_input_state.c src/sv_font.c src/sv_font_data.c)
add_library(segarecomp::viewer_core ALIAS segarecomp_viewer_core)
target_include_directories(segarecomp_viewer_core PUBLIC include)
set_target_properties(segarecomp_viewer_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(NOT WIN32)
  target_link_libraries(segarecomp_viewer_core PUBLIC m)
endif()
segarecomp_enable_warnings(segarecomp_viewer_core)

# Input diagnostics (SDL3, no window): shared by the launcher and the viewer host so there is one detection engine.
if(SEGARECOMP_ENABLE_SDL3_VIEWER OR SEGARECOMP_BUILD_LAUNCHER)
  find_package(SDL3 REQUIRED CONFIG)
  add_library(segarecomp_viewer_input STATIC src/sv_input_diag.c)
  add_library(segarecomp::viewer_input ALIAS segarecomp_viewer_input)
  target_include_directories(segarecomp_viewer_input PUBLIC include)
  target_link_libraries(segarecomp_viewer_input PUBLIC segarecomp::viewer_core SDL3::SDL3)
  set_target_properties(segarecomp_viewer_input PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  segarecomp_enable_warnings(segarecomp_viewer_input)
endif()

if(SEGARECOMP_ENABLE_SDL3_VIEWER)
  add_library(segarecomp_viewer_host STATIC src/sv_host.c src/sv_pages.c src/sv_draw.c src/sv_ui.c)
  add_library(segarecomp::viewer_host ALIAS segarecomp_viewer_host)
  target_include_directories(segarecomp_viewer_host PUBLIC include PRIVATE src)
  target_link_libraries(segarecomp_viewer_host PUBLIC segarecomp::viewer_core segarecomp::viewer_input SDL3::SDL3)
  set_target_properties(segarecomp_viewer_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  segarecomp_enable_warnings(segarecomp_viewer_host)
endif()

if((SEGARECOMP_ENABLE_SDL3_VIEWER OR SEGARECOMP_BUILD_LAUNCHER) AND BUILD_TESTING)
  # The diagnostics against SDL's virtual joysticks (real SDL events): A-F classification, hot-plug, open failure, user mapping file.
  add_executable(sv_input_diag_test test/sv_input_diag_test.c)
  target_link_libraries(sv_input_diag_test PRIVATE segarecomp::viewer_input)
  set_target_properties(sv_input_diag_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  segarecomp_enable_warnings(sv_input_diag_test)
  add_test(NAME sv_input_diag_test COMMAND sv_input_diag_test)
  set_property(TEST sv_input_diag_test APPEND PROPERTY LABELS full fast)
  set_property(TEST sv_input_diag_test APPEND PROPERTY ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
endif()

if(SEGARECOMP_ENABLE_SDL3_VIEWER AND BUILD_TESTING)
  # The SDL3 host under the dummy video driver, the software renderer and virtual gamepads (no display, audio device or ROM).
  add_executable(sv_host_test test/sv_host_test.c)
  target_link_libraries(sv_host_test PRIVATE segarecomp::viewer_host)
  set_target_properties(sv_host_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  segarecomp_enable_warnings(sv_host_test)
  # Documentation/review tool, not a test: renders the viewer's screens from a fictional picture.
  add_executable(sv_gallery test/sv_gallery.c test/sv_gallery_scene.c)  # review art: this tool only, never the library or a package
  target_link_libraries(sv_gallery PRIVATE segarecomp::viewer_host)
  set_target_properties(sv_gallery PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  segarecomp_enable_warnings(sv_gallery)
  # SEG-123: Comfort / Borders / Raw side presentation and the authored-widescreen bypass, read back from the rendered output on every geometry.
  add_executable(sv_side_style_test test/sv_side_style_test.c)
  target_link_libraries(sv_side_style_test PRIVATE segarecomp::viewer_host)
  set_target_properties(sv_side_style_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  segarecomp_enable_warnings(sv_side_style_test)
  add_test(NAME sv_side_style_test COMMAND sv_side_style_test)
  set_property(TEST sv_side_style_test APPEND PROPERTY LABELS full fast)
  set_property(TEST sv_side_style_test PROPERTY TIMEOUT 240)
  add_test(NAME sv_host_test COMMAND sv_host_test)
  set_property(TEST sv_host_test APPEND PROPERTY LABELS full fast)
  set_property(TEST sv_host_test PROPERTY TIMEOUT 240)  # a stuck overlay loop must fail with its output, not hang the job
endif()
