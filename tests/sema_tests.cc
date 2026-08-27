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

void void_callable_contract(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Stage11.co",
            "func Explicit(): void { return; }\n"
            "func Implicit() { Explicit(); }\n"
            "func Main(): void { Implicit(); return; }\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid explicit or implicit void function failed");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::TypeId void_type = semantics.void_type();
  test.expect(semantics.find_type("void") == void_type &&
                  semantics.type(void_type).kind == cloth::TypeKind::kVoid &&
                  semantics.type(void_type).name == "void",
              "void was not registered as the canonical semantic type");
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  for (const cloth::SymbolId function : file.functions) {
    test.expect(semantics.symbol(function).type == void_type,
                "explicit and omitted returns did not canonicalize to void");
  }

  AnalyzedCompilation invalid_storage;
  invalid_storage.add("VoidStorage.co",
                      "void Field;\n"
                      "void[] Values;\n"
                      "func Parameter(void value) {}\n"
                      "func ReturnArray(): void[] {}\n"
                      "func Locals(int32[] values) {\n"
                      "  void local;\n"
                      "  for (void value in values) {}\n"
                      "}\n");
  invalid_storage.analyze();
  test.expect(invalid_storage.has_diagnostic(
                  "'void' is only valid as a function return type"),
              "void storage or parameter position was accepted");
  test.expect(
      invalid_storage.has_diagnostic("'void' cannot be an array element type"),
      "void array type was accepted");

  AnalyzedCompilation invalid_values;
  invalid_values.add("VoidValues.co",
                     "func Nothing(): void {}\n"
                     "func Take(int32 value): void {}\n"
                     "func Bad(): void {\n"
                     "  int32 local = Nothing();\n"
                     "  Take(Nothing());\n"
                     "  print(Nothing());\n"
                     "  int32[] values = [Nothing()];\n"
                     "  if (Nothing()) {}\n"
                     "  return Nothing();\n"
                     "}\n");
  invalid_values.analyze();
  test.expect(invalid_values.has_diagnostic(
                  "void expression cannot be used as a value"),
              "void call was accepted in a value context");
  test.expect(invalid_values.has_diagnostic(
                  "cannot return a value from a void function"),
              "value return from void function was accepted");
  test.expect(!invalid_values.has_diagnostic("internal"),
              "invalid void use caused an internal compiler diagnostic");
}

void final_binding_contract(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("FinalBindings.co",
            "final int32 Code;\n"
            "final String Label = \"cloth\";\n"
            "FinalBindings(final bool alternate, int32 code) {\n"
            "  if (alternate) { Code = code; } "
            "else { self.Code = code + 1; }\n"
            "}\n"
            "func Read(final int32 input, int32[] values): int32 {\n"
            "  final var copy = input;\n"
            "  final int32 result = copy;\n"
            "  final var mutableContents = [1, 2];\n"
            "  mutableContents[0] = 3;\n"
            "  for (final var value in values) { println(value); }\n"
            "  return result;\n"
            "}\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid final bindings produced semantic errors");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  test.expect(semantics.symbol(file.fields[0]).is_final &&
                  semantics.symbol(file.fields[1]).is_final,
              "final field metadata was not preserved");
  const cloth::SemanticSymbol& constructor =
      semantics.symbol(file.constructors[0]);
  const cloth::SemanticSymbol& function = semantics.symbol(file.functions[0]);
  test.expect(!constructor.parameter_symbols.empty() &&
                  semantics.symbol(constructor.parameter_symbols[0]).is_final,
              "final constructor parameter metadata was not preserved");
  test.expect(!function.parameter_symbols.empty() &&
                  semantics.symbol(function.parameter_symbols[0]).is_final,
              "final function parameter metadata was not preserved");

  bool found_inferred_final = false;
  bool found_final_iteration_binding = false;
  for (const cloth::SemanticSymbol& symbol : semantics.symbols()) {
    found_inferred_final =
        found_inferred_final ||
        (symbol.kind == cloth::SymbolKind::kLocal && symbol.name == "copy" &&
         symbol.is_final &&
         semantics.type(symbol.type).kind == cloth::TypeKind::kInt32);
    found_final_iteration_binding = found_final_iteration_binding ||
                                    (symbol.kind == cloth::SymbolKind::kLocal &&
                                     symbol.name == "value" && symbol.is_final);
  }
  test.expect(found_inferred_final,
              "final var did not infer and retain its canonical type");
  test.expect(found_final_iteration_binding,
              "final for binding metadata was not preserved");

  AnalyzedCompilation invalid;
  invalid.add("InvalidFinal.co",
              "final int32 Initialized = 1;\n"
              "final int32 Missing;\n"
              "InvalidFinal(bool assign, int32 value) {\n"
              "  int32 early = Missing;\n"
              "  if (assign) { Missing = value; }\n"
              "  Initialized = value;\n"
              "}\n"
              "func Mutate(final int32 parameter, int32[] values) {\n"
              "  final int32 local = 1;\n"
              "  local = 2;\n"
              "  parameter = 3;\n"
              "  for (final var element in values) { element = 4; }\n"
              "  Missing = 5;\n"
              "}\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic(
                  "final field 'Missing' is read before it is initialized"),
              "read-before-initialization was accepted");
  test.expect(invalid.has_diagnostic(
                  "final field 'Initialized' may only be initialized once"),
              "second final field initialization was accepted");
  test.expect(
      invalid.has_diagnostic(
          "constructor exits before final field 'Missing' is initialized"),
      "partial branch initialization was accepted");
  test.expect(invalid.has_diagnostic("cannot assign to final local 'local'"),
              "final local reassignment was accepted");
  test.expect(
      invalid.has_diagnostic("cannot assign to final parameter 'parameter'"),
      "final parameter reassignment was accepted");
  test.expect(invalid.has_diagnostic("cannot assign to final local 'element'"),
              "final iteration binding reassignment was accepted");
  test.expect(invalid.has_diagnostic("cannot assign to final field 'Missing'"),
              "final field assignment outside construction was accepted");

  AnalyzedCompilation loop;
  loop.add("LoopFinal.co",
           "final int32 Value;\n"
           "LoopFinal(bool repeat) {\n"
           "  while (repeat) { Value = 1; }\n"
           "}\n");
  loop.analyze();
  test.expect(loop.has_diagnostic(
                  "final field 'Value' cannot be initialized inside a loop"),
              "loop-based final field initialization was accepted");

  AnalyzedCompilation early_return;
  early_return.add("EarlyReturn.co",
                   "final int32 Value;\n"
                   "EarlyReturn(bool stop) {\n"
                   "  if (stop) { return; }\n"
                   "  Value = 1;\n"
                   "}\n");
  early_return.analyze();
  test.expect(
      early_return.has_diagnostic(
          "constructor exits before final field 'Value' is initialized"),
      "early constructor return bypassed final initialization");

  AnalyzedCompilation missing_constructor;
  missing_constructor.add("MissingConstructor.co", "final int32 Value;\n");
  missing_constructor.analyze();
  test.expect(missing_constructor.has_diagnostic(
                  "requires an initializer or a constructor"),
              "uninitialized final field without a constructor was accepted");

  AnalyzedCompilation initializer_order;
  initializer_order.add("InitializerOrder.co",
                        "final int32 First = Second;\n"
                        "final int32 Second = 2;\n");
  initializer_order.analyze();
  test.expect(initializer_order.has_diagnostic(
                  "final field 'Second' is read before it is initialized"),
              "final field initializer order was not enforced");

  AnalyzedCompilation invalid_inference;
  invalid_inference.add("Inference.co",
                        "func Bad() {\n"
                        "  var missing;\n"
                        "  final var alsoMissing;\n"
                        "  var unknown = null;\n"
                        "}\n");
  invalid_inference.analyze();
  test.expect(invalid_inference.has_diagnostic(
                  "inferred local 'missing' requires an initializer"),
              "var without an initializer was accepted");
  test.expect(invalid_inference.has_diagnostic(
                  "final local 'alsoMissing' requires an initializer"),
              "final local without an initializer was accepted");
  test.expect(invalid_inference.has_diagnostic("from null"),
              "null-only local inference was accepted");
}

void core_print_intrinsic(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("HelloWorld.co",
            "HelloWorld() {}\n"
            "func Main() {\n"
            "  HelloWorld value = HelloWorld();\n"
            "  print(\"hello\"); print(1); print(true); print('C');\n"
            "  print(1.5); print(value); print(null);\n"
            "  println(\"cloth\"); println(value); println();\n"
            "}\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid core print call produced semantic errors");
  const std::vector<cloth::SymbolId> print =
      valid.result->semantics.find_intrinsics("print");
  const std::vector<cloth::SymbolId> println =
      valid.result->semantics.find_intrinsics("println");
  test.expect(print.size() == 16,
              "complete core print overload set was not registered");
  test.expect(println.size() == 17,
              "complete core println overload set was not registered");
  bool bound_string = false;
  bool bound_int32 = false;
  bool bound_bool = false;
  bool bound_char = false;
  bool bound_float64 = false;
  bool bound_object = false;
  bool bound_newline = false;
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
      bound_char = bound_char || intrinsic == cloth::IntrinsicKind::kPrintChar;
      bound_float64 =
          bound_float64 || intrinsic == cloth::IntrinsicKind::kPrintFloat64;
      bound_object =
          bound_object || intrinsic == cloth::IntrinsicKind::kPrintObject;
      bound_newline =
          bound_newline || intrinsic == cloth::IntrinsicKind::kPrintNewline;
    }
  }
  test.expect(bound_string && bound_int32 && bound_bool && bound_char &&
                  bound_float64 && bound_object && bound_newline,
              "print and println overloads were not retained in typed HIR");

  const std::vector<std::pair<std::string_view, cloth::IntrinsicKind>>
      primitive_overloads{
          {"String", cloth::IntrinsicKind::kPrintString},
          {"bool", cloth::IntrinsicKind::kPrintBool},
          {"char", cloth::IntrinsicKind::kPrintChar},
          {"byte", cloth::IntrinsicKind::kPrintUint8},
          {"int8", cloth::IntrinsicKind::kPrintInt8},
          {"int16", cloth::IntrinsicKind::kPrintInt16},
          {"int32", cloth::IntrinsicKind::kPrintInt32},
          {"int64", cloth::IntrinsicKind::kPrintInt64},
          {"uint8", cloth::IntrinsicKind::kPrintUint8},
          {"uint16", cloth::IntrinsicKind::kPrintUint16},
          {"uint32", cloth::IntrinsicKind::kPrintUint32},
          {"uint64", cloth::IntrinsicKind::kPrintUint64},
          {"float32", cloth::IntrinsicKind::kPrintFloat32},
          {"float64", cloth::IntrinsicKind::kPrintFloat64},
      };
  for (const auto& [type_name, expected_intrinsic] : primitive_overloads) {
    const std::optional<cloth::TypeId> type =
        valid.result->semantics.find_type(type_name);
    bool found = false;
    for (const cloth::SymbolId symbol_id : print) {
      const cloth::SemanticSymbol& symbol =
          valid.result->semantics.symbol(symbol_id);
      found = found || (type && symbol.parameter_types == std::vector{*type} &&
                        symbol.intrinsic == expected_intrinsic);
    }
    test.expect(found, "primitive print overload is missing");
  }

  AnalyzedCompilation invalid;
  invalid.add("BadPrint.co", "func Main() { print(); println(1, 2); }\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic("no matching overload"),
              "invalid print arity was accepted");

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

void for_binding_scope_and_types(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Binding.co",
            "func Read(String[] values): String {\n"
            "  String value = \"outer\";\n"
            "  for (var values in values) {\n"
            "    String copy = values;\n"
            "    values = copy;\n"
            "  }\n"
            "  for (String value in values) { value = \"changed\"; }\n"
            "  return value;\n"
            "}\n");
  valid.analyze();
  test.expect(valid.error_count() == 0,
              "valid reference iteration or loop shadowing failed");

  std::size_t string_bindings = 0;
  const cloth::TypeId string_type =
      *valid.result->semantics.find_type("String");
  for (const cloth::HirStatement& statement :
       valid.result->hir.storage.statements()) {
    const auto* loop = std::get_if<cloth::HirForStatement>(&statement.data);
    if (loop != nullptr && loop->variable &&
        valid.result->semantics.symbol(*loop->variable).type == string_type) {
      ++string_bindings;
    }
  }
  test.expect(string_bindings == 2,
              "reference iteration bindings have the wrong inferred type");

  AnalyzedCompilation invalid;
  invalid.add("BadBindings.co",
              "func Duplicate(int32[] values) {\n"
              "  for (var value in values) { int32 value = 0; }\n"
              "}\n"
              "func InvalidType(int32[] values) {\n"
              "  for (Missing value in values) {}\n"
              "  for (var value in missing) {}\n"
              "}\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic("duplicate local name 'value'"),
              "loop binding and body local did not share a scope");
  test.expect(invalid.has_diagnostic("unknown type 'Missing'"),
              "unknown explicit iteration type was accepted");
  test.expect(invalid.has_diagnostic("unknown name 'missing'"),
              "unknown iterable did not produce its primary diagnostic");
  test.expect(!invalid.has_diagnostic("type '<error>' is not iterable"),
              "invalid iterable produced a cascading type diagnostic");
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
      {"void callable contract", void_callable_contract},
      {"final binding contract", final_binding_contract},
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
      {"for binding scope and types", for_binding_scope_and_types},
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
