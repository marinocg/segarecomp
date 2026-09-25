find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
# SEG-018-T006: several bridge tests write generated output under the git-ignored
# <source>/build scratch directory (the bridge refuses non-ignored output paths).
# An out-of-tree build directory (CI, Linux, Windows) does not create it, so
# guarantee it exists rather than depending on the in-tree preset layout.
file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/build")
add_executable(recompiler_fake_cpu_tests recompiler_fake_cpu_test.cpp)
target_link_libraries(recompiler_fake_cpu_tests PRIVATE segarecomp::recompiler)
segarecomp_enable_warnings(recompiler_fake_cpu_tests)
add_test(NAME recompiler_fake_cpu_tests COMMAND recompiler_fake_cpu_tests)
add_executable(segarecomp_tests rom_test.cpp)
target_link_libraries(segarecomp_tests PRIVATE segarecomp::media segarecomp::codegen_c11_genesis)
segarecomp_enable_warnings(segarecomp_tests)
add_test(NAME segarecomp_tests COMMAND segarecomp_tests)
add_executable(moveq_tests moveq_test.cpp)
 target_link_libraries(moveq_tests PRIVATE segarecomp::legacy_compat)
segarecomp_enable_warnings(moveq_tests)
add_test(NAME moveq_tests COMMAND moveq_tests)
add_executable(direct_flow_tests direct_flow_test.cpp)
 target_link_libraries(direct_flow_tests PRIVATE segarecomp::legacy_compat)
segarecomp_enable_warnings(direct_flow_tests)
add_test(NAME direct_flow_tests COMMAND direct_flow_tests)
add_executable(m68k_pipeline_tests m68k_pipeline_test.cpp)
  target_link_libraries(m68k_pipeline_tests PRIVATE segarecomp::machine_genesis segarecomp::codegen_c11_genesis segarecomp::legacy_compat)
segarecomp_enable_warnings(m68k_pipeline_tests)
add_test(NAME m68k_pipeline_tests COMMAND m68k_pipeline_tests)
add_executable(compiled_entry_table_tests compiled_entry_table_test.cpp)
target_link_libraries(compiled_entry_table_tests PRIVATE segarecomp::codegen_c11)
segarecomp_enable_warnings(compiled_entry_table_tests)
add_test(NAME compiled_entry_table_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/compiled_entry_table_driver.py
  $<TARGET_FILE:compiled_entry_table_tests> ${CMAKE_C_COMPILER} ${CMAKE_CURRENT_BINARY_DIR}/compiled_entry_table_work)
add_executable(m68k_timing_tests m68k_timing_test.cpp)
target_link_libraries(m68k_timing_tests PRIVATE segarecomp::cpu_m68k)
segarecomp_enable_warnings(m68k_timing_tests)
add_test(NAME m68k_timing_tests COMMAND m68k_timing_tests)
# SEG-007-T252 / ADR-0040: the former SEG-007-T107 loop-progress-note
# wrap-guard test has been removed along with the generated-runtime
# progress watchdog and its note-emission mechanism.
add_executable(controller_io_policy_parity_tests
  controller_io_policy_parity_test.cpp
)
target_link_libraries(controller_io_policy_parity_tests PRIVATE segarecomp::device_genesis segarecomp::runtime_genesis)
segarecomp_enable_warnings(controller_io_policy_parity_tests)
add_test(NAME controller_io_policy_parity_tests COMMAND controller_io_policy_parity_tests)
add_executable(work_ram_policy_parity_tests
  work_ram_policy_parity_test.cpp
)
target_link_libraries(work_ram_policy_parity_tests PRIVATE segarecomp::machine_genesis segarecomp::runtime_genesis)
segarecomp_enable_warnings(work_ram_policy_parity_tests)
add_test(NAME work_ram_policy_parity_tests COMMAND work_ram_policy_parity_tests)
# SEG-007-T252 / ADR-0040: focused unit coverage of the runner/guest
# ownership split (genesis_runtime_step / genesis_runtime_run) and the new
# disjoint GENESIS_RUNNER_RESOURCE_LIMIT outcome, replacing the former
# SEG-007-T155/ADR-0017 and SEG-007-T211/ADR-0035 watchdog-note tests.
add_executable(genesis_runtime_run_tests
  genesis_runtime_run_test.cpp
)
target_link_libraries(genesis_runtime_run_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_runtime_run_tests)
add_test(NAME genesis_runtime_run_tests COMMAND genesis_runtime_run_tests)
# SEG-007-T252 / ADR-0040 correction: the bounded 64-entry diagnostic-only
# GenesisRuntime.recent_pc_history circular buffer, surfaced only in the
# EPHEMERAL_FRONTIER diagnostic line for a runner-resource-limit frontier.
add_executable(genesis_recent_pc_history_tests
  genesis_recent_pc_history_test.cpp
)
target_link_libraries(genesis_recent_pc_history_tests PRIVATE segarecomp::runtime_genesis)
target_compile_features(genesis_recent_pc_history_tests PRIVATE cxx_std_20)
segarecomp_enable_warnings(genesis_recent_pc_history_tests)
add_test(NAME genesis_recent_pc_history_tests COMMAND genesis_recent_pc_history_tests)
add_executable(genesis_execution_history_tests genesis_execution_history_test.cpp)
target_link_libraries(genesis_execution_history_tests PRIVATE segarecomp::runtime_genesis)
target_compile_features(genesis_execution_history_tests PRIVATE cxx_std_20)
segarecomp_enable_warnings(genesis_execution_history_tests)
add_test(NAME genesis_execution_history_tests COMMAND genesis_execution_history_tests)
add_executable(genesis_m68k_checkpoint_tests genesis_m68k_checkpoint_test.cpp)
target_link_libraries(genesis_m68k_checkpoint_tests PRIVATE segarecomp::runtime_genesis)
target_compile_features(genesis_m68k_checkpoint_tests PRIVATE cxx_std_20)
segarecomp_enable_warnings(genesis_m68k_checkpoint_tests)
add_test(NAME genesis_m68k_checkpoint_tests COMMAND genesis_m68k_checkpoint_tests)

add_executable(genesis_device_checkpoint_tests genesis_device_checkpoint_test.cpp)
target_link_libraries(genesis_device_checkpoint_tests PRIVATE segarecomp::runtime_genesis)
target_compile_features(genesis_device_checkpoint_tests PRIVATE cxx_std_20)
segarecomp_enable_warnings(genesis_device_checkpoint_tests)
add_test(NAME genesis_device_checkpoint_tests COMMAND genesis_device_checkpoint_tests)
# SEG-007-T252 / ADR-0040: asserts this runtime translation unit references
# no wall-clock/sleep symbol -- headless/automated execution is full-speed
# by construction.
add_executable(genesis_runtime_run_no_wallclock_tests
  genesis_runtime_run_no_wallclock_test.cpp
)
segarecomp_enable_warnings(genesis_runtime_run_no_wallclock_tests)
add_test(NAME genesis_runtime_run_no_wallclock_tests COMMAND genesis_runtime_run_no_wallclock_tests
  ${PROJECT_SOURCE_DIR}/platforms/genesis/runtime/runtime.c)
# SEG-007-T175: focused unit coverage of the 68000 bus-stall/suspension
# modeled for a memory-to-VDP DMA transfer in genesis_runtime_step.
add_executable(genesis_vdp_dma_cpu_suspension_tests
  genesis_vdp_dma_cpu_suspension_test.cpp
)
target_link_libraries(genesis_vdp_dma_cpu_suspension_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vdp_dma_cpu_suspension_tests)
add_test(NAME genesis_vdp_dma_cpu_suspension_tests COMMAND genesis_vdp_dma_cpu_suspension_tests)
# SEG-007-T049 checkpoint C1: focused unit coverage of the standalone
# tile-pattern / CRAM-palette decode module (runtime/genesis/vdp_render.c),
# using only project-authored synthetic fixtures with hand-computed
# expected values.
add_executable(genesis_vdp_render_tile_palette_tests
  genesis_vdp_render_tile_palette_test.cpp
)
target_link_libraries(genesis_vdp_render_tile_palette_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vdp_render_tile_palette_tests)
add_test(NAME genesis_vdp_render_tile_palette_tests COMMAND genesis_vdp_render_tile_palette_tests)
# SEG-007-T049 checkpoint C2: focused unit coverage for the plane/nametable
# addressing and bound scrolling behavior added to
# runtime/genesis/vdp_render.c, using only project-authored synthetic
# fixtures with hand-computed expected values.
add_executable(genesis_vdp_render_plane_scroll_tests
  genesis_vdp_render_plane_scroll_test.cpp
)
target_link_libraries(genesis_vdp_render_plane_scroll_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vdp_render_plane_scroll_tests)
add_test(NAME genesis_vdp_render_plane_scroll_tests COMMAND genesis_vdp_render_plane_scroll_tests)
# SEG-007-T049 checkpoint C3: focused unit coverage for the
# sprite-attribute-table decoding/placement helpers added to
# runtime/genesis/vdp_render.c, using only project-authored synthetic
# fixtures with hand-computed expected values.
add_executable(genesis_vdp_render_sprite_tests
  genesis_vdp_render_sprite_test.cpp
)
target_link_libraries(genesis_vdp_render_sprite_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vdp_render_sprite_tests)
add_test(NAME genesis_vdp_render_sprite_tests COMMAND genesis_vdp_render_sprite_tests)
# SEG-007-T049 checkpoint C4: focused unit coverage for the
# priority/transparency/composition helpers added to
# runtime/genesis/vdp_render.c, using only project-authored synthetic
# fixtures with independently hand-derived expected values.
add_executable(genesis_vdp_render_composition_tests
  genesis_vdp_render_composition_test.cpp
)
target_link_libraries(genesis_vdp_render_composition_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vdp_render_composition_tests)
add_test(NAME genesis_vdp_render_composition_tests COMMAND genesis_vdp_render_composition_tests)
# SEG-007-T049 checkpoint C5: focused unit coverage for the deterministic
# frame-output producer added to runtime/genesis/vdp_render.c, using only
# project-authored synthetic fixtures with independently hand-derived
# expected values, applying (not redefining) SEG-007-T042's committed
# GenesisFrameArtifact schema.
add_executable(genesis_vdp_render_frame_tests
  genesis_vdp_render_frame_test.cpp
)
target_link_libraries(genesis_vdp_render_frame_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vdp_render_frame_tests)
add_test(NAME genesis_vdp_render_frame_tests COMMAND genesis_vdp_render_frame_tests)
# SEG-007-T049 checkpoint C6: independent synthetic validation of the
# already-complete C1-C5 renderer/frame path, using a single project-
# authored combined fixture and an independently (re-)derived expected-
# result oracle (see the test file header for the derivation).
add_executable(genesis_vdp_render_c6_independent_validation_tests
  genesis_vdp_render_c6_independent_validation_test.cpp
)
target_link_libraries(genesis_vdp_render_c6_independent_validation_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vdp_render_c6_independent_validation_tests)
add_test(NAME genesis_vdp_render_c6_independent_validation_tests
         COMMAND genesis_vdp_render_c6_independent_validation_tests)
# SEG-007-T050 add-on: focused unit coverage for the operator-facing local
# frame-export module (runtime/genesis/frame_export.c), using only
# project-authored synthetic pixel/palette fixtures with independently
# hand-derived expected RGB values -- no commercial ROM content.
add_executable(genesis_frame_export_tests
  genesis_frame_export_test.cpp
)
target_link_libraries(genesis_frame_export_tests PRIVATE segarecomp::runtime_genesis)
target_compile_features(genesis_frame_export_tests PRIVATE cxx_std_20)
segarecomp_enable_warnings(genesis_frame_export_tests)
add_test(NAME genesis_frame_export_tests COMMAND genesis_frame_export_tests)
# SEG-007-T047 / ADR-0020: focused unit coverage of the deterministic VBlank /
# MC68000 IRQ6 interrupt mechanism and RTE restoration.
add_executable(genesis_vblank_irq6_interrupt_tests
  genesis_vblank_irq6_interrupt_test.cpp
)
target_link_libraries(genesis_vblank_irq6_interrupt_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_vblank_irq6_interrupt_tests)
add_test(NAME genesis_vblank_irq6_interrupt_tests COMMAND genesis_vblank_irq6_interrupt_tests)
add_executable(genesis_virtual_timing_tests genesis_virtual_timing_test.cpp)
target_link_libraries(genesis_virtual_timing_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_virtual_timing_tests)
add_test(NAME genesis_virtual_timing_tests COMMAND genesis_virtual_timing_tests)
add_executable(genesis_live_frame_tests genesis_live_frame_test.cpp)
target_link_libraries(genesis_live_frame_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_live_frame_tests)
add_test(NAME genesis_live_frame_tests COMMAND genesis_live_frame_tests)
# SEG-007-T254: optional viewer core; deliberately not linked by runtime_genesis.
add_executable(genesis_viewer_tests genesis_viewer_test.cpp ${PROJECT_SOURCE_DIR}/platforms/genesis/viewer/viewer.c)
target_include_directories(genesis_viewer_tests PRIVATE ${PROJECT_SOURCE_DIR}/platforms/genesis/viewer)
target_link_libraries(genesis_viewer_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_viewer_tests)
add_test(NAME genesis_viewer_tests COMMAND genesis_viewer_tests)
add_test(NAME genesis_viewer_generated_integration_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_viewer_generated_integration_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})

# SEG-020-T006: CPU vs Genesis device first-divergence classification.
add_test(NAME genesis_device_divergence_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_device_divergence_test.py ${CMAKE_C_COMPILER})
# SEG-020-T007: unified failure-diagnosis assembly and durable-safe projection.
add_test(NAME genesis_failure_diagnosis_workflow_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_failure_diagnosis_workflow_test.py)
add_test(NAME genesis_virtual_timing_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_virtual_timing_generated_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR}/platforms/genesis/runtime
  ${PROJECT_SOURCE_DIR}/platforms/genesis/runtime/runtime.c)
add_test(NAME genesis_virtual_timing_frontend_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_virtual_timing_frontend_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T222 / ADR-0037: focused unit coverage of the synchronous
# divide-by-zero (vector 5) CPU exception seam, reusing ADR-0020's shared
# frame-construction helper/RTE without perturbing IRQ6.
add_executable(genesis_divide_by_zero_exception_tests
  genesis_divide_by_zero_exception_test.cpp
)
target_link_libraries(genesis_divide_by_zero_exception_tests PRIVATE segarecomp::runtime_genesis)
segarecomp_enable_warnings(genesis_divide_by_zero_exception_tests)
add_test(NAME genesis_divide_by_zero_exception_tests COMMAND genesis_divide_by_zero_exception_tests)
# SEG-007-T047 / ADR-0020: generated-C coverage for the RTE (C4) lowering and
# the IRQ6 autovector generated-main wiring -- emit, strict-C11 compile, run.
add_test(NAME genesis_vblank_irq6_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_vblank_irq6_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_divide_by_zero_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_divide_by_zero_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-021-T010 correction: generated-native execution regressions for the routed auto-updating
# MULS/MULU/DIVS/DIVU source (timing-source capture, DIV auto-update exception ordering) and the
# widened DIVS/DIVU (d8,PC,Xn) source.
add_test(NAME m68k_muldiv_auto_update_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_muldiv_auto_update_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_executable(cpu_m68k_static_discovery_tests cpu_m68k_static_discovery_test.cpp)
target_link_libraries(cpu_m68k_static_discovery_tests PRIVATE segarecomp::cpu_m68k)
segarecomp_enable_warnings(cpu_m68k_static_discovery_tests)
add_test(NAME cpu_m68k_static_discovery_tests COMMAND cpu_m68k_static_discovery_tests)
add_executable(cpu_m68k_static_discovery_continuation_tests cpu_m68k_static_discovery_continuation_test.cpp)
target_link_libraries(cpu_m68k_static_discovery_continuation_tests PRIVATE segarecomp::cpu_m68k)
segarecomp_enable_warnings(cpu_m68k_static_discovery_continuation_tests)
add_test(NAME cpu_m68k_static_discovery_continuation_tests COMMAND cpu_m68k_static_discovery_continuation_tests)
add_executable(cpu_m68k_static_discovery_resolved_control_target_tests cpu_m68k_static_discovery_resolved_control_target_test.cpp)
target_link_libraries(cpu_m68k_static_discovery_resolved_control_target_tests PRIVATE segarecomp::cpu_m68k)
segarecomp_enable_warnings(cpu_m68k_static_discovery_resolved_control_target_tests)
add_test(NAME cpu_m68k_static_discovery_resolved_control_target_tests COMMAND cpu_m68k_static_discovery_resolved_control_target_tests)
add_executable(cpu_m68k_static_discovery_edge_constraint_tests cpu_m68k_static_discovery_edge_constraint_test.cpp)
target_link_libraries(cpu_m68k_static_discovery_edge_constraint_tests PRIVATE segarecomp::cpu_m68k)
segarecomp_enable_warnings(cpu_m68k_static_discovery_edge_constraint_tests)
add_test(NAME cpu_m68k_static_discovery_edge_constraint_tests COMMAND cpu_m68k_static_discovery_edge_constraint_tests)
add_test(NAME cpu_m68k_static_discovery_boundary_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/cpu_m68k_static_discovery_boundary_test.py)
add_test(NAME genesis_machine_frontend_ownership_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_machine_frontend_ownership_test.py)
add_test(NAME architecture_dependency_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/architecture_dependency_test.py)
add_executable(m68k_frontend_rejection_tests m68k_frontend_rejection_test.cpp)
 target_link_libraries(m68k_frontend_rejection_tests PRIVATE segarecomp::machine_genesis)
segarecomp_enable_warnings(m68k_frontend_rejection_tests)
add_test(NAME m68k_frontend_rejection_tests COMMAND m68k_frontend_rejection_tests)
add_test(NAME m68k_frontend_static_slice_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m68k_frontend_static_slice_test.py $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER})
add_test(NAME m68k_frontend_adversarial_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m68k_frontend_adversarial_test.py $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER})
add_test(NAME genesis_rom_startup_static_slice_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/genesis_rom_startup_static_slice_test.py $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER})
add_test(NAME genesis_general_startup_cli_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/genesis_general_startup_cli_test.py $<TARGET_FILE:segarecomp>)
add_test(NAME genesis_startup_runtime_c2_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_c2_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_c3_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_c3_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_c4_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_c4_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_code_pointer_descriptor_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_code_pointer_descriptor_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T238 / ADR-0039 deferred workstream #4: strict-C11 compile/execute
# proof for the build-time-only, explicitly opt-in broad aligned-M68k-ROM
# AOT representation experiment.
add_test(NAME genesis_experiment_aligned_aot_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_experiment_aligned_aot_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_experiment_aligned_aot_report_failure_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_experiment_aligned_aot_report_failure_test.py
  ${PROJECT_SOURCE_DIR}/tools/genesis_experiment_aligned_aot_report.py
  $<TARGET_FILE:segarecomp>)
# SEG-007-T245: strict-C11 compile/execute proof that the narrow register-
# indirect-source `write_move` family now admitted into the immutable-ROM
# AOT fact-free contract actually routes through the runtime memory gate
# and defers its address-register commit correctly on both success and a
# forced routing failure.
add_test(NAME genesis_immutable_rom_aot_write_move_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_write_move_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T246: strict-C11 compile/execute proof that an isolated,
# ownerless RTS immutable-ROM AOT candidate validates against ADR-0011's
# real whole-program return-target authority (existing-owner integration,
# not a new architecture), and that the new BCHG/BCLR/BSET plain-`(An)`
# carve-out routes its read-modify-write through the runtime memory gate.
add_test(NAME genesis_immutable_rom_aot_return_from_subroutine_and_bit_clear_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_return_from_subroutine_and_bit_clear_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T247: strict-C11 compile/execute proof that the tenth-iteration
# absolute_word/absolute_long MOVE-destination family now admitted into
# the immutable-ROM AOT fact-free contract routes through the runtime
# memory gate correctly on both a storage-free source and a mutating
# register-indirect source, and fails closed with no partial mutation.
add_test(NAME genesis_immutable_rom_aot_move_absolute_destination_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_move_absolute_destination_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T248: strict-C11 compile/execute proof that the newly admitted
# brief-format indexed `test_operand` source (`address_index8`) routes
# through the runtime memory gate correctly, mutates no register, and
# fails closed with no partial mutation; the excluded PC-relative sibling
# never even reaches the IR/AOT layer.
add_test(NAME genesis_immutable_rom_aot_test_operand_index8_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_test_operand_index8_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T249: strict-C11 compile/execute proof that the general
# (non-quick) ADD/SUB/AND/OR/EOR read-modify-write memory-destination
# admission and compare's own broader memory-operand admission route
# through the runtime memory gate correctly, mutate no unrelated register,
# and fail closed with no partial mutation.
add_test(NAME genesis_immutable_rom_aot_general_arithmetic_memory_operand_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_general_arithmetic_memory_operand_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_owner_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_owner_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_body_factoring_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_body_factoring_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_negate_disp16_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_negate_disp16_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_extended_arithmetic_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_extended_arithmetic_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_bcd_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_bcd_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_exg_movep_scc_tas_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_exg_movep_scc_tas_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-021-T018 / ADR 0043: supervisor/user model, privilege violation and the SR/CCR/USP family on the AOT, C4 and
# whole-program bridge routes.
add_test(NAME genesis_status_register_privilege_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_status_register_privilege_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_movem_link_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_movem_link_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_pea_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_pea_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_immutable_rom_aot_jmp_pc_indexed_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_immutable_rom_aot_jmp_pc_indexed_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME m68k_indirect_control_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_indirect_control_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_indexed_lea_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_indexed_lea_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_pc_indexed_move_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_pc_indexed_move_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_pc_indexed_lea_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_pc_indexed_lea_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_index8_pea_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_index8_pea_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_pc_index8_pea_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_pc_index8_pea_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_t028_case_c_pc_indexed_tier2_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_t028_case_c_pc_indexed_tier2_generated_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_index8_indirect_jsr_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_index8_indirect_jsr_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_immutable_offset_table_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_immutable_offset_table_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_cmp_absolute_source_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_cmp_absolute_source_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_indexed_arithmetic_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_indexed_arithmetic_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_pc_indexed_logical_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_pc_indexed_logical_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_muls_word_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_muls_word_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_mulu_word_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_mulu_word_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_divs_word_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_divs_word_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME m68k_divu_word_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_divu_word_musashi_differential_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER})
add_test(NAME genesis_startup_runtime_c4_multi_exit_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_c4_multi_exit_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_c4_frontier_above_cap_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_c4_frontier_above_cap_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_c4_discovery_boundary_multi_frame_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_c4_discovery_boundary_multi_frame_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_c4_movem_adjacent_lea_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_c4_movem_adjacent_lea_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_controller_io_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_controller_io_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_vdp_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_vdp_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_z80_bus_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_z80_bus_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_z80_ram_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_z80_ram_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_psg_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_psg_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_runtime_ym2612_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_runtime_ym2612_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_checkpoint_evidence_runtime_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_checkpoint_evidence_runtime_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T050: end-to-end runner -> checkpoint entry -> two VBlank
# transitions -> T049 composition -> deterministic frame -> T131 bundle
# wiring, plus its required negatives (no checkpoint, one VBlank,
# unsupported render state / renderer failure).
add_test(NAME genesis_checkpoint_evidence_frame_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_checkpoint_evidence_frame_test.py
  ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T252 / ADR-0040: instruction_budget_value() boundary coverage
# (malformed/negative/fractional/zero/out-of-range/UINT32_MAX). No fixture
# arguments needed; the test imports tools/genesis_startup_bridge.py directly.
add_test(NAME genesis_instruction_budget_option_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_instruction_budget_option_test.py)
# SEG-007-T132: the independent checkpoint-evidence oracle (oracle/genesis)
# and its mechanical independence audit. Both link no production code and
# gracefully skip (exit 0) unless SEGARECOMP_T132_ORACLE_MUSASHI_CHECKOUT is
# set to a pinned local Musashi checkout, mirroring
# m68k_batch_b_musashi_differential_test's own env-var-gated pattern.
add_test(NAME genesis_checkpoint_oracle_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_checkpoint_oracle_test.py
  ${CMAKE_C_COMPILER})
add_test(NAME genesis_checkpoint_oracle_independence_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_checkpoint_oracle_independence_test.py
  ${CMAKE_C_COMPILER})
add_test(NAME genesis_report_transport_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_report_transport_test.py ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_c4_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_c4_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T169: this SEG-007-T084 fixture existed but was never wired into
# CTest; registering it now closes that latent CI gap discovered while
# extending the same bounded VDP DMA capability family.
add_test(NAME genesis_startup_bridge_vdp_dma_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_vdp_dma_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_full_validation_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_full_validation_test.py
  ${PROJECT_SOURCE_DIR})
# ADR 0013 Decision §7 Phase A: the discovery-prefix-boundary end-to-end
# generate/strict-C11-compile/link/execute proof on a project-authored
# synthetic fixture (no commercial input).
add_test(NAME genesis_startup_bridge_discovery_prefix_boundary_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_discovery_prefix_boundary_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_ctrl3_byte_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_ctrl3_byte_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_c5_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_c5_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_c5_full_chain_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_c5_full_chain_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_c6_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_c6_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_c6_compare_runs_privacy_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_c6_compare_runs_privacy_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_c6_accepted_loop_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_c6_accepted_loop_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T252 / ADR-0040 correction: the dedicated, FD-only ephemeral
# recent-PC-history transport, exercised end-to-end against a real
# compiled synthetic fixture.
add_test(NAME genesis_ephemeral_pc_history_transport_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_ephemeral_pc_history_transport_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_execution_history_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_execution_history_generated_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_c6_persistence_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_c6_persistence_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T252 / ADR-0040: the former SEG-007-T107 generated-runtime
# loop-progress watchdog test has been replaced by
# genesis_runtime_run_test.cpp and by the end-to-end
# runner_resource_limit coverage in
# genesis_vblank_irq6_generated_test.py,
# genesis_startup_bridge_c6_accepted_loop_test.py, and
# genesis_startup_bridge_c6_persistence_test.py.
add_test(NAME genesis_startup_bridge_report_metadata_representability_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_report_metadata_representability_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T077: generic immutable/generated cartridge-data region ownership
# -- a runtime-computed (non-constant-foldable) effective address resolved
# against a statically owned, generated backing region through
# genesis_route_access, proven end to end through the real compile-and-run
# driver (never a private test-only reimplementation).
add_test(NAME genesis_startup_bridge_owned_cartridge_region_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_owned_cartridge_region_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_startup_bridge_mapping_preparation_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_mapping_preparation_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# ADR 0013 Decision §7 Phase B: the driver-owned build/execute/expand loop's
# own round-management/seed-promotion/terminal-result logic, isolated from
# CPU discovery correctness via a scripted proxy (same pattern as the C6/C7
# driver tests immediately above).
add_test(NAME genesis_startup_bridge_phase_b_expansion_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_phase_b_expansion_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# ADR-0013 Decision §7c: the driver's generic checkpoint/resume mechanism
# (persistent confirmed-root set across invocations, fail-closed binding).
add_test(NAME genesis_startup_bridge_checkpoint_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_checkpoint_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T180 / ADR-0026: the offline-inventory stitch metrics line is parsed
# in-process from the emitter stderr string; no artifact is written into the
# compare-runs out-dir surface, and the normalized counts still reach
# ONE_SHOT_SUMMARY / EPHEMERAL_FRONTIER.
add_test(NAME genesis_startup_bridge_offline_inventory_stitch_metrics_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_offline_inventory_stitch_metrics_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T180: ONE_SHOT_SUMMARY normalized metrics (generation_attempts /
# compile_attempts) count actual invocations -- an emitter rejection before
# any C is emitted reports compile_attempts=0, not compile_attempts=1.
add_test(NAME genesis_startup_bridge_one_shot_attempt_metrics_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_one_shot_attempt_metrics_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T252 / ADR-0040 correction: a one-shot runner-resource-limit
# frontier must normalize driver_result to "runner_resource_limit", never
# "offline_inventory_incomplete" (a guest/static inventory deficiency label).
add_test(NAME genesis_startup_bridge_one_shot_runner_resource_limit_normalization_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_startup_bridge_one_shot_runner_resource_limit_normalization_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-007-T252 / ADR-0040 correction: the generated bridge's own
# genesis_parse_bridge_argv must itself reject --instruction-budget 0
# before genesis_runtime_run is ever called, not just the Python CLI.
add_test(NAME genesis_general_startup_bridge_argv_zero_budget_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_general_startup_bridge_argv_zero_budget_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-022-T003: sharded generated-C translation units (strict C11, same behaviour as the single file).
add_test(NAME genesis_generated_c_translation_units_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_generated_c_translation_units_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
# SEG-022-T004: bounded parallel generated-TU compilation (fake compiler, no ROM).
add_test(NAME genesis_bridge_parallel_compile_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_bridge_parallel_compile_test.py ${PROJECT_SOURCE_DIR})
add_test(NAME genesis_bridge_object_cache_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_bridge_object_cache_test.py ${PROJECT_SOURCE_DIR} ${CMAKE_C_COMPILER})
add_test(NAME move_an_usp_bridge_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/move_an_usp_bridge_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME direct_flow_static_slice_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/direct_flow_static_slice_test.py $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER})
add_test(
  NAME moveq_static_slice_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/moveq_static_slice_test.py
    $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER}
)
# Test-only harness (SEG-007-T008): exercises the shared TST.L (xxx).L
# decode -> lift -> operand-resolution -> effect -> emit pipeline directly
# for tst_l_static_slice_test.py, without exposing a second
# instruction-specific production API/header. Not part of the segarecomp
# production CLI.
add_executable(tst_l_test_harness tools/tst_l_test_harness.cpp)
 target_link_libraries(tst_l_test_harness PRIVATE segarecomp::media segarecomp::machine_genesis segarecomp::codegen_c11_genesis)
segarecomp_enable_warnings(tst_l_test_harness)
add_test(
  NAME tst_l_static_slice_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tst_l_static_slice_test.py
    $<TARGET_FILE:tst_l_test_harness> ${CMAKE_C_COMPILER}
)
# SEG-007-T009: independent adversarial validation of the same TST.L (xxx).L
# capability -- boundaries, step-8 precedence ties, determinism, and an
# optional pinned Musashi differential (SEGARECOMP_TST_L_MUSASHI_ORACLE).
add_test(
  NAME tst_l_adversarial_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tst_l_adversarial_test.py
    $<TARGET_FILE:tst_l_test_harness> $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER}
)
# Test-only harness (SEG-007-T023): exercises the shared common MC68000
# startup/data-movement batch (TST/MOVE/MOVEA/CLR/LEA/JMP/JSR) decode ->
# lift -> operand-resolution -> effect -> emit pipeline directly, modeled on
# tst_l_test_harness.cpp above. Not part of the segarecomp production CLI.
add_executable(m68k_batch_a_test_harness tools/m68k_batch_a_test_harness.cpp)
 target_link_libraries(m68k_batch_a_test_harness PRIVATE segarecomp::media segarecomp::machine_genesis segarecomp::codegen_c11_genesis)
segarecomp_enable_warnings(m68k_batch_a_test_harness)
add_test(
  NAME m68k_batch_a_static_slice_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m68k_batch_a_static_slice_test.py
    $<TARGET_FILE:m68k_batch_a_test_harness> ${CMAKE_C_COMPILER}
)
# SEG-007-T023: independent adversarial validation of the same batch --
# legality/size/truncation/alignment/mapping boundaries, CCR proofs, and an
# optional pinned Musashi differential (SEGARECOMP_M68K_BATCH_A_MUSASHI_CHECKOUT).
add_test(
  NAME m68k_batch_a_adversarial_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m68k_batch_a_adversarial_test.py
    $<TARGET_FILE:m68k_batch_a_test_harness> $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER}
)
add_test(
  NAME m68k_batch_a_review_corrections_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m68k_batch_a_review_corrections_test.py
    $<TARGET_FILE:m68k_batch_a_test_harness> ${CMAKE_C_COMPILER}
)
add_executable(m68k_batch_b_test_harness tools/m68k_batch_b_test_harness.cpp)
 target_link_libraries(m68k_batch_b_test_harness PRIVATE segarecomp::media segarecomp::machine_genesis segarecomp::codegen_c11_genesis)
segarecomp_enable_warnings(m68k_batch_b_test_harness)
add_test(NAME m68k_batch_b_static_slice_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_batch_b_static_slice_test.py
  $<TARGET_FILE:m68k_batch_b_test_harness> ${CMAKE_C_COMPILER})
add_test(NAME m68k_batch_b_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_batch_b_musashi_differential_test.py
  $<TARGET_FILE:m68k_batch_b_test_harness> ${CMAKE_C_COMPILER})
# SEG-007-T025 (Batch C): the one Batch-C differential harness, reused
# unmodified by every C1-C7 checkpoint (see tools/m68k_batch_c_test_harness.cpp).
add_executable(m68k_batch_c_test_harness tools/m68k_batch_c_test_harness.cpp)
 target_link_libraries(m68k_batch_c_test_harness PRIVATE segarecomp::media segarecomp::machine_genesis segarecomp::codegen_c11_genesis)
segarecomp_enable_warnings(m68k_batch_c_test_harness)
add_test(NAME m68k_batch_c_static_slice_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_batch_c_static_slice_test.py
  $<TARGET_FILE:m68k_batch_c_test_harness> ${CMAKE_C_COMPILER})
add_test(NAME m68k_neg_word_generated_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_neg_word_generated_test.py
  $<TARGET_FILE:m68k_batch_c_test_harness> ${CMAKE_C_COMPILER})
add_test(NAME m68k_batch_c_musashi_differential_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_batch_c_musashi_differential_test.py
  $<TARGET_FILE:m68k_batch_c_test_harness> ${CMAKE_C_COMPILER})
add_test(
  NAME ingestion_cli_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ingestion_cli_test.py
    $<TARGET_FILE:segarecomp>
)
add_test(
  NAME genesis_reset_image_cli_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/genesis_reset_image_cli_test.py
    $<TARGET_FILE:segarecomp>
)
add_test(
  NAME genesis_reset_image_adversarial_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/genesis_reset_image_adversarial_test.py
    $<TARGET_FILE:segarecomp>
)
add_test(
  NAME ingestion_adversarial_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ingestion_adversarial_test.py
    $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER}
)
add_test(
  NAME generated_c_test
  COMMAND ${CMAKE_COMMAND}
    -DSEGARECOMP=$<TARGET_FILE:segarecomp>
    -DC_COMPILER=${CMAKE_C_COMPILER}
    -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/generated-c-test
    -P ${CMAKE_CURRENT_SOURCE_DIR}/generated_c_test.cmake
)
add_test(
  NAME ghidra_tool_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ghidra_tool_test.py
)
# SEG-007-T165 correction: the canonical, worktree-independent private
# external-analysis-hints path resolver (mirrors ghidra_tool_test's own
# worktree-independence proof for tools/ghidra.py).
add_test(
  NAME analysis_hints_path_tool_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/analysis_hints_path_tool_test.py
)
# SEG-007-T162: real bridge->Ghidra MCP smoke path. Skips (exit 0 with a
# reported reason) when Docker/Compose or the pinned stack is unavailable, so
# it never makes CTest depend on Docker or a running Ghidra stack.
add_test(
  NAME ghidra_mcp_smoke_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ghidra_mcp_smoke_test.py
)
add_test(
  NAME restricted_files_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/restricted_files_test.py
)
# SEG-007-T206 / ADR-0034: structural schema guard for the narrow
# compat/genesis/<rom-sha256>.json committed-compatibility-metadata
# surface -- fails the build if a future change widens it beyond the
# documented scalar assertion schema.
add_test(
  NAME compat_genesis_schema_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/compat_genesis_schema_test.py
)
# SEG-021-T017 / ADR-0043: MC68000 Group 1/2 exception-frame contract guard (six-byte
# SR-at-SP, PC-at-SP+2 frame; no 68010 format word) in the ADR and the runtime primitive.
add_test(
  NAME m68k_exception_contract_doc_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m68k_exception_contract_doc_test.py
)
add_test(
  NAME probe_genesis_startup_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/probe_genesis_startup_test.py
    $<TARGET_FILE:segarecomp>
)
add_test(
  NAME stage1_classifier_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stage1_classifier_test.py
)
add_test(
  NAME stage1_classifier_tooling_only_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stage1_classifier_tooling_only_test.py
)
# SEG-014-T002: fails closed if any cpu/m68k/ file textually #includes the
# remaining Genesis-shaped m68k_pipeline.hpp god-header or references a
# Genesis/machine/recompiler/codegen-owned symbol, complementing the
# link-level separation segarecomp_cpu_m68k's own target_link_libraries
# (linking only segarecomp_base, never segarecomp_core) already provides.
# SEG-021-T018 / ADR 0043 §7: the M68K-owned exception-entry/RTE core is machine-independent strict C11.
add_test(
  NAME m68k_exception_core_ownership_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/m68k_exception_core_ownership_test.py ${CMAKE_C_COMPILER}
)
add_test(
  NAME cpu_m68k_no_genesis_dependency_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cpu_m68k_no_genesis_dependency_test.py
)
add_test(
  NAME recompiler_no_genesis_policy_dependency_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recompiler_no_genesis_policy_dependency_test.py
)
add_test(
  NAME sonic_startup_inventory_privacy_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sonic_startup_inventory_privacy_test.py
    $<TARGET_FILE:segarecomp>
)
add_test(
  NAME sonic_startup_inventory_gates_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sonic_startup_inventory_gates_test.py
    $<TARGET_FILE:segarecomp>
)
add_test(
  NAME sonic_startup_inventory_semantic_gap_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sonic_startup_inventory_semantic_gap_test.py
)
add_test(
  NAME sonic_startup_inventory_adapter_failure_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sonic_startup_inventory_adapter_failure_test.py
    $<TARGET_FILE:segarecomp>
)
add_test(
  NAME sonic_first_unsupported_local_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sonic_first_unsupported_local_test.py
    $<TARGET_FILE:segarecomp>
)
set_tests_properties(
  sonic_first_unsupported_local_test
  PROPERTIES SKIP_RETURN_CODE 77
)
add_test(
  NAME sonic_general_startup_controller_frontier_local_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sonic_general_startup_controller_frontier_local_test.py
    $<TARGET_FILE:segarecomp>
)
set_tests_properties(
  sonic_general_startup_controller_frontier_local_test
  PROPERTIES SKIP_RETURN_CODE 77
)
# SEG-014-T001: the deterministic project-owned architecture baseline
# verifier -- the regression anchor later SEG-014 structural-move tasks
# must run instead of relying only on "tests are still green".
add_test(
  NAME architecture_baseline_verifier_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/architecture_baseline_verifier_test.py
    $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER}
)
# SEG-022-T001: generated-code scalability attribution tool must partition bytes exactly.
add_test(
  NAME generated_code_scalability_report_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generated_code_scalability_report_test.py
    ${PROJECT_SOURCE_DIR}/tools/generated_code_scalability_report.py
)
add_test(
  NAME genesis_runtime_ownership_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/genesis_runtime_ownership_test.py
)
# SEG-014-T005 correction: prove the real C11 rendering bodies the T001
# migration map assigns to codegen/c11 physically live there, not in the
# legacy m68k_pipeline.cpp/m68k_pipeline_direct_flow.cpp/
# machine/genesis/frontend.cpp/c_emitter.cpp files.
add_test(
  NAME codegen_c11_emitter_ownership_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/codegen_c11_emitter_ownership_test.py
)
# SEG-018-T004: common/M68k-lowering/Genesis-wrapper C11 codegen dependency direction.
add_test(
  NAME codegen_c11_layering_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/codegen_c11_layering_test.py
)

add_test(NAME genesis_bridge_defaults_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/genesis_bridge_defaults_test.py)

add_executable(codegen_c11_provenance_diagnostics_tests codegen_c11_provenance_diagnostics_test.cpp)
target_link_libraries(codegen_c11_provenance_diagnostics_tests PRIVATE segarecomp::codegen_c11_m68k)
segarecomp_enable_warnings(codegen_c11_provenance_diagnostics_tests)
add_test(NAME codegen_c11_provenance_diagnostics_tests COMMAND codegen_c11_provenance_diagnostics_tests)
# SEG-022-T003: deterministic generated-C translation-unit sharder.
add_executable(codegen_c11_translation_units_tests codegen_c11_translation_units_test.cpp)
target_link_libraries(codegen_c11_translation_units_tests PRIVATE segarecomp::codegen_c11)
segarecomp_enable_warnings(codegen_c11_translation_units_tests)
add_test(NAME codegen_c11_translation_units_tests COMMAND codegen_c11_translation_units_tests)
# Safety net only (a portability regression must fail CI, not occupy a runner); the test takes well under a second.
set_tests_properties(codegen_c11_translation_units_tests PROPERTIES TIMEOUT 30)
add_executable(codegen_c11_shard_selection_tests codegen_c11_shard_selection_test.cpp)
target_link_libraries(codegen_c11_shard_selection_tests PRIVATE segarecomp::codegen_c11_genesis)
segarecomp_enable_warnings(codegen_c11_shard_selection_tests)
add_test(NAME codegen_c11_shard_selection_tests COMMAND codegen_c11_shard_selection_tests)
# SEG-018-T001: tier labels. `full` = every normal hermetic test (parallel-safe),
# `fast` = representative hermetic subset of `full`, `extended` = external or
# environment-heavy checks kept out of the ordinary gate. Selection is explicit
# (labels), never inferred from changed files. `focused` runs use `ctest -R <name>`.
# genesis_experiment_aligned_aot_report_failure_test drives the ADR-0039 developer
# measurement utility, which is built on POSIX resource.getrusage(RUSAGE_CHILDREN); it is
# not a product/generated-execution gate, so it lives in `extended` on every host.
set(SEGARECOMP_EXTENDED_TESTS ghidra_mcp_smoke_test
    genesis_experiment_aligned_aot_report_failure_test)
set(SEGARECOMP_FAST_TESTS segarecomp_tests moveq_tests direct_flow_tests m68k_timing_tests
    genesis_runtime_run_tests genesis_vdp_render_frame_tests genesis_frame_export_tests
    m68k_frontend_rejection_tests genesis_general_startup_cli_test architecture_dependency_test
    codegen_c11_emitter_ownership_test codegen_c11_translation_units_tests codegen_c11_shard_selection_tests restricted_files_test genesis_viewer_tests
    cpu_m68k_static_discovery_tests genesis_startup_runtime_c2_test recompiler_fake_cpu_tests recompiler_no_genesis_policy_dependency_test
    genesis_bridge_defaults_test m68k_exception_contract_doc_test)
# Evidence (SEG-018-T001): its genuine 1 s adapter/probe timeout cases race process start-up
# under parallel load (observed failing at -j 8 and -j 12), so it runs alone.
set_tests_properties(sonic_startup_inventory_adapter_failure_test sonic_startup_inventory_privacy_test PROPERTIES RUN_SERIAL TRUE)
get_property(_segarecomp_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _segarecomp_all_tests)
  if(_t IN_LIST SEGARECOMP_EXTENDED_TESTS)
    set_property(TEST ${_t} APPEND PROPERTY LABELS extended)
  else()
    set_property(TEST ${_t} APPEND PROPERTY LABELS full)
    if(_t IN_LIST SEGARECOMP_FAST_TESTS)
      set_property(TEST ${_t} APPEND PROPERTY LABELS fast)
    endif()
  endif()
endforeach()

# SEG-018-T006: several tests write UTF-8 fixtures through Python's locale
# encoding (cp1252 on Windows); force UTF-8 mode for every test so behaviour
# matches the POSIX hosts.
get_property(_segarecomp_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _segarecomp_all_tests)
  set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
  if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # Test-authored C harnesses call ISO C stdio directly before any project header;
    # the clang driver reads this to prepend the CRT selection to every compile.
    set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "CCC_OVERRIDE_OPTIONS=set:#^-D_CRT_SECURE_NO_WARNINGS ^-D_CRT_NONSTDC_NO_WARNINGS")
  endif()
endforeach()

# SEG-021-T001: independent legal base-MC68000 form dataset (test-side expectation only) and its
# pinned-Musashi primary-word cross-check. The crosscheck skips cleanly (exit 0) unless
# SEGARECOMP_M68K_MULS_WORD_MUSASHI_CHECKOUT points at the pinned checkout.
add_test(NAME m68k_legal_forms_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_legal_forms_test.py ${PROJECT_SOURCE_DIR})
add_test(NAME m68k_legal_forms_musashi_crosscheck_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_legal_forms_musashi_crosscheck_test.py ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
set_property(TEST m68k_legal_forms_test m68k_legal_forms_musashi_crosscheck_test APPEND PROPERTY LABELS full fast)
set_property(TEST m68k_legal_forms_test m68k_legal_forms_musashi_crosscheck_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")

# SEG-020-T005: first-divergence diagnosis (generated M68k boundaries vs pinned Musashi).
add_test(NAME m68k_first_divergence_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_first_divergence_test.py
  $<TARGET_FILE:m68k_pipeline_tests> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})

# SEG-020-T008: registered after the label loop, so label it explicitly (full gate coverage of the
# CPU-domain first-divergence demonstration; it skips the Musashi comparison when the pin is unset).
set_property(TEST m68k_first_divergence_test APPEND PROPERTY LABELS full)
set_property(TEST m68k_first_divergence_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")

# SEG-021-T002: per-word pipeline capability probe (public entry points only); driven by
# tools/m68k_capability_coverage.py and tests/m68k_capability_ratchet_test.py.
add_executable(m68k_capability_probe tools/m68k_capability_probe.cpp)
target_link_libraries(m68k_capability_probe PRIVATE segarecomp::machine_genesis segarecomp::codegen_c11_genesis)
target_compile_features(m68k_capability_probe PRIVATE cxx_std_20)
segarecomp_enable_warnings(m68k_capability_probe)
# SEG-021-T003: emitter driver of the table-driven generated-native differential conformance harness
# (driven by tools/m68k_conformance.py and tests/m68k_conformance_harness_test.py).
add_executable(m68k_conformance_emitter tools/m68k_conformance_emitter.cpp)
target_link_libraries(m68k_conformance_emitter PRIVATE segarecomp::machine_genesis segarecomp::codegen_c11_genesis)
target_compile_features(m68k_conformance_emitter PRIVATE cxx_std_20)
segarecomp_enable_warnings(m68k_conformance_emitter)
add_test(NAME m68k_capability_ratchet_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_capability_ratchet_test.py
  $<TARGET_FILE:m68k_capability_probe> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME m68k_capability_coverage_unit_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_capability_coverage_unit_test.py ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
add_test(NAME m68k_conformance_harness_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_conformance_harness_test.py
  $<TARGET_FILE:m68k_conformance_emitter> ${CMAKE_C_COMPILER})
# SEG-021-T005: routed-versus-direct lowering differential (deferred address-register commit paths).
add_test(NAME m68k_routed_lowering_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_routed_lowering_test.py
  $<TARGET_FILE:m68k_conformance_emitter> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
set_property(TEST m68k_routed_lowering_test APPEND PROPERTY LABELS full fast)
set_property(TEST m68k_routed_lowering_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
# SEG-021-T015: exhaustive BCD byte sweeps versus the pinned Musashi core (skips with exit 0 without the checkout).
add_test(NAME m68k_bcd_exhaustive_musashi_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/m68k_bcd_exhaustive_musashi_test.py
  $<TARGET_FILE:m68k_conformance_emitter> ${CMAKE_C_COMPILER})
set_property(TEST m68k_bcd_exhaustive_musashi_test PROPERTY TIMEOUT 1500)
set_property(TEST m68k_conformance_harness_test APPEND PROPERTY LABELS full fast)
set_property(TEST m68k_conformance_harness_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
set_property(TEST m68k_capability_ratchet_test m68k_capability_coverage_unit_test APPEND PROPERTY LABELS full fast)
set_property(TEST m68k_capability_coverage_unit_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
set_property(TEST m68k_capability_ratchet_test APPEND PROPERTY ENVIRONMENT_MODIFICATION "PYTHONUTF8=set:1")
