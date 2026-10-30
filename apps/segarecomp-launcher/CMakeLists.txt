# Consumer launcher (SDL3 + Dear ImGui). Opt-in: never linked by headless/library/test targets.
find_package(SDL3 REQUIRED CONFIG)

include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
  URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

# stb_image.h (public domain / MIT dual-license, single header): the only decoder needed for the
# launcher's own PNG pixel-art assets. Pinned by content hash; not a general image-library dependency.
set(_stb_dir "${CMAKE_CURRENT_BINARY_DIR}/stb")
file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/f0569113c93ad095470c54bf34a17b36646bbbb/stb_image.h"
  "${_stb_dir}/stb_image.h"
  EXPECTED_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)

add_library(segarecomp_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
# SYSTEM: third-party code is not held to the project warning policy.
target_include_directories(segarecomp_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(segarecomp_imgui PUBLIC SDL3::SDL3)

# The launcher's non-UI core (layout, cache identity, `segarecomp build` orchestration, status reading) is a separate target so its
# policy/cache-identity contract is unit-testable without a window.
add_library(segarecomp_launcher_core STATIC launcher_core.cpp older_build.cpp file_picker.cpp process_tree.cpp json_lite.cpp library_catalog.cpp storage.cpp launcher_settings.cpp sega_cd.cpp)
target_include_directories(segarecomp_launcher_core PUBLIC .)
target_link_libraries(segarecomp_launcher_core PUBLIC SDL3::SDL3 segarecomp::media)
# Linux only: the glibc the generated programs link against (release default 2.35 = the bundled SDL3's baseline; a developer who built SDL3 on a
# newer distribution overrides it so a locally staged package links).
set(SEGARECOMP_ZIG_GLIBC "2.35" CACHE STRING "Linux glibc version the generated programs target")
target_compile_definitions(segarecomp_launcher_core PUBLIC
  SEGARECOMP_LAUNCHER_VERSION="${SEGARECOMP_RELEASE_VERSION}"
  SEGARECOMP_ZIG_VERSION="${SEGARECOMP_ZIG_VERSION}"
  SEGARECOMP_ZIG_GLIBC="${SEGARECOMP_ZIG_GLIBC}")
segarecomp_enable_warnings(segarecomp_launcher_core)

# Image loading (the launcher's pixel-art assets and user-chosen covers) and the cover/Libretro naming helpers; no window needed.
add_library(segarecomp_launcher_ui_support STATIC pixel_assets.cpp covers.cpp cover_download.cpp rom_zip.cpp)
target_include_directories(segarecomp_launcher_ui_support SYSTEM PRIVATE ${_stb_dir})
target_include_directories(segarecomp_launcher_ui_support PUBLIC .)
target_link_libraries(segarecomp_launcher_ui_support PUBLIC SDL3::SDL3 segarecomp_launcher_core)
target_compile_features(segarecomp_launcher_ui_support PUBLIC cxx_std_20)
target_compile_definitions(segarecomp_launcher_ui_support PUBLIC
  SEGARECOMP_LAUNCHER_SOURCE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
segarecomp_enable_warnings(segarecomp_launcher_ui_support)

add_executable(segarecomp-launcher WIN32 main.cpp app.cpp app_sega_cd.cpp app_picker.cpp app_screens.cpp app_pages.cpp app_details.cpp app_input.cpp app_art.cpp ui_kit.cpp)
if(WIN32)
  # The executable's own icon (Explorer, taskbar, shortcuts): a resource script embedding packaging/windows/Segarecomp.ico, when a resource compiler exists.
  include(CheckLanguage)
  check_language(RC)
  if(CMAKE_RC_COMPILER)
    enable_language(RC)
    target_sources(segarecomp-launcher PRIVATE ${PROJECT_SOURCE_DIR}/packaging/windows/segarecomp.rc)
  endif()
endif()
target_include_directories(segarecomp-launcher SYSTEM PRIVATE ${_stb_dir})
target_link_libraries(segarecomp-launcher PRIVATE segarecomp_imgui segarecomp_launcher_core segarecomp_launcher_ui_support segarecomp::viewer_core segarecomp::viewer_input segarecomp::media)
set_target_properties(segarecomp-launcher PROPERTIES OUTPUT_NAME Segarecomp)
segarecomp_enable_warnings(segarecomp-launcher)

# The launcher finds SDL3 in the package (sdl3/lib) at run time.
if(APPLE)
  set_target_properties(segarecomp-launcher PROPERTIES BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@executable_path/../Resources/sdl3/lib")
elseif(UNIX)
  set_target_properties(segarecomp-launcher PROPERTIES BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "$ORIGIN/sdl3/lib")
endif()

# SEG-047-T009 (ADR 0096): the AOT-policy cache identity and status reading (no window). Registered with CTest; the CI SDL3 job (macOS/Linux/Windows) builds and runs it with `ctest -R launcher_policy_test`. Never a POST_BUILD step (it must not gate packaging on runtime SDL3 DLL lookup).
add_executable(launcher_policy_test launcher_policy_test.cpp)
target_link_libraries(launcher_policy_test PRIVATE segarecomp_launcher_core)
segarecomp_enable_warnings(launcher_policy_test)
# SEG-057-T002 (ADR 0119): product-CLI and generated-program doubles for the launcher's end-to-end restart tests (test-only, not installed).
add_executable(fake_segarecomp fake_segarecomp.cpp)
add_executable(fake_game fake_game.cpp)
target_compile_features(fake_segarecomp PRIVATE cxx_std_20)
target_compile_features(fake_game PRIVATE cxx_std_20)
segarecomp_enable_warnings(fake_segarecomp)
segarecomp_enable_warnings(fake_game)
if(BUILD_TESTING)
  # SEG-049: the real CLI provides the policy identity the launcher keys on (no launcher-side model knowledge); built as a dependency.
  add_dependencies(launcher_policy_test segarecomp)
  add_test(NAME launcher_policy_test COMMAND launcher_policy_test $<TARGET_FILE:segarecomp>)
  set_property(TEST launcher_policy_test APPEND PROPERTY LABELS full fast)
endif()

# SEG-057-T002 (ADR 0119): the Master System Optimized contract of the launcher: product-stated availability, cache separation, the typed
# unadmitted_pc restart decision and the end-to-end headless flow (build, run, one-shot Compatibility restart) against product/program doubles.
add_executable(launcher_sms_policy_test launcher_sms_policy_test.cpp)
target_link_libraries(launcher_sms_policy_test PRIVATE segarecomp_launcher_core)
segarecomp_enable_warnings(launcher_sms_policy_test)
if(BUILD_TESTING)
  add_dependencies(launcher_sms_policy_test segarecomp fake_segarecomp fake_game segarecomp-launcher)
  add_test(NAME launcher_sms_policy_test COMMAND launcher_sms_policy_test $<TARGET_FILE:segarecomp>
    ${PROJECT_SOURCE_DIR}/platforms/master-system/runtime/sms_machine.h $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:fake_segarecomp> $<TARGET_FILE:fake_game>)
  set_property(TEST launcher_sms_policy_test APPEND PROPERTY LABELS full fast)
endif()

# SEG-069-T001: BuildJob cancellation (process-tree shutdown, attempt-local cleanup, publish race) against the product double.
add_executable(launcher_build_cancel_test launcher_build_cancel_test.cpp)
target_link_libraries(launcher_build_cancel_test PRIVATE segarecomp_launcher_core)
target_compile_features(launcher_build_cancel_test PRIVATE cxx_std_20)
segarecomp_enable_warnings(launcher_build_cancel_test)
if(BUILD_TESTING)
  add_dependencies(launcher_build_cancel_test fake_segarecomp fake_game)
  add_test(NAME launcher_build_cancel_test COMMAND launcher_build_cancel_test $<TARGET_FILE:fake_segarecomp> $<TARGET_FILE:fake_game>)
  set_property(TEST launcher_build_cancel_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_build_cancel_test PROPERTY TIMEOUT 300)
endif()

if(BUILD_TESTING)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_test(NAME launcher_build_cancel_gui_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_build_cancel_gui_test.py
    $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:fake_segarecomp> $<TARGET_FILE:fake_game>)
  add_dependencies(launcher_build_cancel_test segarecomp-launcher)
  set_property(TEST launcher_build_cancel_gui_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_build_cancel_gui_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
  set_property(TEST launcher_build_cancel_gui_test PROPERTY TIMEOUT 300)
endif()

# SEG-069-T002: Optimized is the default for new Genesis (gen2) and Master System games, with the product-stated SMS capability gate.
if(BUILD_TESTING)
  add_test(NAME launcher_default_mode_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_default_mode_test.py
    $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:fake_segarecomp> $<TARGET_FILE:fake_game>)
  set_property(TEST launcher_default_mode_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_default_mode_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
  set_property(TEST launcher_default_mode_test PROPERTY TIMEOUT 600)
endif()

# SEG-069-T003: the one-time first-run question about build intermediates (real window, virtual gamepad).
if(BUILD_TESTING)
  add_test(NAME launcher_first_run_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_first_run_test.py
    $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:fake_segarecomp> $<TARGET_FILE:fake_game>)
  set_property(TEST launcher_first_run_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_first_run_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
  set_property(TEST launcher_first_run_test PROPERTY TIMEOUT 600)
endif()

# SEG-069-T005: the playability predicate for builds of an earlier release (fail-closed, separate from the exact reuse identity).
add_executable(older_build_test older_build_test.cpp)
target_link_libraries(older_build_test PRIVATE segarecomp_launcher_core)
target_compile_features(older_build_test PRIVATE cxx_std_20)
segarecomp_enable_warnings(older_build_test)
if(BUILD_TESTING)
  add_test(NAME older_build_test COMMAND older_build_test)
  set_property(TEST older_build_test APPEND PROPERTY LABELS full fast)
endif()

if(BUILD_TESTING)
  add_test(NAME launcher_older_build_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_older_build_test.py
    $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:fake_segarecomp> $<TARGET_FILE:fake_game>)
  set_property(TEST launcher_older_build_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_older_build_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
  set_property(TEST launcher_older_build_test PROPERTY TIMEOUT 900)
endif()

# SEG-078-T002 (ADR 0204): the Sega CD core of the launcher (identity, the program's statement, verification, typed refusals, import, run arguments,
# outcomes, the library record and its schema). A program double stands in for the generated viewer program; no window, no compiler.
add_executable(fake_scd_program fake_scd_program.cpp)
target_compile_features(fake_scd_program PRIVATE cxx_std_20)
segarecomp_enable_warnings(fake_scd_program)
add_executable(launcher_sega_cd_test launcher_sega_cd_test.cpp)
target_link_libraries(launcher_sega_cd_test PRIVATE segarecomp_launcher_core)
target_compile_features(launcher_sega_cd_test PRIVATE cxx_std_20)
segarecomp_enable_warnings(launcher_sega_cd_test)
if(BUILD_TESTING)
  add_dependencies(launcher_sega_cd_test fake_scd_program)
  add_test(NAME launcher_sega_cd_test COMMAND launcher_sega_cd_test $<TARGET_FILE:fake_scd_program>)
  set_property(TEST launcher_sega_cd_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_sega_cd_test PROPERTY TIMEOUT 300)
endif()

# SEG-108 (ADR 0252): the save functions over a command line, so the generated-native persistence test drives the launcher's own code.
add_executable(scd_save_tool scd_save_tool.cpp)
target_link_libraries(scd_save_tool PRIVATE segarecomp_launcher_core)
target_compile_features(scd_save_tool PRIVATE cxx_std_20)
segarecomp_enable_warnings(scd_save_tool)
if(BUILD_TESTING)
  add_test(NAME launcher_sega_cd_save_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_sega_cd_save_test.py
    $<TARGET_FILE:scd_save_tool> $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
  set_property(TEST launcher_sega_cd_save_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_sega_cd_save_test PROPERTY TIMEOUT 900)
endif()

if(BUILD_TESTING AND SEGARECOMP_ENABLE_SDL3_VIEWER)
  # The whole flow without a window: launcher -> verified program (the real viewer variant of a project-authored fixture) -> SDL3 viewer under the dummy
  # drivers -> scripted keys -> close / typed stop, plus the typed refusals. Skips itself without SDL3 or off Linux.
  get_filename_component(_scd_flow_sdl_dir "${SDL3_DIR}" ABSOLUTE)
  if(_scd_flow_sdl_dir MATCHES "/share/sdl3$")
    get_filename_component(_scd_flow_sdl_prefix "${_scd_flow_sdl_dir}/../.." ABSOLUTE)
  else()
    get_filename_component(_scd_flow_sdl_prefix "${_scd_flow_sdl_dir}/../../.." ABSOLUTE)
  endif()
  add_test(NAME launcher_sega_cd_flow_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_sega_cd_flow_test.py
    $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:fake_scd_program> $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
  add_test(NAME launcher_sega_cd_gui_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_sega_cd_gui_test.py
    $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
  set_property(TEST launcher_sega_cd_gui_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_sega_cd_gui_test APPEND PROPERTY ENVIRONMENT "SDL3_PREFIX=${_scd_flow_sdl_prefix}")
  set_property(TEST launcher_sega_cd_gui_test PROPERTY TIMEOUT 900)
  add_dependencies(launcher_sega_cd_test segarecomp-launcher)
  set_property(TEST launcher_sega_cd_flow_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_sega_cd_flow_test APPEND PROPERTY ENVIRONMENT "SDL3_PREFIX=${_scd_flow_sdl_prefix}")
  set_property(TEST launcher_sega_cd_flow_test PROPERTY TIMEOUT 900)
endif()

# SEG-069-T004: the controller-operable file picker's model (root confinement, filtering, symlink refusal).
add_executable(file_picker_test file_picker_test.cpp)
target_link_libraries(file_picker_test PRIVATE segarecomp_launcher_core)
target_compile_features(file_picker_test PRIVATE cxx_std_20)
segarecomp_enable_warnings(file_picker_test)
if(BUILD_TESTING)
  add_test(NAME file_picker_test COMMAND file_picker_test)
  set_property(TEST file_picker_test APPEND PROPERTY LABELS full fast)
endif()

# SEG-069-T004: the launcher's ordinary journeys with a virtual gamepad only (no mouse event), at 1080x740 and 720x520, on the real window.
if(BUILD_TESTING)
  # One test per journey and window size: they are independent, slow on some hosts, and `ctest -j` runs them side by side.
  foreach(journey first_run import library build older_build settings storage hotplug)
    foreach(size wide:1080x740 narrow:720x520)
      string(REPLACE ":" ";" size_parts ${size})
      list(GET size_parts 0 size_name)
      list(GET size_parts 1 size_px)
      set(t launcher_gamepad_journey_${journey}_${size_name}_test)
      add_test(NAME ${t} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_gamepad_journey_test.py
        $<TARGET_FILE:segarecomp-launcher> $<TARGET_FILE:fake_segarecomp> $<TARGET_FILE:fake_game> ${size_px} ${journey})
      set_property(TEST ${t} APPEND PROPERTY LABELS full fast)
      set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
      set_property(TEST ${t} PROPERTY TIMEOUT 900)
    endforeach()
  endforeach()
endif()

# SEG-058: covers, Libretro candidate naming and launcher settings.
add_executable(launcher_ui_support_test launcher_ui_support_test.cpp)
target_link_libraries(launcher_ui_support_test PRIVATE segarecomp_launcher_core segarecomp_launcher_ui_support segarecomp_zlib)
segarecomp_enable_warnings(launcher_ui_support_test)
if(BUILD_TESTING)
  add_test(NAME launcher_ui_support_test COMMAND launcher_ui_support_test)
  set_property(TEST launcher_ui_support_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_ui_support_test PROPERTY TIMEOUT 300)  # seconds of work at most; never the 25-minute default
endif()

# SEG-058: keyboard and gamepad navigation of the real window (dummy video driver, virtual gamepad).
if(BUILD_TESTING)
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  add_test(NAME launcher_gui_navigation_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_gui_navigation_test.py $<TARGET_FILE:segarecomp-launcher>)
  set_property(TEST launcher_gui_navigation_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_gui_navigation_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
endif()

# SEG-058 corrective delivery: the Input diagnostics classes A-F against real SDL events from virtual joysticks (never a physical-device claim).
if(BUILD_TESTING)
  add_test(NAME launcher_input_diagnostics_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_input_diagnostics_test.py $<TARGET_FILE:segarecomp-launcher>)
  set_property(TEST launcher_input_diagnostics_test APPEND PROPERTY LABELS full fast)
  set_property(TEST launcher_input_diagnostics_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
endif()

# SEG-058: the persistent library catalog (no window, no product CLI).
add_executable(library_catalog_test library_catalog_test.cpp)
target_link_libraries(library_catalog_test PRIVATE segarecomp_launcher_core)
segarecomp_enable_warnings(library_catalog_test)
if(BUILD_TESTING)
  add_test(NAME library_catalog_test COMMAND library_catalog_test)
  set_property(TEST library_catalog_test APPEND PROPERTY LABELS full fast)
endif()

# SEG-058: cache maintenance on fake trees (symlink escapes, forged plans, busy entries).
add_executable(storage_test storage_test.cpp)
target_link_libraries(storage_test PRIVATE segarecomp_launcher_core)
segarecomp_enable_warnings(storage_test)
if(BUILD_TESTING)
  add_test(NAME storage_test COMMAND storage_test)
  set_property(TEST storage_test APPEND PROPERTY LABELS full fast)
endif()

include(${PROJECT_SOURCE_DIR}/packaging/install.cmake)
