add_executable(segarecomp main.cpp build_command.cpp sega_cd_command.cpp sega_cd_build.cpp sega_cd_aot.cpp process_run.cpp patch_command.cpp mod_command.cpp luau_build.cpp)
find_package(Threads REQUIRED)
target_link_libraries(segarecomp PRIVATE segarecomp::media segarecomp::luau_host segarecomp::machine_genesis segarecomp::machine_genesis_z80 segarecomp::machine_genesis_z80_static segarecomp::machine_master_system segarecomp::machine_sega_cd
  segarecomp::codegen_c11_genesis segarecomp::legacy_compat Threads::Threads)
segarecomp_enable_warnings(segarecomp)

# SEG-137-T003 (ADR 0294): the bounded child of the trusted Luau compile step (metered allocator = the 256 MiB ceiling; the CLI's run_process kill = the 5 s limit). Must sit next to `segarecomp`.
add_executable(segarecomp-luau-compile luau_compile_main.cpp)
target_link_libraries(segarecomp-luau-compile PRIVATE segarecomp::luau_host)
segarecomp_enable_warnings(segarecomp-luau-compile)
add_dependencies(segarecomp segarecomp-luau-compile)
