// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    producer.add_package_source(std::move(*loaded), "cloth", "math", "0.1.0");
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
    producer_options.package = cloth::PackageIdentity{"cloth", "0.1.0"};
    const auto producer_ir =
        cloth::emit_llvm_ir(produced.mir, produced.abi, produced.semantics,
                            producer_diagnostics, producer_options);
    test.expect(producer_ir.has_value() &&
                    !producer_ir->text.contains("define i32 @main("),
                "standard-library package IR is missing or contains an entry");

    const cloth::ImportedPackageResult imported =
        cloth::build_imported_package_view(
            {"cloth", "0.1.0"}, produced.semantics, produced.mir, produced.abi);
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
    whole.add_package_source(std::move(*whole_math), "cloth", "math", "0.1.0");
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
      {"Math package and source-free consumer",
       math_package_and_source_free_consumer},
      {"reserved source package", reserved_source_package},
  };
  return cloth::test::run_tests(tests);
}
