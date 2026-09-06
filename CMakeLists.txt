# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.25)

project(Cloth VERSION 0.3.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(ClothOptions)
include(ClothCompilerOptions)
include(ClothTooling)
include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CLOTH_STANDARD_LIBRARY_MANIFEST_SOURCE
    "${PROJECT_SOURCE_DIR}/std/Shuttle.toml")
if(NOT EXISTS "${CLOTH_STANDARD_LIBRARY_MANIFEST_SOURCE}")
    message(FATAL_ERROR
        "The Cloth standard-library submodule is unavailable at "
        "${CLOTH_STANDARD_LIBRARY_MANIFEST_SOURCE}")
endif()
file(RELATIVE_PATH CLOTH_STANDARD_LIBRARY_MANIFEST
    "${PROJECT_BINARY_DIR}" "${CLOTH_STANDARD_LIBRARY_MANIFEST_SOURCE}")
file(TO_CMAKE_PATH "${CLOTH_STANDARD_LIBRARY_MANIFEST}"
    CLOTH_STANDARD_LIBRARY_MANIFEST)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/cloth-toolchain.json.in"
    "${PROJECT_BINARY_DIR}/cloth-toolchain.json"
    @ONLY)

# Keep artifacts in the top-level build directory as the project grows into
# directory-owned targets.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

add_subdirectory(runtime)
add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
