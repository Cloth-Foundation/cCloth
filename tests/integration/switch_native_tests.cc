// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_verifier.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/backend/native_toolchain.h"
#include "cloth/compiler/compilation.h"
#include "cloth/mir/mir_verifier.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string table_source() {
  std::string source =
      "static func Dense(int8 value): int32 { switch (value) {";
  for (int value = -128; value < 128; ++value) {
    source += "case " + std::to_string(value) + ": { return " +
              std::to_string(value + 128) + "; }";
  }
  // Even a fully covered integer keeps its conservative no-match path.
  source +=
      "} return -1; } static func Sparse(uint64 value): int32 { switch (value) "
      "{";
  for (std::uint64_t index = 0; index < 256; ++index) {
    source += "case " + std::to_string(index << 48) + ": { return " +
              std::to_string(index) + "; }";
  }
  source += R"(default: { return -1; } } }
    static func Main(): int32 {
      for (int32 i = -128; i < 128; i++) {
        if (Dense(int8(i)) != i + 128) { return 1; }
      }
      for (int32 i = 0; i < 256; i++) {
        uint64 key = uint64(i) << 48;
        if (Sparse(key) != i) { return 2; }
        if (Sparse(key + 1) != -1) { return 3; }
      }
      return 0;
    })";
  return source;
}

void print_errors(const cloth::DiagnosticEngine& diagnostics) {
  for (const auto& diagnostic : diagnostics.diagnostics())
    std::cerr << diagnostic.message << '\n';
}

bool install_phis(cloth::CompilationResult& result) {
  using cloth::MirBlockId;
  using cloth::MirValueId;
  for (auto& file : result.mir.files) {
    if (result.semantics.file(file.file).identity.name != "Main") continue;
    std::optional<cloth::SymbolId> churn;
    for (const auto& function : file.functions)
      if (result.semantics.symbol(function.symbol).name == "Churn")
        churn = function.symbol;
    if (!churn) return false;
    for (auto& function : file.functions) {
      if (result.semantics.symbol(function.symbol).name != "Phi") continue;
      const auto& symbol = result.semantics.symbol(function.symbol);
      const auto range = symbol.range;
      const auto data = symbol.parameter_types[0];
      const auto integer = symbol.parameter_types[2];
      const auto string = *result.semantics.find_type("string");
      const auto instruction = [&](std::size_t id, cloth::TypeId type,
                                   cloth::MirInstructionData value) {
        return cloth::MirInstruction{MirValueId{id}, type, range,
                                     std::move(value)};
      };
      // A label group and a shared case/default destination must each present
      // exactly one predecessor to both scalar and GC-bearing aggregate phis.
      function.body = cloth::MirBody{
          range,
          {0},
          {
              {true,
               {instruction(
                    0, data,
                    cloth::MirLoadSymbolInstruction{function.parameters[0]}),
                instruction(
                    1, data,
                    cloth::MirLoadSymbolInstruction{function.parameters[1]}),
                instruction(
                    2, integer,
                    cloth::MirLoadSymbolInstruction{function.parameters[2]}),
                instruction(3, integer,
                            cloth::MirLiteralInstruction{
                                cloth::LiteralKind::kInteger, "0"}),
                instruction(4, integer,
                            cloth::MirLiteralInstruction{
                                cloth::LiteralKind::kInteger, "1"})},
               {range, cloth::MirSwitchTerminator{{2},
                                                  integer,
                                                  {{{integer, 0}, {1}},
                                                   {{integer, 1}, {1}},
                                                   {{integer, 2}, {2}}},
                                                  {2},
                                                  std::nullopt}}},
              {true,
               {instruction(5, data, cloth::MirPhiInstruction{{{{0}, {0}}}}),
                instruction(6, integer,
                            cloth::MirPhiInstruction{{{{0}, {3}}}})},
               {range, cloth::MirJumpTerminator{{3}}}},
              {true,
               {instruction(7, data, cloth::MirPhiInstruction{{{{0}, {1}}}}),
                instruction(8, integer,
                            cloth::MirPhiInstruction{{{{0}, {4}}}})},
               {range, cloth::MirJumpTerminator{{3}}}},
              {true,
               {instruction(9, data,
                            cloth::MirPhiInstruction{{{{1}, {5}}, {{2}, {7}}}}),
                instruction(10, integer,
                            cloth::MirPhiInstruction{{{{1}, {6}}, {{2}, {8}}}}),
                instruction(
                    11, string,
                    cloth::MirCallInstruction{cloth::MirCallKind::kUnqualified,
                                              cloth::MirDispatchKind::kDirect,
                                              false,
                                              *churn,
                                              std::nullopt,
                                              {}})},
               {range, cloth::MirSwitchTerminator{{10},
                                                  integer,
                                                  {{{integer, 0}, {4}},
                                                   {{integer, 1}, {4}}},
                                                  {5},
                                                  std::nullopt}}},
              {true, {}, {range, cloth::MirReturnTerminator{MirValueId{9}}}},
              {true, {}, {range, cloth::MirTrapTerminator{}}},
          },
          12};
      // A case and guarded enum default may share a scalar-phi destination.
      // Their physical LLVM edges must still collapse to one MIR predecessor.
      for (auto& enum_function : file.functions) {
        if (result.semantics.symbol(enum_function.symbol).name != "EnumPhi")
          continue;
        const auto enum_type =
            result.semantics.symbol(enum_function.symbol).parameter_types[0];
        enum_function.body = cloth::MirBody{
            range,
            {0},
            {{true,
              {instruction(0, enum_type,
                           cloth::MirLoadSymbolInstruction{
                               enum_function.parameters[0]}),
               instruction(1, integer,
                           cloth::MirLiteralInstruction{
                               cloth::LiteralKind::kInteger, "10"})},
              {range, cloth::MirSwitchTerminator{{0},
                                                 enum_type,
                                                 {{{enum_type, 0}, {1}}},
                                                 {1},
                                                 MirBlockId{2}}}},
             {true,
              {instruction(2, integer, cloth::MirPhiInstruction{{{{0}, {1}}}})},
              {range, cloth::MirReturnTerminator{MirValueId{2}}}},
             {true, {}, {range, cloth::MirTrapTerminator{}}}},
            3};
        return true;
      }
      return false;
    }
  }
  return false;
}

int build(const char* source_path, const char* output_path,
          std::string_view mode) {
#if defined(CLOTH_DEFAULT_LLC)
  cloth::Compilation compilation;
  const bool stress = mode == "stress";
  if (mode == "tables") {
    compilation.add_source(
        cloth::SourceFile::from_memory("Main.co", table_source()));
  } else if (stress) {
    compilation.set_source_root(
        std::filesystem::path{source_path}.parent_path());
    auto source = cloth::SourceFile::load(source_path);
    if (!source) return 2;
    compilation.add_source(std::move(*source));
  } else {
    std::string arms;
    if (mode == "exhaustive") {
      arms = "case State.A, State.B, State.C: { return 0; }";
    } else if (mode == "default") {
      arms = "case State.A: { return 0; } default: { return 0; }";
    } else if (mode == "default_only") {
      arms = "default: { return 0; }";
    } else {
      return 2;
    }
    compilation.add_source(
        cloth::SourceFile::from_memory("State.co", "enum { A, B, C }"));
    compilation.add_source(cloth::SourceFile::from_memory(
        "Main.co",
        "static func Corrupt(): State { return State.A; } "
        "static func Main(): int32 { switch (Corrupt()) { " +
            arms + " } }"));
  }
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  if (!result.is_valid || (stress && !install_phis(result)) ||
      !cloth::verify_mir(result.mir, result.semantics, diagnostics) ||
      !cloth::verify_abi(result.abi, result.mir, result.semantics,
                         diagnostics)) {
    print_errors(diagnostics);
    return 1;
  }
  auto llvm =
      cloth::emit_llvm_ir(result.mir, result.abi, result.semantics, diagnostics,
                          cloth::LlvmIrOptions{true, "Main", std::nullopt});
  if (!llvm) {
    print_errors(diagnostics);
    return 1;
  }
  if (stress) {
    llvm->text.insert(llvm->text.find("declare"),
                      "declare void @cloth_rt_gc_collect()\n");
    std::string instrumented;
    std::istringstream lines{llvm->text};
    for (std::string line; std::getline(lines, line);) {
      if (line.contains("call ptr @cloth_rt_alloc(") ||
          line.contains("call ptr @cloth_rt_array_alloc(") ||
          line.contains("call ptr @cloth_rt_string_literal(") ||
          line.contains("call ptr @cloth_rt_string_concat("))
        instrumented += "  call void @cloth_rt_gc_collect()\n";
      instrumented += line + '\n';
    }
    llvm->text = std::move(instrumented);
  } else if (mode != "tables") {
    // Corrupt only the native producer, after typed MIR/ABI verification. No
    // source-language cast or unchecked enum literal is introduced for tests.
    std::string name;
    for (const auto& file : result.abi.files)
      for (const auto& function : file.functions)
        if (result.semantics.symbol(function.symbol).name == "Corrupt")
          name = function.mangled_name;
    const auto start = llvm->text.find("define i32 @" + name + "(");
    const auto returned = llvm->text.find("ret i32 0", start);
    if (name.empty() || start == std::string::npos ||
        returned == std::string::npos ||
        returned > llvm->text.find("\n}", start))
      return 2;
    // Exercise both the first invalid tag and high-bit/negative tag patterns.
    llvm->text.replace(returned, std::string_view{"ret i32 0"}.size(),
                       mode == "default" ? "ret i32 3" : "ret i32 4294967295");
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
  return 2;
#endif
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  return build(argv[1], argv[2], argv[3]);
}
