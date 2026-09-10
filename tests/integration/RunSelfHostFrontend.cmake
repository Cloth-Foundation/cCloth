# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

foreach(required IN ITEMS CLOTH_BOOTSTRAP_SOURCE CLOTH_COMPILER CLOTH_ORACLE
                          CLOTH_DECLARATION_ORACLE CLOTH_DEFINITION_ORACLE
                          CLOTH_SHUTTLE
                          CLOTH_STANDARD_LIBRARY
                          CLOTH_STANDARD_LIBRARY_VERSION CLOTH_WORK_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT EXISTS "${CLOTH_BOOTSTRAP_SOURCE}/Shuttle.toml")
    message(FATAL_ERROR
        "bootstrap manifest not found: ${CLOTH_BOOTSTRAP_SOURCE}/Shuttle.toml")
endif()
if(NOT EXISTS "${CLOTH_BOOTSTRAP_SOURCE}/tests/self_host/Shuttle.toml")
    message(FATAL_ERROR
        "self-host test manifest not found: "
        "${CLOTH_BOOTSTRAP_SOURCE}/tests/self_host/Shuttle.toml")
endif()
if(NOT EXISTS "${CLOTH_STANDARD_LIBRARY}/io/File.co")
    message(FATAL_ERROR
        "standard-library source not found: ${CLOTH_STANDARD_LIBRARY}")
endif()
foreach(executable IN ITEMS CLOTH_COMPILER CLOTH_ORACLE
                            CLOTH_DECLARATION_ORACLE CLOTH_DEFINITION_ORACLE
                            CLOTH_SHUTTLE)
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

function(read_definition_records output_name input file_name executable)
    execute_process(
        COMMAND "${executable}" --definition-records "${input}" "${file_name}"
        RESULT_VARIABLE record_result
        OUTPUT_VARIABLE records
        ERROR_VARIABLE record_error
        ENCODING UTF-8
    )
    string(REPLACE "\r\n" "\n" records "${records}")
    if(NOT record_result EQUAL 0 OR NOT record_error STREQUAL "" OR
       records STREQUAL "")
        message(FATAL_ERROR
            "definition record adapter failed for ${input} with status "
            "${record_result}\nstdout: ${records}\nstderr: ${record_error}")
    endif()
    set(${output_name} "${records}" PARENT_SCOPE)
endfunction()

function(compare_definitions input file_name direct_executable
                             serial_executable parallel_executable)
    execute_process(
        COMMAND "${CLOTH_DEFINITION_ORACLE}" "${input}"
        RESULT_VARIABLE oracle_result
        OUTPUT_VARIABLE oracle_records
        ERROR_VARIABLE oracle_error
        ENCODING UTF-8
    )
    if(NOT oracle_result EQUAL 0)
        message(FATAL_ERROR
            "C++ definition oracle failed for ${input}:\n${oracle_error}")
    endif()
    string(REPLACE "\r\n" "\n" oracle_records "${oracle_records}")
    read_definition_records(direct_records "${input}" "${file_name}"
        "${direct_executable}")
    read_definition_records(first_serial_records "${input}" "${file_name}"
        "${serial_executable}")
    read_definition_records(second_serial_records "${input}" "${file_name}"
        "${serial_executable}")
    read_definition_records(parallel_records "${input}" "${file_name}"
        "${parallel_executable}")
    if(NOT oracle_records STREQUAL direct_records)
        first_record_mismatch("definition parity" "${input}" "C++"
            "${oracle_records}" "direct Cloth" "${direct_records}")
    endif()
    if(NOT direct_records STREQUAL first_serial_records)
        first_record_mismatch("definition determinism" "${input}"
            "direct Cloth" "${direct_records}" "serial Shuttle"
            "${first_serial_records}")
    endif()
    if(NOT first_serial_records STREQUAL second_serial_records)
        first_record_mismatch("definition repeatability" "${input}"
            "first serial run" "${first_serial_records}" "second serial run"
            "${second_serial_records}")
    endif()
    if(NOT first_serial_records STREQUAL parallel_records)
        first_record_mismatch("definition determinism" "${input}"
            "serial Shuttle" "${first_serial_records}" "parallel Shuttle"
            "${parallel_records}")
    endif()
endfunction()

function(compare_declarations input bootstrap_executable)
    get_filename_component(file_name "${input}" NAME_WE)
    execute_process(
        COMMAND "${CLOTH_DECLARATION_ORACLE}" "${input}"
        RESULT_VARIABLE oracle_result
        OUTPUT_VARIABLE oracle_records
        ERROR_VARIABLE oracle_error
        ENCODING UTF-8
    )
    if(NOT oracle_result EQUAL 0)
        message(FATAL_ERROR
            "C++ declaration oracle failed for ${input}:\n${oracle_error}")
    endif()
    execute_process(
        COMMAND "${bootstrap_executable}" --declaration-records
            "${input}" "${file_name}"
        RESULT_VARIABLE bootstrap_result
        OUTPUT_VARIABLE bootstrap_records
        ERROR_VARIABLE bootstrap_error
        ENCODING UTF-8
    )
    if(NOT bootstrap_result EQUAL 0)
        message(FATAL_ERROR
            "Cloth declaration parser failed for ${input}:\n"
            "${bootstrap_error}")
    endif()
    string(REPLACE "\r\n" "\n" oracle_records "${oracle_records}")
    string(REPLACE "\r\n" "\n" bootstrap_records "${bootstrap_records}")
    if(NOT oracle_records STREQUAL bootstrap_records)
        message(FATAL_ERROR
            "declaration parity mismatch in ${input}\n"
            "C++ records:\n${oracle_records}\n"
            "Cloth records:\n${bootstrap_records}")
    endif()
endfunction()

function(read_declaration_records output_name input bootstrap_executable)
    get_filename_component(file_name "${input}" NAME_WE)
    execute_process(
        COMMAND "${bootstrap_executable}" --declaration-records
            "${input}" "${file_name}"
        RESULT_VARIABLE record_result
        OUTPUT_VARIABLE records
        ERROR_VARIABLE record_error
        ENCODING UTF-8
    )
    string(REPLACE "\r\n" "\n" records "${records}")
    if(NOT record_result EQUAL 0 OR NOT record_error STREQUAL "" OR
       records STREQUAL "")
        message(FATAL_ERROR
            "declaration record adapter failed for ${input} with status "
            "${record_result}\nstdout: ${records}\nstderr: ${record_error}")
    endif()
    set(${output_name} "${records}" PARENT_SCOPE)
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

function(first_record_mismatch description input left_name left_records
                               right_name right_records)
    string(REPLACE "\n" ";" left_lines "${left_records}")
    string(REPLACE "\n" ";" right_lines "${right_records}")
    list(LENGTH left_lines left_count)
    list(LENGTH right_lines right_count)
    if(left_count GREATER right_count)
        set(limit ${left_count})
    else()
        set(limit ${right_count})
    endif()

    set(index 0)
    while(index LESS limit)
        if(index LESS left_count)
            list(GET left_lines ${index} left_line)
        else()
            set(left_line "<missing>")
        endif()
        if(index LESS right_count)
            list(GET right_lines ${index} right_line)
        else()
            set(right_line "<missing>")
        endif()
        if(NOT left_line STREQUAL right_line)
            math(EXPR line "${index} + 1")
            message(FATAL_ERROR
                "${description} mismatch in ${input} at record ${index}, "
                "line ${line}\n${left_name}: ${left_line}\n"
                "${right_name}: ${right_line}")
        endif()
        math(EXPR index "${index} + 1")
    endwhile()

    message(FATAL_ERROR "${description} mismatch in ${input}")
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
        first_record_mismatch("lexer parity" "${input}" "C++"
            "${oracle_records}" "Cloth" "${bootstrap_records}")
    endif()
endfunction()

file(REMOVE_RECURSE "${CLOTH_WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${CLOTH_WORK_DIRECTORY}")
set(serial_source "${CLOTH_WORK_DIRECTORY}/serial")
set(parallel_source "${CLOTH_WORK_DIRECTORY}/parallel")
set(serial_test_root "${serial_source}/tests/self_host")
set(parallel_test_root "${parallel_source}/tests/self_host")
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
    --root-package clothc-tests
    --entry BootstrapMain.co
    --package clothc-tests 0.0.1 "${serial_test_root}/src"
    --package clothc 0.0.1 "${serial_source}/src"
    --package cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
        "${CLOTH_STANDARD_LIBRARY}"
    --dependency clothc-tests clothc clothc
    --dependency clothc-tests cloth cloth
    --dependency clothc cloth cloth)
run_required("direct x86-64 self-host test build" "${serial_test_root}"
    "${CLOTH_COMPILER}" --target x86_64 --output-kind executable
    --output "${direct_executable}" ${direct_arguments})
file(SHA256 "${direct_executable}" direct_hash)
run_required("repeated direct x86-64 self-host test build"
    "${serial_test_root}"
    "${CLOTH_COMPILER}" --target x86_64 --output-kind executable
    --output "${direct_executable}" ${direct_arguments})
file(SHA256 "${direct_executable}" repeated_direct_hash)
if(NOT direct_hash STREQUAL repeated_direct_hash)
    message(FATAL_ERROR "repeated direct bootstrap build is not deterministic")
endif()
run_required("direct wasm32 self-host test build" "${serial_test_root}"
    "${CLOTH_COMPILER}" --target wasm32 --output-kind llvm-ir
    --output "${direct_ir}" ${direct_arguments})
file(SHA256 "${direct_ir}" direct_wasm_hash)
run_required("repeated direct wasm32 self-host test build"
    "${serial_test_root}"
    "${CLOTH_COMPILER}" --target wasm32 --output-kind llvm-ir
    --output "${direct_ir}" ${direct_arguments})
file(SHA256 "${direct_ir}" repeated_direct_wasm_hash)
if(NOT direct_wasm_hash STREQUAL repeated_direct_wasm_hash)
    message(FATAL_ERROR "repeated direct wasm32 build is not deterministic")
endif()

set(direct_compiler_executable
    "${CLOTH_WORK_DIRECTORY}/clothc${CLOTH_EXECUTABLE_SUFFIX}")
set(direct_compiler_arguments
    --shuttle-protocol 1
    --root-package clothc
    --entry Main.co
    --package clothc 0.0.1 "${serial_source}/src"
    --package cloth "${CLOTH_STANDARD_LIBRARY_VERSION}"
        "${CLOTH_STANDARD_LIBRARY}"
    --dependency clothc cloth cloth)
run_required("direct production clothc build" "${serial_source}"
    "${CLOTH_COMPILER}" --target x86_64 --output-kind executable
    --output "${direct_compiler_executable}" ${direct_compiler_arguments})
run_required("production clothc help" "${serial_source}"
    "${direct_compiler_executable}" --help)
run_required("production clothc frontend check" "${serial_source}"
    "${direct_compiler_executable}" check
    "${serial_test_root}/testdata/parser/DefinitionValid.co")
execute_process(
    COMMAND "${direct_compiler_executable}" check
        "${serial_test_root}/testdata/parser/definition_recovery.co"
    WORKING_DIRECTORY "${serial_source}"
    RESULT_VARIABLE compiler_failure_result
    OUTPUT_VARIABLE compiler_failure_output
    ERROR_VARIABLE compiler_failure_error
    ENCODING UTF-8
)
if(NOT compiler_failure_result EQUAL 1 OR
   NOT compiler_failure_output MATCHES "syntax analysis failed" OR
   NOT compiler_failure_error STREQUAL "")
    message(FATAL_ERROR
        "production clothc did not reject malformed syntax predictably\n"
        "status: ${compiler_failure_result}\n"
        "stdout: ${compiler_failure_output}\n"
        "stderr: ${compiler_failure_error}")
endif()

run_required("serial Shuttle self-host test build" "${serial_test_root}"
    "${CLOTH_SHUTTLE}" build --manifest-path
    "${serial_test_root}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target x86_64 --jobs 1)
set(serial_executable
    "${serial_test_root}/target/x86_64/clothc-tests${CLOTH_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${serial_executable}")
    message(FATAL_ERROR "serial Shuttle build did not publish an executable")
endif()

run_required("parallel Shuttle self-host test build" "${parallel_test_root}"
    "${CLOTH_SHUTTLE}" build --manifest-path
    "${parallel_test_root}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target x86_64 --jobs 4)
set(parallel_executable
    "${parallel_test_root}/target/x86_64/clothc-tests${CLOTH_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${parallel_executable}")
    message(FATAL_ERROR "parallel Shuttle build did not publish an executable")
endif()
require_same_file("serial and parallel Shuttle executables"
    "${serial_executable}" "${parallel_executable}")
foreach(package IN ITEMS cloth clothc clothc-tests)
    require_same_file("serial and parallel ${package} artifacts"
        "${serial_test_root}/target/x86_64/packages/${package}.cpa"
        "${parallel_test_root}/target/x86_64/packages/${package}.cpa")
endforeach()

file(SHA256 "${serial_executable}" cold_shuttle_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/cloth.cpa"
    cold_library_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc.cpa"
    cold_bootstrap_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc-tests.cpa"
    cold_test_hash)
run_required("warm Shuttle self-host test build" "${serial_test_root}"
    "${CLOTH_SHUTTLE}" build --manifest-path
    "${serial_test_root}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target x86_64 --jobs 4)
string(REPLACE "." "\\." standard_library_version_pattern
    "${CLOTH_STANDARD_LIBRARY_VERSION}")
if(NOT CLOTH_LAST_PROCESS_OUTPUT MATCHES
       "reusing cloth v${standard_library_version_pattern}" OR
   NOT CLOTH_LAST_PROCESS_OUTPUT MATCHES "reusing clothc v0\\.0\\.1")
    message(FATAL_ERROR
        "warm Shuttle build did not reuse both cloth and clothc exactly")
endif()
file(SHA256 "${serial_executable}" warm_shuttle_hash)
if(NOT cold_shuttle_hash STREQUAL warm_shuttle_hash)
    message(FATAL_ERROR "warm Shuttle build changed the executable")
endif()
file(SHA256 "${serial_test_root}/target/x86_64/packages/cloth.cpa"
    warm_library_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc.cpa"
    warm_bootstrap_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc-tests.cpa"
    warm_test_hash)
if(NOT cold_library_hash STREQUAL warm_library_hash OR
   NOT cold_bootstrap_hash STREQUAL warm_bootstrap_hash OR
   NOT cold_test_hash STREQUAL warm_test_hash)
    message(FATAL_ERROR "warm Shuttle build changed a reused package artifact")
endif()

run_required("Shuttle wasm32 self-host test check" "${serial_test_root}"
    "${CLOTH_SHUTTLE}" check --manifest-path
    "${serial_test_root}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
    --target wasm32 --jobs 4)

execute_process(
    COMMAND "${serial_executable}"
    WORKING_DIRECTORY "${serial_test_root}"
    RESULT_VARIABLE native_result
    OUTPUT_VARIABLE native_output
    ERROR_VARIABLE native_error
    ENCODING UTF-8
)
string(REPLACE "\r\n" "\n" native_output "${native_output}")
if(NOT native_result EQUAL 0 OR
   NOT native_output STREQUAL "self-host checks ok\n" OR
   NOT native_error STREQUAL "")
    message(FATAL_ERROR
        "native bootstrap self-check failed with status ${native_result}\n"
        "stdout: ${native_output}\nstderr: ${native_error}")
endif()

function(require_declaration_substrate_failure mode expected)
    execute_process(
        COMMAND "${serial_executable}" --declaration-substrate-failure
            "${mode}"
        WORKING_DIRECTORY "${serial_test_root}"
        RESULT_VARIABLE failure_result
        OUTPUT_VARIABLE failure_output
        ERROR_VARIABLE failure_error
        ENCODING UTF-8
    )
    if(failure_result EQUAL 0 OR NOT failure_output STREQUAL "" OR
       NOT failure_error MATCHES "${expected}")
        message(FATAL_ERROR
            "declaration substrate failure '${mode}' was not preserved\n"
            "status: ${failure_result}\nstdout: ${failure_output}\n"
            "stderr: ${failure_error}")
    endif()
endfunction()

require_declaration_substrate_failure("range" "invalid token index range")
require_declaration_substrate_failure(
    "token-buffer" "parser token buffer is not immutable and complete")
require_declaration_substrate_failure(
    "foreign-source" "parser token does not belong to its source")
require_declaration_substrate_failure(
    "location" "parser token does not belong to its source")
require_declaration_substrate_failure(
    "slice" "token cursor slice is outside its parent")
require_declaration_substrate_failure(
    "slice-parent" "token cursor slice is outside its parent")
require_declaration_substrate_failure(
    "lookahead" "token cursor lookahead is negative")
require_declaration_substrate_failure(
    "progress" "token cursor did not make progress")
require_declaration_substrate_failure(
    "progress-checkpoint" "token cursor progress checkpoint is invalid")
require_declaration_substrate_failure(
    "diagnostic-order" "parse diagnostics must be added in source order")
require_declaration_substrate_failure(
    "diagnostic-finished" "parse diagnostic builder is already finished")
require_declaration_substrate_failure(
    "member-finished" "member outline builder is already finished")
require_declaration_substrate_failure(
    "import-finished" "import syntax builder is already finished")
require_declaration_substrate_failure(
    "type-finished" "type syntax builder is already finished")
require_declaration_substrate_failure(
    "parameter-finished" "parameter syntax builder is already finished")
require_declaration_substrate_failure(
    "enum-finished" "enum case syntax builder is already finished")
require_declaration_substrate_failure(
    "diagnostic-bounds" "parse diagnostic sequence index is out of bounds")
require_declaration_substrate_failure(
    "member-bounds" "member outline sequence index is out of bounds")
require_declaration_substrate_failure(
    "result-validity" "declaration result validity is inconsistent")
require_declaration_substrate_failure(
    "body-delimiter" "valid body token range is not brace delimited")
require_declaration_substrate_failure(
    "body-bounds" "deferred token range is outside its declaration")
require_declaration_substrate_failure(
    "visibility" "member outline token identity is inconsistent")
require_declaration_substrate_failure(
    "file-range" "declaration file range does not cover its source")

execute_process(
    COMMAND "${serial_executable}" --declaration-substrate-gc-check
    WORKING_DIRECTORY "${serial_test_root}"
    RESULT_VARIABLE declaration_gc_result
    OUTPUT_VARIABLE declaration_gc_output
    ERROR_VARIABLE declaration_gc_error
    ENCODING UTF-8
)
if(NOT declaration_gc_result EQUAL 0 OR
   NOT declaration_gc_output STREQUAL "" OR
   NOT declaration_gc_error STREQUAL "")
    message(FATAL_ERROR
        "declaration substrate GC check failed with status "
        "${declaration_gc_result}\nstdout: ${declaration_gc_output}\n"
        "stderr: ${declaration_gc_error}")
endif()

execute_process(
    COMMAND "${serial_executable}" --declaration-grammar-gc-check
    WORKING_DIRECTORY "${serial_test_root}"
    RESULT_VARIABLE declaration_grammar_gc_result
    OUTPUT_VARIABLE declaration_grammar_gc_output
    ERROR_VARIABLE declaration_grammar_gc_error
    ENCODING UTF-8
)
if(NOT declaration_grammar_gc_result EQUAL 0 OR
   NOT declaration_grammar_gc_output STREQUAL "" OR
   NOT declaration_grammar_gc_error STREQUAL "")
    message(FATAL_ERROR
        "declaration grammar GC check failed with status "
        "${declaration_grammar_gc_result}\n"
        "stdout: ${declaration_grammar_gc_output}\n"
        "stderr: ${declaration_grammar_gc_error}")
endif()

foreach(expression_check IN ITEMS depth gc)
    execute_process(
        COMMAND "${serial_executable}" --expression-${expression_check}-check
        WORKING_DIRECTORY "${serial_test_root}"
        RESULT_VARIABLE expression_result
        OUTPUT_VARIABLE expression_output
        ERROR_VARIABLE expression_error
        ENCODING UTF-8
    )
    if(NOT expression_result EQUAL 0 OR
       NOT expression_output STREQUAL "" OR
       NOT expression_error STREQUAL "")
        message(FATAL_ERROR
            "expression ${expression_check} check failed with status "
            "${expression_result}\nstdout: ${expression_output}\n"
            "stderr: ${expression_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${serial_executable}" --definition-depth-check
    WORKING_DIRECTORY "${serial_test_root}"
    RESULT_VARIABLE definition_depth_result
    OUTPUT_VARIABLE definition_depth_output
    ERROR_VARIABLE definition_depth_error
    ENCODING UTF-8
)
if(NOT definition_depth_result EQUAL 0 OR
   NOT definition_depth_output STREQUAL "" OR
   NOT definition_depth_error STREQUAL "")
    message(FATAL_ERROR
        "definition depth check failed with status ${definition_depth_result}\n"
        "stdout: ${definition_depth_output}\n"
        "stderr: ${definition_depth_error}")
endif()

execute_process(
    COMMAND "${serial_executable}" --definition-gc-check
    WORKING_DIRECTORY "${serial_test_root}"
    RESULT_VARIABLE definition_gc_result
    OUTPUT_VARIABLE definition_gc_output
    ERROR_VARIABLE definition_gc_error
    ENCODING UTF-8
)
if(NOT definition_gc_result EQUAL 0 OR
   NOT definition_gc_output STREQUAL "" OR
   NOT definition_gc_error STREQUAL "")
    message(FATAL_ERROR
        "definition GC check failed with status ${definition_gc_result}\n"
        "stdout: ${definition_gc_output}\n"
        "stderr: ${definition_gc_error}")
endif()

function(require_storage_failure mode expected)
    execute_process(
        COMMAND "${serial_executable}" --syntax-storage-failure "${mode}"
        WORKING_DIRECTORY "${serial_test_root}"
        RESULT_VARIABLE failure_result
        OUTPUT_VARIABLE failure_output
        ERROR_VARIABLE failure_error
        ENCODING UTF-8
    )
    if(failure_result EQUAL 0 OR NOT failure_output STREQUAL "" OR
       NOT failure_error MATCHES "${expected}")
        message(FATAL_ERROR
            "syntax storage failure '${mode}' was not preserved\n"
            "status: ${failure_result}\nstdout: ${failure_output}\n"
            "stderr: ${failure_error}")
    endif()
endfunction()

require_storage_failure("sealed-append" "syntax storage is sealed")
require_storage_failure(
    "foreign-read" "expression handle does not belong to syntax storage")
require_storage_failure(
    "mixed-children" "syntax child builder rejects this handle")
require_storage_failure(
    "child-bounds" "syntax child sequence index is out of bounds")

function(require_tree_failure mode expected)
    execute_process(
        COMMAND "${serial_executable}" --syntax-tree-failure "${mode}"
        WORKING_DIRECTORY "${serial_test_root}"
        RESULT_VARIABLE failure_result
        OUTPUT_VARIABLE failure_output
        ERROR_VARIABLE failure_error
        ENCODING UTF-8
    )
    if(failure_result EQUAL 0 OR NOT failure_output STREQUAL "" OR
       NOT failure_error MATCHES "${expected}")
        message(FATAL_ERROR
            "syntax tree failure '${mode}' was not preserved\n"
            "status: ${failure_result}\nstdout: ${failure_output}\n"
            "stderr: ${failure_error}")
    endif()
endfunction()

require_tree_failure("range" "syntax range is outside its parent")
require_tree_failure(
    "foreign-child" "syntax tree contains a foreign expression handle")
require_tree_failure(
    "foreign-statement" "syntax tree contains a foreign statement handle")
require_tree_failure(
    "foreign-block" "syntax tree contains a foreign block handle")
require_tree_failure(
    "kind-mismatch" "expression kind does not match its node type")
require_tree_failure(
    "statement-kind" "statement kind does not match its node type")
require_tree_failure(
    "block-kind" "block handle does not contain a block node")
require_tree_failure(
    "validity" "valid syntax node contains an invalid child")
require_tree_failure(
    "span-source" "syntax span belongs to another source")
require_tree_failure(
    "type-flags" "only array syntax may have nullable elements")

execute_process(
    COMMAND "${serial_executable}" --syntax-tree-gc-check
    WORKING_DIRECTORY "${serial_test_root}"
    RESULT_VARIABLE gc_result
    OUTPUT_VARIABLE gc_output
    ERROR_VARIABLE gc_error
    ENCODING UTF-8
)
if(NOT gc_result EQUAL 0 OR NOT gc_output STREQUAL "" OR
   NOT gc_error STREQUAL "")
    message(FATAL_ERROR
        "syntax tree GC check failed with status ${gc_result}\n"
        "stdout: ${gc_output}\nstderr: ${gc_error}")
endif()

function(read_tree_records output_name executable)
    execute_process(
        COMMAND "${executable}" --syntax-tree-records
        WORKING_DIRECTORY "${serial_test_root}"
        RESULT_VARIABLE record_result
        OUTPUT_VARIABLE records
        ERROR_VARIABLE record_error
        ENCODING UTF-8
    )
    string(REPLACE "\r\n" "\n" records "${records}")
    if(NOT record_result EQUAL 0 OR NOT record_error STREQUAL "" OR
       records STREQUAL "")
        message(FATAL_ERROR
            "syntax tree record adapter failed with status ${record_result}\n"
            "stdout: ${records}\nstderr: ${record_error}")
    endif()
    string(REGEX MATCHALL "[^\n]+" record_lines "${records}")
    list(LENGTH record_lines record_count)
    if(NOT record_count EQUAL 46 OR
       NOT records MATCHES "^F\\|0\\|1\\|0\\|0\\|64\\|Fixture" OR
       NOT records MATCHES "X\\|23\\|23\\|1\\|0\\|64\\|7" OR
       NOT records MATCHES "B\\|37\\|0\\|0\\|64\\|12")
        message(FATAL_ERROR
            "syntax tree records have an unexpected shape (${record_count})\n"
            "${records}")
    endif()
    set(${output_name} "${records}" PARENT_SCOPE)
endfunction()

read_tree_records(direct_tree_records "${direct_executable}")
read_tree_records(first_serial_tree_records "${serial_executable}")
read_tree_records(second_serial_tree_records "${serial_executable}")
read_tree_records(parallel_tree_records "${parallel_executable}")
if(NOT direct_tree_records STREQUAL first_serial_tree_records OR
   NOT first_serial_tree_records STREQUAL second_serial_tree_records OR
   NOT first_serial_tree_records STREQUAL parallel_tree_records)
    message(FATAL_ERROR
        "syntax tree records differ across direct, serial, or parallel builds")
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

file(GLOB declaration_inputs LIST_DIRECTORIES FALSE
    "${CMAKE_CURRENT_LIST_DIR}/declaration_corpus/*.co")
set(generated_declaration_directory
    "${CLOTH_WORK_DIRECTORY}/generated-declarations")
run_required("generate bounded declaration corpus" "${CLOTH_WORK_DIRECTORY}"
    "${CLOTH_DECLARATION_ORACLE}" generate
    "${generated_declaration_directory}")
file(GLOB generated_declaration_inputs LIST_DIRECTORIES FALSE
    "${generated_declaration_directory}/*.co")
list(APPEND declaration_inputs ${generated_declaration_inputs})
list(SORT declaration_inputs)
list(LENGTH declaration_inputs declaration_input_count)
set(declaration_input_index 0)
foreach(input IN LISTS declaration_inputs)
    math(EXPR declaration_input_index "${declaration_input_index} + 1")
    message(STATUS
        "declaration parity ${declaration_input_index}/"
        "${declaration_input_count}: ${input}")
    compare_declarations("${input}" "${serial_executable}")
endforeach()

file(GLOB_RECURSE real_declaration_inputs LIST_DIRECTORIES FALSE
    "${CLOTH_BOOTSTRAP_SOURCE}/src/*.co")
list(SORT real_declaration_inputs)
list(LENGTH real_declaration_inputs real_declaration_input_count)
set(real_declaration_input_index 0)
foreach(input IN LISTS real_declaration_inputs)
    math(EXPR real_declaration_input_index
        "${real_declaration_input_index} + 1")
    math(EXPR progress_remainder "${real_declaration_input_index} % 25")
    if(real_declaration_input_index EQUAL 1 OR
       progress_remainder EQUAL 0 OR
       real_declaration_input_index EQUAL real_declaration_input_count)
        message(STATUS
            "real bootstrap declaration parity "
            "${real_declaration_input_index}/${real_declaration_input_count}: "
            "${input}")
    endif()
    compare_declarations("${input}" "${serial_executable}")
endforeach()

file(GLOB definition_inputs LIST_DIRECTORIES FALSE
    "${CMAKE_CURRENT_LIST_DIR}/definition_corpus/*.co")
file(GLOB declaration_recovery_definition_inputs LIST_DIRECTORIES FALSE
    "${CMAKE_CURRENT_LIST_DIR}/declaration_corpus/*.co")
list(APPEND definition_inputs ${declaration_recovery_definition_inputs})
list(APPEND definition_inputs
    "${CLOTH_BOOTSTRAP_SOURCE}/tests/self_host/testdata/parser/DefinitionValid.co"
    "${CLOTH_BOOTSTRAP_SOURCE}/tests/self_host/testdata/parser/definition_recovery.co"
    "${CLOTH_BOOTSTRAP_SOURCE}/tests/self_host/testdata/parser/expression_valid.txt"
    "${CLOTH_BOOTSTRAP_SOURCE}/tests/self_host/testdata/parser/expression_recovery.txt")
list(SORT definition_inputs)
list(LENGTH definition_inputs definition_input_count)
set(definition_input_index 0)
foreach(input IN LISTS definition_inputs)
    math(EXPR definition_input_index "${definition_input_index} + 1")
    get_filename_component(file_name "${input}" NAME_WE)
    message(STATUS
        "definition parity ${definition_input_index}/"
        "${definition_input_count}: ${input}")
    compare_definitions("${input}" "${file_name}" "${direct_executable}"
        "${serial_executable}" "${parallel_executable}")
endforeach()

list(LENGTH real_declaration_inputs real_definition_input_count)
set(real_definition_input_index 0)
foreach(input IN LISTS real_declaration_inputs)
    math(EXPR real_definition_input_index
        "${real_definition_input_index} + 1")
    math(EXPR progress_remainder "${real_definition_input_index} % 25")
    if(real_definition_input_index EQUAL 1 OR
       progress_remainder EQUAL 0 OR
       real_definition_input_index EQUAL real_definition_input_count)
        message(STATUS
            "real bootstrap definition parity "
            "${real_definition_input_index}/${real_definition_input_count}: "
            "${input}")
    endif()
    get_filename_component(file_name "${input}" NAME_WE)
    compare_definitions("${input}" "${file_name}" "${direct_executable}"
        "${serial_executable}" "${parallel_executable}")
endforeach()

set(declaration_determinism_input
    "${CMAKE_CURRENT_LIST_DIR}/declaration_corpus/Implicit.co")
read_declaration_records(direct_declaration_records
    "${declaration_determinism_input}" "${direct_executable}")
read_declaration_records(first_serial_declaration_records
    "${declaration_determinism_input}" "${serial_executable}")
read_declaration_records(second_serial_declaration_records
    "${declaration_determinism_input}" "${serial_executable}")
read_declaration_records(parallel_declaration_records
    "${declaration_determinism_input}" "${parallel_executable}")
if(NOT direct_declaration_records STREQUAL
       first_serial_declaration_records OR
   NOT first_serial_declaration_records STREQUAL
       second_serial_declaration_records OR
   NOT first_serial_declaration_records STREQUAL
       parallel_declaration_records)
    message(FATAL_ERROR
        "declaration records differ across direct, repeated serial, or "
        "parallel builds")
endif()

file(SHA256 "${serial_executable}" preserved_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/cloth.cpa"
    preserved_library_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc.cpa"
    preserved_bootstrap_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc-tests.cpa"
    preserved_test_hash)
file(APPEND "${serial_test_root}/src/BootstrapMain.co" "\n@\n")
execute_process(
    COMMAND "${CLOTH_SHUTTLE}" build --manifest-path
        "${serial_test_root}/Shuttle.toml" --compiler "${CLOTH_COMPILER}"
        --target x86_64 --jobs 4
    WORKING_DIRECTORY "${serial_test_root}"
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
file(SHA256 "${serial_test_root}/target/x86_64/packages/cloth.cpa"
    failed_library_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc.cpa"
    failed_bootstrap_hash)
file(SHA256 "${serial_test_root}/target/x86_64/packages/clothc-tests.cpa"
    failed_test_hash)
if(NOT preserved_library_hash STREQUAL failed_library_hash OR
   NOT preserved_bootstrap_hash STREQUAL failed_bootstrap_hash OR
   NOT preserved_test_hash STREQUAL failed_test_hash)
    message(FATAL_ERROR "failed rebuild replaced a completed package artifact")
endif()

message(STATUS
    "Self-host frontend and compiler/test separation passed for "
    "${input_count} lexer, ${declaration_input_count} bounded declaration, "
    "${real_declaration_input_count} real declaration, "
    "${definition_input_count} bounded definition, and "
    "${real_definition_input_count} real definition inputs; direct and "
    "Shuttle test builds and parser records are deterministic, the "
    "production compiler driver and both targets pass, warm reuse is exact, "
    "and failed output is preserved")
