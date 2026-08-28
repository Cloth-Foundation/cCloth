add_library(cloth_runtime STATIC runtime.cc)
target_compile_features(cloth_runtime PUBLIC cxx_std_23)
target_include_directories(cloth_runtime
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
cloth_enable_warnings(cloth_runtime)
