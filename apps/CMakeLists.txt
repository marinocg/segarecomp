add_subdirectory(segarecomp)
add_subdirectory(m68k-primary-word-classify)

# Consumer launcher and release packaging (opt-in; needs SDL3 and downloads Dear ImGui at configure time).
option(SEGARECOMP_BUILD_LAUNCHER "Build the consumer launcher (SDL3 + Dear ImGui)" OFF)
if(SEGARECOMP_BUILD_LAUNCHER)
  # Pinned bundled C compiler (Zig, used only to compile generated C); see packaging/README.md.
  set(SEGARECOMP_ZIG_VERSION "0.15.2" CACHE STRING "Pinned Zig version bundled as the C toolchain")
  set(SEGARECOMP_RELEASE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version shown by the launcher and part of the build-cache key")
  add_subdirectory(segarecomp-launcher)
endif()
