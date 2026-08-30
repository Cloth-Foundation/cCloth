include_guard(GLOBAL)

include(CMakeParseArguments)

function(cloth_add_unit_test target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE cloth_compiler)
    target_include_directories(${target} PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/support
    )
    cloth_enable_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

function(cloth_native_executable_path output_variable output_name)
    set(${output_variable}
        "${PROJECT_BINARY_DIR}/${output_name}${CMAKE_EXECUTABLE_SUFFIX}"
        PARENT_SCOPE
    )
endfunction()

function(cloth_add_native_output_test name)
    cmake_parse_arguments(ARG "EXACT" "OUTPUT;SOURCE;SOURCE_ROOT;EXPECTED" "" ${ARGN})
    if(NOT ARG_OUTPUT OR NOT ARG_SOURCE OR NOT ARG_EXPECTED)
        message(FATAL_ERROR
            "cloth_add_native_output_test requires OUTPUT, SOURCE, and EXPECTED"
        )
    endif()

    cloth_native_executable_path(executable "${ARG_OUTPUT}")
    set(compile_command clothc "--build=${executable}")
    if(ARG_SOURCE_ROOT)
        list(APPEND compile_command "--source-root=${ARG_SOURCE_ROOT}")
    endif()
    list(APPEND compile_command "${ARG_SOURCE}")
    add_test(
        NAME cloth_cli_build_${name}
        COMMAND ${compile_command}
    )

    set(check_command
        ${CMAKE_COMMAND}
        "-DCLOTH_PROGRAM=${executable}"
        "-DCLOTH_EXPECTED=${ARG_EXPECTED}"
    )
    if(ARG_EXACT)
        list(APPEND check_command -DCLOTH_EXACT_OUTPUT=ON)
    endif()
    list(APPEND check_command
        -P ${PROJECT_SOURCE_DIR}/tests/integration/RunProgram.cmake)
    add_test(NAME cloth_${name} COMMAND ${check_command})
    set_tests_properties(cloth_${name} PROPERTIES
        DEPENDS cloth_cli_build_${name}
    )
endfunction()

function(cloth_add_native_failure_test name)
    cmake_parse_arguments(ARG "" "OUTPUT;SOURCE;ERROR" "" ${ARGN})
    if(NOT ARG_OUTPUT OR NOT ARG_SOURCE OR NOT ARG_ERROR)
        message(FATAL_ERROR
            "cloth_add_native_failure_test requires OUTPUT, SOURCE, and ERROR"
        )
    endif()

    cloth_native_executable_path(executable "${ARG_OUTPUT}")
    add_test(
        NAME cloth_cli_build_${name}
        COMMAND clothc "--build=${executable}" "${ARG_SOURCE}"
    )
    add_test(
        NAME cloth_${name}
        COMMAND ${CMAKE_COMMAND}
                "-DCLOTH_PROGRAM=${executable}"
                "-DCLOTH_EXPECTED_ERROR=${ARG_ERROR}"
                -P ${PROJECT_SOURCE_DIR}/tests/integration/RunProgram.cmake
    )
    set_tests_properties(cloth_${name} PROPERTIES
        DEPENDS cloth_cli_build_${name}
    )
endfunction()

function(cloth_add_cli_failure_test name source)
    add_test(NAME cloth_cli_${name} COMMAND clothc "${source}")
    set_tests_properties(cloth_cli_${name} PROPERTIES WILL_FAIL TRUE)
endfunction()

function(cloth_finalize_tests)
    cmake_parse_arguments(ARG "" "LABEL" "" ${ARGN})
    get_property(registered_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${registered_tests} PROPERTIES
        TIMEOUT ${CLOTH_TEST_TIMEOUT_SECONDS}
    )
    if(ARG_LABEL)
        set_tests_properties(${registered_tests} PROPERTIES LABELS ${ARG_LABEL})
    endif()
    if(CLOTH_ENABLE_SANITIZERS AND WIN32 AND
       CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_tests_properties(${registered_tests} PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${CLOTH_CLANG_RUNTIME_DIRECTORY}"
        )
    endif()
endfunction()
