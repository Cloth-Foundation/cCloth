# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(required IN ITEMS CLOTH_BOOTSTRAP_SOURCE CLOTH_COMPILER CLOTH_ORACLE
                          CLOTH_SHUTTLE CLOTH_STANDARD_LIBRARY
                          CLOTH_STANDARD_LIBRARY_VERSION CLOTH_WORK_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT EXISTS "${CLOTH_BOOTSTRAP_SOURCE}/Shuttle.toml")
    message(FATAL_ERROR
        "bootstrap manifest not found: ${CLOTH_BOOTSTRAP_SOURCE}/Shuttle.toml")
endif()
if(NOT EXISTS "${CLOTH_STANDARD_LIBRARY}/io/File.co")
    message(FATAL_ERROR
        "standard-library source not found: ${CLOTH_STANDARD_LIBRARY}")
endif()
foreach(executable IN ITEMS CLOTH_COMPILER CLOTH_ORACLE CLOTH_SHUTTLE)
    if(NOT EXISTS "${${executable}}")
        message(FATAL_ERROR "executable not found: ${${executable}}")
    endif()
endforeach()

function(run_required description working_directory)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${working_directory}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        ENCODING UTF-8
    )
    set(output "${stdout}${stderr}")
    string(STRIP "${output}" output)
    if(NOT output STREQUAL "")
        message(STATUS "${description}\n${output}")
    else()
        message(STATUS "${description}")
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${description} failed with status ${result}")
    endif()
    set(CLOTH_LAST_PROCESS_OUTPUT "${stdout}${stderr}" PARENT_SCOPE)
endfunction()

function(require_same_file description left right)
    file(SHA256 "${left}" left_hash)
    file(SHA256 "${right}" right_hash)
    if(NOT left_hash STREQUAL right_hash)
        message(FATAL_ERROR
            "${description} differs:\n  ${left}: ${left_hash}\n"
            "  ${right}: ${right_hash}")
    endif()
endfunction()

function(first_record_mismatch input oracle_records bootstrap_records)
    string(REPLACE "\n" ";" oracle_lines "${oracle_records}")
    string(REPLACE "\n" ";" bootstrap_lines "${bootstrap_records}")
    list(LENGTH oracle_lines oracle_count)
    list(LENGTH bootstrap_lines bootstrap_count)
    if(oracle_count GREATER bootstrap_count)
        set(limit ${oracle_count})
    else()
        set(limit ${bootstrap_count})
    endif()

    set(index 0)
    while(index LESS limit)
        if(index LESS oracle_count)
            list(GET oracle_lines ${index} oracle_line)
        else()
            set(oracle_line "<missing>")
        endif()
        if(index LESS bootstrap_count)
            list(GET bootstrap_lines ${index} bootstrap_line)
        else()
            set(bootstrap_line "<missing>")
        endif()
        if(NOT oracle_line STREQUAL bootstrap_line)
            set(offset "unknown")
            if(NOT oracle_line STREQUAL "<missing>")
                string(REPLACE "|" ";" fields "${oracle_line}")
                list(LENGTH fields field_count)
                if(field_count GREATER 2)
                    list(GET fields 2 offset)
                endif()
            endif()
            message(FATAL_ERROR
                "lexer parity mismatch in ${input} at record ${index}, "
                "input offset ${offset}\n"
                "C++:   ${oracle_line}\nCloth: ${bootstrap_line}")
        endif()
        math(EXPR index "${index} + 1")
    endwhile()

    message(FATAL_ERROR "lexer parity mismatch in ${input}")
endfunction()

function(compare_lexers input bootstrap_executable)
    execute_process(
        COMMAND "${CLOTH_ORACLE}" record "${input}"
        RESULT_VARIABLE oracle_result
        OUTPUT_VARIABLE oracle_records
        ERROR_VARIABLE oracle_error
        ENCODING UTF-8
    )
    if(NOT oracle_result EQUAL 0)
        message(FATAL_ERROR
            "C++ lexer oracle failed for ${input}:\n${oracle_error}")
    endif()
    execute_process(
        COMMAND "${bootstrap_executable}" --lex-records "${input}"
        RESULT_VARIABLE bootstrap_result
        OUTPUT_VARIABLE bootstrap_records
        ERROR_VARIABLE bootstrap_error
        ENCODING UTF-8
    )
    if(NOT bootstrap_result EQUAL 0)
        message(FATAL_ERROR
            "Cloth lexer failed for ${input}:\n${bootstrap_error}")
    endif()
    string(REPLACE "\r\n" "\n" oracle_records "${oracle_records}")
    string(REPLACE "\r\n" "\n" bootstrap_records "${bootstrap_records}")
    if(NOT oracle_records STREQUAL bootstrap_records)
        first_record_mismatch(
            "${input}" "${oracle_records}" "${bootstrap_records}")
    endif()
endfunction()

file(REMOVE_RECURSE "${CLOTH_WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${CLOTH_WORK_DIRECTORY}")
set(serial_source "${CLOTH_WORK_DIRECTORY}/serial")
set(parallel_source "${CLOTH_WORK_DIRECTORY}/parallel")
file(MAKE_DIRECTORY "${serial_source}" "${parallel_source}")
file(COPY "${CLOTH_BOOTSTRAP_SOURCE}/" DESTINATION "${serial_source}"
    PATTERN ".git" EXCLUDE PATTERN "target" EXCLUDE)
file(COPY "${CLOTH_BOOTSTRAP_SOURCE}/" DESTINATION "${parallel_source}"
    PATTERN ".git" EXCLUDE PATTERN "target" EXCLUDE)

set(direct_executable
    "${CLOTH_WORK_DIRECTORY}/direct${CLOTH_EXECUTABLE_SUFFIX}")
set(direct_ir "${CLOTH_WORK_DIRECTORY}/direct-wasm32.ll")
set(direct_arguments
    --shuttle-protocol 1
    --root-package clothc
    --entry Main.co
    --package clothc 0.0.1 "${serial_source}/src"
    --package cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
        "${CLOTH_STANDARD_LIBRARY}"
    --dependency clothc cloth cloth)
run_required("direct x86-64 bootstrap build" "${serial_source}"
    "${CLOTH_COMPILER}" --target x86_64 --output-kind executable
    --output "${direct_executable}" ${direct_arguments})
file(SHA256 "${direct_executable}" direct_hash)
run_required("repeated direct x86-64 bootstrap build" "${serial_source}"
    "${CLOTH_COMPILER}" --target x86_64 --output-kind executable
    --output "${direct_executable}" ${direct_arguments})
file(SHA256 "${direct_executable}" repeated_direct_hash)
if(NOT direct_hash STREQUAL repeated_direct_hash)
    message(FATAL_ERROR "repeated direct bootstrap build is not deterministic")
endif()
run_required("direct wasm32 bootstrap build" "${serial_source}"
    "${CLOTH_COMPILER}" --target wasm32 --output-kind llvm-ir
    --output "${direct_ir}" ${direct_arguments})
file(SHA256 "${direct_ir}" direct_wasm_hash)
run_required("repeated direct wasm32 bootstrap build" "${serial_source}"
    "${CLOTH_COMPILER}" --target wasm32 --output-kind llvm-ir
    --output "${direct_ir}" ${direct_arguments})
file(SHA256 "${direct_ir}" repeated_direct_wasm_hash)
if(NOT direct_wasm_hash STREQUAL repeated_direct_wasm_hash)
    message(FATAL_ERROR "repeated direct wasm32 build is not deterministic")
endif()

run_required("serial Shuttle bootstrap build" "${serial_source}"
    "${CLOTH_SHUTTLE}" build --manifest-path
    "${serial_source}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target x86_64 --jobs 1)
set(serial_executable
    "${serial_source}/target/x86_64/cloth${CLOTH_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${serial_executable}")
    message(FATAL_ERROR "serial Shuttle build did not publish an executable")
endif()

run_required("parallel Shuttle bootstrap build" "${parallel_source}"
    "${CLOTH_SHUTTLE}" build --manifest-path
    "${parallel_source}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target x86_64 --jobs 4)
set(parallel_executable
    "${parallel_source}/target/x86_64/cloth${CLOTH_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${parallel_executable}")
    message(FATAL_ERROR "parallel Shuttle build did not publish an executable")
endif()
require_same_file("serial and parallel Shuttle executables"
    "${serial_executable}" "${parallel_executable}")
foreach(package IN ITEMS cloth clothc)
    require_same_file("serial and parallel ${package} artifacts"
        "${serial_source}/target/x86_64/packages/${package}.cpa"
        "${parallel_source}/target/x86_64/packages/${package}.cpa")
endforeach()

file(SHA256 "${serial_executable}" cold_shuttle_hash)
file(SHA256 "${serial_source}/target/x86_64/packages/cloth.cpa"
    cold_library_hash)
file(SHA256 "${serial_source}/target/x86_64/packages/clothc.cpa"
    cold_bootstrap_hash)
run_required("warm Shuttle bootstrap build" "${serial_source}"
    "${CLOTH_SHUTTLE}" build --manifest-path
    "${serial_source}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target x86_64 --jobs 4)
if(NOT CLOTH_LAST_PROCESS_OUTPUT MATCHES "reusing cloth v0\\.4\\.0" OR
   NOT CLOTH_LAST_PROCESS_OUTPUT MATCHES "reusing clothc v0\\.0\\.1")
    message(FATAL_ERROR
        "warm Shuttle build did not reuse both cloth and clothc exactly")
endif()
file(SHA256 "${serial_executable}" warm_shuttle_hash)
if(NOT cold_shuttle_hash STREQUAL warm_shuttle_hash)
    message(FATAL_ERROR "warm Shuttle build changed the executable")
endif()
file(SHA256 "${serial_source}/target/x86_64/packages/cloth.cpa"
    warm_library_hash)
file(SHA256 "${serial_source}/target/x86_64/packages/clothc.cpa"
    warm_bootstrap_hash)
if(NOT cold_library_hash STREQUAL warm_library_hash OR
   NOT cold_bootstrap_hash STREQUAL warm_bootstrap_hash)
    message(FATAL_ERROR "warm Shuttle build changed a reused package artifact")
endif()

run_required("Shuttle wasm32 bootstrap check" "${serial_source}"
    "${CLOTH_SHUTTLE}" check --manifest-path
    "${serial_source}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target wasm32 --jobs 4)

execute_process(
    COMMAND "${serial_executable}"
    WORKING_DIRECTORY "${serial_source}"
    RESULT_VARIABLE native_result
    OUTPUT_VARIABLE native_output
    ERROR_VARIABLE native_error
    ENCODING UTF-8
)
string(REPLACE "\r\n" "\n" native_output "${native_output}")
if(NOT native_result EQUAL 0 OR
   NOT native_output STREQUAL "lexer literals ok\n" OR
   NOT native_error STREQUAL "")
    message(FATAL_ERROR
        "native bootstrap self-check failed with status ${native_result}\n"
        "stdout: ${native_output}\nstderr: ${native_error}")
endif()

set(generated_directory "${CLOTH_WORK_DIRECTORY}/generated")
run_required("generate bounded lexer corpus" "${CLOTH_WORK_DIRECTORY}"
    "${CLOTH_ORACLE}" generate "${generated_directory}")
file(GLOB_RECURSE bootstrap_inputs LIST_DIRECTORIES FALSE
    "${CLOTH_BOOTSTRAP_SOURCE}/*.co")
list(FILTER bootstrap_inputs EXCLUDE REGEX "[/\\\\]target[/\\\\]")
file(GLOB generated_inputs LIST_DIRECTORIES FALSE
    "${generated_directory}/*")
list(APPEND bootstrap_inputs ${generated_inputs})
list(SORT bootstrap_inputs)
list(LENGTH bootstrap_inputs input_count)
set(input_index 0)
foreach(input IN LISTS bootstrap_inputs)
    math(EXPR input_index "${input_index} + 1")
    math(EXPR progress_remainder "${input_index} % 50")
    if(input_index EQUAL 1 OR progress_remainder EQUAL 0 OR
       input_index EQUAL input_count)
        message(STATUS
            "lexer parity ${input_index}/${input_count}: ${input}")
    endif()
    compare_lexers("${input}" "${serial_executable}")
endforeach()

file(SHA256 "${serial_executable}" preserved_hash)
file(SHA256 "${serial_source}/target/x86_64/packages/cloth.cpa"
    preserved_library_hash)
file(SHA256 "${serial_source}/target/x86_64/packages/clothc.cpa"
    preserved_bootstrap_hash)
file(APPEND "${serial_source}/src/Main.co" "\n@\n")
execute_process(
    COMMAND "${CLOTH_SHUTTLE}" build --manifest-path
        "${serial_source}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
        --target x86_64 --jobs 4
    WORKING_DIRECTORY "${serial_source}"
    RESULT_VARIABLE failed_result
    OUTPUT_VARIABLE failed_stdout
    ERROR_VARIABLE failed_stderr
    ENCODING UTF-8
)
if(failed_result EQUAL 0)
    message(FATAL_ERROR "invalid bootstrap rebuild unexpectedly succeeded")
endif()
set(failed_output "${failed_stdout}${failed_stderr}")
if(NOT failed_output MATCHES "unexpected character '@'")
    message(FATAL_ERROR
        "invalid bootstrap rebuild failed for the wrong reason:\n"
        "${failed_output}")
endif()
if(NOT EXISTS "${serial_executable}")
    message(FATAL_ERROR "failed rebuild removed the completed executable")
endif()
file(SHA256 "${serial_executable}" failed_hash)
if(NOT preserved_hash STREQUAL failed_hash)
    message(FATAL_ERROR "failed rebuild replaced the completed executable")
endif()
file(SHA256 "${serial_source}/target/x86_64/packages/cloth.cpa"
    failed_library_hash)
file(SHA256 "${serial_source}/target/x86_64/packages/clothc.cpa"
    failed_bootstrap_hash)
if(NOT preserved_library_hash STREQUAL failed_library_hash OR
   NOT preserved_bootstrap_hash STREQUAL failed_bootstrap_hash)
    message(FATAL_ERROR "failed rebuild replaced a completed package artifact")
endif()

message(STATUS
    "Stage 44 lexer parity passed for ${input_count} inputs; direct and "
    "Shuttle builds are deterministic, both targets pass, warm reuse is "
    "exact, and failed output is preserved")
