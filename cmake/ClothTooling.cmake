include_guard(GLOBAL)

find_program(CLOTH_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
find_program(CLOTH_LLVM_OPT_EXECUTABLE NAMES opt)
find_program(CLOTH_LLVM_LLC_EXECUTABLE NAMES llc)

if(CLOTH_CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE CLOTH_FORMAT_SOURCES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/include/cloth/*.h
        ${PROJECT_SOURCE_DIR}/runtime/*.cc
        ${PROJECT_SOURCE_DIR}/src/*.cc
        ${PROJECT_SOURCE_DIR}/tests/*.cc
        ${PROJECT_SOURCE_DIR}/tests/*.h
    )
    add_custom_target(format
        COMMAND "${CLOTH_CLANG_FORMAT_EXECUTABLE}" -i --style=file
                ${CLOTH_FORMAT_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Formatting Cloth C++ sources"
    )
    add_custom_target(check_format
        COMMAND "${CLOTH_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror
                --style=file ${CLOTH_FORMAT_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Checking Cloth C++ formatting"
    )
endif()
