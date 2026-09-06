# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

if(DEFINED CLOTH_INPUT_MODE)
    if(NOT DEFINED CLOTH_INPUT_WORK_FILE)
        message(FATAL_ERROR "CLOTH_INPUT_WORK_FILE is required with CLOTH_INPUT_MODE")
    endif()
    if(CLOTH_INPUT_MODE STREQUAL "edges")
        execute_process(
            COMMAND "${CLOTH_PROGRAM}" emit_console_edges
            OUTPUT_FILE "${CLOTH_INPUT_WORK_FILE}"
            RESULT_VARIABLE input_result
            ERROR_VARIABLE input_error
        )
        if(NOT input_result EQUAL 0)
            message(FATAL_ERROR
                "could not generate console edge input: ${input_error}")
        endif()
    elseif(CLOTH_INPUT_MODE STREQUAL "invalid")
        string(ASCII 192 175 invalid_utf8)
        file(WRITE "${CLOTH_INPUT_WORK_FILE}" "${invalid_utf8}")
    else()
        message(FATAL_ERROR "unknown input mode '${CLOTH_INPUT_MODE}'")
    endif()
    set(CLOTH_INPUT "${CLOTH_INPUT_WORK_FILE}")
endif()

if(DEFINED CLOTH_INPUT)
    execute_process(
        COMMAND "${CLOTH_PROGRAM}" ${CLOTH_PROGRAM_ARGUMENTS}
        INPUT_FILE "${CLOTH_INPUT}"
        TIMEOUT "${CLOTH_PROGRAM_TIMEOUT}"
        RESULT_VARIABLE program_result
        OUTPUT_VARIABLE program_output
        ERROR_VARIABLE program_error
    )
else()
    execute_process(
        COMMAND "${CLOTH_PROGRAM}" ${CLOTH_PROGRAM_ARGUMENTS}
        TIMEOUT "${CLOTH_PROGRAM_TIMEOUT}"
        RESULT_VARIABLE program_result
        OUTPUT_VARIABLE program_output
        ERROR_VARIABLE program_error
    )
endif()

if(DEFINED CLOTH_COMPARE_PROGRAM)
    execute_process(
        COMMAND "${CLOTH_COMPARE_PROGRAM}" ${CLOTH_PROGRAM_ARGUMENTS}
        TIMEOUT "${CLOTH_PROGRAM_TIMEOUT}"
        RESULT_VARIABLE comparison_result
        OUTPUT_VARIABLE comparison_output
        ERROR_VARIABLE comparison_error
    )
    if(NOT "${program_result}" STREQUAL "${comparison_result}" OR
       NOT program_output STREQUAL comparison_output OR
       NOT program_error STREQUAL comparison_error)
        message(FATAL_ERROR
            "programs produced different status or streams\n"
            "first: ${program_result}\n${program_output}${program_error}\n"
            "second: ${comparison_result}\n"
            "${comparison_output}${comparison_error}")
    endif()
endif()

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

if(CLOTH_EXPECT_TRAP)
    # llvm.trap lowers to an illegal instruction on the native x86_64 target.
    # Do not count a timeout, ordinary error exit, or missing executable as a trap.
    if(NOT "${program_result}" MATCHES "[Ii]llegal instruction|0xc000001d|^-1073741795$|^3221225501$")
        message(FATAL_ERROR "expected an illegal-instruction trap, got ${program_result}: ${program_error}")
    endif()
    if(NOT "${program_output}" STREQUAL "")
        message(FATAL_ERROR "invalid enum dispatch executed observable arm code: ${program_output}")
    endif()
endif()

if("${program_result}" STREQUAL "0")
    message(FATAL_ERROR
        "program unexpectedly succeeded; stdout: ${program_output}")
endif()
if(NOT "${program_output}" STREQUAL "")
    message(FATAL_ERROR
        "failing program produced unexpected stdout: ${program_output}")
endif()
if(CLOTH_EXACT_ERROR)
    string(REPLACE "\r\n" "\n" program_error "${program_error}")
    if(NOT program_error STREQUAL "${CLOTH_EXPECTED_ERROR}\n")
        message(FATAL_ERROR
            "program error did not exactly match '${CLOTH_EXPECTED_ERROR}'; "
            "stderr: ${program_error}")
    endif()
    return()
endif()
string(FIND "${program_error}" "${CLOTH_EXPECTED_ERROR}" error_position)
if(error_position EQUAL -1)
    message(FATAL_ERROR
        "program failed without expected error '${CLOTH_EXPECTED_ERROR}'; "
        "stderr: ${program_error}")
endif()
