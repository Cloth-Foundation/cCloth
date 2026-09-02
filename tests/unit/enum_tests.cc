// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_printer.h"
#include "cloth/abi/abi_verifier.h"
#include "cloth/artifact/package_artifact.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/hir/hir_printer.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/lexer/lexer.h"
#include "cloth/mir/mir_printer.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/parser/parser.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "test.h"

namespace {
using cloth::test::TestCase;
using cloth::test::TestContext;

std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string text;
  for (const auto& diagnostic : diagnostics.diagnostics())
    text += diagnostic.message + "\n";
  return text;
}

void add(cloth::Compilation& compilation, std::string name, std::string text) {
  compilation.add_source(
      cloth::SourceFile::from_memory(std::move(name), std::move(text)));
}

void named_values(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Status.co", "enum { Ready, ready, _Done, }");
  add(compilation, "Main.co", R"(
    static final Status Initial = (Status.Ready);
    Status state;
    Main() { state = Status.ready; }
    static func Identity(Status value): Status { return value; }
    static func Run() {
      var current = Identity(Initial);
      Status[] values = [current, Status.ready, Status._Done];
      Status[]? optional = values;
      for (Status item in values) { println(item); }
      current = Status.ready;
      println(current == Status.ready);
      println(current::typeName);
    }
  )");
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto type = result.semantics.find_type("Status");
  test.expect(type.has_value(), "enum type is missing");
  if (!type) return;
  test.expect(result.semantics.type(*type).kind == cloth::TypeKind::kEnum,
              "enum was modeled as a reference or integer");
  test.expect(
      result.abi.types[type->value].kind == cloth::AbiTypeKind::kInteger &&
          result.abi.types[type->value].storage.size == 4,
      "enum scalar ABI is wrong");
  for (const auto symbol :
       result.semantics.file(*result.semantics.type(*type).file).enum_cases) {
    test.expect(result.semantics.symbol(symbol).visibility ==
                    cloth::Visibility::kPublic,
                "case spelling changed visibility");
  }
  auto llvm = cloth::emit_llvm_ir(result.mir, result.abi, result.semantics,
                                  diagnostics);
  test.expect(llvm.has_value(), messages(diagnostics));
  if (llvm) {
    test.expect(llvm->text.find("icmp ult i32 %tag, 3") != std::string::npos,
                "enum output is not bounds checked");
  }
  std::ostringstream summary;
  cloth::print_hir_summary(result.hir, result.semantics, summary);
  cloth::print_mir_summary(result.mir, result.semantics, summary);
  cloth::print_abi_summary(result.abi, result.semantics, summary);
  test.expect(summary.str().find("Enum Status") != std::string::npos &&
                  summary.str().find("Case _Done") != std::string::npos &&
                  summary.str().find("scalar uint32") != std::string::npos,
              "enum summaries lost declaration or scalar identity");
}

void invalid_programs(TestContext& test) {
  const std::vector<std::string> bodies{
      "Status value;",
      "Status value = 0;",
      "Status value = Other.Ready;",
      "Status? value = null;",
      "object value = Status.Ready;",
      "Status value = Status(0);",
      "uint32 value = uint32(Status.Ready);",
      "Status.Ready = Status.ready;",
      "var value = Status.Ready + Status.ready;",
      "var value = Status.Ready < Status.ready;",
      "if (Status.Ready) {}",
      "var value = Status.Ready == Other.Ready;",
      "var value = Status.Ready.ready;",
      "var value = Status::Ready;",
      "var value = Status.Missing;",
      "var value = [Status.Ready, Other.Ready];",
      "Status value = Status.Ready; value++;",
      "Status value = Status.Ready; value += 1;",
      "var value = Status.Ready is object;",
      "var value = Status.Ready as object;",
      "var value = Status.Ready!;"};
  for (const auto& body : bodies) {
    cloth::Compilation compilation;
    add(compilation, "Status.co", "enum { Ready, ready }");
    add(compilation, "Other.co", "enum { Ready }");
    add(compilation, "Main.co", "static func Main() { " + body + " }");
    cloth::DiagnosticEngine diagnostics;
    auto result = compilation.analyze(diagnostics);
    test.expect(!result.is_valid && diagnostics.has_errors(),
                "accepted invalid enum use: " + body);
  }
  const std::vector<std::string> declarations{"enum {}",
                                              "enum { Ready, Ready }",
                                              "enum { Ready = 1 }",
                                              "enum { Ready(int32 value) }",
                                              "enum { func Run() {} }",
                                              "enum { Ready } int32 field;",
                                              "enum { Ready",
                                              "enum { ,Ready }",
                                              "enum { Ready Other }",
                                              "enum : Parent { Ready }",
                                              "enum is Parent { Ready }",
                                              "abstract enum { Ready }",
                                              "enum Status { Ready }"};
  for (const auto& declaration : declarations) {
    cloth::Compilation compilation;
    add(compilation, "Status.co", declaration);
    cloth::DiagnosticEngine diagnostics;
    auto result = compilation.analyze(diagnostics);
    test.expect(!result.is_valid && diagnostics.has_errors(),
                "accepted invalid enum declaration: " + declaration);
  }
}

void parser_boundaries(TestContext& test) {
  std::string text = "enum {";
  for (std::size_t i = 0; i < cloth::kMaxEnumCases; ++i) {
    text += "Case" + std::to_string(i) + ",";
  }
  for (const bool oversized : {false, true}) {
    const auto source = cloth::SourceFile::from_memory(
        "Status.co", text + (oversized ? "Overflow}" : "}"));
    cloth::DiagnosticEngine diagnostics;
    const auto tokens = cloth::Lexer{source, diagnostics}.lex();
    const auto parsed = cloth::Parser{source, tokens, diagnostics}.parse();
    test.expect(parsed.is_valid != oversized &&
                    parsed.file_class.enum_cases.size() == cloth::kMaxEnumCases,
                "enum case limit is not enforced at the declaration boundary");
  }
}

void imports_and_identity(TestContext& test) {
  for (const bool reverse : {false, true}) {
    cloth::Compilation compilation;
    compilation.set_package_dependencies(
        {{"app", "left", "first"}, {"app", "right", "second"}});
    const auto source = [&](std::string package) {
      compilation.add_package_source(
          cloth::SourceFile::from_memory("Status.co",
                                         "enum { Ready, ready, _Done }"),
          std::move(package), "", "1.0.0");
    };
    source(reverse ? "second" : "first");
    source(reverse ? "first" : "second");
    compilation.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
      import left::Status as Left;
      import right::Status as Right;
      static func Select(Left value): int32 { return 1; }
      static func Select(Right value): int32 { return 2; }
      static func Main() { println(Left.ready); println(Right._Done);
        println(Select(Left.Ready)); println(Select(Right.Ready)); }
    )"),
                                   "app", "", "1.0.0");
    cloth::DiagnosticEngine diagnostics;
    const auto result = compilation.analyze(diagnostics);
    test.expect(result.is_valid, messages(diagnostics));
    test.expect(result.semantics.find_type("first.Status") !=
                    result.semantics.find_type("second.Status"),
                "package identity or discovery order collapsed enums");
  }
  cloth::Compilation private_type;
  add(private_type, "state.co", "enum { Ready, ready, _Done }");
  add(private_type, "Main.co", "static func Main() { println(state.ready); }");
  cloth::DiagnosticEngine diagnostics;
  test.expect(!private_type.analyze(diagnostics).is_valid,
              "public enum case exposed an inaccessible enum type");
}

void invalid_ir(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Status.co", "enum { Ready, ready }");
  add(compilation, "Main.co",
      "static func Main() { Status value = Status.Ready; "
      "println(value == Status.ready); }");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto type = *result.semantics.find_type("Status");
  const auto range =
      result.semantics
          .symbol(
              result.semantics.file(*result.semantics.type(type).file).symbol)
          .range;
  for (const auto& literal : std::vector<cloth::HirLiteralExpression>{
           {cloth::LiteralKind::kEnum, "2"},
           {cloth::LiteralKind::kEnum, "01"},
           {cloth::LiteralKind::kEnum, "-1"},
           {cloth::LiteralKind::kInteger, "0"}}) {
    auto broken = result.hir;
    static_cast<void>(broken.storage.add_expression({type, range, literal}));
    cloth::DiagnosticEngine invalid;
    test.expect(!cloth::verify_hir(broken, result.semantics, invalid),
                "HIR accepted malformed enum literal");
  }
  auto broken_hir = result.hir;
  const auto value = broken_hir.storage.add_expression(
      {type, range,
       cloth::HirLiteralExpression{cloth::LiteralKind::kEnum, "0"}});
  static_cast<void>(broken_hir.storage.add_expression(
      {type, range,
       cloth::HirBinaryExpression{value, cloth::TokenKind::kPlus, value}}));
  cloth::DiagnosticEngine hir_diagnostics;
  test.expect(!cloth::verify_hir(broken_hir, result.semantics, hir_diagnostics),
              "HIR accepted arithmetic on enum values");

  for (int mutation = 0; mutation < 4; ++mutation) {
    auto broken = result.mir;
    bool changed = false;
    for (auto& file : broken.files) {
      for (auto& function : file.functions) {
        for (auto& block : function.body.blocks) {
          for (auto& instruction : block.instructions) {
            if (auto* literal = std::get_if<cloth::MirLiteralInstruction>(
                    &instruction.data);
                mutation < 2 && literal &&
                literal->kind == cloth::LiteralKind::kEnum) {
              if (mutation == 0)
                literal->lexeme = "2";
              else
                literal->kind = cloth::LiteralKind::kInteger;
              changed = true;
            } else if (auto* binary = std::get_if<cloth::MirBinaryInstruction>(
                           &instruction.data);
                       mutation == 2 && binary) {
              binary->operation = cloth::TokenKind::kPlus;
              changed = true;
            } else if (auto* local =
                           std::get_if<cloth::MirDeclareLocalInstruction>(
                               &instruction.data);
                       mutation == 3 && local) {
              local->initializer.reset();
              changed = true;
            }
          }
        }
      }
    }
    cloth::DiagnosticEngine invalid;
    test.expect(
        changed && !cloth::verify_mir(broken, result.semantics, invalid),
        "MIR accepted malformed enum operation or initialization");
  }
  auto broken_abi = result.abi;
  broken_abi.types[type.value].kind = cloth::AbiTypeKind::kReference;
  cloth::DiagnosticEngine abi_diagnostics;
  test.expect(!cloth::verify_abi(broken_abi, result.mir, result.semantics,
                                 abi_diagnostics),
              "ABI accepted enum reference storage");
}

void initialization(TestContext& test) {
  cloth::Compilation valid;
  add(valid, "Status.co", "enum { Ready }");
  add(valid, "Main.co",
      "final Status State; Main(bool value) { "
      "if (value) { State = Status.Ready; return; } State = Status.Ready; }");
  cloth::DiagnosticEngine diagnostics;
  test.expect(valid.analyze(diagnostics).is_valid, messages(diagnostics));
  const std::vector<std::string> invalid{
      "Status State;",
      "Status State; Main() {}",
      "Status State; Main(bool value) { if (value) { State = Status.Ready; } }",
      "Status State; Main(bool value) { if (value) { return; } State = "
      "Status.Ready; }",
      "Status State; Main() { println(State); State = Status.Ready; }",
      "Status State; Main() { var value = self; State = Status.Ready; }",
      "Status State; func Read() {} Main() { Read(); State = Status.Ready; }",
      "Status State; Main() { while (false) { State = Status.Ready; } }",
      "final Status State; Main() { State = Status.Ready; State = "
      "Status.Ready; }",
      "static final Status A = Status.Ready; static final Status B = A;"};
  for (const auto& declaration : invalid) {
    cloth::Compilation compilation;
    add(compilation, "Status.co", "enum { Ready }");
    add(compilation, "Main.co", declaration);
    cloth::DiagnosticEngine diagnostics;
    auto result = compilation.analyze(diagnostics);
    test.expect(!result.is_valid,
                "accepted invalid enum initialization: " + declaration);
  }
}

void artifact_round_trip(TestContext& test) {
  cloth::Compilation compilation;
  constexpr std::string_view source = "enum { Ready, ready, _Done }";
  compilation.add_package_source(
      cloth::SourceFile::from_memory("Status.co", std::string{source}),
      "models", "", "1.0.0");
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  auto imported = cloth::build_imported_package_view(
      {"models", "1.0.0"}, result.semantics, result.mir, result.abi);
  std::string issues;
  for (const auto& issue : imported.issues) issues += issue.message + "\n";
  test.expect(imported.is_valid(), issues);
  if (!imported.view) return;
  cloth::PackageArtifact artifact{
      cloth::PackageArtifactKind::kInterface,
      {cloth::kCompilerAbiVersion, cloth::kRuntimeAbiVersion,
       cloth::sha256("enum-test"), result.abi.target, std::nullopt},
      {{"Status.co", cloth::sha256(source)}},
      {},
      *imported.view,
      {},
      {}};
  auto encoded = cloth::write_package_artifact(artifact);
  test.expect(encoded.is_valid(), "enum artifact encoding failed");
  if (!encoded.is_valid()) return;
  auto decoded = cloth::read_package_artifact(encoded.artifact->bytes);
  test.expect(decoded.is_valid(), "enum artifact decoding failed");
  if (!decoded.artifact) return;
  test.expect(decoded.artifact->imported == *imported.view,
              "enum metadata changed on round trip");
  auto broken = *imported.view;
  broken.files[0].enum_cases[0].tag = 20;
  test.expect(!cloth::verify_imported_package_view(broken).empty(),
              "accepted invalid enum tag");
  broken = *imported.view;
  broken.files[0].enum_cases[1].identity =
      broken.files[0].enum_cases[0].identity;
  test.expect(!cloth::verify_imported_package_view(broken).empty(),
              "accepted wrong case identity");
  broken = *imported.view;
  broken.files[0].enum_cases.clear();
  test.expect(!cloth::verify_imported_package_view(broken).empty(),
              "accepted empty imported enum");
  broken = *imported.view;
  auto& reserved = broken.files[0].enum_cases[0];
  reserved.name = "func";
  reserved.identity = cloth::canonical_member_identity(
      broken.files[0].nominal_identity, cloth::CanonicalMemberKind::kEnumCase,
      reserved.name);
  test.expect(!cloth::verify_imported_package_view(broken).empty(),
              "accepted keyword as imported case name");
  broken = *imported.view;
  broken.files[0].enum_cases[1].name = broken.files[0].enum_cases[0].name;
  test.expect(!cloth::verify_imported_package_view(broken).empty(),
              "accepted duplicate imported enum case");
  broken = *imported.view;
  broken.files[0].abi.descriptor->kind = cloth::AbiHeapObjectKind::kArray;
  test.expect(!cloth::verify_imported_package_view(broken).empty(),
              "accepted enum heap descriptor confusion");
  broken = *imported.view;
  broken.files[0].abi.descriptor->reference_offsets = {0};
  test.expect(!cloth::verify_imported_package_view(broken).empty(),
              "accepted enum GC reference slot");
  cloth::Compilation consumer;
  consumer.set_package_dependencies({{"app", "dep", "models"}});
  consumer.add_imported_package(decoded.artifact->imported);
  consumer.add_package_source(
      cloth::SourceFile::from_memory(
          "Main.co",
          "import dep::Status as State; static func Main() { "
          "println(State.ready); println(State._Done); }"),
      "app", "", "1.0.0");
  cloth::DiagnosticEngine consumer_diagnostics;
  auto consumed = consumer.analyze(consumer_diagnostics);
  test.expect(consumed.is_valid, messages(consumer_diagnostics));
  if (consumed.is_valid) {
    auto llvm = cloth::emit_llvm_ir(consumed.mir, consumed.abi,
                                    consumed.semantics, consumer_diagnostics);
    test.expect(llvm.has_value(), messages(consumer_diagnostics));
  }
}

void imported_constants(TestContext& test) {
  cloth::Compilation compilation;
  compilation.add_package_source(
      cloth::SourceFile::from_memory("Status.co", "enum { Ready }"), "models",
      "", "1.0.0");
  compilation.add_package_source(
      cloth::SourceFile::from_memory(
          "Defaults.co", "static final Status Initial = Status.Ready;"),
      "models", "", "1.0.0");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto imported = cloth::build_imported_package_view(
      {"models", "1.0.0"}, result.semantics, result.mir, result.abi);
  test.expect(imported.is_valid(), "static enum export failed");
  if (!imported.view) return;
  for (const std::string value : {"1", "65536", "01", "-1"}) {
    auto broken = *imported.view;
    for (auto& file : broken.files) {
      for (auto& member : file.members) {
        if (member.static_value) member.static_value->lexeme = value;
      }
    }
    test.expect(!cloth::verify_imported_package_view(broken).empty(),
                "accepted invalid exported enum constant: " + value);
  }
}
}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"named enum values", named_values},
      {"invalid enum programs", invalid_programs},
      {"enum parser boundaries", parser_boundaries},
      {"enum imports and identity", imports_and_identity},
      {"enum initialization", initialization},
      {"invalid enum IR", invalid_ir},
      {"enum artifact round trip", artifact_round_trip},
      {"imported enum constants", imported_constants}};
  return cloth::test::run_tests(tests);
}
