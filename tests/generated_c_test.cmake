file(MAKE_DIRECTORY "${TEST_DIR}")
string(REPEAT " " 256 padding)
string(REPEAT " " 17 system_padding)
string(REPEAT " " 33 title_padding)
file(WRITE "${TEST_DIR}/synthetic.bin" "${padding}SEGA MEGA DRIVE${system_padding}SYNTHETIC TITLE${title_padding}")
execute_process(COMMAND "${SEGARECOMP}" emit-c "${TEST_DIR}/synthetic.bin" OUTPUT_FILE "${TEST_DIR}/generated.c" RESULT_VARIABLE emit_result)
if(NOT emit_result EQUAL 0)
  message(FATAL_ERROR "segarecomp emit-c failed with status ${emit_result}")
endif()
file(READ "${TEST_DIR}/generated.c" generated_c)
foreach(required_metadata IN ITEMS segarecomp_input_size segarecomp_size_limit segarecomp_outcome segarecomp_diagnostic segarecomp_inspection)
  string(FIND "${generated_c}" "${required_metadata}" metadata_offset)
  if(metadata_offset EQUAL -1)
    message(FATAL_ERROR "generated C is missing ${required_metadata}")
  endif()
endforeach()
foreach(optimization IN ITEMS -O0 -O2)
  string(REPLACE "-" "" optimization_name "${optimization}")
  execute_process(
    COMMAND "${C_COMPILER}" -std=c11 -Wall -Wextra -Werror "${optimization}" -c
            "${TEST_DIR}/generated.c" -o "${TEST_DIR}/generated-${optimization_name}.o"
    OUTPUT_VARIABLE compiler_output
    ERROR_VARIABLE compiler_error
    RESULT_VARIABLE compiler_result
  )
  if(NOT compiler_result EQUAL 0)
    message(FATAL_ERROR "generated C did not compile at ${optimization}:\n${compiler_output}${compiler_error}")
  endif()
endforeach()
