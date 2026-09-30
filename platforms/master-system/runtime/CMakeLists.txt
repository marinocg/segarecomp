# Standalone strict-C11 Master System runtime linked by generated programs (ADR 0064 section 1); never links host C++
# code. The Z80 runtime ABI header is only included from its owner (codegen_c11 include tree), never copied.
add_library(segarecomp_runtime_master_system STATIC
  sms_memory.c
  sms_sha256.c
  sms_input.c
  sms_machine.c
  sms_psg.c
  sms_vdp.c
)
add_library(segarecomp::runtime_master_system ALIAS segarecomp_runtime_master_system)
set_target_properties(segarecomp_runtime_master_system PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
)
target_include_directories(segarecomp_runtime_master_system PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/libs/codegen/c11/include)
target_link_libraries(segarecomp_runtime_master_system PUBLIC segarecomp::device_psg)
segarecomp_enable_warnings(segarecomp_runtime_master_system)
