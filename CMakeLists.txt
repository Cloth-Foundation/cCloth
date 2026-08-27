cmake_minimum_required(VERSION 3.25)

project(Cloth VERSION 0.1.0 LANGUAGES CXX)

option(CLOTH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLOTH_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
find_program(CLOTH_LLVM_OPT_EXECUTABLE NAMES opt)
find_program(CLOTH_LLVM_LLC_EXECUTABLE NAMES llc)
if(CLOTH_CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE CLOTH_FORMAT_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/cloth/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc
    )
    add_custom_target(format
        COMMAND ${CLOTH_CLANG_FORMAT_EXECUTABLE} -i --style=file
                ${CLOTH_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting Cloth C++ sources"
    )
    add_custom_target(check_format
        COMMAND ${CLOTH_CLANG_FORMAT_EXECUTABLE} --dry-run --Werror
                --style=file ${CLOTH_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking Cloth C++ formatting"
    )
endif()

add_library(cloth_compiler
    src/abi/abi.cc
    src/abi/abi_printer.cc
    src/abi/abi_verifier.cc
    src/ast/ast.cc
    src/ast/ast_printer.cc
    src/backend/llvm_ir.cc
    src/backend/native_toolchain.cc
    src/compiler/compilation.cc
    src/diagnostics/diagnostic_engine.cc
    src/flow/control_flow.cc
    src/hir/hir.cc
    src/hir/hir_printer.cc
    src/hir/hir_verifier.cc
    src/lexer/lexer.cc
    src/lexer/token.cc
    src/mir/mir.cc
    src/mir/mir_printer.cc
    src/mir/mir_verifier.cc
    src/parser/declaration_pass.cc
    src/parser/definition_pass.cc
    src/parser/expression_parser.cc
    src/parser/parser.cc
    src/project/project_layout.cc
    src/sema/file_class_symbols.cc
    src/sema/semantic_analyzer.cc
    src/sema/semantic_model.cc
    src/sema/visibility.cc
    src/source/source_file.cc
    src/target/data_layout.cc
)

target_compile_features(cloth_compiler PUBLIC cxx_std_23)

target_include_directories(cloth_compiler
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

function(cloth_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
        if(CLOTH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
        )
        if(CLOTH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

cloth_enable_warnings(cloth_compiler)

add_library(cloth_runtime STATIC runtime/runtime.cc)
target_compile_features(cloth_runtime PUBLIC cxx_std_23)
target_include_directories(cloth_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
cloth_enable_warnings(cloth_runtime)

add_executable(clothc src/main.cc)
target_link_libraries(clothc PRIVATE cloth_compiler)
cloth_enable_warnings(clothc)
add_dependencies(clothc cloth_runtime)

file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" CLOTH_NATIVE_LINKER_PATH)
if(WIN32)
    if(MSVC)
        set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-pc-windows-msvc")
        target_compile_definitions(clothc PRIVATE CLOTH_NATIVE_LINKER_MSVC=1)
    else()
        set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-w64-windows-gnu")
        target_compile_definitions(clothc PRIVATE
            CLOTH_NATIVE_STATIC_RUNTIME=1)
    endif()
elseif(APPLE)
    set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-apple-macosx")
else()
    set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-unknown-linux-gnu")
endif()
target_compile_definitions(clothc PRIVATE
    CLOTH_DEFAULT_NATIVE_LINKER="${CLOTH_NATIVE_LINKER_PATH}"
    CLOTH_DEFAULT_NATIVE_TARGET="${CLOTH_NATIVE_TARGET_TRIPLE}"
    CLOTH_DEFAULT_RUNTIME_LIBRARY="$<TARGET_FILE:cloth_runtime>"
)
if(CLOTH_LLVM_LLC_EXECUTABLE)
    file(TO_CMAKE_PATH "${CLOTH_LLVM_LLC_EXECUTABLE}"
         CLOTH_LLVM_LLC_PATH)
    target_compile_definitions(clothc PRIVATE
        CLOTH_DEFAULT_LLC="${CLOTH_LLVM_LLC_PATH}"
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(cloth_abi_tests tests/abi_tests.cc)
    target_link_libraries(cloth_abi_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_abi_tests)
    add_test(NAME cloth_abi_tests COMMAND cloth_abi_tests)
    add_executable(cloth_backend_tests tests/backend_tests.cc)
    target_link_libraries(cloth_backend_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_backend_tests)
    add_test(NAME cloth_backend_tests COMMAND cloth_backend_tests)
    add_executable(cloth_lexer_tests tests/lexer_tests.cc)
    target_link_libraries(cloth_lexer_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_lexer_tests)
    add_test(NAME cloth_lexer_tests COMMAND cloth_lexer_tests)
    add_executable(cloth_parser_tests tests/parser_tests.cc)
    target_link_libraries(cloth_parser_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_parser_tests)
    add_test(NAME cloth_parser_tests COMMAND cloth_parser_tests)
    add_executable(cloth_project_tests tests/project_tests.cc)
    target_link_libraries(cloth_project_tests PRIVATE cloth_compiler)
    target_compile_definitions(cloth_project_tests PRIVATE
        CLOTH_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    cloth_enable_warnings(cloth_project_tests)
    add_test(NAME cloth_project_tests COMMAND cloth_project_tests)
    add_executable(cloth_sema_tests tests/sema_tests.cc)
    target_link_libraries(cloth_sema_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_sema_tests)
    add_test(NAME cloth_sema_tests COMMAND cloth_sema_tests)
    add_executable(cloth_mir_tests tests/mir_tests.cc)
    target_link_libraries(cloth_mir_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_mir_tests)
    add_test(NAME cloth_mir_tests COMMAND cloth_mir_tests)
    add_test(
        NAME cloth_cli_example
        COMMAND clothc
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/User.co
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/Repository.co
    )
    add_test(
        NAME cloth_cli_wasm32
        COMMAND clothc --target=wasm32
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/User.co
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/Repository.co
    )
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        set(CLOTH_TEST_LLVM_IR ${CMAKE_CURRENT_BINARY_DIR}/cloth-example.ll)
        add_test(
            NAME cloth_cli_emit_llvm
            COMMAND clothc --emit-llvm=${CLOTH_TEST_LLVM_IR}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/User.co
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/Repository.co
        )
        add_test(
            NAME cloth_llvm_verify
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                    -passes=verify -disable-output ${CLOTH_TEST_LLVM_IR}
        )
        set_tests_properties(cloth_llvm_verify PROPERTIES
            DEPENDS cloth_cli_emit_llvm
        )
    endif()
    if(CLOTH_LLVM_LLC_EXECUTABLE)
        if(WIN32)
            set(CLOTH_TEST_HELLO_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth hello.exe")
        else()
            set(CLOTH_TEST_HELLO_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth hello")
        endif()
        add_test(
            NAME cloth_cli_build_hello
            COMMAND clothc "--build=${CLOTH_TEST_HELLO_EXECUTABLE}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello/HelloWorld.co
        )
        add_test(
            NAME cloth_hello_world
            COMMAND ${CLOTH_TEST_HELLO_EXECUTABLE}
        )
        set_tests_properties(cloth_hello_world PROPERTIES
            DEPENDS cloth_cli_build_hello
            PASS_REGULAR_EXPRESSION "Hello, World!"
        )

        if(WIN32)
            set(CLOTH_TEST_FIZZBUZZ_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth fizzbuzz.exe")
        else()
            set(CLOTH_TEST_FIZZBUZZ_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth fizzbuzz")
        endif()
        add_test(
            NAME cloth_cli_build_fizzbuzz
            COMMAND clothc "--build=${CLOTH_TEST_FIZZBUZZ_EXECUTABLE}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/FizzBuzz.co
        )
        add_test(
            NAME cloth_fizzbuzz
            COMMAND ${CMAKE_COMMAND}
                    "-DCLOTH_PROGRAM=${CLOTH_TEST_FIZZBUZZ_EXECUTABLE}"
                    "-DCLOTH_EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fizzbuzz.txt"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_program_output.cmake
        )
        set_tests_properties(cloth_fizzbuzz PROPERTIES
            DEPENDS cloth_cli_build_fizzbuzz
        )

        if(WIN32)
            set(CLOTH_TEST_PRINT_VALUES_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth print values.exe")
        else()
            set(CLOTH_TEST_PRINT_VALUES_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth print values")
        endif()
        add_test(
            NAME cloth_cli_build_print_values
            COMMAND clothc "--build=${CLOTH_TEST_PRINT_VALUES_EXECUTABLE}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/PrintValues.co
        )
        add_test(
            NAME cloth_print_values
            COMMAND ${CMAKE_COMMAND}
                    "-DCLOTH_PROGRAM=${CLOTH_TEST_PRINT_VALUES_EXECUTABLE}"
                    "-DCLOTH_EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/print_values.txt"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_program_output.cmake
        )
        set_tests_properties(cloth_print_values PROPERTIES
            DEPENDS cloth_cli_build_print_values
        )

        if(WIN32)
            set(CLOTH_TEST_IMPORT_PROJECT_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth import project.exe")
        else()
            set(CLOTH_TEST_IMPORT_PROJECT_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth import project")
        endif()
        add_test(
            NAME cloth_cli_build_import_project
            COMMAND clothc
                    "--build=${CLOTH_TEST_IMPORT_PROJECT_EXECUTABLE}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/projects/imports/src/Main.co
        )
        add_test(
            NAME cloth_import_project
            COMMAND ${CMAKE_COMMAND}
                    "-DCLOTH_PROGRAM=${CLOTH_TEST_IMPORT_PROJECT_EXECUTABLE}"
                    "-DCLOTH_EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/import_project.txt"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_program_output.cmake
        )
        set_tests_properties(cloth_import_project PROPERTIES
            DEPENDS cloth_cli_build_import_project
        )

        if(WIN32)
            set(CLOTH_TEST_ARRAY_SUM_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth array sum.exe")
        else()
            set(CLOTH_TEST_ARRAY_SUM_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth array sum")
        endif()
        add_test(
            NAME cloth_cli_build_array_sum
            COMMAND clothc "--build=${CLOTH_TEST_ARRAY_SUM_EXECUTABLE}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/ArraySum.co
        )
        add_test(
            NAME cloth_array_sum
            COMMAND ${CMAKE_COMMAND}
                    "-DCLOTH_PROGRAM=${CLOTH_TEST_ARRAY_SUM_EXECUTABLE}"
                    "-DCLOTH_EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/array_sum.txt"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_program_output.cmake
        )
        set_tests_properties(cloth_array_sum PROPERTIES
            DEPENDS cloth_cli_build_array_sum
        )

        if(WIN32)
            set(CLOTH_TEST_ARRAY_BOUNDS_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth array bounds.exe")
        else()
            set(CLOTH_TEST_ARRAY_BOUNDS_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth array bounds")
        endif()
        add_test(
            NAME cloth_cli_build_array_bounds
            COMMAND clothc "--build=${CLOTH_TEST_ARRAY_BOUNDS_EXECUTABLE}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ArrayBounds.co
        )
        add_test(
            NAME cloth_array_bounds
            COMMAND ${CMAKE_COMMAND}
                    "-DCLOTH_PROGRAM=${CLOTH_TEST_ARRAY_BOUNDS_EXECUTABLE}"
                    "-DCLOTH_EXPECTED_ERROR=array index is out of bounds"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_program_failure.cmake
        )
        set_tests_properties(cloth_array_bounds PROPERTIES
            DEPENDS cloth_cli_build_array_bounds
        )

        if(WIN32)
            set(CLOTH_TEST_FOR_EACH_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth for each.exe")
        else()
            set(CLOTH_TEST_FOR_EACH_EXECUTABLE
                "${CMAKE_CURRENT_BINARY_DIR}/cloth for each")
        endif()
        add_test(
            NAME cloth_cli_build_for_each
            COMMAND clothc "--build=${CLOTH_TEST_FOR_EACH_EXECUTABLE}"
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/ForEach.co
        )
        add_test(
            NAME cloth_for_each
            COMMAND ${CMAKE_COMMAND}
                    "-DCLOTH_PROGRAM=${CLOTH_TEST_FOR_EACH_EXECUTABLE}"
                    "-DCLOTH_EXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/for_each.txt"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_program_output.cmake
        )
        set_tests_properties(cloth_for_each PROPERTIES
            DEPENDS cloth_cli_build_for_each
        )
    endif()
    add_test(
        NAME cloth_cli_lexical_error
        COMMAND clothc ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/errors.co
    )
    set_tests_properties(cloth_cli_lexical_error PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME cloth_cli_parser_error
        COMMAND clothc ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/parser_errors.co
    )
    set_tests_properties(cloth_cli_parser_error PROPERTIES WILL_FAIL TRUE)
    add_test(
        NAME cloth_cli_sema_error
        COMMAND clothc ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sema_errors.co
    )
    set_tests_properties(cloth_cli_sema_error PROPERTIES WILL_FAIL TRUE)
endif()

add_custom_target(run_compiler
    COMMAND $<TARGET_FILE:clothc>
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/User.co
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/Repository.co
    DEPENDS clothc
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)
