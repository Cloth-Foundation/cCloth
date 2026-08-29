cloth_add_unit_test(cloth_abi_tests abi_tests.cc)
cloth_add_unit_test(cloth_backend_tests backend_tests.cc)
cloth_add_unit_test(cloth_lexer_tests lexer_tests.cc)
cloth_add_unit_test(cloth_parser_tests parser_tests.cc)
cloth_add_unit_test(cloth_project_tests project_tests.cc)
target_compile_definitions(cloth_project_tests PRIVATE
    CLOTH_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/integration"
)
cloth_add_unit_test(cloth_sema_tests sema_tests.cc)
cloth_add_unit_test(cloth_mir_tests mir_tests.cc)

add_executable(cloth_runtime_tests runtime_tests.cc)
target_link_libraries(cloth_runtime_tests PRIVATE cloth_runtime)
target_include_directories(cloth_runtime_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/support
)
cloth_enable_warnings(cloth_runtime_tests)
add_test(NAME cloth_runtime_tests COMMAND cloth_runtime_tests)

cloth_finalize_tests(LABEL unit)
