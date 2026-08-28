include_guard(GLOBAL)

option(CLOTH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CLOTH_ENABLE_SANITIZERS
       "Enable supported address and undefined-behavior sanitizers" OFF)
option(CLOTH_ENABLE_COVERAGE "Enable compiler coverage instrumentation" OFF)
option(CLOTH_BUILD_FUZZERS "Build opt-in lexer/parser fuzz targets" OFF)
set(CLOTH_TEST_TIMEOUT_SECONDS 15 CACHE STRING
    "Maximum runtime in seconds for each CTest test")

if(NOT CLOTH_TEST_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "CLOTH_TEST_TIMEOUT_SECONDS must be a positive integer")
endif()

if(CLOTH_ENABLE_SANITIZERS AND CLOTH_ENABLE_COVERAGE)
    message(FATAL_ERROR
        "CLOTH_ENABLE_SANITIZERS and CLOTH_ENABLE_COVERAGE cannot be combined"
    )
endif()

if(CLOTH_BUILD_FUZZERS AND
   (NOT CLOTH_ENABLE_SANITIZERS OR MSVC OR
    NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    message(FATAL_ERROR
        "CLOTH_BUILD_FUZZERS requires Clang with "
        "CLOTH_ENABLE_SANITIZERS=ON"
    )
endif()

if(CLOTH_ENABLE_SANITIZERS AND WIN32 AND
   CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
        RESULT_VARIABLE CLOTH_CLANG_RESOURCE_RESULT
        OUTPUT_VARIABLE CLOTH_CLANG_RESOURCE_DIRECTORY
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT CLOTH_CLANG_RESOURCE_RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to locate the Clang runtime directory")
    endif()
    file(TO_CMAKE_PATH "${CLOTH_CLANG_RESOURCE_DIRECTORY}/lib/windows"
         CLOTH_CLANG_RUNTIME_DIRECTORY)
endif()

if(CLOTH_ENABLE_SANITIZERS OR CLOTH_ENABLE_COVERAGE)
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)

    if(CLOTH_ENABLE_SANITIZERS)
        if(MSVC)
            set(CMAKE_REQUIRED_FLAGS "/fsanitize=address")
            set(CMAKE_REQUIRED_LINK_OPTIONS "/fsanitize=address")
        else()
            set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
            set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
        endif()
        check_cxx_source_compiles(
            "int main() { return 0; }"
            CLOTH_SANITIZER_RUNTIME_AVAILABLE
        )
        if(NOT CLOTH_SANITIZER_RUNTIME_AVAILABLE)
            message(FATAL_ERROR
                "The selected compiler does not provide the requested "
                "sanitizer runtimes"
            )
        endif()
    else()
        set(CMAKE_REQUIRED_FLAGS "--coverage")
        set(CMAKE_REQUIRED_LINK_OPTIONS "--coverage")
        check_cxx_source_compiles(
            "int main() { return 0; }"
            CLOTH_COVERAGE_RUNTIME_AVAILABLE
        )
        if(NOT CLOTH_COVERAGE_RUNTIME_AVAILABLE)
            message(FATAL_ERROR
                "The selected compiler does not provide coverage "
                "instrumentation"
            )
        endif()
    endif()

    cmake_pop_check_state()
endif()
