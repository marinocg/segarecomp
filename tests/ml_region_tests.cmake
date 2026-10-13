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
