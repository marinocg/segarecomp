# Consumer launcher (SDL3 + Dear ImGui). Opt-in: never linked by headless/library/test targets.
find_package(SDL3 REQUIRED CONFIG)

include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
  URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(segarecomp_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
# SYSTEM: third-party code is not held to the project warning policy.
target_include_directories(segarecomp_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(segarecomp_imgui PUBLIC SDL3::SDL3)

add_executable(segarecomp-launcher WIN32 launcher_core.cpp main.cpp)
target_link_libraries(segarecomp-launcher PRIVATE segarecomp_imgui segarecomp::media)
target_compile_definitions(segarecomp-launcher PRIVATE
  SEGARECOMP_LAUNCHER_VERSION="${SEGARECOMP_RELEASE_VERSION}"
  SEGARECOMP_ZIG_VERSION="${SEGARECOMP_ZIG_VERSION}")
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

include(${PROJECT_SOURCE_DIR}/packaging/install.cmake)
