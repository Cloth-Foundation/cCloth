set(CLOTH_INTEGRATION_PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/programs")
set(CLOTH_INTEGRATION_EXPECTED "${CMAKE_CURRENT_SOURCE_DIR}/expected")
set(CLOTH_INTEGRATION_ERRORS "${CMAKE_CURRENT_SOURCE_DIR}/errors")
set(CLOTH_INTEGRATION_PROJECTS "${CMAKE_CURRENT_SOURCE_DIR}/projects")

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
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/VoidMain.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/void_main.txt
        EXACT
    )
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
    cloth_add_native_output_test(integer_binary_data
        OUTPUT "cloth integer binary data"
        SOURCE ${CLOTH_INTEGRATION_PROGRAMS}/IntegerBinaryData.co
        EXPECTED ${CLOTH_INTEGRATION_EXPECTED}/integer_binary_data.txt
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
            TIMEOUT 120
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
