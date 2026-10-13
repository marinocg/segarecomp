# Reusable libraries, added in permitted dependency order: core -> cpu -> recompiler ->
# device -> media -> codegen -> legacy_compat. The root adds platforms/ (e.g. the Genesis
# machine) and apps/ after libs/. Target link dependencies are declared by alias, so the
# machine/codegen relationship (owned by later SEG-018 tasks) does not depend on directory order.
add_subdirectory(core)
add_subdirectory(cpu/m68k)
add_subdirectory(cpu/z80)
add_subdirectory(recompiler)
add_subdirectory(device/sega/genesis)
add_subdirectory(device/sega/psg)
add_subdirectory(device/sega/ym2612)
add_subdirectory(media)
add_subdirectory(codegen/c11)
add_subdirectory(legacy_compat)
# SEG-058: option must exist before libs/viewer (declared again, harmlessly, by platforms/genesis/viewer).
option(SEGARECOMP_ENABLE_SDL3_VIEWER "Build the optional SDL3 Genesis viewer presenter" OFF)
option(SEGARECOMP_BUILD_LAUNCHER "Build the consumer launcher (SDL3 + Dear ImGui)" OFF) # also read by libs/viewer (input diagnostics); apps/ declares it again, harmlessly
add_subdirectory(viewer)
