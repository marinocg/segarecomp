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

# SEG-030-T006 (ADR 0079): interrupt mask, handler instances, exception frames and RTE/RTR/computed-RTS provenance.
add_executable(analysis_m68k_frames_test analysis_m68k_frames_test.cpp)
target_link_libraries(analysis_m68k_frames_test PRIVATE segarecomp::cpu_m68k_analysis)
segarecomp_enable_warnings(analysis_m68k_frames_test)
add_test(NAME analysis_m68k_frames_test COMMAND analysis_m68k_frames_test)
set_property(TEST analysis_m68k_frames_test APPEND PROPERTY LABELS full fast)

# SEG-030-T008 (ADR 0079 decision 8): the return slot of an RTS at the entry stack delta and the return-slot integrity premise.
add_executable(analysis_m68k_return_slot_test analysis_m68k_return_slot_test.cpp)
target_link_libraries(analysis_m68k_return_slot_test PRIVATE segarecomp::cpu_m68k_analysis)
segarecomp_enable_warnings(analysis_m68k_return_slot_test)
add_test(NAME analysis_m68k_return_slot_test COMMAND analysis_m68k_return_slot_test)
set_property(TEST analysis_m68k_return_slot_test APPEND PROPERTY LABELS full fast)

# SEG-030-T008 part 2 (ADR 0079 decision 8): the seeded randomized differential of the analysis against a bounded test-only
# concrete executor (fixed seed list 0x5E6030008 + 0..399; never linked into production). It requires 0 unsound results and 0
# non-deterministic runs in all three configurations (premise violations, and in the historical baseline/contexts configurations the
# divergences caused only by handler register writes, are counted apart; SEG-030-T009 correction cycle). The second entry pins one seed beyond
# the list (base + 1920, found by a 2,000-seed run) that crashed the contexts derivation on a pinned computed call site (ADR 0079 T008 record).
add_executable(analysis_m68k_differential_test analysis_m68k_differential_test.cpp)
target_link_libraries(analysis_m68k_differential_test PRIVATE segarecomp::cpu_m68k_analysis)
segarecomp_enable_warnings(analysis_m68k_differential_test)
add_test(NAME analysis_m68k_differential_test COMMAND analysis_m68k_differential_test)
add_test(NAME analysis_m68k_differential_pinned_call_test COMMAND analysis_m68k_differential_test 1 0x5E6030788)
set_property(TEST analysis_m68k_differential_test analysis_m68k_differential_pinned_call_test APPEND PROPERTY LABELS full)
set_property(TEST analysis_m68k_differential_test PROPERTY TIMEOUT 600)
