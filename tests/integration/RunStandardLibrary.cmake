# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(required IN ITEMS CLOTH_COMPILER CLOTH_STANDARD_LIBRARY_ROOT
                          CLOTH_APPLICATION_ROOT CLOTH_WORK_DIRECTORY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${CLOTH_WORK_DIRECTORY}")

function(compile_package output_digest target kind output package source_root)
    set(arguments
        --shuttle-protocol 2
        --operation compile
        --target "${target}"
        --artifact-kind "${kind}"
        --output "${output}"
        --package "${package}" 0.1.0 "${source_root}")
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
        "${library_artifact}" cloth "${CLOTH_STANDARD_LIBRARY_ROOT}")

    set(application_artifact
        "${CLOTH_WORK_DIRECTORY}/app-${target}-interface.cpa")
    compile_package(application_digest "${target}" interface
        "${application_artifact}" app "${CLOTH_APPLICATION_ROOT}"
        --entry Main.co
        --dependency cloth cloth
        --artifact cloth 0.1.0 "${library_digest}" "${library_artifact}")
endforeach()

if(NOT CLOTH_TEST_NATIVE)
    return()
endif()

set(library_artifact "${CLOTH_WORK_DIRECTORY}/cloth-x86_64-object.cpa")
compile_package(library_digest x86_64 object "${library_artifact}" cloth
    "${CLOTH_STANDARD_LIBRARY_ROOT}")

set(application_artifact "${CLOTH_WORK_DIRECTORY}/app-x86_64-object.cpa")
compile_package(application_digest x86_64 object "${application_artifact}" app
    "${CLOTH_APPLICATION_ROOT}"
    --entry Main.co
    --dependency cloth cloth
    --artifact cloth 0.1.0 "${library_digest}" "${library_artifact}")

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
        --artifact cloth 0.1.0 "${library_digest}" "${library_artifact}"
    RESULT_VARIABLE link_result
    OUTPUT_VARIABLE link_output
    ERROR_VARIABLE link_diagnostics)
if(NOT link_result EQUAL 0 OR NOT link_output STREQUAL "")
    message(FATAL_ERROR
        "standard-library link failed:\n${link_output}${link_diagnostics}")
endif()

execute_process(
    COMMAND "${executable}"
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
