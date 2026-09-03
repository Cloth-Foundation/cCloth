// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/sema/numeric_types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
  for (const auto& diagnostic : diagnostics.diagnostics())
    result += diagnostic.message + "\n";
  return result;
}

void add(cloth::Compilation& compilation, std::string name, std::string text) {
  compilation.add_source(
      cloth::SourceFile::from_memory(std::move(name), std::move(text)));
}

void check(TestContext& test, std::string source, std::string_view error = {}) {
  cloth::Compilation compilation;
  add(compilation, "Example.co", source);
  add(compilation, "State.co", "enum { Ready, running, Done }");
  add(compilation, "Other.co", "enum { Ready }");
  add(compilation, "Constants.co",
      "static final int8 Small = 7; static final int64 Wide = 7; static final "
      "int32 hidden = 1; static final State Initial = State.Ready;");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  const auto detail = messages(diagnostics);
  test.expect(result.is_valid == error.empty(), source + "\n" + detail);
  if (!error.empty())
    test.expect(detail.find(error) != std::string::npos,
                "missing diagnostic: " + std::string{error} + "\n" + detail);
  test.expect(
      detail.find("internal HIR") == std::string::npos,
      "source diagnostics leaked an internal verification error\n" + detail);
  if (result.is_valid) {
    cloth::DiagnosticEngine native_errors;
    const auto native = compilation.analyze(native_errors);
    test.expect(native.is_valid,
                "MIR/ABI lowering failed\n" + messages(native_errors));
  }
}

void syntax_and_constants(TestContext& test) {
  for (const auto word : {"switch", "case", "default"}) {
    const auto kind = cloth::identifier_token_kind(word);
    test.expect(kind != cloth::TokenKind::kIdentifier &&
                    cloth::token_kind_name(kind) == word,
                "keyword classification/name mismatch");
  }
  check(test, R"(
    static func Read(State state): int32 {
      switch (state) {
        case State.Ready, (State.running): { int32 result = 1; return result; }
        case State.Done: { int32 result = 2; return result; }
      }
    }
    static func Number(int64 value) {
      switch (value) { case Constants.Small, -9223372036854775808, 9223372036854775807: {} }
      switch (value) { case Local: {} default: {} }
    }
    static final int32 Local = 3;
    static func Unsigned(uint64 value) {
      switch (value) { case 18446744073709551615, 0000: {} }
    }
    static func Fallback(State value) { switch (value) { default: { break; } } }
    static func EnumConstant(State value) {
      switch (value) { case Constants.Initial: {} case State.running, State.Done: {} }
    }
  )");
  for (const auto type : {"int8", "int16", "int32", "int64", "uint8", "uint16",
                          "uint32", "uint64", "int", "uint", "byte"}) {
    check(test, "static func Read(" + std::string{type} +
                    " value) { switch (value) { case (1): {} } }");
  }
  check(test,
        "static final int32 Rounded = int32(1.9); static func Read(int64 n) { "
        "switch (n) { case Rounded: {} } }");
  check(test, "static func Read(int32 n) { switch (n) { case 1, 01: {} } }",
        "duplicate switch case");
  check(test,
        "static func Read(int32 n) { switch (n) { case -0: {} case 0: {} } }",
        "duplicate switch case");
  check(test,
        "static func Read(int64 n) { switch (n) { case 7, Constants.Small: {} "
        "} }",
        "duplicate switch case");
  check(test,
        "static func Read(State n) { switch (n) { case State.Ready, "
        "Constants.Initial: {} default: {} } }",
        "duplicate switch case");
  check(test, "static func Read(int8 n) { switch (n) { case 128: {} } }",
        "out of range");
  check(test,
        "static func Read(uint64 n) { switch (n) { case 18446744073709551616: "
        "{} } }",
        "out of range");
  check(test, "static func Read(uint32 n) { switch (n) { case -0: {} } }",
        "out of range");
  check(test,
        "static func Read(int32 n) { switch (n) { case Constants.Wide: {} } }",
        "cannot be used");
  check(
      test,
      "static func Read(uint64 n) { switch (n) { case Constants.Small: {} } }",
      "cannot be used");
  check(
      test,
      "static func Read(int32 n) { switch (n) { case Constants.hidden: {} } }",
      "private");
}

void invalid_forms(TestContext& test) {
  for (const auto selector :
       {"true", "'x'", "1.0", "\"hello\"", "[1, 2]", "null"}) {
    check(test,
          "static func Read() { switch (" + std::string{selector} +
              ") { default: {} } }",
          "selector must be an enum or integer");
  }
  check(test, "static func Read(Example? n) { switch (n) { default: {} } }",
        "selector must be an enum or integer");
  check(test,
        "static func Nothing() {} static func Read() { switch (Nothing()) { "
        "default: {} } }",
        "void");
  for (const auto label :
       {"+1", "-(-1)", "1 + 1", "int8(1)", "n", "f", "Next()", "n = 1", "n++",
        "true", "Constants.Small + 1", "-Constants.Small"}) {
    check(test,
          "static func Next(): int32 { return 1; } static func Read(int32 n) { "
          "final int32 f = 1; switch (n) { case " +
              std::string{label} + ": {} } }",
          "case label must be");
  }
  check(test,
        "static func Read(State n) { switch (n) { case Other.Ready: {} "
        "default: {} } }",
        "cannot be used");
  check(
      test,
      "static func Read(State n) { switch (n) { case Ready: {} default: {} } }",
      "unknown name");
  check(test,
        "static func Read(State n) { switch (n) { case State.Ready: {} } }",
        "missing cases: running, Done");
  check(test,
        "static func Read(State n): int32 { switch (n) { case State.Ready, "
        "State.running, State.Done: { return 1; } default: {} } }",
        "does not return a value");
  check(test,
        "static func Read(int32 n): int32 { switch (n) { case 1: { return 1; } "
        "} }",
        "does not return a value");
  check(test,
        "static func Read(int32 n) { switch (n) { case 0: { int32 local = 1; } "
        "default: { println(local); } } }",
        "unknown name 'local'");
  check(test,
        "static func Read(int32 n) { switch (n) { default: { continue; } } }",
        "'continue' is only valid inside a loop");
}

void parser_recovery(TestContext& test) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"switch (1) {}", "requires at least one arm"},
      {"switch 1 { default: {} }", "expected '('"},
      {"switch (1) { case 1 {} }", "expected ':'"},
      {"switch (1) { case 1: break; default: {} }", "before switch arm body"},
      {"switch (1) { case 1,: {} }", "no trailing comma"},
      {"switch (1) { case : {} }", "expected case label"},
      {"switch (1) { default: {} default: {} }", "only one default"},
      {"switch (1) { default: {} case 1: {} }", "default must be the last"},
      {"switch (1) { int32 x = 1; case 1: {} }",
       "expected 'case' or 'default'"},
      {"switch (1) { case 1: case 2: {} }", "before switch arm body"},
      {"case 1: {}", "only valid directly inside a switch"},
      {"default: {}", "only valid directly inside a switch"},
      {"switch (1) { case 1: { default: {} } }",
       "only valid directly inside a switch"},
      {"var value = switch (1) { default: {} };", "expected expression"},
  };
  for (const auto& [body, error] : cases)
    check(test, "static func Read() { " + body + " } static func After() {}",
          error);
}

void flow_and_initialization(TestContext& test) {
  check(test, R"(
    static func Infinite(int32 n): int32 {
      while (true) { switch (n) { default: { break; } } }
    }
    static func Loops(int32 n): int32 {
      for (;;) { switch (n) { default: { continue; } } }
    }
    static func Read(int32 n, string? text) {
      switch (n) {
        case 0: { if (!text) { return; } break; }
        default: { if (!text) { return; } }
      }
      println(text::length);
    }
    static func Nested(int32 n) {
      for (int32 i = 0; i < 4; i++) {
        switch (n) { case 0: { continue; } default: { while (true) { break; } break; } }
      }
      for (var item in [1, 2]) { switch (item) { default: { continue; } } }
    }
  )");
  check(test,
        "static func Read(int32 n, string? text) { switch (n) { case 0: { if "
        "(!text) { return; } } } println(text::length); }",
        "nullable");
  check(test,
        "static func Read(int32 n, string? text) { if (text) { switch (n) { "
        "default: { text = null; break; } } println(text::length); } }",
        "nullable");
  check(test,
        "static func Read(int32 n, string? text) { if (text) { switch (n) { "
        "default: { break; text = null; } } println(text::length); } }");
  check(test,
        "static func Read(int32 n, string? text) { switch (n) { default: { "
        "break; if (!text) { return; } } } println(text::length); }",
        "nullable");
  check(test,
        "static func Read(int32 n): int32 { switch (n) { default: { while "
        "(true) { break; } } } }",
        "does not return a value");
  check(test, R"(
    final string Text;
    Example(State state, bool early) {
      switch (state) {
        case State.Ready: { Text = "ready"; if (early) { break; } }
        case State.running: { Text = "running"; return; }
        case State.Done: { switch (0) { default: { Text = "done"; break; } } }
      }
    }
  )");
  check(test,
        "final int32 Value; Example(int32 n) { switch (n) { case 0: { Value = "
        "1; } } }",
        "exits before final field");
  check(test,
        "final int32 Value; Example(int32 n, bool early) { switch (n) { "
        "default: { if (early) { break; } Value = 1; } } }",
        "exits before final field");
  check(test,
        "final int32 Value; Example(int32 n) { switch (n) { default: { break; "
        "Value = 1; } } }",
        "exits before final field");
  check(test,
        "final int32 Value; Example(int32 n) { switch (n) { default: { Value = "
        "1; break; } } Value = 2; }",
        "may only be initialized once");
  check(test,
        "final int32 Value; Example(int32 n) { while (true) { switch (n) { "
        "default: { Value = 1; } } break; } }",
        "loop");
  check(test,
        "final int32 Value; Example(int32 n) { switch (n) { default: { for "
        "(;;) {} } } }");
  check(test,
        "struct { int32 Value; Example(State s) { switch (s) { case "
        "State.Ready: { Value = 1; } default: { Value = 2; } } } }");
}

void typed_hir(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Example.co",
      "static func Read(int8 n) { switch (n) { case -1, 1: {} default: { "
      "break; } } }");
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze_frontend(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto statements = result.hir.storage.statements();
  const auto found =
      std::ranges::find_if(statements, [](const auto& statement) {
        return std::holds_alternative<cloth::HirSwitchStatement>(
            statement.data);
      });
  test.expect(found != statements.end(), "switch missing from HIR");
  if (found == statements.end()) return;
  const auto index = static_cast<std::size_t>(found - statements.begin());
  const auto& selection = std::get<cloth::HirSwitchStatement>(found->data);
  test.expect(selection.arms.size() == 2 &&
                  selection.arms[0].labels.size() == 2 &&
                  selection.arms[0].labels[0].value.bits == 255 &&
                  selection.is_exhaustive,
              "normalized grouped HIR is incorrect");
  const std::vector<std::function<void(cloth::HirSwitchStatement&)>>
      corruptions{
          [](auto& node) { node.selector.value = 999999; },
          [](auto& node) { node.selector_type.value = 999999; },
          [](auto& node) { node.arms[0].body.value = 999999; },
          [](auto& node) { node.arms[0].labels[0].value.bits = 256; },
          [](auto& node) {
            node.arms[0].labels[1].value = node.arms[0].labels[0].value;
          },
          [](auto& node) { node.arms[0].labels.clear(); },
          [](auto& node) { node.arms[0].is_default = true; },
          [](auto& node) { node.arms[0].body = node.arms[1].body; },
          [](auto& node) {
            node.arms[0].labels[0].symbol = cloth::SymbolId{999999};
          },
          [](auto& node) { node.is_exhaustive = false; },
          [](auto& node) { node.arms.clear(); },
          [](auto& node) {
            node.arms[0].labels.resize(cloth::kMaxSwitchLabels + 1,
                                       node.arms[0].labels[0]);
          },
      };
  for (const auto& corrupt : corruptions) {
    auto broken = result.hir;
    // The copy is mutable; storage intentionally exposes read-only public
    // views.
    auto& statement =
        const_cast<cloth::HirStatement&>(broken.storage.statement({index}));
    corrupt(std::get<cloth::HirSwitchStatement>(statement.data));
    cloth::DiagnosticEngine errors;
    test.expect(!cloth::verify_hir(broken, result.semantics, errors),
                "malformed switch HIR was accepted");
  }
  cloth::DiagnosticEngine native_errors;
  const auto native = compilation.analyze(native_errors);
  test.expect(native.is_valid, messages(native_errors));
}

void typed_mir(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Example.co",
      "static func Read(int8 n) { switch (n) { case -1, 1: {} default: {} } } "
      "static func Enum(State n) { switch (n) { case State.Ready: {} default: "
      "{} } }");
  add(compilation, "State.co", "enum { Ready, Done }");
  add(compilation, "Other.co", "enum { Ready, Done }");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto& body = result.mir.files[0].functions[0].body;
  const auto& selection =
      std::get<cloth::MirSwitchTerminator>(body.blocks[0].terminator.data);
  test.expect(selection.cases[0].value.bits == 1 &&
                  selection.cases[1].value.bits == 255 &&
                  selection.cases[0].target == selection.cases[1].target,
              "MIR cases are not normalized, sorted and grouped");
  test.expect(cloth::mir_successors(body.blocks[0].terminator).size() == 2,
              "grouped labels duplicate CFG successors");
  const auto reject = [&](std::size_t function, const auto& corrupt) {
    auto broken = result.mir;
    corrupt(broken.files[0].functions[function].body);
    cloth::DiagnosticEngine errors;
    test.expect(!cloth::verify_mir(broken, result.semantics, errors),
                "malformed switch MIR was accepted");
  };
  const std::vector<std::function<void(cloth::MirSwitchTerminator&)>>
      corruptions{
          [](auto& s) { s.selector.value = 999999; },
          [](auto& s) { s.selector_type.value = 999999; },
          [&](auto& s) { s.selector_type = result.semantics.bool_type(); },
          [](auto& s) { s.cases[0].value.type.value = 999999; },
          [](auto& s) { s.cases[1].value.bits = 256; },
          [](auto& s) { s.cases[1].value = s.cases[0].value; },
          [](auto& s) { std::swap(s.cases[0], s.cases[1]); },
          [](auto& s) { s.cases[0].target.value = 999999; },
          [](auto& s) { s.default_block.value = 999999; },
          [](auto& s) { s.invalid_block = s.default_block; },
          [](auto& s) {
            s.cases.resize(cloth::kMaxSwitchLabels + 1, s.cases[0]);
          },
      };
  for (const auto& corrupt : corruptions)
    reject(0, [&](auto& b) {
      corrupt(
          std::get<cloth::MirSwitchTerminator>(b.blocks[0].terminator.data));
    });
  reject(0, [](auto& b) { b.blocks[1].is_reachable = false; });
  reject(0, [](auto& b) {
    auto& s = std::get<cloth::MirSwitchTerminator>(b.blocks[0].terminator.data);
    b.blocks[s.cases[0].target.value].instructions.push_back(
        b.blocks[0].instructions.back());
    b.blocks[0].instructions.pop_back();
  });
  for (const auto& corrupt :
       std::vector<std::function<void(cloth::MirSwitchTerminator&)>>{
           [](auto& s) { s.invalid_block.reset(); },
           [](auto& s) { s.invalid_block = cloth::MirBlockId{999999}; },
           [](auto& s) { s.invalid_block = s.default_block; },
           [](auto& s) { s.default_block = *s.invalid_block; },
           [](auto& s) { s.cases[0].value.bits = 2; },
           [&](auto& s) {
             s.cases[0].value.type = result.semantics.file({2}).type;
           }})
    reject(1, [&](auto& b) {
      corrupt(
          std::get<cloth::MirSwitchTerminator>(b.blocks[0].terminator.data));
    });
  reject(1, [](auto& b) {
    const auto& s =
        std::get<cloth::MirSwitchTerminator>(b.blocks[0].terminator.data);
    b.blocks[s.invalid_block->value].terminator.data =
        cloth::MirUnreachableTerminator{};
  });

  auto with_phi = result.mir;
  auto& phi_body = with_phi.files[0].functions[0].body;
  const auto destination = selection.cases[0].target;
  phi_body.blocks[destination.value].instructions.insert(
      phi_body.blocks[destination.value].instructions.begin(),
      cloth::MirInstruction{
          cloth::MirValueId{phi_body.value_count++}, selection.selector_type,
          body.range, cloth::MirPhiInstruction{{{{0}, selection.selector}}}});
  cloth::DiagnosticEngine phi_errors;
  test.expect(cloth::verify_mir(with_phi, result.semantics, phi_errors),
              messages(phi_errors));
  const auto llvm =
      cloth::emit_llvm_ir(with_phi, result.abi, result.semantics, phi_errors);
  test.expect(llvm && llvm->text.contains("switch i8") &&
                  llvm->text.contains("%edge.0.") &&
                  llvm->text.contains("icmp ult i32"),
              "LLVM lost multiway dispatch, unique phi edges or enum guard");
  auto& phi = std::get<cloth::MirPhiInstruction>(
      phi_body.blocks[destination.value].instructions[0].data);
  phi.incoming.push_back(phi.incoming[0]);
  test.expect(!cloth::verify_mir(with_phi, result.semantics, phi_errors),
              "duplicate phi predecessor accepted for grouped labels");
}

void mir_constructor_paths(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Data.co",
      "struct { final int32 Value; Data(int32 n) { switch (n) { "
      "case 0, 1: { Value = 1; break; } default: { Value = 2; return; } } } }");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  auto broken = result.mir;
  auto& body = broken.files[0].constructors[0].body;
  const auto& selection =
      std::get<cloth::MirSwitchTerminator>(body.blocks[0].terminator.data);
  auto& instructions = body.blocks[selection.default_block.value].instructions;
  const auto removed = std::erase_if(instructions, [](const auto& instruction) {
    return std::holds_alternative<cloth::MirStoreStorageInstruction>(
        instruction.data);
  });
  test.expect(removed == 1,
              "constructor fixture did not contain one initialization");
  cloth::DiagnosticEngine errors;
  test.expect(!cloth::verify_mir(broken, result.semantics, errors) &&
                  messages(errors).contains("incomplete struct self"),
              "MIR initialization missed the switch default return path");
}

void source_free_constants(TestContext& test) {
  cloth::Compilation producer;
  producer.add_package_source(cloth::SourceFile::from_memory(
                                  "State.co", "enum { Ready, running, Done }"),
                              "model", "", "1.0.0");
  producer.add_package_source(
      cloth::SourceFile::from_memory(
          "Constants.co",
          "static final State Initial = State.Ready; static final int8 Small = "
          "7; static final uint64 Maximum = 18446744073709551615; static final "
          "int32 Rounded = int32(1.9);"),
      "model", "", "1.0.0");
  cloth::DiagnosticEngine diagnostics;
  const auto produced = producer.analyze(diagnostics);
  test.expect(produced.is_valid, messages(diagnostics));
  if (!produced.is_valid) return;
  auto imported = cloth::build_imported_package_view(
      {"model", "1.0.0"}, produced.semantics, produced.mir, produced.abi);
  test.expect(imported.is_valid(), "failed to export switch dependencies");
  if (!imported.view) return;
  const std::vector<std::pair<std::string, bool>> cases{
      {"switch (state) { case Constants.Initial: {} case Status.running, "
       "Status.Done: {} }",
       true},
      {"switch (state) { case Status.Ready: {} }", false},
      {"switch (state) { case Status.Ready, Constants.Initial: {} default: {} "
       "}",
       false},
      {"int64 n = 7; switch (n) { case Constants.Small: {} }", true},
      {"uint64 n = 0; switch (n) { case Constants.Maximum: {} }", true},
      {"int64 n = 0; switch (n) { case Constants.Rounded: {} }", true},
      {"int64 n = 0; switch (n) { case Constants.Rounded, 1: {} }", false},
  };
  for (const auto& [body, valid] : cases) {
    cloth::Compilation consumer;
    consumer.set_package_dependencies({{"app", "dep", "model"}});
    consumer.add_imported_package(*imported.view);
    consumer.add_package_source(
        cloth::SourceFile::from_memory(
            "Example.co",
            "import dep::State as Status; import dep::Constants; static func "
            "Read(Status state) { " +
                body + " }"),
        "app", "", "1.0.0");
    cloth::DiagnosticEngine errors;
    const auto checked = consumer.analyze(errors);
    test.expect(checked.is_valid == valid, body + "\n" + messages(errors));
    if (checked.is_valid) {
      const auto llvm = cloth::emit_llvm_ir(
          checked.mir, checked.abi, checked.semantics, errors,
          cloth::LlvmIrOptions{false, std::nullopt,
                               cloth::PackageIdentity{"app", "1.0.0"}});
      test.expect(
          llvm.has_value(),
          "source-free switch LLVM emission failed\n" + messages(errors));
    }
  }
}

void limits_and_normalization(TestContext& test) {
  for (const auto kind : {cloth::TypeKind::kInt8, cloth::TypeKind::kInt16,
                          cloth::TypeKind::kInt32, cloth::TypeKind::kInt64}) {
    const auto value = cloth::integer_constant_bits("1", true, kind);
    const auto widened = value ? cloth::widen_integer_constant(
                                     *value, kind, cloth::TypeKind::kInt64)
                               : std::nullopt;
    test.expect(widened == std::numeric_limits<std::uint64_t>::max(),
                "signed constant was not sign extended");
  }
  for (const std::size_t count :
       {cloth::kMaxSwitchLabels, cloth::kMaxSwitchLabels + 1}) {
    std::string source = "static func Read(uint32 n) { switch (n) { case ";
    for (std::size_t i = 0; i < count; ++i) {
      if (i) source += ",";
      source += std::to_string(i);
    }
    source += ": {} } }";
    check(test, std::move(source),
          count == cloth::kMaxSwitchLabels ? "" : "exceeds 65536 value labels");
  }
  std::string all_bytes =
      "static func Read(uint8 n): int32 { switch (n) { case ";
  for (int i = 0; i < 256; ++i) {
    if (i) all_bytes += ",";
    all_bytes += std::to_string(i);
  }
  all_bytes += ": { return 1; } } }";
  check(test, std::move(all_bytes), "does not return a value");

  std::string enum_source = "enum {";
  std::string source = "static func Read(Large value) { switch (value) {";
  for (std::size_t i = 0; i < cloth::kMaxSwitchLabels; ++i) {
    if (i) enum_source += ",";
    enum_source += "Case" + std::to_string(i);
    source += "case Large.Case" + std::to_string(i) + ": {}";
  }
  enum_source += "}";
  source += "default: {} } }";
  cloth::Compilation compilation;
  add(compilation, "Example.co", std::move(source));
  add(compilation, "Large.co", std::move(enum_source));
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, "maximum enum/label/arm boundary failed\n" +
                                   messages(diagnostics));
  if (result.is_valid) {
    const auto llvm = cloth::emit_llvm_ir(result.mir, result.abi,
                                          result.semantics, diagnostics);
    test.expect(llvm && llvm->text.contains("i32 65535, label %bb"),
                "maximum-size switch did not reach LLVM");
  }
}

void diagnostic_locations_and_bounds(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "State.co", "enum { A, B, C, D, E, F, G, H, I, J, K }");
  add(compilation, "Example.co",
      "static func Read(State value) { switch (value) { case State.A: {} } }\n"
      "static func Numbers(int32 value) {\n"
      "  switch (value) {\n"
      "    case 1: {}\n"
      "    case 01: {}\n"
      "  }\n"
      "}\n");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  test.expect(!result.is_valid, "missing and duplicate labels were accepted");
  const auto text = messages(diagnostics);
  test.expect(
      text.contains("missing cases: B, C, D, E, F, G, H, I (and 2 more)"),
      "missing-case diagnostic lost declaration order or bound\n" + text);
  bool saw_duplicate = false;
  bool saw_first = false;
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message.contains("duplicate switch case")) {
      saw_duplicate = diagnostic.range.begin.line == 5;
    }
    if (diagnostic.message.contains("first case with this value")) {
      saw_first = diagnostic.range.begin.line == 4;
    }
  }
  test.expect(saw_duplicate && saw_first,
              "duplicate-label error/note lost their source locations");
}
}  // namespace

int main() {
  const TestCase tests[]{
      {"switch syntax and constants", syntax_and_constants},
      {"switch invalid forms", invalid_forms},
      {"switch parser recovery", parser_recovery},
      {"switch flow and initialization", flow_and_initialization},
      {"switch typed HIR", typed_hir},
      {"switch typed MIR and CFG", typed_mir},
      {"switch MIR constructor paths", mir_constructor_paths},
      {"switch source-free constants", source_free_constants},
      {"switch limits and normalization", limits_and_normalization},
      {"switch diagnostic locations and bounds",
       diagnostic_locations_and_bounds},
  };
  return cloth::test::run_tests(tests);
}
