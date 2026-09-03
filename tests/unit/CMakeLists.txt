# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cloth_add_unit_test(cloth_abi_tests abi_tests.cc)
cloth_add_unit_test(cloth_enum_tests enum_tests.cc)
cloth_add_unit_test(cloth_switch_tests switch_tests.cc)
cloth_add_unit_test(cloth_constant_tests constant_tests.cc)
cloth_add_unit_test(cloth_struct_tests struct_tests.cc)
cloth_add_unit_test(cloth_aggregate_tests aggregate_tests.cc)
cloth_add_unit_test(cloth_backend_tests backend_tests.cc)
cloth_add_unit_test(cloth_identity_tests identity_tests.cc)
cloth_add_unit_test(cloth_imported_package_tests imported_package_tests.cc)
cloth_add_unit_test(cloth_package_artifact_tests package_artifact_tests.cc)
cloth_add_unit_test(cloth_lexer_tests lexer_tests.cc)
cloth_add_unit_test(cloth_parser_tests parser_tests.cc)
cloth_add_unit_test(cloth_project_tests project_tests.cc)
target_compile_definitions(cloth_project_tests PRIVATE
    CLOTH_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/integration"
)
cloth_add_unit_test(cloth_sema_tests sema_tests.cc)
cloth_add_unit_test(cloth_override_tests override_tests.cc)
cloth_add_unit_test(cloth_shuttle_protocol_tests shuttle_protocol_tests.cc)
cloth_add_unit_test(cloth_mir_tests mir_tests.cc)

add_executable(cloth_runtime_tests runtime_tests.cc)
target_link_libraries(cloth_runtime_tests PRIVATE cloth_runtime)
target_include_directories(cloth_runtime_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/support
)
cloth_enable_warnings(cloth_runtime_tests)
add_test(NAME cloth_runtime_tests COMMAND cloth_runtime_tests)
foreach(scenario IN ITEMS null zero alignment stride table count offset
                          unaligned duplicate unsorted reference_alignment overflow negative)
    add_test(NAME cloth_runtime_array_${scenario}
        COMMAND ${CMAKE_COMMAND}
            "-DCLOTH_PROGRAM=$<TARGET_FILE:cloth_runtime_tests>"
            "-DCLOTH_PROGRAM_ARGUMENTS=${scenario}"
            "-DCLOTH_EXPECTED_ERROR=array"
            -P ${PROJECT_SOURCE_DIR}/tests/integration/RunProgram.cmake)
endforeach()
foreach(scenario IN ITEMS integer_overflow integer_division_by_zero
                          integer_remainder_by_zero invalid_integer_arithmetic_reason)
    add_test(NAME cloth_runtime_${scenario}
        COMMAND ${CMAKE_COMMAND}
            "-DCLOTH_PROGRAM=$<TARGET_FILE:cloth_runtime_tests>"
            "-DCLOTH_PROGRAM_ARGUMENTS=${scenario}"
            "-DCLOTH_EXPECTED_ERROR=integer"
            -P ${PROJECT_SOURCE_DIR}/tests/integration/RunProgram.cmake)
endforeach()

cloth_finalize_tests(LABEL unit)

# Includes full MIR/LLVM emission at the 65,536-label/65,537-arm boundary.
# Allow sanitizer instrumentation and concurrent toolchain tests headroom
# without relaxing the timeout for ordinary unit tests.
math(EXPR switch_test_timeout "4 * ${CLOTH_TEST_TIMEOUT_SECONDS}")
set_tests_properties(cloth_switch_tests PROPERTIES TIMEOUT ${switch_test_timeout})
# The maximum-size constant fixtures also undergo independent HIR value and
# dependency verification. Allow both passes to complete under ASan/UBSan.
math(EXPR constant_test_timeout "8 * ${CLOTH_TEST_TIMEOUT_SECONDS}")
set_tests_properties(cloth_constant_tests PROPERTIES
    TIMEOUT ${constant_test_timeout} RUN_SERIAL TRUE)
