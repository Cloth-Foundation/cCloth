if(NOT DEFINED CLOTH_PROGRAM OR NOT DEFINED CLOTH_EXPECTED)
    message(FATAL_ERROR "CLOTH_PROGRAM and CLOTH_EXPECTED are required")
endif()

execute_process(
    COMMAND "${CLOTH_PROGRAM}"
    RESULT_VARIABLE program_result
    OUTPUT_VARIABLE actual_output
    ERROR_VARIABLE program_error
)
if(NOT program_result EQUAL 0)
    message(FATAL_ERROR
        "${CLOTH_PROGRAM} exited with ${program_result}: ${program_error}")
endif()

file(READ "${CLOTH_EXPECTED}" expected_output)
string(REPLACE "\r\n" "\n" actual_output "${actual_output}")
string(REPLACE "\r\n" "\n" expected_output "${expected_output}")
if(NOT actual_output STREQUAL expected_output)
    message(FATAL_ERROR
        "program output did not match ${CLOTH_EXPECTED}\n"
        "actual output:\n${actual_output}")
endif()
