add_library(cloth_compiler
    abi/abi.cc
    abi/abi_printer.cc
    abi/abi_verifier.cc
    artifact/imported_package.cc
    artifact/package_artifact.cc
    artifact/sha256.cc
    ast/ast.cc
    ast/ast_printer.cc
    backend/llvm_ir.cc
    backend/native_toolchain.cc
    compiler/compilation.cc
    compiler/shuttle_protocol.cc
    diagnostics/diagnostic_engine.cc
    flow/control_flow.cc
    hir/hir.cc
    hir/hir_printer.cc
    hir/hir_verifier.cc
    identity/canonical_identity.cc
    identity/package_identity.cc
    lexer/lexer.cc
    lexer/literal.cc
    lexer/token.cc
    mir/mir.cc
    mir/mir_printer.cc
    mir/mir_verifier.cc
    parser/declaration_pass.cc
    parser/definition_pass.cc
    parser/expression_parser.cc
    parser/parser.cc
    sema/canonical_identity.cc
    sema/file_class_symbols.cc
    sema/field_initialization_analysis.cc
    sema/numeric_types.cc
    sema/semantic_analyzer.cc
    sema/semantic_model.cc
    sema/visibility.cc
    source/source_file.cc
    target/data_layout.cc
)
target_compile_features(cloth_compiler PUBLIC cxx_std_23)
target_include_directories(cloth_compiler
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
cloth_enable_warnings(cloth_compiler)

add_executable(clothc main.cc)
target_link_libraries(clothc PRIVATE cloth_compiler)
cloth_enable_warnings(clothc)
add_dependencies(clothc cloth_runtime)
if(WIN32 AND NOT MSVC AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_link_options(clothc PRIVATE -municode)
endif()

file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" CLOTH_NATIVE_LINKER_PATH)
if(WIN32)
    if(MSVC)
        set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-pc-windows-msvc")
        target_compile_definitions(clothc PRIVATE CLOTH_NATIVE_LINKER_MSVC=1)
    elseif(CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-pc-windows-msvc")
    else()
        set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-w64-windows-gnu")
        target_compile_definitions(clothc PRIVATE CLOTH_NATIVE_STATIC_RUNTIME=1)
    endif()
elseif(APPLE)
    set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-apple-macosx")
else()
    set(CLOTH_NATIVE_TARGET_TRIPLE "x86_64-unknown-linux-gnu")
endif()

target_compile_definitions(clothc PRIVATE
    CLOTH_DEFAULT_NATIVE_LINKER="${CLOTH_NATIVE_LINKER_PATH}"
    CLOTH_DEFAULT_NATIVE_TARGET="${CLOTH_NATIVE_TARGET_TRIPLE}"
    CLOTH_DEFAULT_RUNTIME_LIBRARY="$<TARGET_FILE:cloth_runtime>"
)
if(CLOTH_LLVM_LLC_EXECUTABLE)
    file(TO_CMAKE_PATH "${CLOTH_LLVM_LLC_EXECUTABLE}" CLOTH_LLVM_LLC_PATH)
    target_compile_definitions(clothc PRIVATE
        CLOTH_DEFAULT_LLC="${CLOTH_LLVM_LLC_PATH}"
    )
endif()
