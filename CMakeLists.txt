cmake_minimum_required(VERSION 3.25)

project(segarecomp VERSION 0.1.0 LANGUAGES C CXX)

option(SEGARECOMP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(WIN32)
  # The MSVC CRT flags standard fopen/getenv/etc. as deprecated ("unsafe"); the
  # project uses the ISO C functions deliberately with explicit bounds checks.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  # The Genesis runtime keeps whole-machine state (RAM/VRAM/framebuffer) in
  # automatic storage; POSIX hosts provide 8 MiB main-thread stacks but the
  # Windows default is 1 MiB. Match the POSIX reservation for every executable.
  if(MSVC)
    add_link_options(/STACK:8388608)
  else()
    add_link_options(-Wl,/STACK:8388608)
  endif()
endif()

function(segarecomp_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(SEGARECOMP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(SEGARECOMP_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# SEG-018-T002: the root is workspace orchestration only. Reusable libraries live
# under libs/, machine/platform products under platforms/, applications under
# apps/, and the test suite under tests/.
add_subdirectory(libs)
add_subdirectory(platforms)
add_subdirectory(apps)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
