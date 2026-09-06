# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set(CLOTH_INTEGRATION_PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/programs")
set(CLOTH_INTEGRATION_EXPECTED "${CMAKE_CURRENT_SOURCE_DIR}/expected")
set(CLOTH_INTEGRATION_ERRORS "${CMAKE_CURRENT_SOURCE_DIR}/errors")
set(CLOTH_INTEGRATION_PROJECTS "${CMAKE_CURRENT_SOURCE_DIR}/projects")
set(CLOTH_TYPED_ERROR_PROJECT
    "${CLOTH_INTEGRATION_PROJECTS}/typed_errors/src")
set(CLOTH_STANDARD_LIBRARY_PROJECT
    "${CLOTH_INTEGRATION_PROJECTS}/standard_library/src")
set(CLOTH_STANDARD_LIBRARY_ROOT "${PROJECT_SOURCE_DIR}/std/src")

foreach(target IN ITEMS x86_64 wasm32)
    set(standard_library_ir
        "${PROJECT_BINARY_DIR}/cloth-standard-library-${target}.ll")
    add_test(NAME cloth_cli_standard_library_llvm_${target}
        COMMAND clothc
            --shuttle-protocol 1
            --target ${target}
            --output-kind llvm-ir
            --output "${standard_library_ir}"
            --root-package app
            --entry Main.co
            --package app 0.1.0 "${CLOTH_STANDARD_LIBRARY_PROJECT}"
            --package cloth ${PROJECT_VERSION} "${CLOTH_STANDARD_LIBRARY_ROOT}"
            --dependency app cloth cloth)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_standard_library_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${standard_library_ir})
        set_tests_properties(cloth_standard_library_verify_${target}
            PROPERTIES DEPENDS cloth_cli_standard_library_llvm_${target})
    endif()
    add_test(NAME cloth_cli_check_numeric_literal_notation_${target}
        COMMAND clothc --check --target=${target}
            ${CLOTH_INTEGRATION_PROGRAMS}/NumericLiteralNotation.co)
    set(numeric_literal_notation_ir
        "${PROJECT_BINARY_DIR}/cloth-numeric-literal-notation-${target}.ll")
    add_test(NAME cloth_cli_numeric_literal_notation_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${numeric_literal_notation_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/NumericLiteralNotation.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_numeric_literal_notation_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${numeric_literal_notation_ir})
        set_tests_properties(cloth_numeric_literal_notation_verify_${target}
            PROPERTIES
                DEPENDS cloth_cli_numeric_literal_notation_llvm_${target})
    endif()
    add_test(NAME cloth_cli_check_typed_numeric_literals_${target}
        COMMAND clothc --check --target=${target}
            ${CLOTH_INTEGRATION_PROGRAMS}/TypedNumericLiterals.co)
    set(typed_numeric_literal_ir
        "${PROJECT_BINARY_DIR}/cloth-typed-numeric-literals-${target}.ll")
    add_test(NAME cloth_cli_typed_numeric_literals_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${typed_numeric_literal_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/TypedNumericLiterals.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_typed_numeric_literals_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${typed_numeric_literal_ir})
        set_tests_properties(cloth_typed_numeric_literals_verify_${target}
            PROPERTIES
                DEPENDS cloth_cli_typed_numeric_literals_llvm_${target})
    endif()
    add_test(NAME cloth_cli_check_integer_conversion_modes_${target}
        COMMAND clothc --check --target=${target}
            ${CLOTH_INTEGRATION_PROGRAMS}/IntegerConversionModes.co)
    set(integer_conversion_ir
        "${PROJECT_BINARY_DIR}/cloth-integer-conversions-${target}.ll")
    add_test(NAME cloth_cli_integer_conversion_modes_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${integer_conversion_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/IntegerConversionModes.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_integer_conversion_modes_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${integer_conversion_ir})
        set_tests_properties(cloth_integer_conversion_modes_verify_${target}
            PROPERTIES
                DEPENDS cloth_cli_integer_conversion_modes_llvm_${target})
    endif()
    add_test(NAME cloth_cli_check_switch_${target}
        COMMAND clothc --check --target=${target}
            ${CLOTH_INTEGRATION_PROGRAMS}/Switch.co)
    set(constant_ir "${PROJECT_BINARY_DIR}/cloth-constants-${target}.ll")
    add_test(NAME cloth_cli_constants_llvm_${target}
        COMMAND clothc --target=${target} --emit-llvm=${constant_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/Constants.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_constants_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                -passes=verify -disable-output ${constant_ir})
        set_tests_properties(cloth_constants_verify_${target} PROPERTIES
            DEPENDS cloth_cli_constants_llvm_${target})
    endif()
    set(checked_arithmetic_ir
        "${PROJECT_BINARY_DIR}/cloth-checked-arithmetic-${target}.ll")
    add_test(NAME cloth_cli_checked_arithmetic_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${checked_arithmetic_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/CheckedIntegerArithmetic.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_checked_arithmetic_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${checked_arithmetic_ir})
        set_tests_properties(cloth_checked_arithmetic_verify_${target}
            PROPERTIES DEPENDS cloth_cli_checked_arithmetic_llvm_${target})
    endif()
    set(checked_updates_ir
        "${PROJECT_BINARY_DIR}/cloth-checked-updates-${target}.ll")
    add_test(NAME cloth_cli_checked_updates_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${checked_updates_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/CheckedIntegerUpdates.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_checked_updates_optimized_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${checked_updates_ir})
        set_tests_properties(cloth_checked_updates_optimized_verify_${target}
            PROPERTIES DEPENDS cloth_cli_checked_updates_llvm_${target})
    endif()
    set(typed_error_ir
        "${PROJECT_BINARY_DIR}/cloth-typed-error-${target}.ll")
    add_test(NAME cloth_cli_typed_error_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${typed_error_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/TypedErrorPropagation.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_typed_error_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${typed_error_ir})
        set_tests_properties(cloth_typed_error_verify_${target}
            PROPERTIES DEPENDS cloth_cli_typed_error_llvm_${target})
    endif()
    set(typed_error_values_ir
        "${PROJECT_BINARY_DIR}/cloth-typed-error-values-${target}.ll")
    add_test(NAME cloth_cli_typed_error_values_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${typed_error_values_ir}
            --source-root=${CLOTH_TYPED_ERROR_PROJECT}
            ${CLOTH_TYPED_ERROR_PROJECT}/Main.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_typed_error_values_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${typed_error_values_ir})
        set_tests_properties(cloth_typed_error_values_verify_${target}
            PROPERTIES DEPENDS cloth_cli_typed_error_values_llvm_${target})
    endif()
    set(program_arguments_ir
        "${PROJECT_BINARY_DIR}/cloth-program-arguments-${target}.ll")
    add_test(NAME cloth_cli_program_arguments_llvm_${target}
        COMMAND clothc --target=${target}
            --emit-llvm=${program_arguments_ir}
            ${CLOTH_INTEGRATION_PROGRAMS}/ProgramArguments.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_program_arguments_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                "-passes=verify,default<O2>,verify" -disable-output
                ${program_arguments_ir})
        set_tests_properties(cloth_program_arguments_verify_${target}
            PROPERTIES DEPENDS cloth_cli_program_arguments_llvm_${target})
    endif()
    set(switch_ir "${PROJECT_BINARY_DIR}/cloth-switch-${target}.ll")
    add_test(NAME cloth_cli_switch_llvm_${target}
        COMMAND clothc --target=${target} --emit-llvm=${switch_ir}
            --source-root=${CLOTH_INTEGRATION_PROJECTS}/switch/src
            ${CLOTH_INTEGRATION_PROJECTS}/switch/src/Main.co)
    if(CLOTH_LLVM_OPT_EXECUTABLE)
        add_test(NAME cloth_switch_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                -passes=verify -disable-output ${switch_ir})
        set_tests_properties(cloth_switch_verify_${target} PROPERTIES
            DEPENDS cloth_cli_switch_llvm_${target})
    endif()
endforeach()

if(CLOTH_LLVM_LLC_EXECUTABLE)
    set(CLOTH_TEST_STANDARD_LIBRARY_NATIVE ON)
else()
    set(CLOTH_TEST_STANDARD_LIBRARY_NATIVE OFF)
endif()
add_test(NAME cloth_standard_library_artifacts
    COMMAND ${CMAKE_COMMAND}
        "-DCLOTH_COMPILER=$<TARGET_FILE:clothc>"
        "-DCLOTH_STANDARD_LIBRARY_ROOT=${CLOTH_STANDARD_LIBRARY_ROOT}"
        "-DCLOTH_APPLICATION_ROOT=${CLOTH_STANDARD_LIBRARY_PROJECT}"
        "-DCLOTH_WORK_DIRECTORY=${PROJECT_BINARY_DIR}/standard-library-artifacts"
        "-DCLOTH_EXPECTED=${CLOTH_INTEGRATION_EXPECTED}/standard_library.txt"
        "-DCLOTH_STANDARD_LIBRARY_VERSION=${PROJECT_VERSION}"
        "-DCLOTH_TEST_NATIVE=${CLOTH_TEST_STANDARD_LIBRARY_NATIVE}"
        "-DCLOTH_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunStandardLibrary.cmake)

add_test(NAME cloth_cli_check_structs
    COMMAND clothc --check
            --source-root=${CLOTH_INTEGRATION_PROJECTS}/structs/src
            ${CLOTH_INTEGRATION_PROJECTS}/structs/src/Main.co
)
set_tests_properties(cloth_cli_check_structs PROPERTIES
    PASS_REGULAR_EXPRESSION "Struct Point"
)
add_test(NAME cloth_cli_check_structs_wasm32
    COMMAND clothc --check --target=wasm32
            --source-root=${CLOTH_INTEGRATION_PROJECTS}/structs/src
            ${CLOTH_INTEGRATION_PROJECTS}/structs/src/Main.co
)
set_tests_properties(cloth_cli_check_structs_wasm32 PROPERTIES
    PASS_REGULAR_EXPRESSION "Struct Point"
)
add_test(NAME cloth_cli_check_output_conflict
    COMMAND clothc --check --emit-llvm ${CLOTH_INTEGRATION_PROJECTS}/structs/src/Main.co
)
set_tests_properties(cloth_cli_check_output_conflict PROPERTIES WILL_FAIL TRUE)

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

add_test(NAME cloth_cli_shuttle_protocol_version
    COMMAND clothc --shuttle-protocol-version)
set_tests_properties(cloth_cli_shuttle_protocol_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^1"
)

add_test(
    NAME cloth_cli_shuttle_check
    COMMAND clothc
            --shuttle-protocol 1
            --target wasm32
            --output-kind check
            --root-package imports
            --entry Main.co
            --package imports 0.1.0
            ${CLOTH_INTEGRATION_PROJECTS}/imports/src
)

if(CLOTH_LLVM_OPT_EXECUTABLE)
    foreach(target IN ITEMS x86_64 wasm32)
        set(struct_ir "${PROJECT_BINARY_DIR}/cloth-structs-${target}.ll")
        add_test(NAME cloth_cli_structs_llvm_${target}
            COMMAND clothc --target=${target} --emit-llvm=${struct_ir}
                --source-root=${CLOTH_INTEGRATION_PROJECTS}/structs/src
                ${CLOTH_INTEGRATION_PROJECTS}/structs/src/Main.co)
        add_test(NAME cloth_structs_verify_${target}
            COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                -passes=verify -disable-output ${struct_ir})
        set_tests_properties(cloth_structs_verify_${target} PROPERTIES
            DEPENDS cloth_cli_structs_llvm_${target})
    endforeach()
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
    cloth_add_native_output_test(constants
        OUTPUT "cloth constants"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/Constants.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/constants.txt
        EXACT
    )
    cloth_add_native_output_test(switch
        OUTPUT "cloth switch"
        SOURCE ${CLOTH_INTEGRATION_PROJECTS}/switch/src/Main.co
        SOURCE_ROOT ${CLOTH_INTEGRATION_PROJECTS}/switch/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/switch.txt
        EXACT
    )
    cloth_add_native_output_test(structs
        OUTPUT "cloth structs"
        SOURCE ${CLOTH_INTEGRATION_PROJECTS}/structs/src/Main.co
        SOURCE_ROOT ${CLOTH_INTEGRATION_PROJECTS}/structs/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/structs.txt
        EXACT
    )
    get_target_property(native_definitions clothc COMPILE_DEFINITIONS)
    add_executable(cloth_integer_conversion_native_tests
        integer_conversion_native_tests.cc)
    target_link_libraries(cloth_integer_conversion_native_tests
        PRIVATE cloth_compiler)
    target_compile_definitions(cloth_integer_conversion_native_tests
        PRIVATE ${native_definitions})
    cloth_enable_warnings(cloth_integer_conversion_native_tests)
    add_dependencies(cloth_integer_conversion_native_tests cloth_runtime)
    foreach(target IN ITEMS x86_64 wasm32)
        set(integer_conversion_matrix_ir
            "${PROJECT_BINARY_DIR}/cloth-integer-conversion-matrix-${target}.ll")
        add_test(NAME cloth_build_integer_conversion_matrix_llvm_${target}
            COMMAND cloth_integer_conversion_native_tests llvm-${target}
                ${integer_conversion_matrix_ir})
        if(CLOTH_LLVM_OPT_EXECUTABLE)
            add_test(NAME cloth_integer_conversion_matrix_verify_${target}
                COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                    "-passes=verify,default<O2>,verify" -disable-output
                    ${integer_conversion_matrix_ir})
            set_tests_properties(
                cloth_integer_conversion_matrix_verify_${target}
                PROPERTIES DEPENDS
                    cloth_build_integer_conversion_matrix_llvm_${target})
        endif()
    endforeach()
    cloth_native_executable_path(integer_conversion_matrix_executable
        "cloth integer conversion matrix")
    add_test(NAME cloth_build_integer_conversion_matrix_native
        COMMAND cloth_integer_conversion_native_tests native
            "${integer_conversion_matrix_executable}")
    add_test(NAME cloth_integer_conversion_matrix_native
        COMMAND "${integer_conversion_matrix_executable}")
    set_tests_properties(cloth_integer_conversion_matrix_native PROPERTIES
        DEPENDS cloth_build_integer_conversion_matrix_native)
    add_executable(cloth_switch_native_tests switch_native_tests.cc)
    target_link_libraries(cloth_switch_native_tests PRIVATE cloth_compiler)
    target_compile_definitions(cloth_switch_native_tests PRIVATE ${native_definitions})
    cloth_enable_warnings(cloth_switch_native_tests)
    add_dependencies(cloth_switch_native_tests cloth_runtime)

    add_executable(cloth_mir_optimizer_native_tests
        mir_optimizer_native_tests.cc)
    target_link_libraries(cloth_mir_optimizer_native_tests
        PRIVATE cloth_compiler)
    target_compile_definitions(cloth_mir_optimizer_native_tests
        PRIVATE ${native_definitions})
    cloth_enable_warnings(cloth_mir_optimizer_native_tests)
    add_dependencies(cloth_mir_optimizer_native_tests cloth_runtime)
    foreach(pipeline IN ITEMS raw optimized)
        foreach(target IN ITEMS x86_64 wasm32)
            set(optimizer_ir
                "${PROJECT_BINARY_DIR}/cloth-mir-${pipeline}-${target}.ll")
            add_test(NAME cloth_build_mir_${pipeline}_llvm_${target}
                COMMAND cloth_mir_optimizer_native_tests ${pipeline}
                    llvm-${target}
                    ${CLOTH_INTEGRATION_PROGRAMS}/MirOptimization.co
                    ${optimizer_ir})
            if(CLOTH_LLVM_OPT_EXECUTABLE)
                add_test(NAME cloth_mir_${pipeline}_verify_${target}
                    COMMAND ${CLOTH_LLVM_OPT_EXECUTABLE}
                        "-passes=verify,default<O2>,verify" -disable-output
                        ${optimizer_ir})
                set_tests_properties(cloth_mir_${pipeline}_verify_${target}
                    PROPERTIES DEPENDS
                        cloth_build_mir_${pipeline}_llvm_${target})
            endif()
        endforeach()
        foreach(program IN ITEMS success failure)
            if(program STREQUAL "success")
                set(optimizer_source
                    ${CLOTH_INTEGRATION_PROGRAMS}/MirOptimization.co)
            else()
                set(optimizer_source
                    ${CLOTH_INTEGRATION_PROGRAMS}/MirOptimizationFailure.co)
            endif()
            cloth_native_executable_path(optimizer_executable
                "cloth mir ${pipeline} ${program}")
            set(mir_${pipeline}_${program} ${optimizer_executable})
            add_test(NAME cloth_build_mir_${pipeline}_${program}
                COMMAND cloth_mir_optimizer_native_tests ${pipeline} native
                    ${optimizer_source} "${optimizer_executable}")
        endforeach()
    endforeach()
    add_test(NAME cloth_mir_baseline_success
        COMMAND ${CMAKE_COMMAND}
            "-DCLOTH_PROGRAM=${mir_raw_success}"
            "-DCLOTH_COMPARE_PROGRAM=${mir_optimized_success}"
            "-DCLOTH_EXPECTED=${CLOTH_INTEGRATION_EXPECTED}/mir_optimization.txt"
            -DCLOTH_EXACT_OUTPUT=ON
            -P ${CMAKE_CURRENT_SOURCE_DIR}/RunProgram.cmake)
    set_tests_properties(cloth_mir_baseline_success PROPERTIES
        DEPENDS "cloth_build_mir_raw_success;cloth_build_mir_optimized_success")
    add_test(NAME cloth_mir_baseline_failure
        COMMAND ${CMAKE_COMMAND}
            "-DCLOTH_PROGRAM=${mir_raw_failure}"
            "-DCLOTH_COMPARE_PROGRAM=${mir_optimized_failure}"
            "-DCLOTH_EXPECTED_ERROR=cloth error: DivisionByZero"
            -DCLOTH_EXACT_ERROR=ON
            -P ${CMAKE_CURRENT_SOURCE_DIR}/RunProgram.cmake)
    set_tests_properties(cloth_mir_baseline_failure PROPERTIES
        DEPENDS "cloth_build_mir_raw_failure;cloth_build_mir_optimized_failure")

    foreach(mode IN ITEMS stress exhaustive default default_only tables)
        cloth_native_executable_path(switch_executable "cloth switch ${mode}")
        add_test(NAME cloth_build_switch_${mode}
            COMMAND cloth_switch_native_tests
                ${CLOTH_INTEGRATION_PROJECTS}/switch/src/Main.co
                "${switch_executable}" ${mode})
        if(mode STREQUAL "tables")
            add_test(NAME cloth_switch_tables COMMAND "${switch_executable}")
            set_tests_properties(cloth_switch_tables PROPERTIES
                DEPENDS cloth_build_switch_tables)
            continue()
        endif()
        if(mode STREQUAL "stress")
            set(switch_expectation
                "-DCLOTH_EXPECTED=${CLOTH_INTEGRATION_EXPECTED}/switch.txt"
                -DCLOTH_EXACT_OUTPUT=ON)
        else()
            set(switch_expectation -DCLOTH_EXPECTED_ERROR= -DCLOTH_EXPECT_TRAP=ON)
        endif()
        add_test(NAME cloth_switch_${mode}
            COMMAND ${CMAKE_COMMAND}
                "-DCLOTH_PROGRAM=${switch_executable}" ${switch_expectation}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/RunProgram.cmake)
        set_tests_properties(cloth_switch_${mode} PROPERTIES
            DEPENDS cloth_build_switch_${mode})
    endforeach()
    add_executable(cloth_struct_native_tests struct_native_tests.cc)
    target_link_libraries(cloth_struct_native_tests PRIVATE cloth_compiler)
    target_compile_definitions(cloth_struct_native_tests PRIVATE ${native_definitions})
    cloth_enable_warnings(cloth_struct_native_tests)
    add_dependencies(cloth_struct_native_tests cloth_runtime)
    cloth_native_executable_path(struct_stress "cloth struct GC stress")
    add_test(NAME cloth_build_struct_gc_stress
        COMMAND cloth_struct_native_tests
            ${CLOTH_INTEGRATION_PROJECTS}/structs/src/Main.co
            "${struct_stress}")
    add_test(NAME cloth_struct_gc_stress
        COMMAND ${CMAKE_COMMAND}
            "-DCLOTH_PROGRAM=${struct_stress}"
            "-DCLOTH_EXPECTED=${CLOTH_INTEGRATION_EXPECTED}/structs.txt"
            -DCLOTH_EXACT_OUTPUT=ON
            -P ${PROJECT_SOURCE_DIR}/tests/integration/RunProgram.cmake)
    set_tests_properties(cloth_struct_gc_stress PROPERTIES
        DEPENDS cloth_build_struct_gc_stress)
    add_executable(cloth_program_argument_native_tests
        program_argument_native_tests.cc)
    target_link_libraries(cloth_program_argument_native_tests
        PRIVATE cloth_compiler)
    target_compile_definitions(cloth_program_argument_native_tests
        PRIVATE ${native_definitions})
    cloth_enable_warnings(cloth_program_argument_native_tests)
    add_dependencies(cloth_program_argument_native_tests cloth_runtime)
    cloth_native_executable_path(program_argument_gc_stress
        "cloth program argument GC stress")
    add_test(NAME cloth_build_program_argument_gc_stress
        COMMAND cloth_program_argument_native_tests
            ${CLOTH_INTEGRATION_PROGRAMS}/ProgramArguments.co
            "${program_argument_gc_stress}")
    add_test(NAME cloth_program_argument_gc_stress
        COMMAND "${program_argument_gc_stress}"
            "first" "two words" "" "--flag" "é🙂")
    set_tests_properties(cloth_program_argument_gc_stress PROPERTIES
        DEPENDS cloth_build_program_argument_gc_stress
        PASS_REGULAR_EXPRESSION
            "^5\n5\nfirst\n9\ntwo words\n0\n\n6\n--flag\n2\né🙂\n$")
    cloth_add_native_output_test(enums
        OUTPUT "cloth enums"
        SOURCE ${CLOTH_INTEGRATION_PROJECTS}/enums/src/Main.co
        SOURCE_ROOT ${CLOTH_INTEGRATION_PROJECTS}/enums/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/enums.txt
        EXACT
    )
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
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/VoidMain.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/void_main.txt
        EXACT
    )
    cloth_native_executable_path(program_arguments_executable
        "cloth program arguments")
    add_test(NAME cloth_cli_build_program_arguments
        COMMAND clothc "--build=${program_arguments_executable}"
            ${CLOTH_INTEGRATION_PROGRAMS}/ProgramArguments.co)
    add_test(NAME cloth_program_arguments
        COMMAND "${program_arguments_executable}"
            "first" "two words" "" "--flag" "é🙂")
    set_tests_properties(cloth_program_arguments PROPERTIES
        DEPENDS cloth_cli_build_program_arguments
        PASS_REGULAR_EXPRESSION
            "^5\n5\nfirst\n9\ntwo words\n0\n\n6\n--flag\n2\né🙂\n$")
    add_test(NAME cloth_program_arguments_empty
        COMMAND "${program_arguments_executable}")
    set_tests_properties(cloth_program_arguments_empty PROPERTIES
        DEPENDS cloth_cli_build_program_arguments
        PASS_REGULAR_EXPRESSION "^0\n$")
    add_test(NAME cloth_program_arguments_artifacts
        COMMAND ${CMAKE_COMMAND}
            "-DCLOTH_COMPILER=$<TARGET_FILE:clothc>"
            "-DCLOTH_SOURCE_ROOT=${CLOTH_INTEGRATION_PROJECTS}/program_arguments/src"
            "-DCLOTH_WORK_DIRECTORY=${PROJECT_BINARY_DIR}/program-argument-artifacts"
            "-DCLOTH_EXPECTED=${CLOTH_INTEGRATION_EXPECTED}/program_arguments.txt"
            "-DCLOTH_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/RunProgramArgumentsArtifacts.cmake)
    cloth_add_native_output_test(final_bindings
        OUTPUT "cloth final bindings"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/FinalBindings.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/final_bindings.txt
        EXACT
    )
    cloth_add_native_output_test(static_members
        OUTPUT "cloth static members"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/StaticMembers.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/static_members.txt
        EXACT
    )
    cloth_add_native_output_test(nullable_flow
        OUTPUT "cloth nullable flow"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NullableFlow.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/nullable_flow.txt
        EXACT
    )
    cloth_add_native_output_test(null_ergonomics
        OUTPUT "cloth null ergonomics"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NullErgonomics.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/null_ergonomics.txt
        EXACT
    )
    cloth_add_native_output_test(gc_stress
        OUTPUT "cloth gc stress"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/GcStress.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/gc_stress.txt
        EXACT
    )
    cloth_add_native_output_test(gc_collections
        OUTPUT "cloth gc collections"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/GcCollections.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/gc_collections.txt
        EXACT
    )
    cloth_add_native_output_test(string_values
        OUTPUT "cloth string values"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/StringValues.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/string_values.txt
        EXACT
    )
    cloth_add_native_output_test(object_model
        OUTPUT "cloth object model"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/ObjectModel.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/object_model.txt
        EXACT
    )
    cloth_add_native_output_test(inheritance_construction
        OUTPUT "cloth inheritance construction"
        SOURCE
            ${CLOTH_INTEGRATION_PROJECTS}/inheritance_construction/src/Derived.co
        SOURCE_ROOT
            ${CLOTH_INTEGRATION_PROJECTS}/inheritance_construction/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/inheritance_construction.txt
        EXACT
    )
    cloth_add_native_output_test(inheritance_behavior
        OUTPUT "cloth inheritance behavior"
        SOURCE ${CLOTH_INTEGRATION_PROJECTS}/inheritance_behavior/src/Derived.co
        SOURCE_ROOT ${CLOTH_INTEGRATION_PROJECTS}/inheritance_behavior/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/inheritance_behavior.txt
        EXACT
    )
    cloth_add_native_output_test(interfaces
        OUTPUT "cloth interfaces"
        SOURCE ${CLOTH_INTEGRATION_PROJECTS}/interfaces/src/Derived.co
        SOURCE_ROOT ${CLOTH_INTEGRATION_PROJECTS}/interfaces/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/interfaces.txt
        EXACT
    )
    cloth_add_native_output_test(constructor_visibility
        OUTPUT "cloth constructor visibility"
        SOURCE ${CLOTH_INTEGRATION_PROJECTS}/constructor_visibility/src/Main.co
        SOURCE_ROOT ${CLOTH_INTEGRATION_PROJECTS}/constructor_visibility/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/constructor_visibility.txt
        EXACT
    )
    cloth_add_native_failure_test(null_assert_failure
        OUTPUT "cloth null assert failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NullAssertFailure.co
        ERROR "non-null assertion failed"
    )
    cloth_add_native_output_test(fizzbuzz
        OUTPUT "cloth fizzbuzz"
        SOURCE ${PROJECT_SOURCE_DIR}/examples/FizzBuzz.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/fizzbuzz.txt
    )
    cloth_add_native_output_test(print_values
        OUTPUT "cloth print values"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/PrintValues.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/print_values.txt
    )
    cloth_add_native_output_test(print_representation
        OUTPUT "cloth print representation"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/PrintRepresentation.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/print_representation.txt
        EXACT
    )
    cloth_add_native_output_test(import_project
        OUTPUT "cloth import project"
        SOURCE ${CLOTH_INTEGRATION_PROJECTS}/imports/src/Main.co
        SOURCE_ROOT ${CLOTH_INTEGRATION_PROJECTS}/imports/src
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/import_project.txt
    )
    cloth_add_native_output_test(array_sum
        OUTPUT "cloth array sum"
        SOURCE ${PROJECT_SOURCE_DIR}/examples/ArraySum.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/array_sum.txt
    )
    cloth_add_native_failure_test(array_bounds
        OUTPUT "cloth array bounds"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/ArrayBounds.co
        ERROR "array index is out of bounds"
    )
    cloth_add_native_output_test(for_each
        OUTPUT "cloth for each"
        SOURCE ${PROJECT_SOURCE_DIR}/examples/ForEach.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/for_each.txt
    )
    cloth_add_native_output_test(for_contract
        OUTPUT "cloth for contract"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/ForContract.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/for_contract.txt
    )
    cloth_add_native_output_test(for_updates
        OUTPUT "cloth for updates"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/ForUpdates.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/for_updates.txt
        EXACT
    )
    cloth_add_native_output_test(numeric_widening
        OUTPUT "cloth numeric widening"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NumericWidening.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/numeric_widening.txt
        EXACT
    )
    cloth_add_native_output_test(numeric_conversions
        OUTPUT "cloth numeric conversions"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NumericConversions.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/numeric_conversions.txt
        EXACT
    )
    cloth_add_native_output_test(typed_numeric_literals
        OUTPUT "cloth typed numeric literals"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/TypedNumericLiterals.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/typed_numeric_literals.txt
        EXACT
    )
    cloth_add_native_output_test(numeric_literal_notation
        OUTPUT "cloth numeric literal notation"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NumericLiteralNotation.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/numeric_literal_notation.txt
        EXACT
    )
    cloth_add_native_output_test(integer_binary_data
        OUTPUT "cloth integer binary data"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerBinaryData.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/integer_binary_data.txt
        EXACT
    )
    cloth_add_native_output_test(checked_integer_arithmetic
        OUTPUT "cloth checked integer arithmetic"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/CheckedIntegerArithmetic.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/checked_integer_arithmetic.txt
        EXACT
    )
    cloth_add_native_output_test(checked_integer_updates
        OUTPUT "cloth checked integer updates"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/CheckedIntegerUpdates.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/checked_integer_updates.txt
        EXACT
    )
    cloth_add_native_output_test(integer_conversion_modes
        OUTPUT "cloth integer conversion modes"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerConversionModes.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/integer_conversion_modes.txt
        EXACT
    )
    cloth_add_native_output_test(typed_error_success
        OUTPUT "cloth typed error success"
        SOURCE ${CLOTH_TYPED_ERROR_PROJECT}/Main.co
        SOURCE_ROOT ${CLOTH_TYPED_ERROR_PROJECT}
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/typed_error_success.txt
        EXACT
    )
    cloth_add_native_failure_test(typed_error_propagation
        OUTPUT "cloth typed error propagation"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/TypedErrorPropagation.co
        ERROR "cloth error: TypedErrorPropagation: propagated"
        EXACT
    )
    cloth_add_native_failure_test(typed_error_construction
        OUTPUT "cloth typed error construction"
        SOURCE
            ${CLOTH_INTEGRATION_PROJECTS}/typed_error_construction/src/Main.co
        SOURCE_ROOT
            ${CLOTH_INTEGRATION_PROJECTS}/typed_error_construction/src
        ERROR "cloth error: Failure: constructor"
        EXACT
    )
    cloth_add_native_failure_test(integer_overflow_failure
        OUTPUT "cloth integer overflow failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerOverflowFailure.co
        ERROR "cloth runtime error: integer arithmetic overflow"
        EXACT
    )
    cloth_add_native_failure_test(integer_division_by_zero_failure
        OUTPUT "cloth integer division by zero failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerDivisionByZeroFailure.co
        ERROR "cloth error: DivisionByZero"
        EXACT
    )
    cloth_add_native_failure_test(integer_remainder_by_zero_failure
        OUTPUT "cloth integer remainder by zero failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerRemainderByZeroFailure.co
        ERROR "cloth error: DivisionByZero"
        EXACT
    )
    cloth_add_native_failure_test(integer_division_overflow_failure
        OUTPUT "cloth integer division overflow failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerDivisionOverflowFailure.co
        ERROR "cloth runtime error: integer arithmetic overflow"
        EXACT
    )
    cloth_add_native_failure_test(integer_remainder_overflow_failure
        OUTPUT "cloth integer remainder overflow failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerRemainderOverflowFailure.co
        ERROR "cloth runtime error: integer arithmetic overflow"
        EXACT
    )
    cloth_add_native_failure_test(integer_literal_expression_failure
        OUTPUT "cloth integer literal expression failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerLiteralExpressionFailure.co
        ERROR "cloth runtime error: integer arithmetic overflow"
        EXACT
    )
    cloth_add_native_failure_test(integer_update_overflow_failure
        OUTPUT "cloth integer update overflow failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerUpdateOverflowFailure.co
        ERROR "cloth runtime error: integer arithmetic overflow"
        EXACT
    )
    cloth_add_native_failure_test(integer_postfix_underflow_failure
        OUTPUT "cloth integer postfix underflow failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerPostfixUnderflowFailure.co
        ERROR "cloth runtime error: integer arithmetic overflow"
        EXACT
    )
    cloth_add_native_failure_test(integer_compound_overflow_failure
        OUTPUT "cloth integer compound overflow failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerCompoundOverflowFailure.co
        ERROR "cloth runtime error: integer arithmetic overflow"
        EXACT
    )
    cloth_add_native_failure_test(integer_compound_division_failure
        OUTPUT "cloth integer compound division failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerCompoundDivisionFailure.co
        ERROR "cloth error: DivisionByZero"
        EXACT
    )
    cloth_add_native_failure_test(integer_compound_remainder_failure
        OUTPUT "cloth integer compound remainder failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerCompoundRemainderFailure.co
        ERROR "cloth error: DivisionByZero"
        EXACT
    )
    cloth_add_native_failure_test(shift_count_failure
        OUTPUT "cloth shift count failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/ShiftCountFailure.co
        ERROR "shift count is out of range"
    )
    cloth_add_native_failure_test(integer_byte_range_failure
        OUTPUT "cloth integer byte range failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerByteRangeFailure.co
        ERROR "integer byte range is out of bounds"
    )
    cloth_add_native_failure_test(numeric_conversion_failure
        OUTPUT "cloth numeric conversion failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NumericConversionFailure.co
        ERROR "numeric conversion is out of range"
    )
    cloth_add_native_failure_test(numeric_signed_conversion_failure
        OUTPUT "cloth signed numeric conversion failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NumericSignedConversionFailure.co
        ERROR "numeric conversion is out of range"
    )
    cloth_add_native_failure_test(numeric_float_conversion_failure
        OUTPUT "cloth floating numeric conversion failure"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/NumericFloatConversionFailure.co
        ERROR "numeric conversion is out of range"
    )
endif()

cloth_add_cli_failure_test(lexical_error
    ${CLOTH_INTEGRATION_ERRORS}/LexicalErrors.co)
cloth_add_cli_failure_test(parser_error
    ${CLOTH_INTEGRATION_ERRORS}/ParserErrors.co)
cloth_add_cli_failure_test(sema_error
    ${CLOTH_INTEGRATION_ERRORS}/SemanticErrors.co)
cloth_add_cli_failure_test(void_error
    ${CLOTH_INTEGRATION_ERRORS}/VoidErrors.co)
cloth_add_cli_failure_test(final_error
    ${CLOTH_INTEGRATION_ERRORS}/FinalErrors.co)
cloth_add_cli_failure_test(nullable_error
    ${CLOTH_INTEGRATION_ERRORS}/NullableErrors.co)
cloth_add_cli_failure_test(object_model_error
    ${CLOTH_INTEGRATION_ERRORS}/ObjectModelErrors.co)
cloth_add_cli_failure_test(inheritance_error
    ${CLOTH_INTEGRATION_ERRORS}/InheritanceErrors.co)

cloth_finalize_tests(LABEL integration)

# This test compiles five package artifacts, links two native objects, and runs
# the result. Allow sanitizer instrumentation headroom without relaxing the
# timeout for ordinary integration tests.
math(EXPR standard_library_test_timeout
    "4 * ${CLOTH_TEST_TIMEOUT_SECONDS}")
set_tests_properties(cloth_standard_library_artifacts PROPERTIES
    TIMEOUT ${standard_library_test_timeout})

if(CLOTH_TEST_SHUTTLE)
    find_program(CLOTH_CARGO_EXECUTABLE NAMES cargo REQUIRED)
    if(NOT EXISTS "${CLOTH_SHUTTLE_SOURCE_DIR}/tests/toolchain_tests.rs")
        message(FATAL_ERROR
            "CLOTH_TEST_SHUTTLE requires the Stage 22 Shuttle checkout at "
            "CLOTH_SHUTTLE_SOURCE_DIR; initialize the shuttle submodule")
    endif()
    set(shuttle_suites toolchain_tests)
    if(CLOTH_LLVM_LLC_EXECUTABLE)
        list(APPEND shuttle_suites native_tests)
    endif()
    foreach(suite IN LISTS shuttle_suites)
        add_test(NAME cloth_shuttle_${suite}
            COMMAND ${CMAKE_COMMAND} -E env
                "CLOTHC_UNDER_TEST=$<TARGET_FILE:clothc>"
                ${CLOTH_CARGO_EXECUTABLE} test --locked
                --test ${suite} -- --ignored
        )
        set_tests_properties(cloth_shuttle_${suite} PROPERTIES
            WORKING_DIRECTORY "${CLOTH_SHUTTLE_SOURCE_DIR}"
            LABELS "integration;toolchain"
            TIMEOUT 1200
            RESOURCE_LOCK shuttle_cargo
        )
        if(CLOTH_ENABLE_SANITIZERS AND WIN32 AND
           CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set_tests_properties(cloth_shuttle_${suite} PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${CLOTH_CLANG_RUNTIME_DIRECTORY}"
            )
        endif()
    endforeach()
endif()
