# CI sharding: every hermetic test (label `full`, which contains `fast`) gets exactly one `shard-N` label so that CI can build once per
# matrix job and run `ctest -L ^shard-N$`; the union of the shards is the `full` tier, with no test dropped or duplicated.
#
# The assignment is a deterministic longest-processing-time greedy balance: tests are taken in descending estimated cost (ties by name)
# and each goes to the currently least loaded shard (ties to the lowest number). A test missing from the cost table gets
# SEGARECOMP_CI_DEFAULT_COST and is still assigned, so a new test can never be skipped by CI. The estimate is also the CTest COST
# property, which makes `ctest -j` start the longest tests first within a shard.
#
# Costs are seconds of one test running alongside three others on a 4-core runner, taken from the slowest recorded host (Windows,
# LLVM/Clang) of the CI run before sharding; the split entries are the unsplit test's cost distributed over its slices by their
# measured relative durations. They only steer the balance: the guard test (ci_shard_guard_test.py) fails if a shard's estimate
# exceeds SEGARECOMP_CI_SHARD_BUDGET_SECONDS or a single test exceeds SEGARECOMP_CI_MAX_TEST_SECONDS. Update the table when a test
# gets materially slower or faster (a stale entry only unbalances the shards; it never skips a test).
#
# Shard count: wall time of one CI job ~ setup/build (~2 min) + shard cost / 4 cores. Six shards keep the slowest host under ~7 min.
set(SEGARECOMP_CI_SHARD_COUNT 6)
set(SEGARECOMP_CI_SHARD_BUDGET_SECONDS 1300)
set(SEGARECOMP_CI_MAX_TEST_SECONDS 400)
set(SEGARECOMP_CI_DEFAULT_COST 3)
set(SEGARECOMP_CI_TEST_COSTS
  z80_owner_group_differential_test=196 z80_owner_group_differential_test_slice2=208
  z80_owner_group_differential_test_slice3=214 z80_owner_group_differential_test_slice4=231
  genesis_z80_build_pipeline_test=150 genesis_z80_build_pipeline_bound_test=145 genesis_z80_build_pipeline_smc_test=205
  genesis_z80_build_pipeline_falsify_test=195 genesis_z80_build_pipeline_prepare_test=20
  sms_native_build_test=127 sms_native_build_policy_test=226 sms_native_build_failclosed_test=21
  sms_owner_group_equivalence_test=110 sms_owner_group_equivalence_vdp_test=110 sms_owner_group_equivalence_render_test=110
  m68k_pipeline_tests=369 sms_emission_budget_512k_test=307 z80_live_operand_test=303 z80_generated_pipeline_test=195
  m68k_conformance_harness_test=192 m68k_capability_ratchet_test=187 z80_live_guard_test=152 m68k_batch_c_static_slice_test=136
  z80_conformance_harness_test=130 sms_machine_scheduler_test=121 sms_render_test=119 sms_render_native_test=96
  sms_machine_e2e_test=88 sms_emission_budget_test=87 sms_vdp_native_test=86 sms_bank_crossing_test=81 genesis_audio_artifact_test=81
  z80_capability_ratchet_test=65 segarecomp_build_command_test=60 z80_timing_closure_test=54 genesis_z80_machine_test=50
  sms_psg_differential_test=45 cpu_z80_decode_tests=41 genesis_z80_images_test=38 genesis_audio_equivalence_test=36
  sms_pad_native_test=35 sms_viewer_equivalence_test=34 sms_machine_tests=33 genesis_audio_ym_test=27
  z80_control_stack_dispatch_test=25 ci_shard_guard_test=5
  sonic_startup_inventory_adapter_failure_test=80)  # RUN_SERIAL: runs alone, so it occupies all four cores

# The guard runs inside the shard it is assigned to (it only lists tests, so it is cheap) and must exist before the partition below.
add_test(NAME ci_shard_guard_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ci_shard_guard_test.py
  ${CMAKE_CTEST_COMMAND} ${CMAKE_BINARY_DIR} ${PROJECT_SOURCE_DIR} ${SEGARECOMP_CI_SHARD_COUNT}
  ${SEGARECOMP_CI_SHARD_BUDGET_SECONDS} ${SEGARECOMP_CI_MAX_TEST_SECONDS})
set_property(TEST ci_shard_guard_test APPEND PROPERTY LABELS full fast)
set_property(TEST ci_shard_guard_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")

foreach(_entry IN LISTS SEGARECOMP_CI_TEST_COSTS)
  string(REGEX MATCH "^([^=]+)=([0-9]+)$" _m "${_entry}")
  if(NOT _m)
    message(FATAL_ERROR "ci_shards.cmake: malformed cost entry '${_entry}'")
  endif()
  set(_segarecomp_cost_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
endforeach()

# Sort key: the cost zero-padded so that a string sort is a numeric sort; the name breaks ties deterministically.
get_property(_segarecomp_ci_all_tests DIRECTORY PROPERTY TESTS)
set(_segarecomp_ci_keys)
foreach(_t IN LISTS _segarecomp_ci_all_tests)
  get_test_property(${_t} LABELS _labels)
  if("full" IN_LIST _labels)
    if(DEFINED _segarecomp_cost_${_t})
      set(_cost ${_segarecomp_cost_${_t}})
    else()
      set(_cost ${SEGARECOMP_CI_DEFAULT_COST})
    endif()
    string(LENGTH "${_cost}" _len)
    math(EXPR _pad "6 - ${_len}")
    string(REPEAT "0" ${_pad} _zeros)
    list(APPEND _segarecomp_ci_keys "${_zeros}${_cost}:${_t}")
  endif()
endforeach()
list(SORT _segarecomp_ci_keys COMPARE STRING ORDER DESCENDING)

foreach(_n RANGE 1 ${SEGARECOMP_CI_SHARD_COUNT})
  set(_segarecomp_ci_load_${_n} 0)
endforeach()
foreach(_key IN LISTS _segarecomp_ci_keys)
  string(REGEX MATCH "^([0-9]+):(.+)$" _m "${_key}")
  math(EXPR _cost "${CMAKE_MATCH_1}")  # drops the zero padding
  set(_t "${CMAKE_MATCH_2}")
  set(_best 1)
  foreach(_n RANGE 2 ${SEGARECOMP_CI_SHARD_COUNT})
    if(_segarecomp_ci_load_${_n} LESS _segarecomp_ci_load_${_best})
      set(_best ${_n})
    endif()
  endforeach()
  math(EXPR _segarecomp_ci_load_${_best} "${_segarecomp_ci_load_${_best}} + ${_cost}")
  set_property(TEST ${_t} APPEND PROPERTY LABELS shard-${_best})
  set_property(TEST ${_t} PROPERTY COST ${_cost})
endforeach()

