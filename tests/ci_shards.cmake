# CI sharding: every hermetic test (label `full`, which contains `fast`) gets exactly one `shard-N` label so that CI can build once per
# matrix job and run `ctest -L ^shard-N$`; the union of the shards is the `full` tier, with no test dropped or duplicated.
#
# The assignment is a deterministic longest-processing-time greedy balance: tests are taken in descending estimated cost (ties by name)
# and each goes to the currently least loaded shard (ties to the lowest number). A test missing from the cost table gets
# SEGARECOMP_CI_DEFAULT_COST and is still assigned, so a new test can never be skipped by CI. The estimate is also the CTest COST
# property, which makes `ctest -j` start the longest tests first within a shard.
#
# Costs are seconds of one test running alongside three others on a 4-core runner, taken from the slowest recorded host (Linux, GCC)
# of the sharded CI run on the trimmed tests (the contended per-test durations of run 36996711623); the split entries are the unsplit test's cost distributed over its slices by their
# measured relative durations. They only steer the balance: the guard test (ci_shard_guard_test.py) fails if a shard's estimate
# exceeds SEGARECOMP_CI_SHARD_BUDGET_SECONDS or a single test exceeds SEGARECOMP_CI_MAX_TEST_SECONDS. Update the table when a test
# gets materially slower or faster (a stale entry only unbalances the shards; it never skips a test).
#
# The Genesis Z80/audio tests were rescaled by their measured before/after ratio when they were trimmed (shared builds, batched runner
# invocations, --runtime-optimize 1; see docs/testing/z80-build-performance.md), not re-measured on the runners: re-measure on the next CI run.
#
# Shard count: wall time of one CI job ~ setup/build (~2 min) + shard cost / 4 cores. Six shards keep the slowest host under ~7 min.
set(SEGARECOMP_CI_SHARD_COUNT 6)
set(SEGARECOMP_CI_SHARD_BUDGET_SECONDS 2100)
set(SEGARECOMP_CI_MAX_TEST_SECONDS 600)
set(SEGARECOMP_CI_DEFAULT_COST 3)
set(SEGARECOMP_CI_TEST_COSTS
  m68k_capability_ratchet_test=509 genesis_m68k_alias_build_test=90 z80_owner_group_differential_test_slice4=488 z80_owner_group_differential_test_slice2=468
  z80_owner_group_differential_test=429 genesis_z80_build_pipeline_bound_test=401 sms_machine_scheduler_test=333
  z80_owner_group_differential_test_slice3=215 sms_owner_group_equivalence_render_test=209
  m68k_pipeline_tests=178 sms_machine_e2e_test=176 genesis_generated_c_compaction_differential_test=150
  sms_owner_group_equivalence_test=162 sms_native_build_policy_test=161 sms_render_native_test=145 sms_native_build_test=144
  genesis_z80_build_pipeline_test=117 m68k_conformance_harness_test=114 sms_emission_budget_512k_test=112 sms_owner_group_equivalence_vdp_test=112
  z80_conformance_harness_test=112 sms_vdp_native_test=106 z80_timing_closure_test=101 z80_generated_pipeline_test=95 sms_render_test=83
  sonic_startup_inventory_adapter_failure_test=80 sms_bank_crossing_test=79 z80_live_guard_test=71 genesis_z80_build_pipeline_smc_test=70
  genesis_z80_machine_test=54 z80_capability_ratchet_test=53 sms_viewer_equivalence_test=51 z80_live_operand_test=47 sms_pad_native_test=45
  sms_emission_budget_test=40 genesis_audio_artifact_test=39 segarecomp_build_command_test=34 genesis_audio_equivalence_test=20
  genesis_z80_build_pipeline_falsify_test=20 sms_machine_tests=19 cpu_z80_decode_tests=18 genesis_audio_ym_test=18 z80_control_stack_dispatch_test=17
  genesis_z80_images_test=16 m68k_batch_c_static_slice_test=16 genesis_z80_build_pipeline_prepare_test=10 sms_native_build_failclosed_test=9
  sms_psg_differential_test=7 ci_shard_guard_test=1
  sega_cd_bios_partition_test=110 sega_cd_bios_hardware_test=15 sega_cd_epoch_recipe_test=17 sega_cd_epoch_recipe_build_test=45 sega_cd_template_run_test=40
  sega_cd_experimental_m3_test=90 sega_cd_source_relative_test=45 genesis_source_relative_reuse_test=40 sega_cd_unvisited_discovery_test=15 sega_cd_loader_survey_test=12  # SEG-097 (local fast-tier 30/14/12/4 s, x3 for a 4-core runner)
  sega_cd_dual_cpu_test=150 genesis_cartridge_sram_generated_test=145  # measured on the Linux GCC shard runner (PR #128 runs 39210640179, 39214882993); unlisted they
  # defaulted to 3 s and could land in one shard together, where contention pushed a dual-CPU partition run past its 120 s timeout
  segarecomp_ml_v3_test=1 segarecomp_ml_v3_sgdk_test=1 segarecomp_ml_v3_train_test=1 segarecomp_ml_v3_diagnose_test=1
  segarecomp_oracle_bench_test=1 segarecomp_oracle_bench_fixture_test=15 segarecomp_xplat_qualify_test=1 segarecomp_xplat_handoff_test=2 segarecomp_xplat_corpus_test=1 segarecomp_xplat_design_test=1 segarecomp_xplat_models_test=1 segarecomp_xplat_final_test=1 segarecomp_xplat_explore_test=1 segarecomp_xplat_stability_test=1 segarecomp_xplat_cv2_test=1 segarecomp_xplat_seg076_final_test=1
  launcher_input_diagnostics_test=25 launcher_sms_policy_test=20 launcher_gui_navigation_test=12)  # SDL3 viewer/launcher suites (only registered when SDL3 is enabled)  # RUN_SERIAL: sonic_startup_inventory_adapter_failure_test runs alone, so it occupies all four cores

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

# Every directory's tests take part: the SDL3 viewer and launcher suites (libs/viewer, apps/segarecomp-launcher) register their tests in their
# own directories, and a directory-scoped scan would silently leave them unsharded (the guard test then fails the SDL3-enabled configuration).
# Assigning a label or cost to a test of another directory needs CMake 3.28's `set_property(TEST ... DIRECTORY ...)`.
function(_segarecomp_collect_tests dir out)
  get_property(_own DIRECTORY ${dir} PROPERTY TESTS)
  set(_acc)
  foreach(_t IN LISTS _own)
    list(APPEND _acc "${dir}|${_t}")
  endforeach()
  get_property(_subs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
  foreach(_s IN LISTS _subs)
    _segarecomp_collect_tests(${_s} _sub_acc)
    list(APPEND _acc ${_sub_acc})
  endforeach()
  set(${out} "${_acc}" PARENT_SCOPE)
endfunction()
_segarecomp_collect_tests("${PROJECT_SOURCE_DIR}" _segarecomp_ci_all_entries)
set(_segarecomp_ci_all_tests)
foreach(_e IN LISTS _segarecomp_ci_all_entries)
  string(REGEX MATCH "^(.*)\\|(.*)$" _m "${_e}")
  set(_dir_of_${CMAKE_MATCH_2} "${CMAKE_MATCH_1}")
  list(APPEND _segarecomp_ci_all_tests "${CMAKE_MATCH_2}")
  if(NOT "${CMAKE_MATCH_1}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}" AND CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR "ci_shards.cmake: test ${CMAKE_MATCH_2} (${CMAKE_MATCH_1}) is registered outside tests/; labelling it for CI shards needs CMake >= 3.28")
  endif()
endforeach()

# Sort key: the cost zero-padded so that a string sort is a numeric sort; the name breaks ties deterministically.
set(_segarecomp_ci_keys)
foreach(_t IN LISTS _segarecomp_ci_all_tests)
  set(_dir_arg)
  if(NOT "${_dir_of_${_t}}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_dir_arg DIRECTORY "${_dir_of_${_t}}")
  endif()
  get_property(_labels TEST ${_t} ${_dir_arg} PROPERTY LABELS)
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
  set(_dir_arg)
  if(NOT "${_dir_of_${_t}}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_dir_arg DIRECTORY "${_dir_of_${_t}}")
  endif()
  set_property(TEST ${_t} ${_dir_arg} APPEND PROPERTY LABELS shard-${_best})
  set_property(TEST ${_t} ${_dir_arg} PROPERTY COST ${_cost})
endforeach()

