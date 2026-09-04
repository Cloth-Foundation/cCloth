// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi.h"
#include "cloth/abi/abi_verifier.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/backend/native_toolchain.h"
#include "cloth/compiler/compilation.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

namespace {

struct LoweredProgram {
  cloth::SemanticModel semantics;
  cloth::MirModule mir;
  cloth::AbiModule abi;
};

void print_errors(const cloth::DiagnosticEngine& diagnostics) {
  for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    std::cerr << diagnostic.message << '\n';
  }
}

std::optional<LoweredProgram> lower(cloth::Compilation& compilation,
                                    bool optimized,
                                    const cloth::TargetDataLayout& target,
                                    cloth::DiagnosticEngine& diagnostics) {
  if (optimized) {
    cloth::CompilationResult result = compilation.analyze(diagnostics);
    if (!result.is_valid) {
      return std::nullopt;
    }
    return LoweredProgram{std::move(result.semantics), std::move(result.mir),
                          std::move(result.abi)};
  }

  cloth::FrontendResult frontend = compilation.analyze_frontend(diagnostics);
  if (!frontend.is_valid) {
    return std::nullopt;
  }
  cloth::MirModule mir = cloth::lower_to_mir(frontend.hir, frontend.semantics);
  if (!cloth::verify_mir(mir, frontend.semantics, diagnostics)) {
    return std::nullopt;
  }
  auto abi = cloth::lower_to_abi(mir, frontend.semantics, target, diagnostics);
  if (!abi || !cloth::verify_abi(*abi, mir, frontend.semantics, diagnostics)) {
    return std::nullopt;
  }
  return LoweredProgram{std::move(frontend.semantics), std::move(mir),
                        std::move(*abi)};
}

int build(std::string_view pipeline, std::string_view target_name,
          const char* source_path, const char* output_path) {
  const bool optimized = pipeline == "optimized";
  if (!optimized && pipeline != "raw") {
    return 2;
  }
  const bool native = target_name == "native";
  std::optional<cloth::TargetDataLayout> target;
  if (native || target_name == "llvm-x86_64") {
    target = cloth::TargetDataLayout::llvm_x86_64();
  } else if (target_name == "llvm-wasm32") {
    target = cloth::TargetDataLayout::llvm_wasm32();
  } else {
    return 2;
  }

  auto source = cloth::SourceFile::load(source_path);
  if (!source) {
    std::cerr << "unable to read optimizer fixture\n";
    return 2;
  }
  cloth::Compilation compilation{*target};
  compilation.add_source(std::move(*source));
  cloth::DiagnosticEngine diagnostics;
  auto lowered = lower(compilation, optimized, *target, diagnostics);
  if (!lowered) {
    print_errors(diagnostics);
    return 1;
  }
  auto llvm = cloth::emit_llvm_ir(
      lowered->mir, lowered->abi, lowered->semantics, diagnostics,
      cloth::LlvmIrOptions{true,
                           std::filesystem::path{source_path}.stem().string(),
                           std::nullopt});
  if (!llvm) {
    print_errors(diagnostics);
    return 1;
  }
  if (!native) {
    std::ofstream output{output_path, std::ios::binary};
    output << llvm->text;
    return output ? 0 : 1;
  }

#if defined(CLOTH_DEFAULT_LLC)
  cloth::NativeToolchain toolchain{
      CLOTH_DEFAULT_LLC, CLOTH_DEFAULT_NATIVE_LINKER,
      CLOTH_DEFAULT_RUNTIME_LIBRARY, CLOTH_DEFAULT_NATIVE_TARGET};
#if defined(CLOTH_NATIVE_LINKER_MSVC)
  toolchain.linker_flavor = cloth::NativeLinkerFlavor::kMsvc;
#endif
#if defined(CLOTH_NATIVE_STATIC_RUNTIME)
  toolchain.link_static_runtime = true;
#endif
  const auto built =
      cloth::build_native_executable(*llvm, output_path, toolchain);
  if (!built) {
    std::cerr << built.error().message << '\n';
    return 1;
  }
  return 0;
#else
  static_cast<void>(output_path);
  return 2;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    return 2;
  }
  return build(argv[1], argv[2], argv[3], argv[4]);
}
