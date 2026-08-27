#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/project/project_layout.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TestContext {
 public:
  explicit TestContext(std::string_view name) : name_(name) {}

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << "  " << name_ << ": " << message << '\n';
    }
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  std::string_view name_;
  int failures_{0};
};

void project_discovery(TestContext& test) {
  const std::filesystem::path project =
      std::filesystem::path{CLOTH_TEST_SOURCE_DIR} / "tests" / "projects" /
      "imports";
  const std::filesystem::path entry = project / "src" / "Main.co";
  const auto layout = cloth::discover_project_layout(entry);
  test.expect(layout.has_value(), "project manifest was not discovered");
  if (!layout) {
    return;
  }
  test.expect(layout->project_root == project,
              "project root does not contain the manifest");
  test.expect(layout->source_root == project / "src",
              "project source root is wrong");

  auto source = cloth::SourceFile::load(entry);
  test.expect(source.has_value(), "project entry source did not load");
  if (!source) {
    return;
  }

  cloth::Compilation compilation;
  compilation.set_source_root(layout->source_root, true);
  compilation.add_source(std::move(*source));
  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;
  result.emplace(compilation.analyze(diagnostics));

  test.expect(!diagnostics.has_errors(),
              "valid project discovery produced diagnostics");
  test.expect(result->is_valid, "discovered project was marked invalid");
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
      std::filesystem::path{CLOTH_TEST_SOURCE_DIR} / "tests" / "projects" /
      "missing_import";
  const std::filesystem::path entry = project / "src" / "Main.co";
  const auto layout = cloth::discover_project_layout(entry);
  test.expect(layout.has_value(), "error fixture layout was not discovered");
  if (!layout) {
    return;
  }

  auto source = cloth::SourceFile::load(entry);
  test.expect(source.has_value(), "error fixture entry did not load");
  if (!source) {
    return;
  }
  cloth::Compilation compilation;
  compilation.set_source_root(layout->source_root, true);
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

  const auto outside = cloth::discover_project_layout(project / "Outside.co");
  test.expect(!outside && outside.error().find("outside the project's 'src'") !=
                              std::string::npos,
              "entry outside src was accepted");
}

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string_view name;
  TestFunction function;
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"project discovery", project_discovery},
      {"project errors", project_errors},
  };

  int failures = 0;
  for (const TestCase& test_case : tests) {
    TestContext context{test_case.name};
    test_case.function(context);
    if (context.failures() == 0) {
      std::cout << "[pass] " << test_case.name << '\n';
    } else {
      std::cout << "[fail] " << test_case.name << '\n';
      failures += context.failures();
    }
  }
  if (failures == 0) {
    std::cout << tests.size() << " tests passed\n";
    return 0;
  }
  std::cerr << failures << " assertion(s) failed\n";
  return 1;
}
