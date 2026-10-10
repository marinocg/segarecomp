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
