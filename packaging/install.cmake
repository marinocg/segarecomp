# Release staging: `cmake --install <build> --prefix <dir>` produces the self-contained consumer package.
# Included by apps/segarecomp-launcher/CMakeLists.txt. Inputs (all required for a consumer package):
#   SEGARECOMP_PACKAGE_ZIG_DIR      extracted pinned Zig release (zig[.exe], lib/, LICENSE)
#   SEGARECOMP_PACKAGE_SDL3_PREFIX  SDL3 prefix providing include/SDL3 and lib/ (libSDL3.so/.dylib or libSDL3.dll.a)
#   SEGARECOMP_PACKAGE_SDL3_LICENSE SDL3 license text
#   SEGARECOMP_PACKAGE_SDL3_DLL     Windows only: SDL3.dll shipped beside the launcher and in sdl3/lib
# Without them only the project's own files are staged (developer use).

if(APPLE)
  set(_exe_dest Segarecomp.app/Contents/MacOS)
  set(_res Segarecomp.app/Contents/Resources)
else()
  set(_exe_dest .)
  set(_res .)
endif()

install(TARGETS segarecomp-launcher RUNTIME DESTINATION ${_exe_dest})
install(TARGETS segarecomp RUNTIME DESTINATION ${_res}/bin)

# Stable runtime sources compiled together with the generated program by the bundled compiler. The tree
# mirrors the repository because the runtime headers include their contract headers by relative path.
set(_src ${PROJECT_SOURCE_DIR})
set(_rt ${_res}/runtime)
install(DIRECTORY ${_src}/platforms/genesis/runtime/ DESTINATION ${_rt}/platforms/genesis/runtime FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
install(DIRECTORY ${_src}/platforms/genesis/viewer/ DESTINATION ${_rt}/platforms/genesis/viewer FILES_MATCHING PATTERN "*.c" PATTERN "*.h")
install(DIRECTORY ${_src}/platforms/genesis/compat/ DESTINATION ${_rt}/platforms/genesis/compat FILES_MATCHING PATTERN "*.json")
install(DIRECTORY ${_src}/platforms/genesis/machine/include/ DESTINATION ${_rt}/platforms/genesis/machine/include FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${_src}/libs/device/sega/genesis/include/ DESTINATION ${_rt}/libs/device/sega/genesis/include FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${_src}/libs/cpu/m68k/include/ DESTINATION ${_rt}/libs/cpu/m68k/include FILES_MATCHING PATTERN "*.h")

install(DIRECTORY ${PROJECT_SOURCE_DIR}/apps/segarecomp-launcher/assets/ DESTINATION ${_res}/assets
  PATTERN "README.md" EXCLUDE PATTERN "Silkscreen-OFL.txt" EXCLUDE)

install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${_res}/licenses RENAME MPL-2.0.txt)
install(FILES ${PROJECT_SOURCE_DIR}/apps/segarecomp-launcher/assets/fonts/Silkscreen-OFL.txt DESTINATION ${_res}/licenses RENAME Silkscreen-OFL.txt)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/THIRD-PARTY-NOTICES.txt DESTINATION ${_res}/licenses)
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION ${_res}/licenses RENAME DearImGui.txt)
if(APPLE)
  set(SEGARECOMP_BUNDLE_VERSION "${PROJECT_VERSION}")
  configure_file(${PROJECT_SOURCE_DIR}/packaging/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/Info.plist DESTINATION Segarecomp.app/Contents)
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/README.txt DESTINATION .)
else()
  install(FILES ${PROJECT_SOURCE_DIR}/packaging/README.txt DESTINATION .)
endif()

if(SEGARECOMP_PACKAGE_ZIG_DIR)
  # Bundled pinned C toolchain (only used to compile generated C; the host needs no compiler).
  install(DIRECTORY ${SEGARECOMP_PACKAGE_ZIG_DIR}/ DESTINATION ${_res}/toolchain USE_SOURCE_PERMISSIONS
    PATTERN "doc" EXCLUDE PATTERN "README.md" EXCLUDE)
  install(FILES ${SEGARECOMP_PACKAGE_ZIG_DIR}/LICENSE DESTINATION ${_res}/licenses RENAME Zig.txt)
endif()
if(SEGARECOMP_PACKAGE_SDL3_PREFIX)
  install(DIRECTORY ${SEGARECOMP_PACKAGE_SDL3_PREFIX}/include/SDL3 DESTINATION ${_res}/sdl3/include)
  if(WIN32)
    # mingw's official devel package ships plain files (SDL3.dll, libSDL3.dll.a): no symlinks, no
    # duplication concern; a directory copy is simplest and already correct.
    install(DIRECTORY ${SEGARECOMP_PACKAGE_SDL3_PREFIX}/lib/ DESTINATION ${_res}/sdl3/lib
      FILES_MATCHING PATTERN "libSDL3.*" PATTERN "SDL3.dll")
  else()
    # Ship two real (non-symlink) COPIES of the one runtime library, under the two names actually needed --
    # never the symlink chain a naive `FILES_MATCHING PATTERN "libSDL3.*"` directory copy would stage
    # instead, which a later archiving/extraction step (observed: WSL's DrvFs) can turn into an empty or
    # truncated file where the real, larger target should be -- "file too short" at load time. Both names
    # are genuinely required at run time on an end user's machine (the launcher builds each game locally):
    # the bundled compiler's linker resolves a game's "-lSDL3" by searching for the unversioned
    # "libSDL3.<ext>", while the produced game binary's own embedded runtime dependency (its rpath-relative
    # install name/SONAME) names the versioned file. Resolving the real target ourselves at packaging time
    # and installing plain copies under both names means every shipped file is a genuine regular file,
    # immune to how any downstream archiver or filesystem handles symlinks -- at the cost of ~3 MB of
    # harmless duplication.
    if(APPLE)
      set(_sdl3_soname_name "libSDL3.0.dylib")   # SDL3's own install name (`@rpath/libSDL3.0.dylib`); real file, not a symlink
      set(_sdl3_linker_name "libSDL3.dylib")     # what "-lSDL3" searches for
    else()
      set(_sdl3_soname_name "libSDL3.so.0")      # SDL3's SONAME; a symlink to the real, patch-versioned file
      set(_sdl3_linker_name "libSDL3.so")        # what "-lSDL3" searches for
    endif()
    if(NOT EXISTS "${SEGARECOMP_PACKAGE_SDL3_PREFIX}/lib/${_sdl3_soname_name}")
      message(FATAL_ERROR "expected SDL3 runtime library not found: ${SEGARECOMP_PACKAGE_SDL3_PREFIX}/lib/${_sdl3_soname_name}")
    endif()
    file(REAL_PATH "${SEGARECOMP_PACKAGE_SDL3_PREFIX}/lib/${_sdl3_soname_name}" _sdl3_runtime_real)
    install(FILES ${_sdl3_runtime_real} DESTINATION ${_res}/sdl3/lib RENAME ${_sdl3_soname_name})
    install(FILES ${_sdl3_runtime_real} DESTINATION ${_res}/sdl3/lib RENAME ${_sdl3_linker_name})
  endif()
  if(SEGARECOMP_PACKAGE_SDL3_LICENSE)
    install(FILES ${SEGARECOMP_PACKAGE_SDL3_LICENSE} DESTINATION ${_res}/licenses RENAME SDL3.txt)
  endif()
  if(WIN32 AND SEGARECOMP_PACKAGE_SDL3_DLL)
    install(FILES ${SEGARECOMP_PACKAGE_SDL3_DLL} DESTINATION .)
    install(FILES ${SEGARECOMP_PACKAGE_SDL3_DLL} DESTINATION sdl3/lib)
  endif()
endif()
