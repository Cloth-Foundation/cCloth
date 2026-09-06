# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(required IN ITEMS CLOTH_COMPILER CLOTH_SOURCE_ROOT
                          CLOTH_WORK_DIRECTORY CLOTH_EXPECTED)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${CLOTH_WORK_DIRECTORY}")

function(compile_artifact output_digest target kind output)
    execute_process(
        COMMAND "${CLOTH_COMPILER}"
            --shuttle-protocol 2
            --operation compile
            --target "${target}"
            --artifact-kind "${kind}"
            --output "${output}"
            --package app 0.1.0 "${CLOTH_SOURCE_ROOT}"
            --entry Main.co
        RESULT_VARIABLE result
        OUTPUT_VARIABLE receipt
        ERROR_VARIABLE diagnostics)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${target} ${kind} compilation failed:\n${diagnostics}")
    endif()
    string(STRIP "${receipt}" receipt)
    string(JSON digest GET "${receipt}" artifact_id)
    string(LENGTH "${digest}" digest_length)
    if(NOT digest_length EQUAL 64 OR digest MATCHES "[^0-9a-f]")
        message(FATAL_ERROR "compiler returned an invalid artifact digest")
    endif()
    set(${output_digest} "${digest}" PARENT_SCOPE)
endfunction()

foreach(target IN ITEMS x86_64 wasm32)
    set(interface "${CLOTH_WORK_DIRECTORY}/app-${target}-interface.cpa")
    compile_artifact(interface_digest "${target}" interface "${interface}")
endforeach()

set(object "${CLOTH_WORK_DIRECTORY}/app-x86_64-object.cpa")
compile_artifact(object_digest x86_64 object "${object}")

set(executable
    "${CLOTH_WORK_DIRECTORY}/program-arguments${CLOTH_EXECUTABLE_SUFFIX}")
execute_process(
    COMMAND "${CLOTH_COMPILER}"
        --shuttle-protocol 2
        --operation link
        --target x86_64
        --output "${executable}"
        --root-package app
        --entry Main.co
        --artifact app 0.1.0 "${object_digest}" "${object}"
    RESULT_VARIABLE link_result
    OUTPUT_VARIABLE link_output
    ERROR_VARIABLE link_diagnostics)
if(NOT link_result EQUAL 0 OR NOT link_output STREQUAL "")
    message(FATAL_ERROR
        "program-argument link failed:\n${link_output}${link_diagnostics}")
endif()

execute_process(
    COMMAND "${executable}" "first" "two words" "" "--flag"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE run_diagnostics)
if(NOT run_result EQUAL 0 OR NOT run_diagnostics STREQUAL "")
    message(FATAL_ERROR
        "program-argument executable failed:\n${run_diagnostics}")
endif()
file(READ "${CLOTH_EXPECTED}" expected)
string(REPLACE "\r\n" "\n" actual "${actual}")
string(REPLACE "\r\n" "\n" expected "${expected}")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
        "source-free output mismatch\nexpected:\n${expected}\nactual:\n${actual}")
endif()
