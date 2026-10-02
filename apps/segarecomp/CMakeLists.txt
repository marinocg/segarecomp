add_executable(segarecomp main.cpp build_command.cpp)
find_package(Threads REQUIRED)
target_link_libraries(segarecomp PRIVATE segarecomp::media segarecomp::machine_genesis segarecomp::machine_genesis_z80 segarecomp::machine_master_system
  segarecomp::codegen_c11_genesis segarecomp::legacy_compat Threads::Threads)
segarecomp_enable_warnings(segarecomp)
