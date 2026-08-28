include_guard(GLOBAL)

function(cloth_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
        )
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

    if(CLOTH_ENABLE_SANITIZERS AND
       NOT "${target}" STREQUAL "cloth_runtime")
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            target_link_options(${target} PRIVATE /fsanitize=address)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
            )
            target_link_options(${target} PRIVATE
                -fsanitize=address,undefined
            )
        else()
            message(FATAL_ERROR
                "CLOTH_ENABLE_SANITIZERS is unsupported by "
                "${CMAKE_CXX_COMPILER_ID}"
            )
        endif()
    endif()

    if(CLOTH_ENABLE_SANITIZERS AND WIN32 AND
       CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
       NOT "${target}" STREQUAL "cloth_runtime")
        target_compile_definitions(${target} PRIVATE
            _DISABLE_STRING_ANNOTATION=1
            _DISABLE_VECTOR_ANNOTATION=1
        )
    endif()

    if(CLOTH_BUILD_FUZZERS AND
       NOT "${target}" STREQUAL "cloth_runtime")
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
    endif()

    if(CLOTH_ENABLE_COVERAGE AND
       NOT "${target}" STREQUAL "cloth_runtime")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE --coverage -O0 -g)
            target_link_options(${target} PRIVATE --coverage)
        else()
            message(FATAL_ERROR
                "CLOTH_ENABLE_COVERAGE requires Clang or GNU C++"
            )
        endif()
    endif()
endfunction()
