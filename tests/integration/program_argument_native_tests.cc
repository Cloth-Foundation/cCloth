// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_verifier.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/backend/native_toolchain.h"
#include "cloth/compiler/compilation.h"
#include "cloth/mir/mir_verifier.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

#if defined(_WIN32)
constexpr bool kUseWideNativeArguments = true;
#else
constexpr bool kUseWideNativeArguments = false;
#endif

std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string text;
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    text += diagnostic.message + '\n';
  }
  return text;
}

const cloth::AbiCallable* find_entry(const cloth::CompilationResult& result) {
  for (const cloth::AbiFileClass& file : result.abi.files) {
    for (const cloth::AbiCallable& function : file.functions) {
      if (result.semantics.symbol(function.symbol).name == "Main") {
        return &function;
      }
    }
  }
  return nullptr;
}

bool instrument_entry_collections(cloth::LlvmIrModule& module,
                                  std::string_view entry_name) {
  const std::string marker = "@" + std::string{entry_name} + "(";
  const std::size_t name = module.text.find(marker);
  if (name == std::string::npos) return false;
  const std::size_t body = module.text.find(" {\n", name);
  if (body == std::string::npos) return false;
  const std::size_t end = module.text.find("\n}", body);
  if (end == std::string::npos) return false;

  constexpr std::string_view kCollect = "  call void @cloth_rt_gc_collect()\n";
  const std::size_t entry_label = module.text.find(":\n", body + 3);
  if (entry_label == std::string::npos || entry_label > end) return false;
  module.text.insert(entry_label + 2, kCollect);

  std::size_t updated_end = end + kCollect.size();
  std::size_t element = entry_label + 2 + kCollect.size();
  while ((element = module.text.find("call ptr @cloth_rt_array_element(",
                                     element)) != std::string::npos &&
         element < updated_end) {
    const std::size_t line = module.text.rfind('\n', element);
    if (line == std::string::npos) return false;
    module.text.insert(line + 1, kCollect);
    updated_end += kCollect.size();
    element += kCollect.size() + 1;
  }

  const std::size_t declaration = module.text.find("declare ");
  if (declaration == std::string::npos) return false;
  module.text.insert(declaration, "declare void @cloth_rt_gc_collect()\n");
  return true;
}

int build_native_stress(const char* source_path, const char* output_path) {
#if defined(CLOTH_DEFAULT_LLC)
  cloth::Compilation compilation;
  compilation.set_source_root(std::filesystem::path{source_path}.parent_path());
  auto source = cloth::SourceFile::load(source_path);
  if (!source) return 2;
  compilation.add_source(std::move(*source));

  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  if (!result.is_valid ||
      !cloth::verify_mir(result.mir, result.semantics, diagnostics) ||
      !cloth::verify_abi(result.abi, result.mir, result.semantics,
                         diagnostics)) {
    std::cerr << messages(diagnostics);
    return 1;
  }
  const cloth::AbiCallable* entry = find_entry(result);
  if (entry == nullptr) {
    std::cerr << "program-argument entry fixture is missing Main\n";
    return 1;
  }

  auto llvm =
      cloth::emit_llvm_ir(result.mir, result.abi, result.semantics, diagnostics,
                          cloth::LlvmIrOptions{true, std::nullopt, std::nullopt,
                                               kUseWideNativeArguments});
  if (!llvm || !instrument_entry_collections(*llvm, entry->mangled_name)) {
    std::cerr << "could not instrument the program-argument entry\n"
              << messages(diagnostics);
    return 1;
  }

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
  static_cast<void>(source_path);
  static_cast<void>(output_path);
  return 2;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  return build_native_stress(argv[1], argv[2]);
}
