#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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

class AnalyzedCompilation {
 public:
  void add(std::filesystem::path path, std::string text) {
    compilation_.add_source(
        cloth::SourceFile::from_memory(std::move(path), std::move(text)));
  }

  void analyze() { result.emplace(compilation_.analyze(diagnostics)); }

  [[nodiscard]] std::size_t error_count() const {
    std::size_t count = 0;
    for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
      if (diagnostic.severity == cloth::DiagnosticSeverity::kError) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] bool has_diagnostic(std::string_view text) const {
    for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
      if (diagnostic.message.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] const cloth::FileClassDecl& syntax_file(
      std::size_t index) const {
    return compilation_.syntax(index);
  }

  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;

 private:
  cloth::Compilation compilation_;
};

void core_types_and_typed_hir(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Values.co",
                  "int count = 1;\n"
                  "String Label = \"cloth\";\n"
                  "function IsPositive(int value): bool {\n"
                  "  int copy = value;\n"
                  "  if (copy > 0) { return true; } "
                  "else { return false; }\n"
                  "}\n");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "valid program produced semantic errors");
  test.expect(compilation.result->is_valid,
              "valid compilation was marked invalid");
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  test.expect(semantics.find_type("int") == semantics.find_type("int32"),
              "portable int alias does not resolve to int32");
  test.expect(semantics.find_type("uint") == semantics.find_type("uint32"),
              "portable uint alias does not resolve to uint32");
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  test.expect(
      semantics.type(semantics.symbol(file.fields[0]).type).name == "int32",
      "field type was not canonicalized");
  test.expect(compilation.result->hir.storage.expressions().size() ==
                  compilation.syntax_file(0).storage.expressions().size(),
              "HIR did not retain every typed expression");
}

void cross_file_binding(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("App.co",
                  "function Load(int id): User { return User.Find(id); }\n");
  compilation.add("User.co",
                  "function Find(int32 id): User { return null; }\n");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "public cross-file call did not bind");
  bool found_call = false;
  for (const cloth::HirExpression& expression :
       compilation.result->hir.storage.expressions()) {
    const auto* call = std::get_if<cloth::HirCallExpression>(&expression.data);
    if (call == nullptr || !call->callable) {
      continue;
    }
    const cloth::SemanticSymbol& symbol =
        compilation.result->semantics.symbol(*call->callable);
    if (symbol.name == "Find" && symbol.kind == cloth::SymbolKind::kFunction) {
      found_call = true;
    }
  }
  test.expect(found_call, "HIR call does not contain its bound function");
}

void private_member_access(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("User.co", "function hidden(): bool { return true; }\n");
  compilation.add("App.co",
                  "function Read(): bool { return User.hidden(); }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic(
                  "member 'hidden' is private in file class 'User'"),
              "private cross-file member access was accepted");
}

void private_file_class_access(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("secret.co", "");
  compilation.add("App.co", "secret value;\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("file class 'secret' is private"),
              "private file class was exposed across files");
}

void unknown_types_and_names(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Broken.co",
                  "Mystery value;\n"
                  "function Read(): int { return missing; }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("unknown type 'Mystery'"),
              "unknown declared type was not diagnosed");
  test.expect(compilation.has_diagnostic("unknown name 'missing'"),
              "unknown expression name was not diagnosed");
}

void type_checking(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("BadTypes.co",
                  "function Bad(): int {\n"
                  "  bool flag = 1;\n"
                  "  if (1) { return true; }\n"
                  "  return false;\n"
                  "}\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic(
                  "local initializer has type 'int32'; expected 'bool'"),
              "local initializer type mismatch was not diagnosed");
  test.expect(compilation.has_diagnostic(
                  "if condition has type 'int32'; expected 'bool'"),
              "non-boolean condition was accepted");
  test.expect(compilation.has_diagnostic(
                  "return value has type 'bool'; expected 'int32'"),
              "return type mismatch was not diagnosed");
}

void exact_overload_resolution(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Overloads.co",
                  "function Pick(int value): int { return value; }\n"
                  "function Pick(bool value): bool { return value; }\n"
                  "function Choose(): bool { return Pick(true); }\n");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "exact overload call did not resolve");
  const cloth::FileSemantics& file =
      compilation.result->semantics.file(cloth::FileId{0});
  const cloth::SymbolId bool_overload = file.functions[1];
  bool selected_bool_overload = false;
  for (const cloth::ExpressionSemantics& expression : file.expressions) {
    selected_bool_overload =
        selected_bool_overload || expression.symbol == bool_overload;
  }
  test.expect(selected_bool_overload,
              "bound expressions lost the selected overload");
}

void no_matching_overload(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Calls.co",
                  "function Pick(int value): int { return value; }\n"
                  "function Bad(): int { return Pick(true); }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("no matching overload"),
              "invalid call was accepted");
}

void invalid_body_does_not_hide_signature(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Recovery.co",
                  "function Broken(int value): int { return value }\n"
                  "function Use(): int { return Broken(1); }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("expected ';' after return statement"),
              "syntax error fixture did not fail as expected");
  test.expect(!compilation.has_diagnostic("no matching overload"),
              "invalid body erased a usable declaration signature");
}

void constructor_binding(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("User.co",
                  "String Name;\n"
                  "User(String name) { Name = name; }\n");
  compilation.add("App.co",
                  "function Make(): User { return User(\"Ada\"); }\n");
  compilation.analyze();

  test.expect(compilation.error_count() == 0, "constructor call did not bind");
  bool found_constructor = false;
  for (const cloth::HirExpression& expression :
       compilation.result->hir.storage.expressions()) {
    const auto* call = std::get_if<cloth::HirCallExpression>(&expression.data);
    if (call != nullptr && call->callable &&
        compilation.result->semantics.symbol(*call->callable).kind ==
            cloth::SymbolKind::kConstructor) {
      found_constructor = true;
    }
  }
  test.expect(found_constructor, "HIR lost the bound constructor");
}

void lexical_scopes(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Scopes.co",
            "function Read(int value): int {\n"
            "  { int value = 2; }\n"
            "  return value;\n"
            "}\n");
  valid.analyze();
  test.expect(valid.error_count() == 0,
              "nested local shadowing should be valid");

  AnalyzedCompilation duplicate;
  duplicate.add("Scopes.co",
                "function Read(int value): int {\n"
                "  int value = 2;\n"
                "  return value;\n"
                "}\n");
  duplicate.analyze();
  test.expect(duplicate.has_diagnostic("duplicate local name 'value'"),
              "same-scope duplicate was not diagnosed");
}

void complete_return_paths(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Returns.co",
                  "function Maybe(bool flag): int {\n"
                  "  if (flag) { return 1; }\n"
                  "}\n");
  compilation.analyze();

  test.expect(
      compilation.has_diagnostic("does not return a value on every path"),
      "incomplete return paths were accepted");
}

void case_collision(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("User.co", "");
  compilation.add("user.co", "");
  compilation.analyze();

  test.expect(compilation.has_diagnostic(
                  "collides with another source file by ASCII case"),
              "portable file-name collision was not diagnosed");
}

void null_assignability(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("User.co", "");
  compilation.add("Nulls.co",
                  "function Empty(): User { return null; }\n"
                  "function Number(): int { return null; }\n");
  compilation.analyze();

  test.expect(!compilation.has_diagnostic(
                  "return value has type 'null'; expected 'User'"),
              "null was rejected for a reference type");
  test.expect(compilation.has_diagnostic(
                  "return value has type 'null'; expected 'int32'"),
              "null was accepted for a value type");
}

void assignment_requires_location(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Assignments.co",
                  "function Bad(): int { 1 = 2; return 0; }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("assignment target is not mutable"),
              "literal assignment target was accepted");
}

void instance_member_binding(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("User.co", "String Name;\n");
  compilation.add("Reader.co",
                  "function Read(User value): String { return value.Name; }\n");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "public instance field did not bind");
}

void deterministic_diagnostics(TestContext& test) {
  auto analyze = [] {
    AnalyzedCompilation compilation;
    compilation.add("Stable.co",
                    "Mystery value;\n"
                    "function Bad(): int { if (1) { return true; } }\n");
    compilation.analyze();
    std::vector<std::string> messages;
    for (const cloth::Diagnostic& diagnostic :
         compilation.diagnostics.diagnostics()) {
      messages.push_back(diagnostic.message);
    }
    return messages;
  };

  test.expect(analyze() == analyze(),
              "semantic diagnostic order changed between runs");
}

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string_view name;
  TestFunction function;
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"core types and typed HIR", core_types_and_typed_hir},
      {"cross-file binding", cross_file_binding},
      {"private member access", private_member_access},
      {"private file class access", private_file_class_access},
      {"unknown types and names", unknown_types_and_names},
      {"type checking", type_checking},
      {"exact overload resolution", exact_overload_resolution},
      {"no matching overload", no_matching_overload},
      {"invalid body retains signature", invalid_body_does_not_hide_signature},
      {"constructor binding", constructor_binding},
      {"lexical scopes", lexical_scopes},
      {"complete return paths", complete_return_paths},
      {"case collision", case_collision},
      {"null assignability", null_assignability},
      {"assignment requires location", assignment_requires_location},
      {"instance member binding", instance_member_binding},
      {"deterministic diagnostics", deterministic_diagnostics},
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
