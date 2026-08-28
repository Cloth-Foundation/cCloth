include(ClothTesting)

cloth_add_unit_test(cloth_abi_tests abi_tests.cc)
cloth_add_unit_test(cloth_backend_tests backend_tests.cc)
cloth_add_unit_test(cloth_lexer_tests lexer_tests.cc)
cloth_add_unit_test(cloth_parser_tests parser_tests.cc)
cloth_add_unit_test(cloth_project_tests project_tests.cc)
target_compile_definitions(cloth_project_tests PRIVATE
    CLOTH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
cloth_add_unit_test(cloth_sema_tests sema_tests.cc)
cloth_add_unit_test(cloth_mir_tests mir_tests.cc)

add_executable(cloth_runtime_tests runtime_tests.cc)
target_link_libraries(cloth_runtime_tests PRIVATE cloth_runtime)
cloth_enable_warnings(cloth_runtime_tests)
add_test(NAME cloth_runtime_tests COMMAND cloth_runtime_tests)

if(CLOTH_BUILD_FUZZERS)
    add_executable(cloth_lexer_parser_fuzzer lexer_parser_fuzzer.cc)
    target_link_libraries(cloth_lexer_parser_fuzzer PRIVATE cloth_compiler)
    cloth_enable_warnings(cloth_lexer_parser_fuzzer)
    target_link_options(cloth_lexer_parser_fuzzer PRIVATE -fsanitize=fuzzer)
    set(CLOTH_FUZZ_CORPUS_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus/lexer_parser")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_corpus/lexer_parser"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus")
    add_test(
        NAME cloth_lexer_parser_fuzzer_smoke
        COMMAND cloth_lexer_parser_fuzzer
                -runs=100
                -max_len=4096
                ${CLOTH_FUZZ_CORPUS_DIRECTORY}
    )
endif()

add_test(
    NAME cloth_cli_example
    COMMAND clothc
            ${PROJECT_SOURCE_DIR}/examples/User.co
            ${PROJECT_SOURCE_DIR}/examples/Repository.co
)
add_test(
    NAME cloth_cli_wasm32
    COMMAND clothc --target=wasm32
            ${PROJECT_SOURCE_DIR}/examples/User.co
            ${PROJECT_SOURCE_DIR}/examples/Repository.co
)

if(CLOTH_LLVM_OPT_EXECUTABLE)
    set(CLOTH_TEST_LLVM_IR ${PROJECT_BINARY_DIR}/cloth-example.ll)
    add_test(
        NAME cloth_cli_emit_llvm
        COMMAND clothc --emit-llvm=${CLOTH_TEST_LLVM_IR}
                ${PROJECT_SOURCE_DIR}/examples/User.co
                ${PROJECT_SOURCE_DIR}/examples/Repository.co
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
    cloth_native_executable_path(hello_executable "cloth hello")
    add_test(
        NAME cloth_cli_build_hello
        COMMAND clothc "--build=${hello_executable}"
                ${PROJECT_SOURCE_DIR}/examples/hello/HelloWorld.co
    )
    add_test(NAME cloth_hello_world COMMAND "${hello_executable}")
    set_tests_properties(cloth_hello_world PROPERTIES
        DEPENDS cloth_cli_build_hello
        PASS_REGULAR_EXPRESSION "Hello, World!"
    )

    cloth_add_native_output_test(void_main
        OUTPUT "cloth void main"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/VoidMain.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/void_main.txt
        EXACT
    )
    cloth_add_native_output_test(final_bindings
        OUTPUT "cloth final bindings"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/FinalBindings.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/final_bindings.txt
        EXACT
    )
    cloth_add_native_output_test(static_members
        OUTPUT "cloth static members"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/StaticMembers.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/static_members.txt
        EXACT
    )
    cloth_add_native_output_test(nullable_flow
        OUTPUT "cloth nullable flow"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/NullableFlow.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/nullable_flow.txt
        EXACT
    )
    cloth_add_native_output_test(null_ergonomics
        OUTPUT "cloth null ergonomics"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/NullErgonomics.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/null_ergonomics.txt
        EXACT
    )
    cloth_add_native_output_test(gc_stress
        OUTPUT "cloth gc stress"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/GcStress.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gc_stress.txt
        EXACT
    )
    cloth_add_native_output_test(gc_collections
        OUTPUT "cloth gc collections"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/GcCollections.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gc_collections.txt
        EXACT
    )
    cloth_add_native_failure_test(null_assert_failure
        OUTPUT "cloth null assert failure"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/NullAssertFailure.co
        ERROR "non-null assertion failed"
    )
    cloth_add_native_output_test(fizzbuzz
        OUTPUT "cloth fizzbuzz"
        SOURCE ${PROJECT_SOURCE_DIR}/examples/FizzBuzz.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fizzbuzz.txt
    )
    cloth_add_native_output_test(print_values
        OUTPUT "cloth print values"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/PrintValues.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/print_values.txt
    )
    cloth_add_native_output_test(print_representation
        OUTPUT "cloth print representation"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/PrintRepresentation.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/print_representation.txt
        EXACT
    )
    cloth_add_native_output_test(import_project
        OUTPUT "cloth import project"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/projects/imports/src/Main.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import_project.txt
    )
    cloth_add_native_output_test(array_sum
        OUTPUT "cloth array sum"
        SOURCE ${PROJECT_SOURCE_DIR}/examples/ArraySum.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/array_sum.txt
    )
    cloth_add_native_failure_test(array_bounds
        OUTPUT "cloth array bounds"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ArrayBounds.co
        ERROR "array index is out of bounds"
    )
    cloth_add_native_output_test(for_each
        OUTPUT "cloth for each"
        SOURCE ${PROJECT_SOURCE_DIR}/examples/ForEach.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/for_each.txt
    )
    cloth_add_native_output_test(for_contract
        OUTPUT "cloth for contract"
        SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ForContract.co
        EXPECTED ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/for_contract.txt
    )
endif()

cloth_add_cli_failure_test(lexical_error
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/errors.co)
cloth_add_cli_failure_test(parser_error
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/parser_errors.co)
cloth_add_cli_failure_test(sema_error
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sema_errors.co)
cloth_add_cli_failure_test(void_error
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/void_errors.co)
cloth_add_cli_failure_test(final_error
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/FinalErrors.co)
cloth_add_cli_failure_test(nullable_error
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/NullableErrors.co)

cloth_finalize_tests()
