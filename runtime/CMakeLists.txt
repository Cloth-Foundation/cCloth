# Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.txt in the project root for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

add_library(cloth_runtime STATIC runtime.cc)
target_compile_features(cloth_runtime PUBLIC cxx_std_23)
target_include_directories(cloth_runtime
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
cloth_enable_warnings(cloth_runtime)
