// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/identity/package_identity.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
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
    result += diagnostic.message;
    result += '\n';
  }
  return result;
}

const cloth::SemanticSymbol* find_function(
    const cloth::SemanticModel& semantics, const cloth::FileSemantics& file,
    std::string_view name) {
  const auto found =
      std::ranges::find_if(file.functions, [&](cloth::SymbolId symbol) {
        return semantics.symbol(symbol).name == name;
      });
  return found == file.functions.end() ? nullptr : &semantics.symbol(*found);
}

void add_prelude_sources(cloth::Compilation& compilation) {
  compilation.add_package_source(
      cloth::SourceFile::from_memory("PreludeClass.co", "class {}"), "cloth",
      "lang", "0.1.0");
  compilation.add_package_source(
      cloth::SourceFile::from_memory("PreludeError.co", "error {}"), "cloth",
      "lang.errors", "0.1.0");
  compilation.add_package_source(
      cloth::SourceFile::from_memory("PreludeInterface.co", "interface {}"),
      "cloth", "lang.contracts", "0.1.0");
  compilation.add_package_source(
      cloth::SourceFile::from_memory("PreludeEnum.co", "enum { Ready }"),
      "cloth", "lang.values.states", "0.1.0");
  compilation.add_package_source(
      cloth::SourceFile::from_memory("PreludeStruct.co",
                                     "struct { PreludeStruct() {} }"),
      "cloth", "lang.data", "0.1.0");
}

constexpr std::string_view kPreludeConsumer = R"(
  static func KeepClass(PreludeClass value): PreludeClass { return value; }
  static func KeepError(PreludeError value): PreludeError { return value; }
  static func KeepInterface(PreludeInterface value): PreludeInterface {
    return value;
  }
  static func KeepEnum(PreludeEnum value): PreludeEnum { return value; }
  static func KeepStruct(PreludeStruct value): PreludeStruct { return value; }
  static func Main() {}
)";

void expect_package_ir(TestContext& test, cloth::CompilationResult& result,
                       cloth::DiagnosticEngine& diagnostics,
                       std::string_view package, std::string_view failure,
                       std::string_view version = "0.1.0") {
  cloth::LlvmIrOptions options;
  options.package =
      cloth::PackageIdentity{std::string{package}, std::string{version}};
  test.expect(cloth::emit_llvm_ir(result.mir, result.abi, result.semantics,
                                  diagnostics, options)
                  .has_value(),
              std::string{failure});
}

void prelude_whole_and_source_free(TestContext& test) {
  for (const cloth::TargetDataLayout& target :
       {cloth::TargetDataLayout::llvm_x86_64(),
        cloth::TargetDataLayout::llvm_wasm32()}) {
    cloth::Compilation producer{target};
    add_prelude_sources(producer);
    cloth::DiagnosticEngine producer_diagnostics;
    cloth::CompilationResult produced = producer.analyze(producer_diagnostics);
    test.expect(produced.is_valid, messages(producer_diagnostics));
    if (!produced.is_valid) continue;
    expect_package_ir(test, produced, producer_diagnostics, "cloth",
                      "prelude producer IR is incomplete");

    const cloth::ImportedPackageResult imported =
        cloth::build_imported_package_view(
            {"cloth", "0.1.0"}, produced.semantics, produced.mir, produced.abi);
    test.expect(imported.is_valid(),
                "prelude package interface could not be exported");
    if (!imported.view) continue;

    cloth::ImportedPackageView malformed = *imported.view;
    malformed.files.front().nominal_identity.source_package = "outside";
    cloth::Compilation malformed_consumer{target};
    malformed_consumer.add_imported_package(std::move(malformed));
    malformed_consumer.add_package_source(
        cloth::SourceFile::from_memory("Main.co", "static func Main() {}"),
        "app", "", "0.1.0");
    cloth::DiagnosticEngine malformed_diagnostics;
    test.expect(!malformed_consumer.analyze(malformed_diagnostics).is_valid,
                "malformed prelude artifact was accepted:\n" +
                    messages(malformed_diagnostics));

    cloth::Compilation consumer{target};
    consumer.set_package_dependencies({{"app", "cloth", "cloth"}});
    consumer.add_imported_package(*imported.view);
    consumer.add_package_source(cloth::SourceFile::from_memory(
                                    "Main.co", std::string{kPreludeConsumer}),
                                "app", "", "0.1.0");
    cloth::DiagnosticEngine consumer_diagnostics;
    cloth::CompilationResult consumed = consumer.analyze(consumer_diagnostics);
    test.expect(consumed.is_valid, messages(consumer_diagnostics));
    if (consumed.is_valid) {
      expect_package_ir(test, consumed, consumer_diagnostics, "app",
                        "source-free prelude consumer IR is incomplete");
    }

    cloth::Compilation whole{target};
    whole.set_package_dependencies({{"app", "cloth", "cloth"}});
    add_prelude_sources(whole);
    whole.add_package_source(cloth::SourceFile::from_memory(
                                 "Main.co", std::string{kPreludeConsumer}),
                             "app", "", "0.1.0");
    cloth::DiagnosticEngine whole_diagnostics;
    cloth::CompilationResult whole_result = whole.analyze(whole_diagnostics);
    test.expect(whole_result.is_valid, messages(whole_diagnostics));
    if (whole_result.is_valid) {
      expect_package_ir(test, whole_result, whole_diagnostics, "app",
                        "whole-project prelude consumer IR is incomplete");
    }
  }
}

void prelude_precedence_and_boundaries(TestContext& test) {
  cloth::Compilation lexical_and_member;
  lexical_and_member.set_package_dependencies({{"app", "cloth", "cloth"}});
  lexical_and_member.add_package_source(
      cloth::SourceFile::from_memory("LocalName.co", "class {}"), "cloth",
      "lang", "0.1.0");
  lexical_and_member.add_package_source(
      cloth::SourceFile::from_memory("MemberName.co", "class {}"), "cloth",
      "lang", "0.1.0");
  lexical_and_member.add_package_source(
      cloth::SourceFile::from_memory("Main.co", R"(
        static final int32 MemberName = 4;
        static func Main() {
          int32 LocalName = 3;
          println(LocalName);
          println(MemberName);
        }
      )"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine lexical_diagnostics;
  test.expect(lexical_and_member.analyze(lexical_diagnostics).is_valid,
              messages(lexical_diagnostics));

  cloth::Compilation precedence;
  precedence.set_package_dependencies({{"app", "cloth", "cloth"}});
  precedence.add_package_source(
      cloth::SourceFile::from_memory(
          "Choice.co", "static func PreludeOnly(): int32 { return 1; }"),
      "cloth", "lang", "0.1.0");
  precedence.add_package_source(
      cloth::SourceFile::from_memory(
          "Choice.co", "static func LocalOnly(): int32 { return 2; }"),
      "app", "", "0.1.0");
  precedence.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
        import cloth.lang::Choice as StandardChoice;
        static func Main() {
          println(Choice.LocalOnly());
          println(StandardChoice.PreludeOnly());
        }
      )"),
                                "app", "", "0.1.0");
  cloth::DiagnosticEngine precedence_diagnostics;
  test.expect(precedence.analyze(precedence_diagnostics).is_valid,
              messages(precedence_diagnostics));

  cloth::Compilation explicit_import;
  explicit_import.set_package_dependencies(
      {{"app", "cloth", "cloth"}, {"app", "other", "other"}});
  explicit_import.add_package_source(
      cloth::SourceFile::from_memory(
          "Choice.co", "static func PreludeOnly(): int32 { return 1; }"),
      "cloth", "lang", "0.1.0");
  explicit_import.add_package_source(
      cloth::SourceFile::from_memory(
          "Choice.co", "static func ExplicitOnly(): int32 { return 2; }"),
      "other", "", "0.1.0");
  explicit_import.add_package_source(
      cloth::SourceFile::from_memory("Main.co", R"(
        import other::Choice;
        static func Main() { println(Choice.ExplicitOnly()); }
      )"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine explicit_diagnostics;
  test.expect(explicit_import.analyze(explicit_diagnostics).is_valid,
              messages(explicit_diagnostics));

  cloth::Compilation wildcard_import;
  wildcard_import.set_package_dependencies({{"app", "cloth", "cloth"}});
  wildcard_import.add_package_source(
      cloth::SourceFile::from_memory(
          "Choice.co", "static func PreludeOnly(): int32 { return 1; }"),
      "cloth", "lang", "0.1.0");
  wildcard_import.add_package_source(
      cloth::SourceFile::from_memory(
          "Choice.co", "static func WildcardOnly(): int32 { return 2; }"),
      "app", "tools", "0.1.0");
  wildcard_import.add_package_source(
      cloth::SourceFile::from_memory("Main.co", R"(
        import tools.*;
        static func Main() { println(Choice.WildcardOnly()); }
      )"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine wildcard_diagnostics;
  test.expect(wildcard_import.analyze(wildcard_diagnostics).is_valid,
              messages(wildcard_diagnostics));

  cloth::Compilation explicit_prelude;
  explicit_prelude.set_package_dependencies({{"app", "cloth", "cloth"}});
  explicit_prelude.add_package_source(
      cloth::SourceFile::from_memory(
          "Choice.co", "static func PreludeOnly(): int32 { return 1; }"),
      "cloth", "lang.deep", "0.1.0");
  explicit_prelude.add_package_source(
      cloth::SourceFile::from_memory("Main.co", R"(
        import cloth.lang.deep::Choice;
        static func Main() { println(Choice.PreludeOnly()); }
      )"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine explicit_prelude_diagnostics;
  test.expect(explicit_prelude.analyze(explicit_prelude_diagnostics).is_valid,
              messages(explicit_prelude_diagnostics));

  cloth::Compilation recursive_boundaries;
  recursive_boundaries.set_package_dependencies({{"app", "cloth", "cloth"}});
  recursive_boundaries.add_package_source(
      cloth::SourceFile::from_memory("hidden.co", "class {}"), "cloth",
      "lang.deep", "0.1.0");
  recursive_boundaries.add_package_source(
      cloth::SourceFile::from_memory("Nested.co", "class {}"), "cloth",
      "lang.deep.more", "0.1.0");
  recursive_boundaries.add_package_source(
      cloth::SourceFile::from_memory("Outside.co", "class {}"), "cloth",
      "language", "0.1.0");
  recursive_boundaries.add_package_source(
      cloth::SourceFile::from_memory("Main.co", R"(
        static func KeepHidden(hidden value): hidden { return value; }
        static func KeepNested(Nested value): Nested { return value; }
        static func KeepOutside(Outside value): Outside { return value; }
      )"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine boundary_diagnostics;
  const auto boundary_result =
      recursive_boundaries.analyze(boundary_diagnostics);
  const std::string boundary_messages = messages(boundary_diagnostics);
  test.expect(!boundary_result.is_valid &&
                  boundary_messages.contains("unknown type 'hidden'") &&
                  boundary_messages.contains("unknown type 'Outside'") &&
                  !boundary_messages.contains("unknown type 'Nested'"),
              boundary_messages);

  cloth::Compilation explicit_private;
  explicit_private.set_package_dependencies({{"app", "cloth", "cloth"}});
  explicit_private.add_package_source(
      cloth::SourceFile::from_memory("hidden.co", "class {}"), "cloth",
      "lang.deep", "0.1.0");
  explicit_private.add_package_source(
      cloth::SourceFile::from_memory("Main.co", R"(
        import cloth.lang.deep::hidden;
        static func Main() {}
      )"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine private_diagnostics;
  test.expect(
      !explicit_private.analyze(private_diagnostics).is_valid &&
          messages(private_diagnostics)
              .contains("file class 'cloth.lang.deep.hidden' is private"),
      messages(private_diagnostics));

  cloth::Compilation absent;
  absent.add_package_source(
      cloth::SourceFile::from_memory("Main.co",
                                     "static func Keep(PreludeClass value): "
                                     "PreludeClass { return value; }"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine absent_diagnostics;
  test.expect(
      !absent.analyze(absent_diagnostics).is_valid &&
          messages(absent_diagnostics).contains("unknown type 'PreludeClass'"),
      messages(absent_diagnostics));
}

void prelude_collisions(TestContext& test) {
  cloth::Compilation wildcard_collision;
  wildcard_collision.set_package_dependencies(
      {{"app", "cloth", "cloth"}, {"app", "other", "other"}});
  wildcard_collision.add_package_source(
      cloth::SourceFile::from_memory("Choice.co", "class {}"), "cloth", "lang",
      "0.1.0");
  wildcard_collision.add_package_source(
      cloth::SourceFile::from_memory("Choice.co", "class {}"), "other", "",
      "0.1.0");
  wildcard_collision.add_package_source(
      cloth::SourceFile::from_memory("Main.co", R"(
        import cloth.lang.*;
        import other.*;
        static func Main() {}
      )"),
      "app", "", "0.1.0");
  cloth::DiagnosticEngine wildcard_diagnostics;
  test.expect(!wildcard_collision.analyze(wildcard_diagnostics).is_valid &&
                  messages(wildcard_diagnostics)
                      .contains("import name 'Choice' is ambiguous"),
              messages(wildcard_diagnostics));

  auto duplicate_messages = [](bool reverse) {
    cloth::Compilation compilation;
    const auto add = [&](std::string_view package) {
      compilation.add_package_source(
          cloth::SourceFile::from_memory("Choice.co", "class {}"), "cloth",
          std::string{package}, "0.1.0");
    };
    if (reverse) {
      add("lang.two.deep");
      add("lang.one");
    } else {
      add("lang.one");
      add("lang.two.deep");
    }
    compilation.add_package_source(
        cloth::SourceFile::from_memory("Main.co", "static func Main() {}"),
        "app", "", "0.1.0");
    cloth::DiagnosticEngine diagnostics;
    static_cast<void>(compilation.analyze(diagnostics));
    return messages(diagnostics);
  };
  const std::string duplicate_forward = duplicate_messages(false);
  const std::string duplicate_reverse = duplicate_messages(true);
  test.expect(
      duplicate_forward == duplicate_reverse &&
          std::ranges::count(duplicate_forward, '\n') == 1 &&
          duplicate_forward.contains(
              "standard-library prelude name 'Choice' is ambiguous between "
              "'cloth.lang.one.Choice', 'cloth.lang.two.deep.Choice'"),
      duplicate_forward + duplicate_reverse);

  auto conflict_messages = [](bool reverse) {
    cloth::Compilation compilation;
    const auto add = [&](std::string_view name, std::string_view package) {
      compilation.add_package_source(
          cloth::SourceFile::from_memory(std::string{name} + ".co", "class {}"),
          "cloth", std::string{package}, "0.1.0");
    };
    if (reverse) {
      add("Error", "lang.errors.core");
      add("DivisionByZero", "lang.math.errors");
    } else {
      add("DivisionByZero", "lang.math.errors");
      add("Error", "lang.errors.core");
    }
    compilation.add_package_source(
        cloth::SourceFile::from_memory("One.co", "class {}"), "app", "",
        "0.1.0");
    compilation.add_package_source(
        cloth::SourceFile::from_memory("Two.co", "class {}"), "app", "",
        "0.1.0");
    cloth::DiagnosticEngine diagnostics;
    static_cast<void>(compilation.analyze(diagnostics));
    return messages(diagnostics);
  };
  const std::string forward = conflict_messages(false);
  const std::string reverse = conflict_messages(true);
  test.expect(
      forward == reverse && std::ranges::count(forward, '\n') == 2 &&
          forward.contains(
              "standard-library prelude type 'cloth.lang.errors.core.Error' "
              "conflicts with core type 'Error'") &&
          forward.contains("standard-library prelude type "
                           "'cloth.lang.math.errors.DivisionByZero' conflicts "
                           "with core type "
                           "'DivisionByZero'"),
      forward + reverse);
}

void lang_api_and_source_free_consumer(TestContext& test) {
  const std::filesystem::path source_directory =
      std::filesystem::path{CLOTH_STANDARD_LIBRARY_SOURCE_DIR} / "lang" /
      "errors";
  for (const cloth::TargetDataLayout& target :
       {cloth::TargetDataLayout::llvm_x86_64(),
        cloth::TargetDataLayout::llvm_wasm32()}) {
    cloth::Compilation producer{target};
    for (const std::string_view name : {"ArgumentError", "StateError"}) {
      auto source = cloth::SourceFile::load(source_directory /
                                            (std::string{name} + ".co"));
      test.expect(source.has_value(),
                  "standard-library lang source could not be read");
      if (!source) continue;
      producer.add_package_source(
          std::move(*source), "cloth", "lang.errors",
          std::string{cloth::kStandardLibraryPackageVersion});
    }

    cloth::DiagnosticEngine producer_diagnostics;
    cloth::CompilationResult produced = producer.analyze(producer_diagnostics);
    test.expect(produced.is_valid, messages(producer_diagnostics));
    if (!produced.is_valid) continue;
    for (const std::string_view name : {"ArgumentError", "StateError"}) {
      const std::string identity = "cloth.lang.errors." + std::string{name};
      const auto file = std::ranges::find_if(
          produced.semantics.files(), [&](const cloth::FileSemantics& value) {
            return produced.semantics.symbol(value.symbol).name == identity;
          });
      test.expect(file != produced.semantics.files().end() &&
                      file->kind == cloth::FileTypeKind::kError &&
                      file->constructors.size() == 2,
                  identity + " has the wrong public shape");
    }
    expect_package_ir(test, produced, producer_diagnostics, "cloth",
                      "cloth.lang producer IR is incomplete",
                      cloth::kStandardLibraryPackageVersion);

    const cloth::ImportedPackageResult imported =
        cloth::build_imported_package_view(
            {"cloth", std::string{cloth::kStandardLibraryPackageVersion}},
            produced.semantics, produced.mir, produced.abi);
    test.expect(imported.is_valid(),
                "cloth.lang package interface could not be exported");
    if (!imported.view) continue;

    cloth::Compilation consumer{target};
    consumer.set_package_dependencies({{"app", "cloth", "cloth"}});
    consumer.add_imported_package(*imported.view);
    consumer.add_package_source(
        cloth::SourceFile::from_memory("CustomError.co", R"(
          error : ArgumentError {
            CustomError(string message): ArgumentError(message) {}
          }
        )"),
        "app", "", "0.1.0");
    consumer.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
          static func CheckArgument(bool reject): int32 throws ArgumentError {
            if (reject) { throw CustomError("invalid argument"); }
            return 1;
          }
          static func CheckState(bool reject): int32 throws StateError {
            if (reject) { throw StateError("invalid state"); }
            return 2;
          }
          static func Main() throws ArgumentError, StateError {
            println(CheckArgument(false));
            println(CheckState(false));
          }
        )"),
                                "app", "", "0.1.0");
    cloth::DiagnosticEngine consumer_diagnostics;
    cloth::CompilationResult consumed = consumer.analyze(consumer_diagnostics);
    test.expect(consumed.is_valid, messages(consumer_diagnostics));
    if (consumed.is_valid) {
      expect_package_ir(test, consumed, consumer_diagnostics, "app",
                        "source-free cloth.lang consumer IR is incomplete");
    }
  }
}

void console_api_and_source_free_consumer(TestContext& test) {
  const std::filesystem::path source_root{CLOTH_STANDARD_LIBRARY_SOURCE_DIR};
  const auto add_console_sources = [&](cloth::Compilation& compilation,
                                       std::string_view version) {
    struct Source {
      std::filesystem::path path;
      std::string_view package;
    };
    const std::array sources{
        Source{source_root / "lang" / "errors" / "IoError.co", "lang.errors"},
        Source{source_root / "lang" / "errors" / "ParseError.co",
               "lang.errors"},
        Source{source_root / "io" / "Console.co", "io"},
    };
    bool loaded = true;
    for (const Source& source : sources) {
      auto file = cloth::SourceFile::load(source.path);
      test.expect(file.has_value(),
                  "standard-library console source could not be read");
      if (!file) {
        loaded = false;
        continue;
      }
      compilation.add_package_source(std::move(*file), "cloth",
                                     std::string{source.package},
                                     std::string{version});
    }
    return loaded;
  };

  constexpr std::string_view kConsumer = R"(
    import cloth.io::Console;
    static func Keep(ParseError value): ParseError { return value; }
    static func Read(): string? throws IoError {
      return Console.ReadLine();
    }
    func infer(string text): int32 {
      return int32::parse(text);
    }
    static func ParseValues() throws ParseError {
      bool boolean = bool::parse("true");
      char character = char::parse("C");
      byte octet = byte::parse("255");
      int8 signed8 = int8::parse("-128");
      int16 signed16 = int16::parse("-32768");
      int32 signed32 = int32::parse("-2147483648");
      int64 signed64 = int64::parse("-9223372036854775808");
      uint8 unsigned8 = uint8::parse("255");
      uint16 unsigned16 = uint16::parse("65535");
      uint32 unsigned32 = uint32::parse("4294967295");
      uint64 unsigned64 = uint64::parse("18446744073709551615");
      float32 single = float32::parse("1.5");
      float64 double = float64::parse("2.5e1");
      int aliasInt = int::parse("1");
      uint aliasUint = uint::parse("2");
      float aliasFloat = float::parse("3.5");
    }
    static func Main() throws IoError, ParseError {
      string? line = Read();
      ParseValues();
    }
  )";
  for (const cloth::TargetDataLayout& target :
       {cloth::TargetDataLayout::llvm_x86_64(),
        cloth::TargetDataLayout::llvm_wasm32()}) {
    cloth::Compilation producer{target};
    if (!add_console_sources(producer, cloth::kStandardLibraryPackageVersion)) {
      continue;
    }
    cloth::DiagnosticEngine producer_diagnostics;
    cloth::CompilationResult produced = producer.analyze(producer_diagnostics);
    test.expect(produced.is_valid, messages(producer_diagnostics));
    if (!produced.is_valid) continue;

    const auto find_file = [&](std::string_view name) {
      return std::ranges::find_if(
          produced.semantics.files(), [&](const cloth::FileSemantics& file) {
            return produced.semantics.symbol(file.symbol).name == name;
          });
    };
    const auto console = find_file("cloth.io.Console");
    const auto io_error = find_file("cloth.lang.errors.IoError");
    const auto parse_error = find_file("cloth.lang.errors.ParseError");
    test.expect(console != produced.semantics.files().end() &&
                    console->constructors.empty() &&
                    io_error != produced.semantics.files().end() &&
                    io_error->constructors.size() == 2 &&
                    parse_error != produced.semantics.files().end() &&
                    parse_error->constructors.size() == 2,
                "console or input error declarations have the wrong shape");
    if (console == produced.semantics.files().end() ||
        io_error == produced.semantics.files().end()) {
      continue;
    }
    const cloth::SemanticSymbol* read_line =
        find_function(produced.semantics, *console, "ReadLine");
    test.expect(read_line != nullptr && read_line->is_static &&
                    produced.semantics.type(read_line->type).kind ==
                        cloth::TypeKind::kNullable &&
                    read_line->thrown_types == std::vector{io_error->type},
                "Console.ReadLine lost its nullable result or IoError effect");

    cloth::LlvmIrOptions producer_options;
    producer_options.package = cloth::PackageIdentity{
        "cloth", std::string{cloth::kStandardLibraryPackageVersion}};
    const auto producer_ir =
        cloth::emit_llvm_ir(produced.mir, produced.abi, produced.semantics,
                            producer_diagnostics, producer_options);
    test.expect(
        producer_ir.has_value() &&
            producer_ir->text.contains(
                "call ptr @cloth_rt_console_read_line") &&
            producer_ir->text.contains("could not read standard input") &&
            producer_ir->text.contains("standard input is not valid Unicode") &&
            producer_ir->text.contains("standard input line is too large") &&
            producer_ir->text.contains("phi ptr"),
        "Console.ReadLine did not lower through the checked ABI-6 bridge");

    const cloth::ImportedPackageResult imported =
        cloth::build_imported_package_view(
            {"cloth", std::string{cloth::kStandardLibraryPackageVersion}},
            produced.semantics, produced.mir, produced.abi);
    test.expect(imported.is_valid(),
                "console package interface could not be exported");
    if (!imported.view) continue;

    cloth::Compilation consumer{target};
    consumer.set_package_dependencies({{"app", "cloth", "cloth"}});
    consumer.add_imported_package(*imported.view);
    consumer.add_package_source(
        cloth::SourceFile::from_memory("Main.co", std::string{kConsumer}),
        "app", "", "0.1.0");
    cloth::DiagnosticEngine consumer_diagnostics;
    cloth::CompilationResult consumed = consumer.analyze(consumer_diagnostics);
    test.expect(consumed.is_valid, messages(consumer_diagnostics));
    if (consumed.is_valid) {
      const auto app_file = std::ranges::find_if(
          consumed.semantics.files(), [&](const cloth::FileSemantics& file) {
            return consumed.semantics.symbol(file.symbol).name == "app.Main";
          });
      const auto consumed_parse_error = std::ranges::find_if(
          consumed.semantics.files(), [&](const cloth::FileSemantics& file) {
            return consumed.semantics.symbol(file.symbol).name ==
                   "cloth.lang.errors.ParseError";
          });
      const cloth::SemanticSymbol* inferred =
          app_file == consumed.semantics.files().end()
              ? nullptr
              : find_function(consumed.semantics, *app_file, "infer");
      test.expect(
          inferred != nullptr &&
              consumed_parse_error != consumed.semantics.files().end() &&
              inferred->thrown_types ==
                  std::vector{consumed_parse_error->type} &&
              !inferred->has_explicit_throws,
          "private primitive parse effects were not inferred as ParseError");
      std::size_t parse_calls = 0;
      for (const cloth::HirExpression& expression :
           consumed.hir.storage.expressions()) {
        const auto* call =
            std::get_if<cloth::HirCallExpression>(&expression.data);
        if (call != nullptr && call->callable &&
            consumed.semantics.symbol(*call->callable).intrinsic ==
                cloth::IntrinsicKind::kPrimitiveParse) {
          ++parse_calls;
        }
      }
      test.expect(parse_calls == 17,
                  "primitive parse calls were not retained in typed HIR");
      cloth::LlvmIrOptions options;
      options.package = cloth::PackageIdentity{"app", "0.1.0"};
      const auto consumer_ir =
          cloth::emit_llvm_ir(consumed.mir, consumed.abi, consumed.semantics,
                              consumer_diagnostics, options);
      test.expect(
          consumer_ir.has_value() &&
              consumer_ir->text.contains("call i8 @cloth_rt_parse_primitive") &&
              consumer_ir->text.contains("invalid bool text") &&
              consumer_ir->text.contains("int32 value is out of range") &&
              consumer_ir->text.contains("float64 value is out of range"),
          "source-free primitive parsing did not lower through the "
          "checked ABI-6 bridge");
    }

    cloth::Compilation unavailable{target};
    unavailable.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
          static func Main() throws ParseError {
            int32 value = int32::parse("1");
          }
        )"),
                                   "app", "", "0.1.0");
    cloth::DiagnosticEngine unavailable_diagnostics;
    test.expect(!unavailable.analyze(unavailable_diagnostics).is_valid &&
                    messages(unavailable_diagnostics)
                        .contains("primitive parsing requires the "
                                  "compiler-paired "
                                  "'cloth.lang.errors.ParseError'"),
                "primitive parsing was accepted without the paired library");

    cloth::Compilation whole{target};
    whole.set_package_dependencies({{"app", "cloth", "cloth"}});
    if (!add_console_sources(whole, cloth::kStandardLibraryPackageVersion)) {
      continue;
    }
    whole.add_package_source(
        cloth::SourceFile::from_memory("Main.co", std::string{kConsumer}),
        "app", "", "0.1.0");
    cloth::DiagnosticEngine whole_diagnostics;
    cloth::CompilationResult whole_result = whole.analyze(whole_diagnostics);
    test.expect(whole_result.is_valid, messages(whole_diagnostics));
    if (whole_result.is_valid) {
      expect_package_ir(test, whole_result, whole_diagnostics, "app",
                        "whole-project Console consumer IR is incomplete");
    }
  }

  cloth::Compilation inaccessible;
  inaccessible.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
      static func Main() { __readLine(); }
    )"),
                                  "app", "", "0.1.0");
  cloth::DiagnosticEngine inaccessible_diagnostics;
  test.expect(!inaccessible.analyze(inaccessible_diagnostics).is_valid &&
                  messages(inaccessible_diagnostics)
                      .contains("unknown name '__readLine'"),
              "the private Console bridge escaped the compiler-paired library");

  cloth::Compilation mismatched;
  if (add_console_sources(mismatched, "0.2.0")) {
    cloth::DiagnosticEngine mismatched_diagnostics;
    test.expect(!mismatched.analyze(mismatched_diagnostics).is_valid &&
                    messages(mismatched_diagnostics)
                        .contains("unknown name '__readLine'"),
                "the private Console bridge accepted an unpaired library");
  }
}

void primitive_parse_rejections(TestContext& test) {
  const std::filesystem::path parse_error_path =
      std::filesystem::path{CLOTH_STANDARD_LIBRARY_SOURCE_DIR} / "lang" /
      "errors" / "ParseError.co";
  const auto add_parse_error = [&](cloth::Compilation& compilation,
                                   std::string_view version) {
    auto source = cloth::SourceFile::load(parse_error_path);
    test.expect(source.has_value(), "ParseError.co could not be read");
    if (!source) {
      return false;
    }
    compilation.add_package_source(std::move(*source), "cloth", "lang.errors",
                                   std::string{version});
    compilation.set_package_dependencies({{"app", "cloth", "cloth"}});
    return true;
  };

  cloth::Compilation invalid;
  if (add_parse_error(invalid, cloth::kStandardLibraryPackageVersion)) {
    invalid.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
      static final int32 Constant = int32::parse("1");
      static func MissingThrows(): int32 { return int32::parse("1"); }
      static func Main() throws ParseError {
        string? maybe = null;
        int32 value = 1;
        int32 missing = int32::parse();
        int32 wrongType = int32::parse(value);
        int32 nullable = int32::parse(maybe);
        int32 extra = int32::parse("1", "2");
        int32 wrongCase = int32::Parse("1");
        int32 wrongReceiver = value::parse("1");
        string unsupported = string::parse("text");
        int32 hidden = __primitiveParse("1");
      }
    )"),
                               "app", "", "0.1.0");
    cloth::DiagnosticEngine diagnostics;
    const auto result = invalid.analyze(diagnostics);
    const std::string text = messages(diagnostics);
    test.expect(
        !result.is_valid &&
            text.contains("static field initializer must be a scalar constant "
                          "expression") &&
            text.contains("function 'MissingThrows(): int32' may throw ") &&
            text.contains("cloth.lang.errors.ParseError") &&
            text.contains("no matching overload for call with 0 argument(s)") &&
            text.contains("no matching overload for call with 2 argument(s)") &&
            text.contains("type 'int32' has no meta operation 'Parse'") &&
            text.contains("type 'int32' has no Cloth meta queries") &&
            text.contains("type 'string' cannot be parsed") &&
            text.contains("unknown name '__primitiveParse'"),
        text);
  }

  cloth::Compilation mismatched;
  if (add_parse_error(mismatched, "0.2.0")) {
    mismatched.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
          static func Main() throws ParseError {
            int32 value = int32::parse("1");
          }
        )"),
                                  "app", "", "0.1.0");
    cloth::DiagnosticEngine diagnostics;
    const auto result = mismatched.analyze(diagnostics);
    test.expect(
        !result.is_valid &&
            messages(diagnostics)
                .contains("primitive parsing requires the compiler-paired "
                          "'cloth.lang.errors.ParseError'"),
        "primitive parsing accepted a mismatched standard-library version");
  }

  cloth::Compilation malformed;
  malformed.set_package_dependencies({{"app", "cloth", "cloth"}});
  malformed.add_package_source(
      cloth::SourceFile::from_memory("ParseError.co", R"(
        error { ParseError() {} }
      )"),
      "cloth", "lang.errors",
      std::string{cloth::kStandardLibraryPackageVersion});
  malformed.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
        static func Main() throws ParseError {
          int32 value = int32::parse("1");
        }
      )"),
                               "app", "", "0.1.0");
  cloth::DiagnosticEngine malformed_diagnostics;
  const auto malformed_result = malformed.analyze(malformed_diagnostics);
  test.expect(
      !malformed_result.is_valid &&
          messages(malformed_diagnostics)
              .contains("primitive parsing requires the compiler-paired "
                        "'cloth.lang.errors.ParseError'"),
      "primitive parsing accepted an incompatible ParseError declaration");
}

void math_package_and_source_free_consumer(TestContext& test) {
  const std::filesystem::path math_path =
      std::filesystem::path{CLOTH_STANDARD_LIBRARY_SOURCE_DIR} / "math" /
      "Math.co";

  for (const cloth::TargetDataLayout& target :
       {cloth::TargetDataLayout::llvm_x86_64(),
        cloth::TargetDataLayout::llvm_wasm32()}) {
    auto loaded = cloth::SourceFile::load(math_path);
    test.expect(loaded.has_value(),
                "standard-library Math.co could not be read");
    if (!loaded) return;

    cloth::Compilation producer{target};
    producer.add_package_source(
        std::move(*loaded), "cloth", "math",
        std::string{cloth::kStandardLibraryPackageVersion});
    cloth::DiagnosticEngine producer_diagnostics;
    const cloth::CompilationResult produced =
        producer.analyze(producer_diagnostics);
    test.expect(produced.is_valid, messages(producer_diagnostics));
    if (!produced.is_valid) continue;

    const auto math_file = std::ranges::find_if(
        produced.semantics.files(), [&](const cloth::FileSemantics& file) {
          return produced.semantics.symbol(file.symbol).name ==
                 "cloth.math.Math";
        });
    test.expect(math_file != produced.semantics.files().end(),
                "Math lost its cloth.math canonical identity");
    if (math_file == produced.semantics.files().end()) continue;

    const cloth::SemanticSymbol* gcd =
        find_function(produced.semantics, *math_file, "Gcd");
    const cloth::SemanticSymbol* lcm =
        find_function(produced.semantics, *math_file, "Lcm");
    const std::vector expected_errors{
        produced.semantics.division_by_zero_type()};
    test.expect(gcd != nullptr && gcd->thrown_types == expected_errors &&
                    lcm != nullptr && lcm->thrown_types == expected_errors,
                "Math integer division effects are not explicit");

    cloth::LlvmIrOptions producer_options;
    producer_options.package = cloth::PackageIdentity{
        "cloth", std::string{cloth::kStandardLibraryPackageVersion}};
    const auto producer_ir =
        cloth::emit_llvm_ir(produced.mir, produced.abi, produced.semantics,
                            producer_diagnostics, producer_options);
    test.expect(producer_ir.has_value() &&
                    !producer_ir->text.contains("define i32 @main("),
                "standard-library package IR is missing or contains an entry");

    const cloth::ImportedPackageResult imported =
        cloth::build_imported_package_view(
            {"cloth", std::string{cloth::kStandardLibraryPackageVersion}},
            produced.semantics, produced.mir, produced.abi);
    test.expect(imported.is_valid(),
                "standard-library interface could not be exported");
    if (!imported.view) continue;

    const std::vector<std::string_view> consumers{
        R"(
          import cloth.math::Math;
          static func Main() throws DivisionByZero {
            println(Math.Gcd(84, 30));
          }
        )",
        R"(
          import cloth.math::Math as Numbers;
          static func Main() throws DivisionByZero {
            println(Numbers.Gcd(84, 30));
          }
        )",
        R"(
          import cloth.math.*;
          static func Main() throws DivisionByZero {
            println(Math.Gcd(84, 30));
          }
        )",
    };
    for (const std::string_view source : consumers) {
      cloth::Compilation consumer{target};
      consumer.set_package_dependencies({{"app", "cloth", "cloth"}});
      consumer.add_imported_package(*imported.view);
      consumer.add_package_source(
          cloth::SourceFile::from_memory("Main.co", std::string{source}), "app",
          "", "0.1.0");
      cloth::DiagnosticEngine consumer_diagnostics;
      const cloth::CompilationResult consumed =
          consumer.analyze(consumer_diagnostics);
      test.expect(consumed.is_valid, messages(consumer_diagnostics));
      if (!consumed.is_valid) continue;

      cloth::LlvmIrOptions consumer_options;
      consumer_options.package = cloth::PackageIdentity{"app", "0.1.0"};
      const auto consumer_ir =
          cloth::emit_llvm_ir(consumed.mir, consumed.abi, consumed.semantics,
                              consumer_diagnostics, consumer_options);
      test.expect(consumer_ir.has_value(),
                  "source-free standard-library consumer IR is incomplete");
    }

    cloth::Compilation nonrecursive{target};
    nonrecursive.set_package_dependencies({{"app", "cloth", "cloth"}});
    nonrecursive.add_imported_package(*imported.view);
    nonrecursive.add_package_source(
        cloth::SourceFile::from_memory("Main.co", R"(
          import cloth.*;
          static func Main() throws DivisionByZero {
            println(Math.Gcd(84, 30));
          }
        )"),
        "app", "", "0.1.0");
    cloth::DiagnosticEngine nonrecursive_diagnostics;
    test.expect(!nonrecursive.analyze(nonrecursive_diagnostics).is_valid,
                "cloth.* recursively imported cloth.math.Math");

    auto whole_math = cloth::SourceFile::load(math_path);
    test.expect(whole_math.has_value(),
                "whole-project standard-library source could not be read");
    if (!whole_math) continue;
    cloth::Compilation whole{target};
    whole.set_package_dependencies({{"app", "cloth", "cloth"}});
    whole.add_package_source(
        std::move(*whole_math), "cloth", "math",
        std::string{cloth::kStandardLibraryPackageVersion});
    whole.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
      import cloth.math::Math;
      static func Main() throws DivisionByZero {
        println(Math.Gcd(84, 30));
      }
    )"),
                             "app", "", "0.1.0");
    cloth::DiagnosticEngine whole_diagnostics;
    const cloth::CompilationResult whole_result =
        whole.analyze(whole_diagnostics);
    test.expect(whole_result.is_valid, messages(whole_diagnostics));
    if (!whole_result.is_valid) continue;
    cloth::LlvmIrOptions whole_options;
    whole_options.package = cloth::PackageIdentity{"app", "0.1.0"};
    test.expect(cloth::emit_llvm_ir(whole_result.mir, whole_result.abi,
                                    whole_result.semantics, whole_diagnostics,
                                    whole_options)
                    .has_value(),
                "whole-project standard-library consumer IR is incomplete");
  }
}

void reserved_source_package(TestContext& test) {
  for (const std::string_view source_package :
       {"cloth", "Cloth.math", "CLOTH.internal"}) {
    cloth::Compilation compilation;
    compilation.add_package_source(
        cloth::SourceFile::from_memory("Shadow.co", "class {}"), "app",
        std::string{source_package}, "0.1.0");
    cloth::DiagnosticEngine diagnostics;
    test.expect(
        !compilation.analyze(diagnostics).is_valid &&
            messages(diagnostics)
                .contains("is reserved for the standard library"),
        "reserved source package was accepted: " + std::string{source_package});
  }

  cloth::Compilation compilation;
  compilation.add_package_source(
      cloth::SourceFile::from_memory("Math.co", "class {}"), "Cloth", "math",
      "0.1.0");
  cloth::DiagnosticEngine diagnostics;
  test.expect(
      !compilation.analyze(diagnostics).is_valid &&
          messages(diagnostics)
              .contains("standard library package must be spelled 'cloth'"),
      "case-only standard-library package identity was accepted");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"Console API and source-free consumer",
       console_api_and_source_free_consumer},
      {"primitive parse rejections", primitive_parse_rejections},
      {"Math package and source-free consumer",
       math_package_and_source_free_consumer},
      {"prelude whole and source-free", prelude_whole_and_source_free},
      {"prelude precedence and boundaries", prelude_precedence_and_boundaries},
      {"prelude collisions", prelude_collisions},
      {"lang API and source-free consumer", lang_api_and_source_free_consumer},
      {"reserved source package", reserved_source_package},
  };
  return cloth::test::run_tests(tests);
}
