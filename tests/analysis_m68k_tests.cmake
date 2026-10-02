# SEG-029-T003: M68K first-consumer adapter tests (registered by the T003 owner).
add_executable(analysis_m68k_equivalence_test analysis_m68k_equivalence_test.cpp)
target_link_libraries(analysis_m68k_equivalence_test PRIVATE segarecomp::cpu_m68k_analysis segarecomp::machine_genesis)
segarecomp_enable_warnings(analysis_m68k_equivalence_test)
add_test(NAME analysis_m68k_equivalence_test COMMAND analysis_m68k_equivalence_test)
set_property(TEST analysis_m68k_equivalence_test APPEND PROPERTY LABELS full fast)
