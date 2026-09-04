// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_optimizer.h"
#include "cloth/mir/mir_printer.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string result;
  for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    if (!result.empty()) {
      result += "; ";
    }
    result += diagnostic.message;
  }
  return result;
}

std::string_view function_definition(std::string_view module,
                                     std::string_view name) {
  const std::string marker = "@" + std::string{name} + "(";
  std::size_t start = 0;
  while ((start = module.find("define ", start)) != std::string_view::npos) {
    const std::size_t body = module.find(" {", start);
    if (body == std::string_view::npos) {
      return {};
    }
    if (module.substr(start, body - start).contains(marker)) {
      const std::size_t end = module.find("\n}", body);
      return end == std::string_view::npos
                 ? std::string_view{}
                 : module.substr(start, end + 2 - start);
    }
    start = body + 2;
  }
  return {};
}

class CompiledSources {
 public:
  void add(std::filesystem::path path, std::string text) {
    compilation_.add_source(
        cloth::SourceFile::from_memory(std::move(path), std::move(text)));
  }

  void add_package(std::filesystem::path path, std::string text,
                   std::string package, std::string version) {
    compilation_.add_package_source(
        cloth::SourceFile::from_memory(std::move(path), std::move(text)),
        std::move(package), "", std::move(version));
  }

  void set_dependencies(std::vector<cloth::CompilationDependency> values) {
    compilation_.set_package_dependencies(std::move(values));
  }

  void add_imported(cloth::ImportedPackageView package) {
    compilation_.add_imported_package(std::move(package));
  }

  void compile() { result.emplace(compilation_.analyze(diagnostics)); }

  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;

 private:
  cloth::Compilation compilation_;
};

const cloth::MirCallable* find_function(const cloth::MirModule& mir,
                                        const cloth::SemanticModel& semantics,
                                        std::string_view name) {
  for (const cloth::MirFileClass& file : mir.files) {
    for (const cloth::MirCallable& function : file.functions) {
      if (semantics.symbol(function.symbol).name == name) {
        return &function;
      }
    }
  }
  return nullptr;
}

cloth::MirCallable* find_function(cloth::MirModule& mir,
                                  const cloth::SemanticModel& semantics,
                                  std::string_view name) {
  return const_cast<cloth::MirCallable*>(
      find_function(std::as_const(mir), semantics, name));
}

const cloth::AbiCallable* find_abi_function(
    const cloth::AbiModule& abi, const cloth::SemanticModel& semantics,
    std::string_view name) {
  for (const cloth::AbiFileClass& file : abi.files) {
    for (const cloth::AbiCallable& function : file.functions) {
      if (semantics.symbol(function.symbol).name == name) {
        return &function;
      }
    }
  }
  return nullptr;
}

const cloth::MirInstruction* definition(const cloth::MirBody& body,
                                        cloth::MirValueId value) {
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (instruction.result == value) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

template <typename Value>
bool body_has_instruction(const cloth::MirBody& body) {
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<Value>(instruction.data)) {
        return true;
      }
    }
  }
  return false;
}

template <typename Value>
bool body_has_terminator(const cloth::MirBody& body) {
  for (const cloth::MirBasicBlock& block : body.blocks) {
    if (std::holds_alternative<Value>(block.terminator.data)) {
      return true;
    }
  }
  return false;
}

std::optional<cloth::ScalarConstant> returned_constant(
    const cloth::MirCallable& function) {
  for (const cloth::MirBasicBlock& block : function.body.blocks) {
    const auto* returned =
        std::get_if<cloth::MirReturnTerminator>(&block.terminator.data);
    if (returned == nullptr || !returned->value) {
      continue;
    }
    const cloth::MirInstruction* instruction =
        definition(function.body, *returned->value);
    if (instruction == nullptr) {
      return std::nullopt;
    }
    const auto* constant =
        std::get_if<cloth::MirScalarConstantInstruction>(&instruction->data);
    return constant == nullptr ? std::nullopt : std::optional{constant->value};
  }
  return std::nullopt;
}

void expect_return_bits(TestContext& test, const cloth::MirModule& mir,
                        const cloth::SemanticModel& semantics,
                        std::string_view function_name, std::uint64_t bits) {
  const cloth::MirCallable* function =
      find_function(mir, semantics, function_name);
  const auto value =
      function == nullptr ? std::nullopt : returned_constant(*function);
  test.expect(value && value->bits == bits,
              std::string{function_name} + " did not fold to expected bits");
}

void scalar_operations_fold(TestContext& test) {
  CompiledSources sources;
  sources.add("State.co", "enum { Ready, Done }");
  sources.add("Fold.co", R"(
    import State;
    static final int16 Small = -4;
    static final uint64 Huge = 18446744073709551615;
    static func Arithmetic(): int32 { return (1 + 2) * 3; }
    static func Unary(): int16 { return ~Small; }
    static func Boolean(): bool { return !(1 < 2); }
    static func Character(): bool { return 'a' == 'a'; }
    static func NegativeZero(): float32 { return -0.0; }
    static func Floating(): float64 { return (1.5 + 2.25) / 3.0; }
    static func EnumEqual(): bool { return State.Ready != State.Done; }
    static func Widen(): int64 { return Small; }
    static func Checked(): int32 { return int32(Small); }
    static func Wrapped(): int8 { return int8::wrap(Huge); }
    static func Saturated(): int8 { return int8::sat(Huge); }
  )");
  sources.compile();
  test.expect(sources.result && sources.result->is_valid,
              "folding fixture failed: " + messages(sources.diagnostics));
  if (!sources.result || !sources.result->is_valid) {
    return;
  }

  cloth::MirModule optimized = sources.result->mir;
  cloth::optimize_mir(optimized, sources.result->semantics);
  cloth::DiagnosticEngine diagnostics;
  test.expect(
      cloth::verify_mir(optimized, sources.result->semantics, diagnostics),
      "folded MIR failed verification: " + messages(diagnostics));

  expect_return_bits(test, optimized, sources.result->semantics, "Arithmetic",
                     9);
  expect_return_bits(test, optimized, sources.result->semantics, "Unary", 3);
  expect_return_bits(test, optimized, sources.result->semantics, "Boolean", 0);
  expect_return_bits(test, optimized, sources.result->semantics, "Character",
                     1);
  expect_return_bits(test, optimized, sources.result->semantics, "NegativeZero",
                     UINT64_C(0x80000000));
  expect_return_bits(test, optimized, sources.result->semantics, "Floating",
                     UINT64_C(0x3ff4000000000000));
  expect_return_bits(test, optimized, sources.result->semantics, "EnumEqual",
                     1);
  expect_return_bits(test, optimized, sources.result->semantics, "Widen",
                     UINT64_C(0xfffffffffffffffc));
  expect_return_bits(test, optimized, sources.result->semantics, "Checked",
                     UINT64_C(0xfffffffc));
  expect_return_bits(test, optimized, sources.result->semantics, "Wrapped",
                     255);
  expect_return_bits(test, optimized, sources.result->semantics, "Saturated",
                     127);

  cloth::MirModule twice = optimized;
  cloth::optimize_mir(twice, sources.result->semantics);
  test.expect(optimized == twice,
              "scalar folding is not structurally idempotent");
  std::ostringstream once_text;
  std::ostringstream twice_text;
  cloth::print_mir_summary(optimized, sources.result->semantics, once_text);
  cloth::print_mir_summary(twice, sources.result->semantics, twice_text);
  test.expect(once_text.str() == twice_text.str(),
              "scalar folding is not idempotent");
  test.expect(once_text.str().contains("constant type#") &&
                  once_text.str().contains("0x0000000000000009"),
              "MIR printer omitted deterministic typed constant bits");

  cloth::DiagnosticEngine llvm_diagnostics;
  const auto llvm =
      cloth::emit_llvm_ir(optimized, sources.result->abi,
                          sources.result->semantics, llvm_diagnostics);
  test.expect(llvm && !llvm_diagnostics.has_errors(),
              "folded MIR failed LLVM emission: " + messages(llvm_diagnostics));
  test.expect(llvm && llvm->text.contains(
                          "bitcast (i64 4608308318706860032 to double)"),
              "LLVM did not consume exact float64 constant bits");
  test.expect(llvm && llvm->text.contains("bitcast (i32 2147483648 to float)"),
              "LLVM did not consume exact float32 constant bits");
}

void scalar_boundaries_and_operators_fold(TestContext& test) {
  CompiledSources sources;
  sources.add("State.co", "enum { Ready, Done }");
  sources.add("Boundaries.co", R"(
    import State;
    static final int8 I8 = int8(-128);
    static final int16 I16 = int16(-32768);
    static final int32 I32 = -2147483648;
    static final int64 I64 = -9223372036854775808;
    static final uint8 U8 = uint8(255);
    static final uint16 U16 = uint16(65535);
    static final uint32 U32 = uint32(4294967295);
    static final uint64 U64 = 18446744073709551615;
    static final float32 Tiny =
        0.000000000000000000000000000000000000000000001;
    static func ReadI8(): int8 { return I8 + int8(0); }
    static func ReadI16(): int16 { return I16 + int16(0); }
    static func ReadI32(): int32 { return I32 + 0; }
    static func ReadI64(): int64 { return I64 + 0; }
    static func ReadU8(): uint8 { return U8 + uint8(0); }
    static func ReadU16(): uint16 { return U16 + uint16(0); }
    static func ReadU32(): uint32 { return U32 + uint32(0); }
    static func ReadU64(): uint64 { return U64 + uint64(0); }
    static func ReadTiny(): float32 { return Tiny + 0.0; }
    static func Subtract(): int32 { return 9 - 4; }
    static func Divide(): int32 { return 8 / 2; }
    static func Remainder(): int32 { return 9 % 4; }
    static func Bitwise(): int32 { return ((12 & 10) | 1) ^ 3; }
    static func Shift(): int32 { return (1 << 5) >> 2; }
    static func Positive(): int32 { return +5; }
    static func Negative(): int32 { return -5; }
    static func Compare(): bool {
      return 1 <= 1 && 2 > 1 && 2 >= 2 && 1 != 2;
    }
    static func EnumTag(): State { return State.Done; }
  )");
  sources.compile();
  test.expect(sources.result && sources.result->is_valid,
              "boundary fixture failed: " + messages(sources.diagnostics));
  if (!sources.result || !sources.result->is_valid) {
    return;
  }

  const auto& mir = sources.result->mir;
  const auto& semantics = sources.result->semantics;
  for (const auto& [name, bits] :
       std::vector<std::pair<std::string_view, std::uint64_t>>{
           {"ReadI8", 0x80},
           {"ReadI16", 0x8000},
           {"ReadI32", 0x80000000},
           {"ReadI64", UINT64_C(0x8000000000000000)},
           {"ReadU8", 0xff},
           {"ReadU16", 0xffff},
           {"ReadU32", 0xffffffff},
           {"ReadU64", UINT64_MAX},
           {"ReadTiny", 1},
           {"Subtract", 5},
           {"Divide", 4},
           {"Remainder", 1},
           {"Bitwise", 10},
           {"Shift", 8},
           {"Positive", 5},
           {"Negative", 0xfffffffb},
           {"Compare", 1},
           {"EnumTag", 1}}) {
    expect_return_bits(test, mir, semantics, name, bits);
  }
}

void failing_operations_remain(TestContext& test) {
  CompiledSources sources;
  sources.add("Failures.co", R"(
    static final uint64 TooBig = 256;
    static final int32 Width = 32;
    static func Overflow(): int32 { return 2147483647 + 1; }
    static func ZeroDivisor(): int32 { return 1 / 0; }
    static func InvalidShift(): int32 { return 1 << Width; }
    static func FailedCheck(): int8 { return int8(TooBig); }
    static func NonFinite(): float64 { return 0.0; }
  )");
  sources.compile();
  test.expect(sources.result && sources.result->is_valid,
              "failure fixture failed: " + messages(sources.diagnostics));
  if (!sources.result || !sources.result->is_valid) {
    return;
  }

  cloth::MirCallable* non_finite = find_function(
      sources.result->mir, sources.result->semantics, "NonFinite");
  const cloth::TypeId float64 = *sources.result->semantics.find_type("float64");
  const cloth::SourceRange range = non_finite->body.range;
  non_finite->body = cloth::MirBody{
      range,
      cloth::MirBlockId{0},
      {{true,
        {{cloth::MirValueId{0}, float64, range,
          cloth::MirScalarConstantInstruction{
              {float64, UINT64_C(0x7fefffffffffffff)}}},
         {cloth::MirValueId{1}, float64, range,
          cloth::MirScalarConstantInstruction{
              {float64, UINT64_C(0x4000000000000000)}}},
         {cloth::MirValueId{2}, float64, range,
          cloth::MirBinaryInstruction{cloth::MirValueId{0},
                                      cloth::TokenKind::kStar,
                                      cloth::MirValueId{1}}}},
        {range, cloth::MirReturnTerminator{cloth::MirValueId{2}}}}},
      3};

  cloth::DiagnosticEngine baseline_diagnostics;
  test.expect(cloth::verify_mir(sources.result->mir, sources.result->semantics,
                                baseline_diagnostics),
              "failure baseline is invalid: " + messages(baseline_diagnostics));
  cloth::optimize_mir(sources.result->mir, sources.result->semantics);
  for (const std::string_view name :
       {"Overflow", "ZeroDivisor", "InvalidShift", "NonFinite"}) {
    const cloth::MirCallable* function =
        find_function(sources.result->mir, sources.result->semantics, name);
    const auto* returned = std::get_if<cloth::MirReturnTerminator>(
        &function->body.blocks.back().terminator.data);
    const cloth::MirInstruction* instruction =
        definition(function->body, *returned->value);
    test.expect(instruction != nullptr &&
                    std::holds_alternative<cloth::MirBinaryInstruction>(
                        instruction->data),
                std::string{name} + " failure was folded or replaced");
  }
  const cloth::MirCallable* failed_check = find_function(
      sources.result->mir, sources.result->semantics, "FailedCheck");
  const auto* checked_return = std::get_if<cloth::MirReturnTerminator>(
      &failed_check->body.blocks.back().terminator.data);
  const cloth::MirInstruction* checked_instruction =
      definition(failed_check->body, *checked_return->value);
  test.expect(checked_instruction != nullptr &&
                  std::holds_alternative<cloth::MirConvertInstruction>(
                      checked_instruction->data),
              "failed checked conversion was folded or replaced");

  cloth::DiagnosticEngine diagnostics;
  test.expect(
      cloth::verify_mir(sources.result->mir, sources.result->semantics,
                        diagnostics),
      "failure-preserving MIR failed verification: " + messages(diagnostics));
}

void source_free_constants_fold(TestContext& test) {
  CompiledSources producer;
  producer.add_package("Constants.co", "static final int16 Value = 7;",
                       "library", "1.0.0");
  producer.compile();
  test.expect(producer.result && producer.result->is_valid,
              "producer failed: " + messages(producer.diagnostics));
  if (!producer.result || !producer.result->is_valid) {
    return;
  }
  auto exported = cloth::build_imported_package_view(
      {"library", "1.0.0"}, producer.result->semantics, producer.result->mir,
      producer.result->abi);
  test.expect(exported.is_valid(), "producer package export failed");
  if (!exported.view) {
    return;
  }

  CompiledSources consumer;
  consumer.set_dependencies({{"app", "dep", "library"}});
  consumer.add_imported(std::move(*exported.view));
  consumer.add_package("Use.co", R"(
    import dep::Constants;
    static func Read(): int64 { return Constants.Value + 2; }
  )",
                       "app", "1.0.0");
  consumer.compile();
  test.expect(consumer.result && consumer.result->is_valid,
              "source-free consumer failed: " + messages(consumer.diagnostics));
  if (!consumer.result || !consumer.result->is_valid) {
    return;
  }
  cloth::optimize_mir(consumer.result->mir, consumer.result->semantics);
  expect_return_bits(test, consumer.result->mir, consumer.result->semantics,
                     "Read", 9);
  for (const cloth::MirFileClass& file : consumer.result->mir.files) {
    if (!file.is_imported_declaration) {
      continue;
    }
    for (const cloth::MirCallable& function : file.functions) {
      test.expect(function.body.blocks.empty(),
                  "optimizer synthesized an imported function body");
    }
  }
}

void verifier_rejects_malformed_constants(TestContext& test) {
  CompiledSources sources;
  sources.add("Verify.co", "static func Value(): bool { return true; }");
  sources.compile();
  test.expect(sources.result && sources.result->is_valid,
              "verifier fixture failed: " + messages(sources.diagnostics));
  if (!sources.result || !sources.result->is_valid) {
    return;
  }
  cloth::optimize_mir(sources.result->mir, sources.result->semantics);
  cloth::MirCallable* function =
      find_function(sources.result->mir, sources.result->semantics, "Value");
  auto* constant = std::get_if<cloth::MirScalarConstantInstruction>(
      &function->body.blocks[0].instructions[0].data);
  test.expect(constant != nullptr, "valid bool did not become a MIR constant");
  if (constant == nullptr) {
    return;
  }
  constant->value.bits = 2;
  cloth::DiagnosticEngine diagnostics;
  test.expect(!cloth::verify_mir(sources.result->mir, sources.result->semantics,
                                 diagnostics),
              "MIR verifier accepted noncanonical bool bits");
  test.expect(
      messages(diagnostics)
          .contains("scalar constant has an invalid type or bit pattern"),
      "malformed constant produced the wrong diagnostic");
}

void control_flow_folds_and_compacts(TestContext& test) {
  CompiledSources sources;
  sources.add("State.co", "enum { Ready, Done }");
  sources.add("Flow.co", R"(
    import State;
    static func Never(): bool { print("never"); return false; }
    static func Same(bool flag): bool { return flag && false; }
    static func ShortCircuit(): bool { return true || Never(); }
    static func Branch(): int32 {
      if (false) { print("unreachable"); }
      return 7;
    }
    static func Chosen(): int32 {
      switch (2) {
        case 1: { return 10; }
        case 2: { return 20; }
        default: { return 30; }
      }
    }
    static func EnumChosen(): int32 {
      switch (State.Done) {
        case State.Ready: { return 1; }
        case State.Done: { return 2; }
      }
    }
  )");
  sources.compile();
  test.expect(sources.result && sources.result->is_valid,
              "control-flow fixture failed: " + messages(sources.diagnostics));
  if (!sources.result || !sources.result->is_valid) {
    return;
  }

  cloth::MirModule baseline =
      cloth::lower_to_mir(sources.result->hir, sources.result->semantics);
  const cloth::MirCallable* baseline_short =
      find_function(baseline, sources.result->semantics, "ShortCircuit");
  test.expect(baseline_short != nullptr,
              "short-circuit baseline function is missing");
  if (baseline_short == nullptr) {
    return;
  }
  const std::size_t baseline_blocks = baseline_short->body.blocks.size();
  test.expect(
      body_has_instruction<cloth::MirPhiInstruction>(baseline_short->body) &&
          body_has_terminator<cloth::MirBranchTerminator>(baseline_short->body),
      "short-circuit baseline lacks its branch and phi");

  const cloth::MirModule& optimized = sources.result->mir;
  const cloth::MirCallable* same =
      find_function(optimized, sources.result->semantics, "Same");
  const cloth::MirCallable* short_circuit =
      find_function(optimized, sources.result->semantics, "ShortCircuit");
  const cloth::MirCallable* branch =
      find_function(optimized, sources.result->semantics, "Branch");
  test.expect(same != nullptr &&
                  !body_has_instruction<cloth::MirPhiInstruction>(same->body),
              "equal reachable phi constants did not fold");
  expect_return_bits(test, optimized, sources.result->semantics, "Same", 0);
  test.expect(
      short_circuit != nullptr &&
          short_circuit->body.blocks.size() < baseline_blocks &&
          !body_has_instruction<cloth::MirPhiInstruction>(
              short_circuit->body) &&
          !body_has_terminator<cloth::MirBranchTerminator>(
              short_circuit->body) &&
          !body_has_instruction<cloth::MirCallInstruction>(short_circuit->body),
      "constant short-circuit flow was not compacted");
  expect_return_bits(test, optimized, sources.result->semantics, "ShortCircuit",
                     1);
  test.expect(
      branch != nullptr &&
          !body_has_instruction<cloth::MirCallInstruction>(branch->body) &&
          !body_has_terminator<cloth::MirBranchTerminator>(branch->body),
      "constant if retained its unreachable effect path");
  expect_return_bits(test, optimized, sources.result->semantics, "Branch", 7);

  for (const std::string_view name : {"Chosen", "EnumChosen"}) {
    const cloth::MirCallable* function =
        find_function(optimized, sources.result->semantics, name);
    test.expect(
        function != nullptr &&
            !body_has_terminator<cloth::MirSwitchTerminator>(function->body),
        std::string{name} + " retained a constant switch");
  }
  expect_return_bits(test, optimized, sources.result->semantics, "Chosen", 20);
  expect_return_bits(test, optimized, sources.result->semantics, "EnumChosen",
                     2);

  for (const cloth::MirFileClass& file : optimized.files) {
    for (const cloth::MirCallable& function : file.functions) {
      std::vector<bool> values(function.body.value_count);
      for (const cloth::MirBasicBlock& block : function.body.blocks) {
        test.expect(block.is_reachable,
                    "compacted body retained a dead block flag");
        for (const cloth::MirInstruction& instruction : block.instructions) {
          if (instruction.result) {
            test.expect(instruction.result->value < values.size() &&
                            !values[instruction.result->value],
                        "compacted value IDs are not unique and dense");
            if (instruction.result->value < values.size()) {
              values[instruction.result->value] = true;
            }
          }
        }
      }
      for (const bool defined : values) {
        test.expect(defined, "compacted value table contains a gap");
      }
    }
  }

  cloth::DiagnosticEngine diagnostics;
  test.expect(
      cloth::verify_mir(optimized, sources.result->semantics, diagnostics),
      "compacted MIR failed verification: " + messages(diagnostics));

  cloth::MirModule repeated = optimized;
  cloth::optimize_mir(repeated, sources.result->semantics);
  test.expect(optimized == repeated,
              "control-flow optimization is not structurally idempotent");
  std::ostringstream once_text;
  std::ostringstream repeated_text;
  cloth::print_mir_summary(optimized, sources.result->semantics, once_text);
  cloth::print_mir_summary(repeated, sources.result->semantics, repeated_text);
  test.expect(once_text.str() == repeated_text.str(),
              "control-flow optimization is not idempotent");
}

void worklist_stress_and_post_pass_rejection(TestContext& test) {
  CompiledSources sources;
  sources.add("Stress.co", "static func Value(): int32 { return 0; }");
  sources.compile();
  test.expect(sources.result && sources.result->is_valid,
              "stress fixture failed: " + messages(sources.diagnostics));
  if (!sources.result || !sources.result->is_valid) {
    return;
  }

  cloth::MirModule stress =
      cloth::lower_to_mir(sources.result->hir, sources.result->semantics);
  cloth::MirCallable* function =
      find_function(stress, sources.result->semantics, "Value");
  const cloth::TypeId int32 = *sources.result->semantics.find_type("int32");
  const cloth::SourceRange range = function->body.range;
  constexpr std::size_t kChainLength = 16'384;
  std::vector<cloth::MirInstruction> instructions;
  instructions.reserve(kChainLength + 2);
  instructions.push_back({cloth::MirValueId{0}, int32, range,
                          cloth::MirScalarConstantInstruction{{int32, 0}}});
  instructions.push_back({cloth::MirValueId{1}, int32, range,
                          cloth::MirScalarConstantInstruction{{int32, 1}}});
  for (std::size_t index = 0; index < kChainLength; ++index) {
    instructions.push_back(
        {cloth::MirValueId{index + 2}, int32, range,
         cloth::MirBinaryInstruction{
             cloth::MirValueId{index == 0 ? 0 : index + 1},
             cloth::TokenKind::kPlus, cloth::MirValueId{1}}});
  }
  function->body = {
      range,
      cloth::MirBlockId{0},
      {{true,
        std::move(instructions),
        {range,
         cloth::MirReturnTerminator{cloth::MirValueId{kChainLength + 1}}}}},
      kChainLength + 2};

  cloth::DiagnosticEngine baseline_diagnostics;
  test.expect(
      cloth::verify_mir(stress, sources.result->semantics,
                        baseline_diagnostics),
      "stress baseline failed verification: " + messages(baseline_diagnostics));
  cloth::optimize_mir(stress, sources.result->semantics);
  cloth::DiagnosticEngine optimized_diagnostics;
  test.expect(
      cloth::verify_mir(stress, sources.result->semantics,
                        optimized_diagnostics),
      "stress result failed verification: " + messages(optimized_diagnostics));
  expect_return_bits(test, stress, sources.result->semantics, "Value",
                     kChainLength);

  cloth::MirModule repeated = stress;
  cloth::optimize_mir(repeated, sources.result->semantics);
  test.expect(stress == repeated, "long-chain optimization is not idempotent");

  cloth::MirModule broken = stress;
  cloth::MirCallable* broken_function =
      find_function(broken, sources.result->semantics, "Value");
  --broken_function->body.value_count;
  cloth::DiagnosticEngine broken_diagnostics;
  test.expect(
      !cloth::verify_mir(broken, sources.result->semantics, broken_diagnostics),
      "MIR verifier accepted corrupted optimized value metadata");
}

void input_order_is_deterministic(TestContext& test) {
  const auto compile =
      [](bool reverse,
         cloth::DiagnosticEngine& diagnostics) -> std::optional<std::string> {
    cloth::Compilation compilation;
    const auto add_constants = [&] {
      compilation.add_source(cloth::SourceFile::from_memory(
          "Constants.co", "static final int32 Value = (2 + 3) * 4;"));
    };
    const auto add_main = [&] {
      compilation.add_source(cloth::SourceFile::from_memory(
          "Main.co",
          "import Constants; static func Main(): int32 { if (true) { return "
          "Constants.Value; } return 0; }"));
    };
    if (reverse) {
      add_main();
      add_constants();
    } else {
      add_constants();
      add_main();
    }
    auto result = compilation.analyze(diagnostics);
    if (!result.is_valid) {
      return std::nullopt;
    }
    const cloth::AbiCallable* main =
        find_abi_function(result.abi, result.semantics, "Main");
    const auto llvm = cloth::emit_llvm_ir(
        result.mir, result.abi, result.semantics, diagnostics,
        cloth::LlvmIrOptions{true, "Main", std::nullopt});
    if (!llvm || main == nullptr) {
      return std::nullopt;
    }
    return std::string{function_definition(llvm->text, main->mangled_name)};
  };

  cloth::DiagnosticEngine forward_diagnostics;
  cloth::DiagnosticEngine reverse_diagnostics;
  const auto forward = compile(false, forward_diagnostics);
  const auto reverse = compile(true, reverse_diagnostics);
  test.expect(forward && reverse,
              "input-order fixture failed: " + messages(forward_diagnostics) +
                  messages(reverse_diagnostics));
  test.expect(forward && reverse && *forward == *reverse,
              "optimized function body depends on source insertion order");
}

void package_pipeline_is_consistent(TestContext& test) {
  const std::string library_source = "static final int16 Value = 7;";
  const std::string app_source = R"(
    import dep::Constants;
    static func Read(): int64 { return Constants.Value + 2; }
  )";

  CompiledSources whole;
  whole.set_dependencies({{"app", "dep", "library"}});
  whole.add_package("Constants.co", library_source, "library", "1.0.0");
  whole.add_package("Use.co", app_source, "app", "1.0.0");
  whole.compile();
  test.expect(whole.result && whole.result->is_valid,
              "whole package fixture failed: " + messages(whole.diagnostics));
  if (!whole.result || !whole.result->is_valid) {
    return;
  }
  expect_return_bits(test, whole.result->mir, whole.result->semantics, "Read",
                     9);

  auto exported = cloth::build_imported_package_view(
      {"library", "1.0.0"}, whole.result->semantics, whole.result->mir,
      whole.result->abi);
  test.expect(exported.is_valid(), "optimized dependency export failed");
  if (!exported.view) {
    return;
  }

  CompiledSources separate;
  separate.set_dependencies({{"app", "dep", "library"}});
  separate.add_imported(std::move(*exported.view));
  separate.add_package("Use.co", app_source, "app", "1.0.0");
  separate.compile();
  test.expect(
      separate.result && separate.result->is_valid,
      "source-free package fixture failed: " + messages(separate.diagnostics));
  if (!separate.result || !separate.result->is_valid) {
    return;
  }
  expect_return_bits(test, separate.result->mir, separate.result->semantics,
                     "Read", 9);

  cloth::LlvmIrOptions options;
  options.package = cloth::PackageIdentity{"app", "1.0.0"};
  cloth::DiagnosticEngine whole_diagnostics;
  cloth::DiagnosticEngine separate_diagnostics;
  const auto whole_llvm =
      cloth::emit_llvm_ir(whole.result->mir, whole.result->abi,
                          whole.result->semantics, whole_diagnostics, options);
  const auto separate_llvm = cloth::emit_llvm_ir(
      separate.result->mir, separate.result->abi, separate.result->semantics,
      separate_diagnostics, options);
  const cloth::AbiCallable* whole_read_abi =
      find_abi_function(whole.result->abi, whole.result->semantics, "Read");
  const cloth::AbiCallable* separate_read_abi = find_abi_function(
      separate.result->abi, separate.result->semantics, "Read");
  const std::string_view whole_read =
      whole_llvm != std::nullopt && whole_read_abi != nullptr
          ? function_definition(whole_llvm->text, whole_read_abi->mangled_name)
          : std::string_view{};
  const std::string_view separate_read =
      separate_llvm != std::nullopt && separate_read_abi != nullptr
          ? function_definition(separate_llvm->text,
                                separate_read_abi->mangled_name)
          : std::string_view{};
  test.expect(whole_llvm && separate_llvm && !whole_read.empty() &&
                  whole_read == separate_read &&
                  whole_read.contains("ret i64 9"),
              "whole and source-free optimized package bodies differ: whole `" +
                  std::string{whole_read} + "`, separate `" +
                  std::string{separate_read} + "`");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"scalar operations fold", scalar_operations_fold},
      {"scalar boundaries and operators fold",
       scalar_boundaries_and_operators_fold},
      {"failing operations remain", failing_operations_remain},
      {"source-free constants fold", source_free_constants_fold},
      {"verifier rejects malformed constants",
       verifier_rejects_malformed_constants},
      {"control flow folds and compacts", control_flow_folds_and_compacts},
      {"worklist stress and post-pass rejection",
       worklist_stress_and_post_pass_rejection},
      {"input order is deterministic", input_order_is_deterministic},
      {"package pipeline is consistent", package_pipeline_is_consistent},
  };
  return cloth::test::run_tests(tests);
}
