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
# 2026-11 re-measurement (PR #216): see the entries marked below. The budget is unchanged: it is the estimate of what a shard may cost, not something to move when a test is added.
#
# Shard count: the shards are the unit of balance, not of scheduling. A CI job runs a strided selection of them (job j/J runs shards j, j+J, ... in ci.yml), so
# the number of jobs per host is a workflow decision: a free-plan account runs at most 20 jobs at once and 5 on macOS, a Linux (GCC) job is
# ~1.5x slower than the same tests on Windows or macOS, and each job costs ~2.5 min of checkout/setup/build. 35 shards of ~530 s each allow
# 7 x 5 (Linux), 5 x 7 (Windows) and 4 x ~9 (macOS) with selections that carry equal estimated cost. The budget below is per shard (it was 2800 for
# the earlier six shards of 2400 s): a job's estimate is the sum of its shards.
set(SEGARECOMP_CI_SHARD_COUNT 35)
set(SEGARECOMP_CI_SHARD_BUDGET_SECONDS 700)   # per shard (~530 s expected, ~585 s in the SDL3-enabled configuration, which also registers the launcher suites); was 2800 for six shards
set(SEGARECOMP_CI_MAX_TEST_SECONDS 600)
set(SEGARECOMP_CI_DEFAULT_COST 3)
set(SEGARECOMP_CI_TEST_COSTS
  m68k_capability_ratchet_test=298 genesis_m68k_alias_build_test=17 z80_owner_group_differential_test_slice4=488 z80_owner_group_differential_test_slice2=231
  z80_owner_group_differential_test=286 genesis_z80_build_pipeline_bound_test=401 sms_machine_scheduler_test=103
  z80_owner_group_differential_test_slice3=215 sms_owner_group_equivalence_render_test=129
  m68k_pipeline_tests=178 sms_machine_e2e_test=77 genesis_generated_c_compaction_differential_test=150
  sms_owner_group_equivalence_test=162 sms_native_build_policy_test=161 sms_render_native_test=145 sms_native_build_test=94
  genesis_z80_build_pipeline_test=117 m68k_conformance_harness_test=114 sms_emission_budget_512k_test=112 sms_owner_group_equivalence_vdp_test=50
  z80_conformance_harness_test=55 sms_vdp_native_test=56 z80_timing_closure_test=38 z80_generated_pipeline_test=95 sms_render_test=83
  sonic_startup_inventory_adapter_failure_test=10 sms_bank_crossing_test=79 z80_live_guard_test=38 genesis_z80_build_pipeline_smc_test=70
  genesis_z80_machine_test=54 z80_capability_ratchet_test=53 sms_viewer_equivalence_test=51 z80_live_operand_test=47 sms_pad_native_test=45
  sms_emission_budget_test=40 genesis_audio_artifact_test=39 segarecomp_build_command_test=34 genesis_audio_equivalence_test=20
  genesis_z80_build_pipeline_falsify_test=20 sms_machine_tests=19 cpu_z80_decode_tests=18 genesis_audio_ym_test=18 z80_control_stack_dispatch_test=17
  genesis_z80_images_test=16 m68k_batch_c_static_slice_test=16 genesis_z80_build_pipeline_prepare_test=10 sms_native_build_failclosed_test=9
  sms_psg_differential_test=7 ci_shard_guard_test=1 mod_edits_test=1
  sega_cd_bios_partition_test=110 sega_cd_bios_hardware_test=15 sega_cd_epoch_recipe_test=17 sega_cd_epoch_recipe_build_test=45 sega_cd_template_run_test=40
  sega_cd_experimental_m3_test=90 sega_cd_source_relative_test=45 genesis_source_relative_reuse_test=40 sega_cd_unvisited_discovery_test=15 sega_cd_loader_survey_test=12 sega_cd_bios_unvisited_test=200 sega_cd_loader_dispatch_test=15 sega_cd_services_loader_test=200 sega_cd_state_convergence_test=28 sega_cd_recursive_discovery_test=200 sega_cd_bios_scale_test=420 sega_cd_file_first_test=240 sega_cd_dynamic_binding_test=3 sega_cd_dynamic_body_test=2  # SEG-097 (local fast-tier 30/14/12/4 s; SEG-098-T001 survey 12 s estimate, x3 for a 4-core runner)
  sega_cd_dual_cpu_test=79 genesis_cartridge_sram_generated_test=92  # measured on the Linux GCC shard runner (PR #128 runs 39210640179, 39214882993); unlisted they
  # defaulted to 3 s and could land in one shard together, where contention pushed a dual-CPU partition run past its 120 s timeout
  segarecomp_ml_v3_test=1 segarecomp_ml_v3_sgdk_test=1 segarecomp_ml_v3_train_test=1 segarecomp_ml_v3_diagnose_test=1
  segarecomp_oracle_bench_test=1 segarecomp_oracle_bench_fixture_test=15 segarecomp_xplat_qualify_test=1 segarecomp_xplat_handoff_test=2 segarecomp_xplat_corpus_test=1 segarecomp_xplat_design_test=1 segarecomp_xplat_models_test=1 segarecomp_xplat_final_test=1 segarecomp_xplat_explore_test=1 segarecomp_xplat_stability_test=1 segarecomp_xplat_cv2_test=1 segarecomp_xplat_seg076_final_test=1
  segarecomp_oracle_bench_fixture_test=600 sega_cd_bios_bootarea_test=578 sms_native_build_policy_test=561 sega_cd_native_build_test=476 sega_cd_bios_unvisited_test=423 sega_cd_bios_wordram_extent_test=231 sega_cd_services_loader_test=248 sega_cd_file_first_test=364 z80_owner_group_differential_test_slice3=204 sms_optimized_policy_test_cli=318 sega_cd_extent_discovery_test=316 sega_cd_derived_image_test=170 sega_cd_recursive_discovery_test=149 sms_bank_crossing_test=271 sega_cd_bios_partition_test=269 sega_cd_epoch_recipe_build_test=248 sms_owner_group_equivalence_test=140 m68k_pipeline_tests=234 sega_cd_bios_sub_wordram_extent_test=233 z80_selective_admission_test_slice2=110 sms_render_native_test=198 genesis_z80_build_pipeline_bound_test=80 m68k_conformance_harness_test=181 sega_cd_bios_posthandoff_test=180 z80_execution_coverage_test=160 genesis_z80_build_pipeline_test=54 sms_ml_region_z80_test=96 z80_generated_pipeline_test=135 z80_selective_admission_test=132 sms_viewer_equivalence_test=60 sega_cd_epoch_recipe_test=53 sega_cd_template_run_test=67 z80_live_operand_test=62 sega_cd_live_test=61  # SEG-104: re-measured on the Linux GCC shard runner (PR #169 run 40979276169, contended seconds; capped at the per-test maximum). Many Sega CD and Z80 tests were unlisted (3 s) or stale, which put several 400-600 s tests into one shard (shard 2: 18.5 min against 8-16 min elsewhere)
  # Re-measured on the slowest recorded host (Linux, GCC; CI run 42914284128 of PR #216, tests contended 4-wide) and added with the native GEMS tests (ADR 0291): tests that were priced far above their measured
  # duration are lowered to it, tests that were never listed (default cost) but run >= 100 s are priced, and the new tests are priced (launcher_audio_gui_test is estimated: it has not run on a runner yet).
  genesis_rewind_test=140 genesis_z80_static_first_build_determinism_test=113 genesis_z80_static_first_build_equivalence_test=192 genesis_z80_static_first_build_fallback_test=256 sega_cd_main_z80_test=177 sega_cd_reconstruction_test=200 sega_cd_snapshot_test=135 sega_cd_static_boot_test=479 sega_cd_static_producer_test=131 genesis_native_snapshot_test=12 gems_reference_check_test=2 launcher_audio_gui_test=150 genesis_native_gems_build_test=140 gems_native_tests=1
  launcher_input_diagnostics_test=25 launcher_sms_policy_test=20 launcher_gui_navigation_test=12 launcher_patch_gui_test=330 launcher_lock_on_headless_test=15 launcher_lock_on_window_test=45 launcher_lock_on_library_test=50 launcher_lock_on_patch_test=50 launcher_lock_on_gamepad_test=80 segapatch_package_test=3 segapatch_native_package_test=3 segarecomp_native_build_args_test=3 genesis_patch_provider_build_test=120
  launcher_parity_gui_journey_wide_test=220 launcher_parity_gui_journey_narrow_test=120 launcher_parity_gui_persist_wide_test=180 launcher_parity_gui_persist_narrow_test=100 launcher_parity_gui_input_wide_test=170 launcher_parity_gui_input_narrow_test=90  # SEG-109-T005: measured locally (150-170 s alone), x2-3 for a contended 4-core runner  # SDL3 viewer/launcher suites (only registered when SDL3 is enabled)  # RUN_SERIAL: sonic_startup_inventory_adapter_failure_test runs alone, so it occupies all four cores
  # CI-latency re-measurement (Linux GCC, the slowest host; contended 4-wide; mean of CI runs 43124200056 and 42865573591, capped at the per-test
  # maximum; sega_cd_bios_bootarea_test is scaled by the ~0.79 measured gain of building its no-boundary program once). Only entries that moved by more than
  # 30 % and 20 s are listed: the earlier values were up to 4x too low, which is why shards with equal estimates took 9-16 min. The SDL3-only launcher/viewer
  # tests (which run in the viewer job, never in a shard job) keep their estimates: re-pricing them would only add to the SDL3-configured partition the guard also checks.
  sega_cd_bios_posthandoff_test=600 sega_cd_extent_discovery_test=600 sega_cd_derived_image_test=577 genesis_z80_static_first_build_fallback_test=555
  m68k_capability_ratchet_test=472 sega_cd_bios_wordram_extent_test=417 sega_cd_bios_sub_wordram_extent_test=411 m68k_pipeline_tests=360
  genesis_native_gems_build_test=350 genesis_z80_static_first_build_equivalence_test=348 z80_owner_group_differential_test_slice2=311
  z80_owner_group_differential_test_slice3=289 sega_cd_reconstruction_test=262 genesis_generated_c_compaction_differential_test=252
  sega_cd_main_z80_test=231 sega_cd_recursive_discovery_test=201 sega_cd_snapshot_test=186 sms_ml_region_z80_test=186
  sega_cd_static_producer_test=174 sms_machine_scheduler_test=161 z80_selective_admission_test_slice2=145 genesis_z80_build_pipeline_bound_test=112
  sega_cd_dual_cpu_test=111 sms_native_build_failclosed_test=110 sms_machine_e2e_test=101 sms_vdp_native_test=100 genesis_lock_on_cartridge_test=94
  z80_generated_pipeline_test=87 z80_conformance_harness_test=83 sms_owner_group_equivalence_vdp_test=80 genesis_hint_ram_jmp_reference_test=73
  sms_optimized_policy_test_runtime=68 genesis_cartridge_eeprom_generated_test=66 z80_live_guard_test=59 sega_cd_pcm_audio_native_test=47
  sms_snapshot_test=46 perf_integration_test=41 sega_cd_cdda_native_test=40 sega_cd_m5_routes_test=40 genesis_static_ram_jmp_thunk_generated_test=38
  z80_live_alternatives_test=37 sega_cd_disc_to_frame_test=34 genesis_ml_gen2_identity_mutation_test=31 genesis_compat_driver_test=30
  segarecomp_aot_model_lifecycle_test=30 genesis_startup_runtime_c4_test=28 genesis_z80_build_pipeline_region_test=28 genesis_z80_machine_test=28
  genesis_hint_raster_reference_test=27 sega_cd_overlay_test=26 sega_cd_bios_profile_test=24)

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

