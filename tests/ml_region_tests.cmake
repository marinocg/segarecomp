# Exact-map and ML region-producer test registrations (tools-only Python tests; SEG-044 source universe, SEG-046/047 ML producer).
# SEG-044-T002: the source-derived executable-universe extractor (assembler listing -> segarecomp.m68k_source_universe.v1).
# Hermetic: project-authored synthetic listings/ROMs only.
add_test(NAME segarecomp_source_map_extract_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_source_map_extract_test.py)
set_property(TEST segarecomp_source_map_extract_test APPEND PROPERTY LABELS full fast)

# SEG-044-T005: the diagnostic H = C intersect U plan builder (synthetic inputs only).
add_test(NAME segarecomp_source_universe_plan_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_source_universe_plan_test.py)
set_property(TEST segarecomp_source_universe_plan_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_ml_region_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_test.py)
set_property(TEST segarecomp_ml_region_test APPEND PROPERTY LABELS full fast)
# SEG-047-T007 (ADR 0096): the retired report-only analysis framework (and Ghidra/external-proof tooling) stays out of the product graph.
add_test(NAME report_only_analysis_absent_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/report_only_analysis_absent_test.py ${PROJECT_SOURCE_DIR})
set_property(TEST report_only_analysis_absent_test APPEND PROPERTY LABELS full fast)
# SEG-048 (ADR 0099): offline multi-title hardening experiment tooling (LOTO folds, candidates, blind barrier). Hermetic: synthetic data only.
add_test(NAME segarecomp_ml_region_v2_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_v2_test.py)
set_property(TEST segarecomp_ml_region_v2_test APPEND PROPERTY LABELS full fast)
# SEG-049 (ADR 0100): offline second-generation cross-title ML region experiment tooling (representation, FRF, nested calibration, leakage guards,
# v1 immutability). Hermetic: synthetic data only; the numpy/torch/scikit-learn tests self-skip where the private research packages are absent.
add_test(NAME segarecomp_ml_region_gen2_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_gen2_test.py)
set_property(TEST segarecomp_ml_region_gen2_test APPEND PROPERTY LABELS full fast)
# SEG-049-T002 (ADR 0101): rank-budget calibration of the frozen gen2 M3 ranking (budgets, tie-break, headroom, R/K gates, plan freeze, v1 guard).
add_test(NAME segarecomp_ml_region_gen2_rank_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_gen2_rank_test.py)
set_property(TEST segarecomp_ml_region_gen2_rank_test APPEND PROPERTY LABELS full fast)
# SEG-049-T003 (ADR 0102): native gen2 M3 + B35 route (generator drift, identity pins, CLI parity seams, bitwise goldens). Hermetic, synthetic data only.
add_test(NAME segarecomp_ml_region_gen2_native_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_gen2_native_test.py $<TARGET_FILE:segarecomp>)
set_property(TEST segarecomp_ml_region_gen2_native_test APPEND PROPERTY LABELS full fast)
# SEG-049-T003 (ADR 0102): the five-ROM native-vs-research selected-cell / region / K-stage gate on whichever source-backed images are held locally (skip: 77).
add_test(NAME genesis_ml_gen2_native_parity_local_test
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/segarecomp_ml_region_gen2_native_parity.py local
    --binary $<TARGET_FILE:segarecomp> --games ${PROJECT_SOURCE_DIR}/games)
set_tests_properties(genesis_ml_gen2_native_parity_local_test PROPERTIES SKIP_RETURN_CODE 77 LABELS full)
# SEG-049-T004/T005 (ADR 0103/0104): CLI authority order, explicit gen2 opt-in, machine-readable policy report, identities. Synthetic images only.
add_test(NAME segarecomp_aot_model_lifecycle_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_aot_model_lifecycle_test.py
  $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER} ${PROJECT_SOURCE_DIR})
set_property(TEST segarecomp_aot_model_lifecycle_test APPEND PROPERTY LABELS full fast)
# SEG-049-T004 (ADR 0103): ML producer types never reach the validator owner, the emitter, the runtime or the launcher.
add_test(NAME ml_producer_boundary_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/ml_producer_boundary_test.py ${PROJECT_SOURCE_DIR})
set_property(TEST ml_producer_boundary_test APPEND PROPERTY LABELS full fast)
# SEG-049-T004 (ADR 0103): a mutated model/schema/policy/weight table in the embedded gen2 producer fails closed (`model_identity`).
add_test(NAME genesis_ml_gen2_identity_mutation_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/genesis_ml_gen2_identity_mutation_test.py
  ${CMAKE_CXX_COMPILER} ${PROJECT_SOURCE_DIR})
set_property(TEST genesis_ml_gen2_identity_mutation_test APPEND PROPERTY LABELS full fast)
# SEG-049-T007 (ADR 0106): the release-package dependency/file policy (synthetic names; the real package is inspected by the release workflow).
add_test(NAME packaging_dependency_check_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/packaging_dependency_check_test.py)
set_property(TEST packaging_dependency_check_test APPEND PROPERTY LABELS full fast)
# SEG-051 (ADR 0116): Z80 executable-region experiment tooling (cells, ranking, FRF, fixed/adaptive budgets, lineage split and leakage guards,
# frozen-plan digest) and the CPU-owned Z80 decode-token report goldens (prefix families, boundaries, fail-closed). Hermetic, synthetic data only.
add_test(NAME segarecomp_ml_region_z80_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_z80_test.py $<TARGET_FILE:segarecomp_z80_token_report>)
set_property(TEST segarecomp_ml_region_z80_test APPEND PROPERTY LABELS full fast)
# SEG-056 (ADR 0117): second-generation SMS Z80 experiment definitions (structural roots, byte labels, byte->cell reductions, root-aware
# full-recall metrics, SDCC relocated-listing truth, frozen-plan integrity and sealed-cohort carry-over). Synthetic data only.
add_test(NAME segarecomp_ml_region_z80_gen2_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_z80_gen2_test.py)
set_property(TEST segarecomp_ml_region_z80_gen2_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_ml_region_z80_gen2_weights_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_region_z80_gen2_weights_test.py)
set_property(TEST segarecomp_ml_region_z80_gen2_weights_test APPEND PROPERTY LABELS full fast)
# SEG-060 (ADR 0123): oracle-bound screen of the adaptive retention-budget premise (stdlib only, committed SEG-056 results).
add_test(NAME segarecomp_sms_adaptive_budget_screen_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_sms_adaptive_budget_screen_test.py)
set_property(TEST segarecomp_sms_adaptive_budget_screen_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_m68k_adaptive_budget_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_m68k_adaptive_budget_test.py)
set_property(TEST segarecomp_m68k_adaptive_budget_test APPEND PROPERTY LABELS full fast)
# SEG-072 (ADR 0151): M68K ML v3 protocol definitions (fixed corpus, lineage folds, labels/masks, budgets, adaptive policy, selection,
# classification, sealed barrier, frozen-plan integrity). Synthetic data only.
add_test(NAME segarecomp_ml_v3_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_v3_test.py)
set_property(TEST segarecomp_ml_v3_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_ml_v3_sgdk_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_v3_sgdk_test.py)
set_property(TEST segarecomp_ml_v3_sgdk_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_ml_v3_train_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_v3_train_test.py)
set_property(TEST segarecomp_ml_v3_train_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_ml_v3_diagnose_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_ml_v3_diagnose_test.py)
set_property(TEST segarecomp_ml_v3_diagnose_test APPEND PROPERTY LABELS full fast)
# SEG-074-T001 (ADR 0156/0157): exact-code oracle compilation-cost benchmark. Pure helpers and the pre-registered gate on synthetic data; the
# end-to-end variant runs the project-authored exact-map fixture through the CLI (report-only `close` producer, unchanged validator), strict-C11
# compilation and a bounded generated-native run (POSIX only: per-process wait4 accounting; skipped elsewhere).
add_test(NAME segarecomp_oracle_bench_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_oracle_bench_test.py)
set_property(TEST segarecomp_oracle_bench_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_oracle_bench_fixture_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_oracle_bench_test.py $<TARGET_FILE:segarecomp> ${CMAKE_C_COMPILER})
set_property(TEST segarecomp_oracle_bench_fixture_test APPEND PROPERTY LABELS full fast)
set_property(TEST segarecomp_oracle_bench_fixture_test PROPERTY SKIP_RETURN_CODE 77)
# SEG-075-T001 (ADR 0159): cross-platform MC68000 source qualification. Plan invariants, executable-container probes, the CPU-variant scan,
# deterministic archive-pilot selection, the privacy guard, recipe evaluation and the sanitized manifest's contradiction check, all on
# synthetic data (no network, Docker or third-party source); the committed manifest is checked against the frozen plan.
add_test(NAME segarecomp_xplat_qualify_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_qualify_test.py)
set_property(TEST segarecomp_xplat_qualify_test APPEND PROPERTY LABELS full fast)
# SEG-075 C1 (ADR 0160): physical private-artifact audit, content-addressed store, verification (corruption/truncation/loss, worktree
# dependency, fresh isolated process), restore and the T001 audit classes, on synthetic data (git, no network or Docker).
add_test(NAME segarecomp_xplat_handoff_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_handoff_test.py)
set_property(TEST segarecomp_xplat_handoff_test APPEND PROPERTY LABELS full fast)
# SEG-075-T002 (ADR 0160): cross-platform container/listing adapters and label extraction on synthetic fixtures (PRG/hunk/ELF/a.out,
# vasm/A68K/GNU as listings, relocation masking, placement, decoder typing); the end-to-end case runs the real CPU-owned decoder on
# 68020 forms that must stay out of the base-68000 labels.
add_test(NAME segarecomp_xplat_corpus_test COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_corpus_test.py $<TARGET_FILE:segarecomp>)
set_property(TEST segarecomp_xplat_corpus_test APPEND PROPERTY LABELS full fast)
# SEG-075-T003/T004 (ADR 0161): lineage deduplication, feasibility gate, evaluation barrier, frozen plan invariants, metrics and the
# pre-registered transfer rule (standard library, synthetic data + the committed plan).
add_test(NAME segarecomp_xplat_design_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_design_test.py)
set_property(TEST segarecomp_xplat_design_test APPEND PROPERTY LABELS full fast)
# SEG-075-T005 (ADR 0161/0163): pre-registered B/C gates and candidate assembly (pure); structure / feature checks run only where numpy
# and torch are installed (research environment), otherwise skipped.
add_test(NAME segarecomp_xplat_models_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_models_test.py)
set_property(TEST segarecomp_xplat_models_test APPEND PROPERTY LABELS full fast)
# SEG-075-T006 (ADR 0163): the final decision re-derives from the committed results under the bound rules; the C7 receipt is sanitized.
add_test(NAME segarecomp_xplat_final_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_final_test.py)
set_property(TEST segarecomp_xplat_final_test APPEND PROPERTY LABELS full fast)
# SEG-076 (exploratory research tooling): fail-closed sealed-subtree resolver (both slash conventions, nonexistent paths), boundary
# strata, the pre-registered T001 expansion rule and plan tamper refusal (pure; synthetic data only).
add_test(NAME segarecomp_xplat_explore_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_explore_test.py)
set_property(TEST segarecomp_xplat_explore_test APPEND PROPERTY LABELS full fast)
# SEG-076-T003 (exploratory research tooling): pre-registered T003 statistics (exact sign-flip, t interval, transfer / stability
# classes), loss-log parsing and the job list (pure; synthetic data only).
add_test(NAME segarecomp_xplat_stability_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_stability_test.py)
set_property(TEST segarecomp_xplat_stability_test APPEND PROPERTY LABELS full fast)
# SEG-076-T005 (exploratory research tooling): pre-registered C-v2 vs A rule and job list (pure); C-v2's padded in-edge table where
# torch is installed (research environment), otherwise skipped.
add_test(NAME segarecomp_xplat_cv2_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_cv2_test.py)
set_property(TEST segarecomp_xplat_cv2_test APPEND PROPERTY LABELS full fast)
# SEG-076-T004 (exploratory research tooling): every committed SEG-076 class re-derives from its committed result document (research
# environment; skipped without numpy).
add_test(NAME segarecomp_xplat_seg076_final_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_xplat_seg076_final_test.py)
set_property(TEST segarecomp_xplat_seg076_final_test APPEND PROPERTY LABELS full fast)
set_property(TEST segarecomp_xplat_seg076_final_test PROPERTY SKIP_RETURN_CODE 77)
# SEG-077-T001 (research tooling): Genesis evidence-tier labeller pure logic - identity normalization, VDP DMA reconstruction, trace /
# disassembler parsing, executed + direct-flow acceptance, data admission rules, RAM copy origins (standard library, synthetic data).
add_test(NAME segarecomp_genesis_evidence_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_genesis_evidence_test.py)
set_property(TEST segarecomp_genesis_evidence_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_genesis_bizhawk_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_genesis_bizhawk_test.py)
set_property(TEST segarecomp_genesis_bizhawk_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_genesis_bizhawk_full_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_genesis_bizhawk_full_test.py)
set_property(TEST segarecomp_genesis_bizhawk_full_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_genesis_label_policy_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_genesis_label_policy_test.py)
set_property(TEST segarecomp_genesis_label_policy_test APPEND PROPERTY LABELS full fast)
# SEG-081-T001 (research tooling): three-game TAS batch - generic checkpoint Lua, Python port of the SEG-080 control policy (against the Lua when lua is
# installed), synthesized control movie, structural signatures and novelty, frozen classification / batch / model-A decision rules (synthetic data).
add_test(NAME segarecomp_genesis_tasbatch_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_genesis_tasbatch_test.py)
set_property(TEST segarecomp_genesis_tasbatch_test APPEND PROPERTY LABELS full fast)
# SEG-083-T001 (research tooling): five-game TAS batch layer - all-controller control, acquisition / independent-lineage / neural-readiness decision rules,
# region-level (ER, B35/B50) metrics on synthetic cells, frozen-plan integrity (synthetic data only).
add_test(NAME segarecomp_genesis_tasbatch5_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_genesis_tasbatch5_test.py)
set_property(TEST segarecomp_genesis_tasbatch5_test APPEND PROPERTY LABELS full fast)
# SEG-084-T001 (research tooling): M68K corpus audit helpers and the structural model-A reassessment rules (readiness decision, noise reference, independence,
# weighted-fit equivalence when numpy / scikit-learn are available; synthetic data only).
add_test(NAME segarecomp_seg084_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_seg084_test.py)
set_property(TEST segarecomp_seg084_test APPEND PROPERTY LABELS full fast)
# SEG-085 (research tooling): historical-evaluation diagnostics (error taxonomy, label-reliability audit) and the dynamic-budget policies; synthetic data only.
add_test(NAME segarecomp_seg085_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_seg085_test.py)
set_property(TEST segarecomp_seg085_test APPEND PROPERTY LABELS full fast)
# SEG-077-T005 (research tooling): frozen SEG-077 decision rules, labelled-cell blind metrics, the job matrix and, in the research
# environment (numpy, torch), label-free seeds and leakage invariance of A features and C's candidate graph (skipped without numpy).
add_test(NAME segarecomp_seg077_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_seg077_test.py)
set_property(TEST segarecomp_seg077_test APPEND PROPERTY LABELS full fast)
set_property(TEST segarecomp_seg077_test PROPERTY SKIP_RETURN_CODE 77)
# SEG-077-T009 (research tooling): corrected static-evidence measurement (v2) - known vs reference-unknown predictions, error kinds,
# error cells, marginal mixed / hard-negative credit and the root-cause taxonomy, including the v1 denominator bias (synthetic data).
add_test(NAME segarecomp_genesis_static_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_genesis_static_test.py)
set_property(TEST segarecomp_genesis_static_test APPEND PROPERTY LABELS full fast)
# SEG-086 (research tooling): sample-role enforcement (evaluation-only training rejection, frozen-manifest membership, reserved identities first); synthetic data only.
add_test(NAME segarecomp_seg086_roles_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_seg086_roles_test.py)
set_property(TEST segarecomp_seg086_roles_test APPEND PROPERTY LABELS full fast)
# SEG-086-T001..T004 (research tooling): score-distribution diagnostics, normalised dynamic selector, calibration and decision rules; synthetic data only.
add_test(NAME segarecomp_seg086_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_seg086_test.py)
set_property(TEST segarecomp_seg086_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_seg086_norm_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_seg086_norm_test.py)
set_property(TEST segarecomp_seg086_norm_test APPEND PROPERTY LABELS full fast)
add_test(NAME segarecomp_seg086_cal_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_with_resource_shim.py ${CMAKE_CURRENT_SOURCE_DIR}/segarecomp_seg086_cal_test.py)
set_property(TEST segarecomp_seg086_cal_test APPEND PROPERTY LABELS full fast)
