#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

void explicit_source_root(TestContext& test) {
  const std::filesystem::path project =
      std::filesystem::path{CLOTH_TEST_DATA_DIR} / "projects" / "imports";
  const std::filesystem::path entry = project / "src" / "Main.co";
  auto source = cloth::SourceFile::load(entry);
  test.expect(source.has_value(), "project entry source did not load");
  if (!source) {
    return;
  }

  cloth::Compilation compilation;
  compilation.set_source_root(project / "src", true);
  compilation.add_source(std::move(*source));
  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;
  result.emplace(compilation.analyze(diagnostics));

  test.expect(!diagnostics.has_errors(),
              "valid explicit source root produced diagnostics");
  test.expect(result->is_valid, "explicit source-root project was invalid");
  test.expect(compilation.source_count() == 6,
              "project source graph has the wrong size");

  bool found_qualified_type = false;
  bool found_private_dependency = false;
  for (const cloth::SemanticType& type : result->semantics.types()) {
    found_qualified_type =
        found_qualified_type || type.name == "greetings.Greeter";
    found_private_dependency =
        found_private_dependency || type.name == "exports.hidden";
  }
  test.expect(found_qualified_type,
              "package-qualified type identity was not retained");
  test.expect(found_private_dependency,
              "private wildcard package source was not compiled");
}

void project_errors(TestContext& test) {
  const std::filesystem::path project =
      std::filesystem::path{CLOTH_TEST_DATA_DIR} / "projects" /
      "missing_import";
  const std::filesystem::path entry = project / "src" / "Main.co";
  auto source = cloth::SourceFile::load(entry);
  test.expect(source.has_value(), "error fixture entry did not load");
  if (!source) {
    return;
  }
  cloth::Compilation compilation;
  compilation.set_source_root(project / "src", true);
  compilation.add_source(std::move(*source));
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);

  std::size_t matching_errors = 0;
  for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message.find(
            "unknown imported file class 'nowhere.Missing'") !=
        std::string::npos) {
      ++matching_errors;
    }
  }
  test.expect(!result.is_valid, "missing import project was accepted");
  test.expect(matching_errors == 1,
              "missing import did not produce one precise diagnostic");

  cloth::Compilation outside;
  outside.set_source_root(project / "src", false);
  outside.add_source(cloth::SourceFile::from_memory(project / "Outside.co",
                                                    "static func Main() {}"));
  cloth::DiagnosticEngine outside_diagnostics;
  const cloth::CompilationResult outside_result =
      outside.analyze(outside_diagnostics);
  test.expect(!outside_result.is_valid && outside_diagnostics.has_errors(),
              "source outside the explicit root was accepted");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"explicit source root", explicit_source_root},
      {"project errors", project_errors},
  };

  return cloth::test::run_tests(tests);
}
