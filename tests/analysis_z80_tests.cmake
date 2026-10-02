# SEG-029-T004: Z80 second-CPU adapter tests (registered by the T004 owner).
add_executable(analysis_z80_adapter_test analysis_z80_adapter_test.cpp)
target_link_libraries(analysis_z80_adapter_test PRIVATE segarecomp::cpu_z80_analysis)
segarecomp_enable_warnings(analysis_z80_adapter_test)
add_test(NAME analysis_z80_adapter_test COMMAND analysis_z80_adapter_test)
set_property(TEST analysis_z80_adapter_test APPEND PROPERTY LABELS full fast)
