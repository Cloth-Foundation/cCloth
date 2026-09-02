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
#include <sstream>
#include <string>
#include <utility>

namespace {
std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string text;
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    text += diagnostic.message + '\n';
  }
  return text;
}

// Exercise simultaneous aggregate/scalar phi copies on a loop back-edge. The
// source fixture has identical swap semantics; this also covers MIR forms that
// the current local-variable lowering does not yet generate itself.
bool install_phi_loop(cloth::CompilationResult& result) {
  for (auto& file : result.mir.files) {
    if (result.semantics.file(file.file).identity.name != "Phi") continue;
    for (auto& function : file.functions) {
      if (result.semantics.symbol(function.symbol).name != "Swap") continue;
      const auto& symbol = result.semantics.symbol(function.symbol);
      const auto range = symbol.range;
      const auto packet = symbol.parameter_types[0];
      const auto integer = symbol.parameter_types[2];
      const auto boolean = *result.semantics.find_type("bool");
      const auto string = *result.semantics.find_type("string");
      const auto instruction = [&](std::size_t id, cloth::TypeId type,
                                   cloth::MirInstructionData data) {
        return cloth::MirInstruction{cloth::MirValueId{id}, type, range,
                                     std::move(data)};
      };
      using cloth::MirBlockId;
      using cloth::MirValueId;
      function.body = cloth::MirBody{
          range,
          MirBlockId{0},
          {
              {true,
               {
                   instruction(
                       0, packet,
                       cloth::MirLoadSymbolInstruction{function.parameters[0]}),
                   instruction(
                       1, packet,
                       cloth::MirLoadSymbolInstruction{function.parameters[1]}),
                   instruction(
                       2, integer,
                       cloth::MirLoadSymbolInstruction{function.parameters[2]}),
                   instruction(3, integer,
                               cloth::MirLiteralInstruction{
                                   cloth::LiteralKind::kInteger, "0"}),
                   instruction(4, integer,
                               cloth::MirLiteralInstruction{
                                   cloth::LiteralKind::kInteger, "1"}),
               },
               {range, cloth::MirJumpTerminator{MirBlockId{1}}}},
              {true,
               {
                   instruction(5, packet,
                               cloth::MirPhiInstruction{
                                   {{MirBlockId{0}, MirValueId{0}},
                                    {MirBlockId{2}, MirValueId{6}}}}),
                   instruction(6, packet,
                               cloth::MirPhiInstruction{
                                   {{MirBlockId{0}, MirValueId{1}},
                                    {MirBlockId{2}, MirValueId{5}}}}),
                   instruction(7, integer,
                               cloth::MirPhiInstruction{
                                   {{MirBlockId{0}, MirValueId{3}},
                                    {MirBlockId{2}, MirValueId{9}}}}),
                   instruction(8, boolean,
                               cloth::MirBinaryInstruction{
                                   MirValueId{7}, cloth::TokenKind::kLess,
                                   MirValueId{2}}),
               },
               {range, cloth::MirBranchTerminator{MirValueId{8}, MirBlockId{2},
                                                  MirBlockId{3}}}},
              {true,
               {
                   instruction(10, string,
                               cloth::MirLiteralInstruction{
                                   cloth::LiteralKind::kString, R"("cycle")"}),
                   instruction(9, integer,
                               cloth::MirBinaryInstruction{
                                   MirValueId{7}, cloth::TokenKind::kPlus,
                                   MirValueId{4}}),
               },
               {range, cloth::MirJumpTerminator{MirBlockId{1}}}},
              {true, {}, {range, cloth::MirReturnTerminator{MirValueId{5}}}},
          },
          11};
      return true;
    }
  }
  return false;
}

// Force a collection at every generated allocation boundary. This is test-only
// instrumentation: neither Cloth syntax nor the runtime's production policy
// exposes a stress switch.
int build_native_stress(const char* source_path, const char* output_path) {
#if defined(CLOTH_DEFAULT_LLC)
  cloth::Compilation compilation;
  compilation.set_source_root(std::filesystem::path{source_path}.parent_path());
  auto source = cloth::SourceFile::load(source_path);
  if (!source) return 2;
  compilation.add_source(std::move(*source));
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  if (!result.is_valid) {
    std::cerr << messages(diagnostics);
    return 1;
  }
  if (!install_phi_loop(result) ||
      !cloth::verify_mir(result.mir, result.semantics, diagnostics) ||
      !cloth::verify_abi(result.abi, result.mir, result.semantics,
                         diagnostics)) {
    std::cerr << "aggregate phi fixture verification failed\n"
              << messages(diagnostics);
    return 1;
  }
  auto llvm =
      cloth::emit_llvm_ir(result.mir, result.abi, result.semantics, diagnostics,
                          cloth::LlvmIrOptions{true, "Main", std::nullopt});
  if (!llvm) {
    std::cerr << messages(diagnostics);
    return 1;
  }
  llvm->text.insert(llvm->text.find("declare"),
                    "declare void @cloth_rt_gc_collect()\n");
  std::string instrumented;
  std::istringstream lines{llvm->text};
  for (std::string line; std::getline(lines, line);) {
    if (line.contains("call ptr @cloth_rt_alloc(") ||
        line.contains("call ptr @cloth_rt_array_alloc(") ||
        line.contains("call ptr @cloth_rt_string_literal(") ||
        line.contains("call ptr @cloth_rt_string_concat(") ||
        line.contains("call ptr @cloth_rt_object_type_name(")) {
      instrumented += "  call void @cloth_rt_gc_collect()\n";
    }
    instrumented += line + '\n';
  }
  llvm->text = std::move(instrumented);
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
