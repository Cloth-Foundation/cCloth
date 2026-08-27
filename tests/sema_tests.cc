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
  void add(std::filesystem::path path, std::string text,
           std::string package_name = {}) {
    compilation_.add_source(
        cloth::SourceFile::from_memory(std::move(path), std::move(text)),
        std::move(package_name));
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
                  "float ratio;\n"
                  "func IsPositive(int value): bool {\n"
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
  test.expect(semantics.find_type("float") == semantics.find_type("float32"),
              "portable float alias does not resolve to float32");
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  test.expect(
      semantics.type(semantics.symbol(file.fields[0]).type).name == "int32",
      "field type was not canonicalized");
  test.expect(
      semantics.type(semantics.symbol(file.fields[2]).type).name == "float32",
      "float field type was not canonicalized to float32");
  test.expect(compilation.result->hir.storage.expressions().size() ==
                  compilation.syntax_file(0).storage.expressions().size(),
              "HIR did not retain every typed expression");
}

void core_print_intrinsic(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("HelloWorld.co",
            "func Main() { print(\"hello\"); print(1); print(true); }\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid core print call produced semantic errors");
  const std::vector<cloth::SymbolId> print =
      valid.result->semantics.find_intrinsics("print");
  test.expect(print.size() == 3, "core print overload set was not registered");
  bool bound_string = false;
  bool bound_int32 = false;
  bool bound_bool = false;
  for (const cloth::HirExpression& expression :
       valid.result->hir.storage.expressions()) {
    const auto* call = std::get_if<cloth::HirCallExpression>(&expression.data);
    if (call != nullptr && call->callable) {
      const cloth::IntrinsicKind intrinsic =
          valid.result->semantics.symbol(*call->callable).intrinsic;
      bound_string =
          bound_string || intrinsic == cloth::IntrinsicKind::kPrintString;
      bound_int32 =
          bound_int32 || intrinsic == cloth::IntrinsicKind::kPrintInt32;
      bound_bool = bound_bool || intrinsic == cloth::IntrinsicKind::kPrintBool;
    }
  }
  test.expect(bound_string && bound_int32 && bound_bool,
              "core print overloads were not retained in typed HIR");

  AnalyzedCompilation invalid;
  invalid.add("BadPrint.co", "func Main() { print(1.5); }\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic("no matching overload"),
              "print accepted a type without an overload");

  AnalyzedCompilation shadowed;
  shadowed.add("Shadow.co",
               "func print(int value) {}\n"
               "func Main() { print(1); }\n");
  shadowed.analyze();
  test.expect(shadowed.error_count() == 0,
              "source member did not shadow core print");
}

void cross_file_binding(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("App.co",
                  "func Load(int id): User { return User.Find(id); }\n");
  compilation.add("User.co", "func Find(int32 id): User { return null; }\n");
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

void package_imports(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("app/Main.co",
                  "import models::User as ModelUser;\n"
                  "import services.*;\n"
                  "func Load(): ModelUser { return Factory.Make(); }\n"
                  "func Help(): int { return Helper.Value(); }\n",
                  "app");
  compilation.add("app/Helper.co", "func Value(): int { return 8; }\n", "app");
  compilation.add("models/User.co", "", "models");
  compilation.add("services/Factory.co",
                  "import models::User;\n"
                  "func Make(): User { return null; }\n",
                  "services");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "valid package imports produced semantic errors");
  test.expect(compilation.result->is_valid,
              "valid package compilation was marked invalid");

  AnalyzedCompilation cyclic;
  cyclic.add("alpha/A.co",
             "import beta::B;\nfunc Other(): B { return null; }\n", "alpha");
  cyclic.add("beta/B.co",
             "import alpha::A;\nfunc Other(): A { return null; }\n", "beta");
  cyclic.analyze();
  test.expect(cyclic.error_count() == 0, "cyclic type imports were rejected");
}

void invalid_package_imports(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("app/Main.co",
                  "import missing::Absent;\n"
                  "import secrets::hidden;\n"
                  "import left.*;\n"
                  "import right.*;\n",
                  "app");
  compilation.add("secrets/hidden.co", "", "secrets");
  compilation.add("left/Thing.co", "", "left");
  compilation.add("right/Thing.co", "", "right");
  compilation.analyze();

  test.expect(compilation.has_diagnostic(
                  "unknown imported file class 'missing.Absent'"),
              "missing imported type was not diagnosed");
  test.expect(
      compilation.has_diagnostic("file class 'secrets.hidden' is private"),
      "private imported type was not diagnosed");
  test.expect(compilation.has_diagnostic("import name 'Thing' is ambiguous"),
              "wildcard collision was not diagnosed");
}

void private_member_access(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("User.co", "func hidden(): bool { return true; }\n");
  compilation.add("App.co", "func Read(): bool { return User.hidden(); }\n");
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
                  "func Read(): int { return missing; }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("unknown type 'Mystery'"),
              "unknown declared type was not diagnosed");
  test.expect(compilation.has_diagnostic("unknown name 'missing'"),
              "unknown expression name was not diagnosed");
}

void type_checking(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("BadTypes.co",
                  "func Bad(): int {\n"
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
                  "func Pick(int value): int { return value; }\n"
                  "func Pick(bool value): bool { return value; }\n"
                  "func Choose(): bool { return Pick(true); }\n");
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
                  "func Pick(int value): int { return value; }\n"
                  "func Bad(): int { return Pick(true); }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("no matching overload"),
              "invalid call was accepted");
}

void invalid_body_does_not_hide_signature(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Recovery.co",
                  "func Broken(int value): int { return value }\n"
                  "func Use(): int { return Broken(1); }\n");
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
  compilation.add("App.co", "func Make(): User { return User(\"Ada\"); }\n");
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
            "func Read(int value): int {\n"
            "  { int value = 2; }\n"
            "  return value;\n"
            "}\n");
  valid.analyze();
  test.expect(valid.error_count() == 0,
              "nested local shadowing should be valid");

  AnalyzedCompilation duplicate;
  duplicate.add("Scopes.co",
                "func Read(int value): int {\n"
                "  int value = 2;\n"
                "  return value;\n"
                "}\n");
  duplicate.analyze();
  test.expect(duplicate.has_diagnostic("duplicate local name 'value'"),
              "same-scope duplicate was not diagnosed");
}

void structured_loop_semantics(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Loops.co",
            "func Run() {\n"
            "  int value = 0;\n"
            "  while (value < 3) {\n"
            "    value = value + 1;\n"
            "    if (value == 2) { continue; }\n"
            "    if (value == 3) { break; }\n"
            "  }\n"
            "}\n");
  valid.analyze();
  test.expect(valid.error_count() == 0,
              "valid structured loop produced semantic errors");

  AnalyzedCompilation invalid;
  invalid.add("BadLoops.co", "func Run() { while (1) {} break; continue; }\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic(
                  "while condition has type 'int32'; expected 'bool'"),
              "non-boolean while condition was accepted");
  test.expect(invalid.has_diagnostic("'break' is only valid inside a loop"),
              "break outside a loop was accepted");
  test.expect(invalid.has_diagnostic("'continue' is only valid inside a loop"),
              "continue outside a loop was accepted");
}

void complete_return_paths(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("Returns.co",
                  "func Maybe(bool flag): int {\n"
                  "  if (flag) { return 1; }\n"
                  "}\n");
  compilation.analyze();

  test.expect(
      compilation.has_diagnostic("does not return a value on every path"),
      "incomplete return paths were accepted");

  AnalyzedCompilation infinite;
  infinite.add("Forever.co",
               "func Forever(): int { while (true) { continue; } }\n");
  infinite.analyze();
  test.expect(!infinite.has_diagnostic("does not return a value on every path"),
              "non-falling infinite loop was rejected");

  AnalyzedCompilation breaks;
  breaks.add("Breaks.co", "func Maybe(): int { while (true) { break; } }\n");
  breaks.analyze();
  test.expect(breaks.has_diagnostic("does not return a value on every path"),
              "loop break did not preserve callable fallthrough");
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
                  "func Empty(): User { return null; }\n"
                  "func Number(): int { return null; }\n");
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
  compilation.add("Assignments.co", "func Bad(): int { 1 = 2; return 0; }\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("assignment target is not mutable"),
              "literal assignment target was accepted");
}

void array_semantics(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Arrays.co",
            "func Sum(): int32 {\n"
            "  int32[] values = [1, 2, 3];\n"
            "  values[1] = 4;\n"
            "  return values.Length + values[0];\n"
            "}\n"
            "func Empty(): int32[] { return null; }\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid array operations produced semantic errors");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  const cloth::TypeId return_type = semantics.symbol(file.functions[1]).type;
  test.expect(semantics.type(return_type).kind == cloth::TypeKind::kArray &&
                  semantics.type(return_type).element_type ==
                      semantics.find_type("int32"),
              "array return type was not canonicalized");

  AnalyzedCompilation invalid;
  invalid.add("BadArrays.co",
              "func Bad() {\n"
              "  int32[] empty = [];\n"
              "  int32[] mixed = [1, true];\n"
              "  int32 value = mixed[false];\n"
              "  int32 missing = mixed.length;\n"
              "  int32 scalar = 1;\n"
              "  scalar[0] = value;\n"
              "}\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic("cannot infer the element type"),
              "empty array literal was accepted without context");
  test.expect(invalid.has_diagnostic("array element has type 'bool'"),
              "heterogeneous array literal was accepted");
  test.expect(invalid.has_diagnostic("array index has type 'bool'"),
              "non-int32 array index was accepted");
  test.expect(invalid.has_diagnostic("has no member 'length'"),
              "array Length casing was ignored");
  test.expect(invalid.has_diagnostic("cannot be indexed"),
              "non-array value was indexable");
}

void for_iteration_semantics(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Iteration.co",
            "func Sum(int32[] values): int32 {\n"
            "  int32 total = 0;\n"
            "  for (var value in values) {\n"
            "    value = value + 1;\n"
            "    total = total + value;\n"
            "  }\n"
            "  for (int32 value in values) { total = total + value; }\n"
            "  return total;\n"
            "}\n");
  valid.analyze();
  test.expect(valid.error_count() == 0,
              "valid for iteration produced semantic errors");
  bool found_for = false;
  for (const cloth::HirStatement& statement :
       valid.result->hir.storage.statements()) {
    const auto* loop = std::get_if<cloth::HirForStatement>(&statement.data);
    if (loop != nullptr && loop->variable) {
      found_for = true;
      test.expect(valid.result->semantics.symbol(*loop->variable).type ==
                      valid.result->semantics.find_type("int32"),
                  "inferred iteration variable has the wrong type");
    }
  }
  test.expect(found_for, "typed HIR has no for statement");

  AnalyzedCompilation invalid;
  invalid.add("BadIteration.co",
              "func Bad(): int32 {\n"
              "  int32[] values = [1, 2];\n"
              "  for (bool value in values) {}\n"
              "  for (var item in 1) {}\n"
              "  return value;\n"
              "}\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic(
                  "for iteration variable has type 'int32'; expected 'bool'"),
              "mismatched explicit iteration type was accepted");
  test.expect(invalid.has_diagnostic("type 'int32' is not iterable"),
              "non-array iterable was accepted");
  test.expect(invalid.has_diagnostic("unknown name 'value'"),
              "iteration variable escaped its loop scope");

  AnalyzedCompilation return_path;
  return_path.add("ReturnPath.co",
                  "func First(int32[] values): int32 {\n"
                  "  for (var value in values) { return value; }\n"
                  "}\n");
  return_path.analyze();
  test.expect(
      return_path.has_diagnostic("does not return a value on every path"),
      "possibly empty for loop was treated as a guaranteed return");
}

void instance_member_binding(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("User.co", "String Name;\n");
  compilation.add("Reader.co",
                  "func Read(User value): String { return value.Name; }\n");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "public instance field did not bind");
}

void deterministic_diagnostics(TestContext& test) {
  auto analyze = [] {
    AnalyzedCompilation compilation;
    compilation.add("Stable.co",
                    "Mystery value;\n"
                    "func Bad(): int { if (1) { return true; } }\n");
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
      {"core print intrinsic", core_print_intrinsic},
      {"cross-file binding", cross_file_binding},
      {"package imports", package_imports},
      {"invalid package imports", invalid_package_imports},
      {"private member access", private_member_access},
      {"private file class access", private_file_class_access},
      {"unknown types and names", unknown_types_and_names},
      {"type checking", type_checking},
      {"exact overload resolution", exact_overload_resolution},
      {"no matching overload", no_matching_overload},
      {"invalid body retains signature", invalid_body_does_not_hide_signature},
      {"constructor binding", constructor_binding},
      {"lexical scopes", lexical_scopes},
      {"structured loop semantics", structured_loop_semantics},
      {"complete return paths", complete_return_paths},
      {"case collision", case_collision},
      {"null assignability", null_assignability},
      {"assignment requires location", assignment_requires_location},
      {"array semantics", array_semantics},
      {"for iteration semantics", for_iteration_semantics},
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
