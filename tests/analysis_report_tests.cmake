# SEG-030-T002 (ADR 0079): tests of the report-only Genesis M68K analysis driver.
#
# Included from tests/CMakeLists.txt it registers the tests. Run with `cmake -P` (CMAKE_SCRIPT_MODE_FILE) it is the driver test
# itself: it runs `segarecomp-genesis-analysis-report` on the synthetic fixture written by analysis_report_test, checks
# byte-identical repeated output and identity with the library report, fail-closed usage/digest rejection, and that the private
# output is consumed unchanged by tools/reachability_coverage_compare.py (zero recovery escapes).

if(CMAKE_SCRIPT_MODE_FILE)
  foreach(required DRIVER PYTHON SOURCE DIR)
    if(NOT DEFINED ${required})
      message(FATAL_ERROR "analysis_report driver test: ${required} is not set")
    endif()
  endforeach()
  file(READ "${DIR}/rom.sha256" sha)
  file(READ "${DIR}/discovered.txt" discovered)
  set(arguments --rom "${DIR}/rom.bin" --rom-sha256 "${sha}" --entry 00000200 --mapping-base 00000000
    --immutable-copy-alias 00ff0000:00000400:00000080 --immutable-copy-alias 00ff0100:00000500:00000004)
  foreach(run 1 2)
    execute_process(COMMAND "${DRIVER}" ${arguments} --domains baseline --private-output "${DIR}/driver${run}.json"
      OUTPUT_FILE "${DIR}/driver${run}.stdout" RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "driver run ${run} failed: ${status}")
    endif()
  endforeach()
  foreach(pair "driver1.json;driver2.json" "driver1.stdout;driver2.stdout" "driver1.json;library.json")
    list(GET pair 0 left)
    list(GET pair 1 right)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DIR}/${left}" "${DIR}/${right}" RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
      message(FATAL_ERROR "${left} and ${right} differ")
    endif()
  endforeach()
  # SEG-030-T003: the address domain runs deterministically on the same fixture.
  foreach(run 1 2)
    execute_process(COMMAND "${DRIVER}" ${arguments} --domains address --private-output "${DIR}/address${run}.json"
      OUTPUT_FILE "${DIR}/address${run}.stdout" RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "driver address run ${run} failed: ${status}")
    endif()
  endforeach()
  foreach(pair "address1.json;address2.json" "address1.stdout;address2.stdout")
    list(GET pair 0 left)
    list(GET pair 1 right)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DIR}/${left}" "${DIR}/${right}" RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
      message(FATAL_ERROR "${left} and ${right} differ")
    endif()
  endforeach()
  # SEG-030-T004: the memory domain (credited model and the labelled diagnostic premise ablation) runs deterministically.
  foreach(variant "memory" "ablation")
    set(extra)
    if(variant STREQUAL "ablation")
      set(extra --assume-no-z80-ram-writes)
    endif()
    foreach(run 1 2)
      execute_process(COMMAND "${DRIVER}" ${arguments} --domains memory ${extra} --private-output "${DIR}/${variant}${run}.json"
        OUTPUT_FILE "${DIR}/${variant}${run}.stdout" RESULT_VARIABLE status)
      if(NOT status EQUAL 0)
        message(FATAL_ERROR "driver ${variant} run ${run} failed: ${status}")
      endif()
    endforeach()
    foreach(pair "${variant}1.json;${variant}2.json" "${variant}1.stdout;${variant}2.stdout")
      list(GET pair 0 left)
      list(GET pair 1 right)
      execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DIR}/${left}" "${DIR}/${right}" RESULT_VARIABLE differ)
      if(NOT differ EQUAL 0)
        message(FATAL_ERROR "${left} and ${right} differ")
      endif()
    endforeach()
  endforeach()
  # SEG-030-T005: the contexts domain (implies memory and address) runs deterministically and reports its bounds and rounds.
  foreach(run 1 2)
    execute_process(COMMAND "${DRIVER}" ${arguments} --domains contexts --private-output "${DIR}/contexts${run}.json"
      OUTPUT_FILE "${DIR}/contexts${run}.stdout" RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "driver contexts run ${run} failed: ${status}")
    endif()
  endforeach()
  foreach(pair "contexts1.json;contexts2.json" "contexts1.stdout;contexts2.stdout")
    list(GET pair 0 left)
    list(GET pair 1 right)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DIR}/${left}" "${DIR}/${right}" RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
      message(FATAL_ERROR "${left} and ${right} differ")
    endif()
  endforeach()
  # SEG-030-T006: the frames domain (implies contexts, memory and address) runs deterministically and reports its own object.
  foreach(run 1 2)
    execute_process(COMMAND "${DRIVER}" ${arguments} --domains frames --private-output "${DIR}/frames${run}.json"
      OUTPUT_FILE "${DIR}/frames${run}.stdout" RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
      message(FATAL_ERROR "driver frames run ${run} failed: ${status}")
    endif()
  endforeach()
  foreach(pair "frames1.json;frames2.json" "frames1.stdout;frames2.stdout")
    list(GET pair 0 left)
    list(GET pair 1 right)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DIR}/${left}" "${DIR}/${right}" RESULT_VARIABLE differ)
    if(NOT differ EQUAL 0)
      message(FATAL_ERROR "${left} and ${right} differ")
    endif()
  endforeach()
  file(READ "${DIR}/frames1.stdout" frames_aggregate)
  if(NOT frames_aggregate MATCHES "\"frames\":true" OR NOT frames_aggregate MATCHES "\"contexts\":true" OR
     NOT frames_aggregate MATCHES "\"frames\":\\{\"validated\":true" OR NOT frames_aggregate MATCHES "\"instance_depth_bound\":3" OR
     frames_aggregate MATCHES "ff0|\"200\"|00ff")
    message(FATAL_ERROR "the frames aggregate is not the validated frames model: ${frames_aggregate}")
  endif()
  execute_process(COMMAND "${PYTHON}" "${SOURCE}/tools/reachability_coverage_compare.py" --coverage-dir "${DIR}/coverage"
    --challenger "${DIR}/frames1.json" OUTPUT_VARIABLE compared RESULT_VARIABLE status)
  if(NOT status EQUAL 0 OR NOT compared MATCHES "\"escapes_outside_proven_targets\":0")
    message(FATAL_ERROR "the compare tool rejected the frames private output: ${status} ${compared}")
  endif()
  file(READ "${DIR}/contexts1.stdout" contexts_aggregate)
  if(NOT contexts_aggregate MATCHES "\"contexts\":true" OR NOT contexts_aggregate MATCHES "\"memory\":true" OR
     NOT contexts_aggregate MATCHES "\"context_bound\":8" OR NOT contexts_aggregate MATCHES "\"contexts\":\\{\"validated\":true" OR
     contexts_aggregate MATCHES "ff0|\"200\"|00ff")
    message(FATAL_ERROR "the contexts aggregate is not the validated contexts model: ${contexts_aggregate}")
  endif()
  file(READ "${DIR}/memory1.stdout" memory_aggregate)
  file(READ "${DIR}/ablation1.stdout" ablation_aggregate)
  if(NOT memory_aggregate MATCHES "\"memory\":true" OR NOT memory_aggregate MATCHES "\"address\":true" OR
     NOT memory_aggregate MATCHES "\"async_all\":true" OR memory_aggregate MATCHES "diagnostic_premise_ablation")
    message(FATAL_ERROR "the memory aggregate is not the credited model: ${memory_aggregate}")
  endif()
  if(NOT ablation_aggregate MATCHES "\"diagnostic_premise_ablation\":\"assume_no_z80_ram_writes" OR
     NOT ablation_aggregate MATCHES "\"external_writer\":false")
    message(FATAL_ERROR "the ablation aggregate is not labelled: ${ablation_aggregate}")
  endif()
  # An exhausted bound fails the run (no empty D mistaken for a result); the aggregate says complete:false.
  execute_process(COMMAND "${DRIVER}" ${arguments} --max-iterations 1 --private-output "${DIR}/bounded.json"
    OUTPUT_VARIABLE bounded RESULT_VARIABLE status)
  if(status EQUAL 0 OR NOT bounded MATCHES "\"complete\":false")
    message(FATAL_ERROR "a bound-exhausted run succeeded: ${status} ${bounded}")
  endif()
  file(READ "${DIR}/driver1.stdout" aggregate)
  if(aggregate MATCHES "ff0|\"200\"|00ff")
    message(FATAL_ERROR "the aggregate carries an address: ${aggregate}")
  endif()
  # Fail closed: a wrong digest, a malformed or repeated domain list, a malformed bound and a missing private output.
  foreach(bad "--rom-sha256;0000000000000000000000000000000000000000000000000000000000000000"
              "--domains;frames,frames" "--domains;address,bogus" "--domains;baseline,memory" "--max-iterations;0"
              "--assume-no-z80-ram-writes" "--domains;address;--assume-no-z80-ram-writes" "--max-points;8;--max-points;8"
              "--max-iterations;8;--max-iterations;8" "--private-output")
    set(candidate ${arguments})
    list(GET bad 0 option)
    if(option STREQUAL "--rom-sha256")
      list(REMOVE_ITEM candidate "${sha}")
      list(REMOVE_ITEM candidate --rom-sha256)
    endif()
    if(NOT option STREQUAL "--private-output")
      list(APPEND candidate --private-output "${DIR}/rejected.json")
    endif()
    execute_process(COMMAND "${DRIVER}" ${candidate} ${bad} OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE status)
    if(status EQUAL 0)
      message(FATAL_ERROR "the driver accepted invalid input: ${bad}")
    endif()
  endforeach()
  # Untrusted input: an unbounded or oversized image is rejected before hashing.
  if(EXISTS /dev/zero)
    set(candidate ${arguments})
    list(REMOVE_ITEM candidate "${DIR}/rom.bin" --rom)
    execute_process(COMMAND "${DRIVER}" --rom /dev/zero ${candidate} --private-output "${DIR}/rejected.json" OUTPUT_QUIET ERROR_QUIET
      RESULT_VARIABLE status TIMEOUT 60)
    if(status EQUAL 0 OR NOT status MATCHES "^[0-9]+$")
      message(FATAL_ERROR "the driver accepted (or hung on) an unbounded image: ${status}")
    endif()
  endif()
  execute_process(COMMAND "${PYTHON}" "${SOURCE}/tools/reachability_coverage_compare.py" --coverage-dir "${DIR}/coverage"
    --challenger "${DIR}/bounded.json" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE status)
  if(status EQUAL 0)
    message(FATAL_ERROR "the compare tool accepted an incomplete report")
  endif()
  execute_process(COMMAND "${PYTHON}" "${SOURCE}/tools/reachability_coverage_compare.py" --coverage-dir "${DIR}/coverage"
    --challenger "${DIR}/driver1.json" OUTPUT_VARIABLE compared RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "the compare tool rejected the private output: ${status}")
  endif()
  foreach(expected "\"D\":${discovered}," "\"O\":8," "\"O_and_D\":8," "\"escapes_outside_proven_targets\":0,"
                   "\"resolved_sites\":1," "\"proven_targets_observed\":1,")
    string(FIND "${compared}" "${expected}" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "compare tool output lacks ${expected}: ${compared}")
    endif()
  endforeach()
  execute_process(COMMAND "${PYTHON}" "${SOURCE}/tools/reachability_coverage_compare.py" --coverage-dir "${DIR}/coverage"
    --challenger "${DIR}/address1.json" OUTPUT_VARIABLE compared RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "the compare tool rejected the address-domain private output: ${status}")
  endif()
  foreach(expected "\"computed_site_escape_check\":{\"escapes_outside_proven_targets\":0," "\"D\":${discovered},")
    string(FIND "${compared}" "${expected}" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "compare tool output lacks ${expected}: ${compared}")
    endif()
  endforeach()
  message(STATUS "analysis_report_driver_test: all checks passed")
  return()
endif()

add_executable(analysis_report_test analysis_report_test.cpp)
target_link_libraries(analysis_report_test PRIVATE segarecomp::genesis_analysis_report)
segarecomp_enable_warnings(analysis_report_test)
set(SEGARECOMP_ANALYSIS_REPORT_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/analysis_report_fixture")
add_test(NAME analysis_report_test COMMAND analysis_report_test "${SEGARECOMP_ANALYSIS_REPORT_TEST_DIR}")
add_test(NAME analysis_report_driver_test COMMAND ${CMAKE_COMMAND}
  -DDRIVER=$<TARGET_FILE:segarecomp-genesis-analysis-report> -DPYTHON=${Python3_EXECUTABLE} -DSOURCE=${PROJECT_SOURCE_DIR}
  -DDIR=${SEGARECOMP_ANALYSIS_REPORT_TEST_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/analysis_report_tests.cmake)
set_tests_properties(analysis_report_test PROPERTIES FIXTURES_SETUP analysis_report_fixture)
set_tests_properties(analysis_report_driver_test PROPERTIES FIXTURES_REQUIRED analysis_report_fixture)
set_property(TEST analysis_report_test analysis_report_driver_test APPEND PROPERTY LABELS full fast)

# SEG-030-T006 (ADR 0079 decision 7): the named Genesis interrupt-source premise (header-only, over the CPU analysis library).
add_executable(analysis_genesis_interrupt_premise_test analysis_genesis_interrupt_premise_test.cpp)
target_link_libraries(analysis_genesis_interrupt_premise_test PRIVATE segarecomp::cpu_m68k_analysis)
target_include_directories(analysis_genesis_interrupt_premise_test PRIVATE ${PROJECT_SOURCE_DIR}/platforms/genesis/analysis_report/include)
segarecomp_enable_warnings(analysis_genesis_interrupt_premise_test)
add_test(NAME analysis_genesis_interrupt_premise_test COMMAND analysis_genesis_interrupt_premise_test)
set_property(TEST analysis_genesis_interrupt_premise_test APPEND PROPERTY LABELS full fast)

# SEG-030 (ADR 0079 decision 1): the build graph, not the text scan, proves that no production target links, compiles or
# includes the analysis. CMake >= 3.27 adds the codemodel query to this configure; an older CMake makes the test configure a
# private tree instead. The planted-bypass demonstration configures several private copies of the source tree (full tier only).
if(COMMAND cmake_file_api)
  cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
endif()
add_test(NAME analysis_build_graph_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/analysis_build_graph_test.py check
  --source-root ${PROJECT_SOURCE_DIR} --build-dir ${CMAKE_BINARY_DIR} --cmake ${CMAKE_COMMAND} --generator ${CMAKE_GENERATOR})
set_property(TEST analysis_build_graph_test APPEND PROPERTY LABELS full fast)
add_test(NAME analysis_build_graph_plant_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/analysis_build_graph_test.py
  plant --source-root ${PROJECT_SOURCE_DIR} --cmake ${CMAKE_COMMAND} --generator ${CMAKE_GENERATOR})
set_property(TEST analysis_build_graph_plant_test APPEND PROPERTY LABELS full)
set_property(TEST analysis_build_graph_plant_test PROPERTY TIMEOUT 600)

# SEG-030-T010 (ADR 0079 decision 7): the Genesis Z80 work-RAM store-freedom proof and its credited use by the report driver.
add_executable(analysis_genesis_z80_proof_test analysis_genesis_z80_proof_test.cpp)
target_link_libraries(analysis_genesis_z80_proof_test PRIVATE segarecomp::genesis_analysis_report)
segarecomp_enable_warnings(analysis_genesis_z80_proof_test)
add_test(NAME analysis_genesis_z80_proof_test COMMAND analysis_genesis_z80_proof_test)
set_property(TEST analysis_genesis_z80_proof_test APPEND PROPERTY LABELS full fast)
