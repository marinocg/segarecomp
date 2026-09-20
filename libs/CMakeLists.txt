# Reusable libraries, added in permitted dependency order: core -> cpu -> recompiler ->
# device -> media -> codegen -> legacy_compat. The root adds platforms/ (e.g. the Genesis
# machine) and apps/ after libs/. Target link dependencies are declared by alias, so the
# machine/codegen relationship (owned by later SEG-018 tasks) does not depend on directory order.
add_subdirectory(core)
add_subdirectory(cpu/m68k)
add_subdirectory(recompiler)
add_subdirectory(device/sega/genesis)
add_subdirectory(media)
add_subdirectory(codegen/c11)
add_subdirectory(legacy_compat)
