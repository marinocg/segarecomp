add_executable(segarecomp main.cpp)
target_link_libraries(segarecomp PRIVATE segarecomp::media segarecomp::machine_genesis
  segarecomp::codegen_c11_genesis segarecomp::legacy_compat)
segarecomp_enable_warnings(segarecomp)
