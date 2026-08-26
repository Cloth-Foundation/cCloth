cmake_minimum_required(VERSION 3.25)

project(Cloth VERSION 0.1.0 LANGUAGES CXX)

option(CLOTH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLOTH_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLOTH_CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE CLOTH_FORMAT_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/cloth/*.h
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
    src/ast/ast.cc
    src/ast/ast_printer.cc
    src/diagnostics/diagnostic_engine.cc
    src/lexer/lexer.cc
    src/lexer/token.cc
    src/parser/declaration_pass.cc
    src/parser/definition_pass.cc
    src/parser/expression_parser.cc
    src/parser/parser.cc
    src/sema/file_class_symbols.cc
    src/sema/visibility.cc
    src/source/source_file.cc
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

add_executable(clothc src/main.cc)
target_link_libraries(clothc PRIVATE cloth_compiler)
cloth_enable_warnings(clothc)

include(CTest)
if(BUILD_TESTING)
    add_executable(cloth_lexer_tests tests/lexer_tests.cc)
    target_link_libraries(cloth_lexer_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_lexer_tests)
    add_test(NAME cloth_lexer_tests COMMAND cloth_lexer_tests)
    add_executable(cloth_parser_tests tests/parser_tests.cc)
    target_link_libraries(cloth_parser_tests PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_parser_tests)
    add_test(NAME cloth_parser_tests COMMAND cloth_parser_tests)
    add_test(
        NAME cloth_cli_example
        COMMAND clothc ${CMAKE_CURRENT_SOURCE_DIR}/examples/User.co
    )
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
endif()

add_custom_target(run_compiler
    COMMAND $<TARGET_FILE:clothc> ${CMAKE_CURRENT_SOURCE_DIR}/examples/User.co
    DEPENDS clothc
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    USES_TERMINAL
)
