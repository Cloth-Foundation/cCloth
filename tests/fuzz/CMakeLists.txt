if(NOT CLOTH_BUILD_FUZZERS)
    return()
endif()

add_executable(cloth_lexer_parser_fuzzer lexer_parser_fuzzer.cc)
target_link_libraries(cloth_lexer_parser_fuzzer PRIVATE cloth_compiler)
cloth_enable_warnings(cloth_lexer_parser_fuzzer)
target_link_options(cloth_lexer_parser_fuzzer PRIVATE -fsanitize=fuzzer)

set(CLOTH_FUZZ_CORPUS_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/corpus/lexer_parser")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/corpus/lexer_parser"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/corpus")
add_test(
    NAME cloth_lexer_parser_fuzzer_smoke
    COMMAND cloth_lexer_parser_fuzzer
            -runs=100
            -max_len=4096
            ${CLOTH_FUZZ_CORPUS_DIRECTORY}
)

cloth_finalize_tests(LABEL fuzz)
