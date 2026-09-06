# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(required IN ITEMS CLOTH_COMPILER CLOTH_STANDARD_LIBRARY_ROOT
                          CLOTH_STANDARD_LIBRARY_VERSION
                          CLOTH_APPLICATION_ROOT CLOTH_CONSOLE_APPLICATION_ROOT
                          CLOTH_CONSOLE_INPUT CLOTH_CONSOLE_EXPECTED
                          CLOTH_WORK_DIRECTORY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${CLOTH_WORK_DIRECTORY}")

function(compile_package output_digest target kind output package version
                         source_root)
    set(arguments
        --shuttle-protocol 2
        --operation compile
        --target "${target}"
        --artifact-kind "${kind}"
        --output "${output}"
        --package "${package}" "${version}" "${source_root}")
    list(APPEND arguments ${ARGN})
    execute_process(
        COMMAND "${CLOTH_COMPILER}" ${arguments}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE receipt
        ERROR_VARIABLE diagnostics)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${package} ${kind} compilation failed for ${target}:\n${diagnostics}")
    endif()
    string(STRIP "${receipt}" receipt)
    string(JSON digest GET "${receipt}" artifact_id)
    string(LENGTH "${digest}" digest_length)
    if(NOT digest_length EQUAL 64 OR digest MATCHES "[^0-9a-f]")
        message(FATAL_ERROR "${package} returned an invalid artifact digest")
    endif()
    set(${output_digest} "${digest}" PARENT_SCOPE)
endfunction()

foreach(target IN ITEMS x86_64 wasm32)
    set(library_artifact
        "${CLOTH_WORK_DIRECTORY}/cloth-${target}-interface.cpa")
    compile_package(library_digest "${target}" interface
        "${library_artifact}" cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
        "${CLOTH_STANDARD_LIBRARY_ROOT}")

    set(application_artifact
        "${CLOTH_WORK_DIRECTORY}/app-${target}-interface.cpa")
    compile_package(application_digest "${target}" interface
        "${application_artifact}" app 0.1.0 "${CLOTH_APPLICATION_ROOT}"
        --entry Main.co
        --dependency cloth cloth
        --artifact cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
                   "${library_digest}" "${library_artifact}")
endforeach()

if(NOT CLOTH_TEST_NATIVE)
    return()
endif()

set(library_artifact "${CLOTH_WORK_DIRECTORY}/cloth-x86_64-object.cpa")
compile_package(library_digest x86_64 object "${library_artifact}" cloth
    "${CLOTH_STANDARD_LIBRARY_VERSION}" "${CLOTH_STANDARD_LIBRARY_ROOT}")

set(application_artifact "${CLOTH_WORK_DIRECTORY}/app-x86_64-object.cpa")
compile_package(application_digest x86_64 object "${application_artifact}" app
    0.1.0 "${CLOTH_APPLICATION_ROOT}"
    --entry Main.co
    --dependency cloth cloth
    --artifact cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
               "${library_digest}" "${library_artifact}")

set(executable "${CLOTH_WORK_DIRECTORY}/standard-library${CLOTH_EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${CLOTH_COMPILER}"
        --shuttle-protocol 2
        --operation link
        --target x86_64
        --output "${executable}"
        --root-package app
        --entry Main.co
        --artifact app 0.1.0 "${application_digest}" "${application_artifact}"
        --artifact cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
                   "${library_digest}" "${library_artifact}"
    RESULT_VARIABLE link_result
    OUTPUT_VARIABLE link_output
    ERROR_VARIABLE link_diagnostics)
if(NOT link_result EQUAL 0 OR NOT link_output STREQUAL "")
    message(FATAL_ERROR
        "standard-library link failed:\n${link_output}${link_diagnostics}")
endif()

execute_process(
    COMMAND "${executable}"
    ENCODING NONE
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE run_diagnostics)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
        "standard-library executable failed:\n${run_diagnostics}")
endif()
file(READ "${CLOTH_EXPECTED}" expected)
string(REPLACE "\r\n" "\n" actual "${actual}")
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
        "standard-library output mismatch\nexpected:\n${expected}\nactual:\n${actual}")
endif()

foreach(failure IN ITEMS invalid range)
    execute_process(
        COMMAND "${executable}" "${failure}"
        RESULT_VARIABLE failure_result
        OUTPUT_VARIABLE failure_output
        ERROR_VARIABLE failure_diagnostics)
    string(REPLACE "\r\n" "\n" failure_diagnostics
        "${failure_diagnostics}")
    if(failure STREQUAL "invalid")
        set(expected_failure
            "cloth error: cloth.lang.errors.ParseError: invalid int32 text\n")
    else()
        set(expected_failure
            "cloth error: cloth.lang.errors.ParseError: uint32 value is out of range\n")
    endif()
    if(failure_result EQUAL 0 OR NOT failure_output STREQUAL "" OR
       NOT failure_diagnostics STREQUAL expected_failure)
        message(FATAL_ERROR
            "primitive parse ${failure} failure was not mapped to ParseError\n"
            "status: ${failure_result}\nstdout: ${failure_output}\n"
            "stderr: ${failure_diagnostics}")
    endif()
endforeach()

set(console_artifact "${CLOTH_WORK_DIRECTORY}/console-x86_64-object.cpa")
compile_package(console_digest x86_64 object "${console_artifact}" console-app
    0.1.0 "${CLOTH_CONSOLE_APPLICATION_ROOT}"
    --entry Main.co
    --dependency cloth cloth
    --artifact cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
               "${library_digest}" "${library_artifact}")

set(console_executable
    "${CLOTH_WORK_DIRECTORY}/console${CLOTH_EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${CLOTH_COMPILER}"
        --shuttle-protocol 2
        --operation link
        --target x86_64
        --output "${console_executable}"
        --root-package console-app
        --entry Main.co
        --artifact console-app 0.1.0 "${console_digest}" "${console_artifact}"
        --artifact cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
                   "${library_digest}" "${library_artifact}"
    RESULT_VARIABLE console_link_result
    OUTPUT_VARIABLE console_link_output
    ERROR_VARIABLE console_link_diagnostics)
if(NOT console_link_result EQUAL 0 OR NOT console_link_output STREQUAL "")
    message(FATAL_ERROR
        "console link failed:\n${console_link_output}${console_link_diagnostics}")
endif()

execute_process(
    COMMAND "${console_executable}"
    INPUT_FILE "${CLOTH_CONSOLE_INPUT}"
    ENCODING NONE
    RESULT_VARIABLE console_run_result
    OUTPUT_VARIABLE console_actual
    ERROR_VARIABLE console_run_diagnostics)
if(NOT console_run_result EQUAL 0)
    message(FATAL_ERROR
        "console executable failed:\n${console_run_diagnostics}")
endif()
file(READ "${CLOTH_CONSOLE_EXPECTED}" console_expected)
string(REPLACE "\r\n" "\n" console_actual "${console_actual}")
string(REPLACE "\r\n" "\n" console_expected "${console_expected}")
if(NOT console_actual STREQUAL console_expected)
    message(FATAL_ERROR
        "console output mismatch\nexpected:\n${console_expected}\nactual:\n${console_actual}")
endif()

string(ASCII 192 175 invalid_utf8)
set(invalid_input "${CLOTH_WORK_DIRECTORY}/console-invalid.input")
file(WRITE "${invalid_input}" "${invalid_utf8}")
execute_process(
    COMMAND "${console_executable}"
    INPUT_FILE "${invalid_input}"
    ENCODING NONE
    RESULT_VARIABLE invalid_run_result
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_diagnostics)
string(REPLACE "\r\n" "\n" invalid_diagnostics "${invalid_diagnostics}")
set(expected_invalid_diagnostics
    "cloth error: cloth.lang.errors.IoError: standard input is not valid Unicode\n")
if(invalid_run_result EQUAL 0 OR NOT invalid_output STREQUAL "" OR
   NOT invalid_diagnostics STREQUAL expected_invalid_diagnostics)
    message(FATAL_ERROR
        "console encoding failure was not mapped to IoError\n"
        "status: ${invalid_run_result}\nstdout: ${invalid_output}\n"
        "stderr: ${invalid_diagnostics}")
endif()
