// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

if(NOT DEFINED CLOTH_PROGRAM)
    message(FATAL_ERROR "CLOTH_PROGRAM is required")
endif()
if((DEFINED CLOTH_EXPECTED AND DEFINED CLOTH_EXPECTED_ERROR) OR
   (NOT DEFINED CLOTH_EXPECTED AND NOT DEFINED CLOTH_EXPECTED_ERROR))
    message(FATAL_ERROR
        "Define exactly one of CLOTH_EXPECTED or CLOTH_EXPECTED_ERROR")
endif()
if(NOT DEFINED CLOTH_PROGRAM_TIMEOUT)
    set(CLOTH_PROGRAM_TIMEOUT 10)
endif()

execute_process(
    COMMAND "${CLOTH_PROGRAM}"
    TIMEOUT "${CLOTH_PROGRAM_TIMEOUT}"
    RESULT_VARIABLE program_result
    OUTPUT_VARIABLE program_output
    ERROR_VARIABLE program_error
)

if(DEFINED CLOTH_EXPECTED)
    if(NOT "${program_result}" STREQUAL "0")
        message(FATAL_ERROR
            "${CLOTH_PROGRAM} exited with ${program_result}: ${program_error}")
    endif()
    file(READ "${CLOTH_EXPECTED}" expected_output)
    if(NOT CLOTH_EXACT_OUTPUT)
        string(REPLACE "\r\n" "\n" program_output "${program_output}")
        string(REPLACE "\r\n" "\n" expected_output "${expected_output}")
    endif()
    if(NOT program_output STREQUAL expected_output)
        message(FATAL_ERROR
            "program output did not match ${CLOTH_EXPECTED}\n"
            "actual output:\n${program_output}")
    endif()
    return()
endif()

if("${program_result}" STREQUAL "0")
    message(FATAL_ERROR
        "program unexpectedly succeeded; stdout: ${program_output}")
endif()
string(FIND "${program_error}" "${CLOTH_EXPECTED_ERROR}" error_position)
if(error_position EQUAL -1)
    message(FATAL_ERROR
        "program failed without expected error '${CLOTH_EXPECTED_ERROR}'; "
        "stderr: ${program_error}")
endif()
