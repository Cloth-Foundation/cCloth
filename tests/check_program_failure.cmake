if(NOT DEFINED CLOTH_PROGRAM OR NOT DEFINED CLOTH_EXPECTED_ERROR)
    message(FATAL_ERROR "CLOTH_PROGRAM and CLOTH_EXPECTED_ERROR are required")
endif()
if(NOT DEFINED CLOTH_PROGRAM_TIMEOUT)
    set(CLOTH_PROGRAM_TIMEOUT 10)
endif()

execute_process(
    COMMAND "${CLOTH_PROGRAM}"
    TIMEOUT "${CLOTH_PROGRAM_TIMEOUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if("${result}" STREQUAL "0")
    message(FATAL_ERROR "program unexpectedly succeeded; stdout: ${output}")
endif()

string(FIND "${error}" "${CLOTH_EXPECTED_ERROR}" error_position)
if(error_position EQUAL -1)
    message(FATAL_ERROR
        "program failed without expected error '${CLOTH_EXPECTED_ERROR}'; stderr: ${error}")
endif()
