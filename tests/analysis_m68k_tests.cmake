# SEG-029-T003: M68K first-consumer adapter tests (registered by the T003 owner).
add_executable(analysis_m68k_equivalence_test analysis_m68k_equivalence_test.cpp)
target_link_libraries(analysis_m68k_equivalence_test PRIVATE segarecomp::cpu_m68k_analysis segarecomp::machine_genesis)
segarecomp_enable_warnings(analysis_m68k_equivalence_test)
add_test(NAME analysis_m68k_equivalence_test COMMAND analysis_m68k_equivalence_test)
set_property(TEST analysis_m68k_equivalence_test APPEND PROPERTY LABELS full fast)

# SEG-030-T003 (ADR 0079): the CPU-owned address domain and the (An) control families.
add_executable(analysis_m68k_value_test analysis_m68k_value_test.cpp)
target_link_libraries(analysis_m68k_value_test PRIVATE segarecomp::cpu_m68k_analysis)
segarecomp_enable_warnings(analysis_m68k_value_test)
add_test(NAME analysis_m68k_value_test COMMAND analysis_m68k_value_test)
set_property(TEST analysis_m68k_value_test APPEND PROPERTY LABELS full fast)

# SEG-030-T004 (ADR 0079): the CPU-owned abstract memory, alias exclusion and the asynchronous/external writer model.
add_executable(analysis_m68k_memory_test analysis_m68k_memory_test.cpp)
target_link_libraries(analysis_m68k_memory_test PRIVATE segarecomp::cpu_m68k_analysis)
segarecomp_enable_warnings(analysis_m68k_memory_test)
add_test(NAME analysis_m68k_memory_test COMMAND analysis_m68k_memory_test)
set_property(TEST analysis_m68k_memory_test APPEND PROPERTY LABELS full fast)

# SEG-030-T005 (ADR 0079): bounded call contexts and callee summaries.
add_executable(analysis_m68k_contexts_test analysis_m68k_contexts_test.cpp)
target_link_libraries(analysis_m68k_contexts_test PRIVATE segarecomp::cpu_m68k_analysis)
segarecomp_enable_warnings(analysis_m68k_contexts_test)
add_test(NAME analysis_m68k_contexts_test COMMAND analysis_m68k_contexts_test)
set_property(TEST analysis_m68k_contexts_test APPEND PROPERTY LABELS full fast)
