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
                  "string Label = \"cloth\";\n"
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
            "static func Explicit(): void { return; }\n"
            "static func Implicit() { Explicit(); }\n"
            "static func Main(): void { Implicit(); return; }\n");
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
            "final string Label = \"cloth\";\n"
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

void non_null_field_initialization(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Profile.co",
            "string Name;\n"
            "string? Nickname;\n"
            "int32[] Scores;\n"
            "int32 Count;\n"
            "Profile(string name, int32[] scores, bool replace) {\n"
            "  if (replace) { Name = name; } else { self.Name = name; }\n"
            "  Scores = scores;\n"
            "  Name = name;\n"
            "  println(Name);\n"
            "  Touch();\n"
            "  self.Touch();\n"
            "}\n"
            "Profile(string name, int32[] scores) {\n"
            "  Name = name;\n"
            "  Scores = scores;\n"
            "}\n"
            "func Touch() {}\n");
  valid.analyze();
  test.expect(valid.error_count() == 0,
              "valid non-null field initialization produced errors");

  AnalyzedCompilation declaration_initialized;
  declaration_initialized.add(
      "Defaults.co",
      "string Label = \"ready\";\nstring? Optional;\nint32 Count;\n");
  declaration_initialized.analyze();
  test.expect(declaration_initialized.error_count() == 0,
              "declaration-initialized non-null field was rejected");

  AnalyzedCompilation missing_constructor;
  missing_constructor.add("Missing.co",
                          "string Name;\nstring? Optional;\nint32 Count;\n");
  missing_constructor.analyze();
  test.expect(missing_constructor.has_diagnostic(
                  "non-null field 'Name' requires an initializer or a "
                  "constructor"),
              "non-null field without construction was accepted");

  AnalyzedCompilation partial;
  partial.add("Partial.co",
              "string Name;\n"
              "int32[] Values;\n"
              "Partial(bool assign, string name, int32[] values) {\n"
              "  if (assign) { Name = name; }\n"
              "  Values = values;\n"
              "}\n");
  partial.analyze();
  test.expect(partial.has_diagnostic(
                  "constructor exits before non-null field 'Name' is "
                  "initialized"),
              "partial non-null initialization was accepted");

  AnalyzedCompilation early_return;
  early_return.add("Early.co",
                   "string Name;\n"
                   "Early(bool stop, string name) {\n"
                   "  if (stop) { return; }\n"
                   "  Name = name;\n"
                   "}\n");
  early_return.analyze();
  test.expect(early_return.has_diagnostic(
                  "constructor exits before non-null field 'Name' is "
                  "initialized"),
              "early constructor return bypassed non-null initialization");

  AnalyzedCompilation read_before_initialization;
  read_before_initialization.add(
      "ReadBefore.co", "string First = Second;\nstring Second = \"ready\";\n");
  read_before_initialization.analyze();
  test.expect(read_before_initialization.has_diagnostic(
                  "non-null field 'Second' is read before it is initialized"),
              "non-null field initializer order was ignored");

  AnalyzedCompilation indirect;
  indirect.add("Indirect.co",
               "string Name;\n"
               "Indirect(string name) { println(Name = name); }\n");
  indirect.analyze();
  test.expect(indirect.has_diagnostic(
                  "non-null field 'Name' initialization must be a direct "
                  "constructor statement"),
              "indirect non-null initialization was accepted");
  test.expect(indirect.has_diagnostic(
                  "constructor exits before non-null field 'Name' is "
                  "initialized"),
              "indirect assignment established definite initialization");

  AnalyzedCompilation premature_self_use;
  premature_self_use.add("Escape.co",
                         "string Name;\n"
                         "Escape(string name) {\n"
                         "  Publish(self);\n"
                         "  Touch();\n"
                         "  self.Touch();\n"
                         "  Name = name;\n"
                         "}\n"
                         "static func Publish(Escape value) {}\n"
                         "func Touch() {}\n");
  premature_self_use.analyze();
  test.expect(premature_self_use.has_diagnostic(
                  "cannot use 'self' before non-null field 'Name' is "
                  "initialized"),
              "self escaped before non-null initialization completed");
  test.expect(premature_self_use.has_diagnostic(
                  "instance function 'Touch' cannot be called before "
                  "non-null field 'Name' is initialized"),
              "instance function observed a partially initialized object");
  test.expect(premature_self_use.error_count() == 3,
              "implicit and self-qualified premature calls were not both "
              "diagnosed");

  AnalyzedCompilation loop;
  loop.add("Loop.co",
           "string Name;\n"
           "Loop(string name) { while (false) { Name = name; } }\n");
  loop.analyze();
  test.expect(
      loop.has_diagnostic("constructor exits before non-null field 'Name' is "
                          "initialized"),
      "loop-only non-null initialization was accepted");
  test.expect(!loop.has_diagnostic("internal"),
              "invalid non-null initialization leaked an internal error");
}

void core_print_intrinsic(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("HelloWorld.co",
            "HelloWorld() {}\n"
            "static func Main() {\n"
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
          {"string", cloth::IntrinsicKind::kPrintString},
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
  invalid.add("BadPrint.co",
              "static func Main() { print(); println(1, 2); }\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic("no matching overload"),
              "invalid print arity was accepted");

  AnalyzedCompilation shadowed;
  shadowed.add("Shadow.co",
               "static func print(int value) {}\n"
               "static func Main() { print(1); }\n");
  shadowed.analyze();
  test.expect(shadowed.error_count() == 0,
              "source member did not shadow core print");
}

void cross_file_binding(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("App.co",
                  "func Load(int id): User? { return User.Find(id); }\n");
  compilation.add("User.co",
                  "static func Find(int32 id): User? { return null; }\n");
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
                  "func Load(): ModelUser? { return Factory.Make(); }\n"
                  "func Help(): int { return Helper.Value(); }\n",
                  "app");
  compilation.add("app/Helper.co", "static func Value(): int { return 8; }\n",
                  "app");
  compilation.add("models/User.co", "", "models");
  compilation.add("services/Factory.co",
                  "import models::User;\n"
                  "static func Make(): User? { return null; }\n",
                  "services");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "valid package imports produced semantic errors");
  test.expect(compilation.result->is_valid,
              "valid package compilation was marked invalid");

  AnalyzedCompilation cyclic;
  cyclic.add("alpha/A.co",
             "import beta::B;\nfunc Other(): B? { return null; }\n", "alpha");
  cyclic.add("beta/B.co",
             "import alpha::A;\nfunc Other(): A? { return null; }\n", "beta");
  cyclic.analyze();
  test.expect(cyclic.error_count() == 0, "cyclic type imports were rejected");
}

void inheritance_graph(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add("models/Base.co", "class {}\n", "models");
  compilation.add("app/Middle.co",
                  "import models::Base as Parent;\n"
                  "class : Parent {}\n",
                  "app");
  compilation.add("app/Leaf.co", "class : Middle {}\n", "app");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "valid inheritance graph produced semantic errors");
  test.expect(compilation.result->is_valid,
              "valid inheritance graph was marked invalid");
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  test.expect(!semantics.file(cloth::FileId{0}).base_file,
              "root class unexpectedly has a base");
  test.expect(semantics.file(cloth::FileId{1}).base_file == cloth::FileId{0},
              "import alias did not resolve as the base class");
  test.expect(semantics.file(cloth::FileId{2}).base_file == cloth::FileId{1},
              "same-package base class did not resolve");
}

void constructor_initialization(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Base.co", "Base(object value) {}\nBase(string value) {}\n");
  valid.add("Derived.co",
            "class : Base {\n"
            "  Derived(string value): Base(value) {}\n"
            "}\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid base constructor initializer produced errors");
  test.expect(valid.result->is_valid,
              "valid constructor chain was marked invalid");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::SymbolId base_constructor =
      semantics.file(cloth::FileId{0}).constructors[1];
  const cloth::SymbolId derived_constructor =
      semantics.file(cloth::FileId{1}).constructors[0];
  test.expect(semantics.symbol(derived_constructor).base_constructor ==
                  base_constructor,
              "derived constructor lost its bound base constructor");
  test.expect(
      valid.result->hir.files[1].constructors[0].initializer &&
          valid.result->hir.files[1].constructors[0].initializer->constructor ==
              base_constructor,
      "HIR lost the base constructor binding");

  AnalyzedCompilation invalid;
  invalid.add("Base.co", "Base(int32 value) {}\n");
  invalid.add("Other.co", "Other() {}\n");
  invalid.add("Missing.co", "class : Base {\nMissing(int32 value) {}\n}\n");
  invalid.add("Wrong.co", "class : Base {\nWrong(): Other() {}\n}\n");
  invalid.add("Mismatch.co",
              "class : Base {\nMismatch(): Base(\"bad\") {}\n}\n");
  invalid.add("Premature.co",
              "class : Base {\n"
              "  int32 Value;\n"
              "  Premature(): Base(Value) {}\n"
              "}\n");
  invalid.add("PrematureSelf.co",
              "class : Base { PrematureSelf(): Base(self) {} }\n");
  invalid.add("PrematureCall.co",
              "class : Base {\n"
              "  func Value(): int32 { return 0; }\n"
              "  PrematureCall(): Base(Value()) {}\n"
              "}\n");
  invalid.add("Root.co", "Root(): Root() {}\n");
  invalid.analyze();

  test.expect(invalid.has_diagnostic("must initialize base 'Base'"),
              "missing base initializer was accepted");
  test.expect(invalid.has_diagnostic("must name direct base 'Base'"),
              "non-base constructor initializer was accepted");
  test.expect(invalid.has_diagnostic("no matching base constructor 'Base'"),
              "mismatched base constructor arguments were accepted");
  test.expect(invalid.has_diagnostic(
                  "cannot use instance field 'Value' in a base constructor"),
              "derived instance state was available before base construction");
  test.expect(invalid.has_diagnostic(
                  "cannot use 'self' in a base constructor initializer"),
              "self was available before base construction");
  test.expect(
      invalid.has_diagnostic(
          "cannot call instance function 'Value' in a base constructor"),
      "instance behavior was available before base construction");
  test.expect(invalid.has_diagnostic(
                  "root class constructor cannot have a base initializer"),
              "root constructor accepted a base initializer");
}

void inherited_members_and_subtyping(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Base.co",
            "string Name;\n"
            "Base(string name) { Name = name; }\n"
            "func Read(): string { return Name; }\n"
            "static func Kind(): string { return \"base\"; }\n");
  valid.add("Middle.co",
            "class : Base { Middle(string name): Base(name) {} }\n");
  valid.add("Derived.co",
            "class : Middle {\n"
            "  Derived(string name): Middle(name) {}\n"
            "  func Rename(string name) { Name = name; }\n"
            "  func Own(): string { return Read(); }\n"
            "}\n");
  valid.add("Use.co",
            "func TakeBase(Base value): string { return value.Read(); }\n"
            "func Test(Derived value): string {\n"
            "  Base parent = value;\n"
            "  Base? maybe = value;\n"
            "  string direct = value.Name;\n"
            "  string called = value.Read();\n"
            "  string staticValue = Derived.Kind();\n"
            "  string? safe = maybe?.Name;\n"
            "  bool ancestor = value is Base;\n"
            "  bool descendant = parent is Derived;\n"
            "  Derived? cast = parent as Derived?;\n"
            "  return TakeBase(value);\n"
            "}\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid inherited lookup and subtyping produced errors");
  test.expect(valid.result->is_valid,
              "valid inherited lookup and subtyping were marked invalid");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::SymbolId base_name = semantics.file(cloth::FileId{0}).fields[0];
  const cloth::SymbolId base_read =
      semantics.file(cloth::FileId{0}).functions[0];
  bool derived_bound_field = false;
  bool derived_bound_function = false;
  for (const cloth::ExpressionSemantics& expression :
       semantics.file(cloth::FileId{2}).expressions) {
    derived_bound_field = derived_bound_field || expression.symbol == base_name;
    derived_bound_function =
        derived_bound_function || expression.symbol == base_read;
  }
  test.expect(derived_bound_field && derived_bound_function,
              "unqualified inherited members lost their base symbols");

  AnalyzedCompilation invalid;
  invalid.add("Base.co",
              "int32 secret;\n"
              "Base() {}\n"
              "func hidden(): int32 { return secret; }\n");
  invalid.add("Derived.co",
              "class : Base {\n"
              "  Derived(): Base() {}\n"
              "  func HiddenCall(): int32 { return hidden(); }\n"
              "  func HiddenField(): int32 { return secret; }\n"
              "}\n");
  invalid.add("Other.co", "Other() {}\n");
  invalid.add("Use.co",
              "func Reverse(Base parent, Other other) {\n"
              "  Derived child = parent;\n"
              "  Base unrelated = other;\n"
              "  Other? cast = parent as Other?;\n"
              "}\n"
              "func Explicit(Derived value): int32 {\n"
              "  return value.hidden();\n"
              "}\n");
  invalid.analyze();

  test.expect(invalid.has_diagnostic("inherited member 'hidden' is private"),
              "private inherited function was accessible without a receiver");
  test.expect(invalid.has_diagnostic("inherited member 'secret' is private"),
              "private inherited field was accessible without a receiver");
  test.expect(invalid.has_diagnostic("member 'hidden' is private"),
              "private inherited function was accessible through a receiver");
  test.expect(invalid.has_diagnostic(
                  "local initializer has type 'Base'; expected 'Derived'"),
              "base-to-derived assignment was accepted implicitly");
  test.expect(invalid.has_diagnostic(
                  "local initializer has type 'Other'; expected 'Base'"),
              "unrelated file classes were assignment-compatible");
  test.expect(
      invalid.has_diagnostic(
          "types 'Base' and 'Other' cannot overlap without inheritance"),
      "unrelated checked cast was accepted");
}

void virtual_override_contract(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Base.co",
            "func Describe(): string { return \"base\"; }\n"
            "func Count(): int32 { return 1; }\n"
            "func hidden(): int32 { return 2; }\n");
  valid.add("Middle.co",
            "class : Base {\n"
            "  override func Describe(): string { return \"middle\"; }\n"
            "}\n");
  valid.add("Derived.co",
            "class : Middle {\n"
            "  override func Describe(): string { return \"derived\"; }\n"
            "  func Extra(): int32 { return Count(); }\n"
            "}\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid override hierarchy produced semantic errors");
  test.expect(valid.result->is_valid,
              "valid override hierarchy was marked invalid");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::SemanticSymbol& base_describe =
      semantics.symbol(semantics.file(cloth::FileId{0}).functions[0]);
  const cloth::SemanticSymbol& middle_describe =
      semantics.symbol(semantics.file(cloth::FileId{1}).functions[0]);
  const cloth::SemanticSymbol& derived_describe =
      semantics.symbol(semantics.file(cloth::FileId{2}).functions[0]);
  test.expect(base_describe.virtual_slot == 0 &&
                  middle_describe.virtual_slot == 0 &&
                  derived_describe.virtual_slot == 0,
              "override chain did not retain a stable virtual slot");
  test.expect(middle_describe.overridden_symbol ==
                      semantics.file(cloth::FileId{0}).functions[0] &&
                  derived_describe.overridden_symbol ==
                      semantics.file(cloth::FileId{1}).functions[0],
              "override declarations lost their immediate base symbols");
  test.expect(semantics.file(cloth::FileId{2}).virtual_functions.size() == 3 &&
                  semantics.file(cloth::FileId{2}).virtual_functions[0] ==
                      semantics.file(cloth::FileId{2}).functions[0],
              "derived virtual table has the wrong implementations");
  const cloth::SemanticSymbol& hidden =
      semantics.symbol(semantics.file(cloth::FileId{0}).functions[2]);
  test.expect(!hidden.virtual_slot,
              "private function unexpectedly received a virtual slot");

  AnalyzedCompilation invalid;
  invalid.add("Base.co", "func Describe(): string { return \"base\"; }\n");
  invalid.add("Missing.co",
              "class : Base {\n"
              "  func Describe(): string { return \"missing\"; }\n"
              "}\n");
  invalid.add("WrongReturn.co",
              "class : Base {\n"
              "  override func Describe(): int32 { return 1; }\n"
              "}\n");
  invalid.add("NoTarget.co",
              "class : Base {\n"
              "  override func Other(): string { return \"other\"; }\n"
              "}\n");
  invalid.add("Static.co",
              "class : Base {\n"
              "  static override func Utility() {}\n"
              "}\n");
  invalid.add("Private.co",
              "class : Base {\n"
              "  override func hidden() {}\n"
              "}\n");
  invalid.analyze();

  test.expect(invalid.has_diagnostic("add 'override'"),
              "accidental override was accepted");
  test.expect(invalid.has_diagnostic("inherited function returns 'string'"),
              "incompatible override return type was accepted");
  test.expect(invalid.has_diagnostic("does not override an inherited function"),
              "override without a target was accepted");
  test.expect(invalid.has_diagnostic(
                  "static function 'Utility' cannot be declared override"),
              "static override was accepted");
  test.expect(invalid.has_diagnostic(
                  "private function 'hidden' cannot be declared override"),
              "private override was accepted");
}

void invalid_inheritance_graph(TestContext& test) {
  AnalyzedCompilation self_cycle;
  self_cycle.add("Self.co", "class : Self {}\n");
  self_cycle.analyze();
  test.expect(
      self_cycle.has_diagnostic("file class 'Self' cannot inherit from itself"),
      "direct inheritance cycle was accepted");

  AnalyzedCompilation indirect_cycle;
  indirect_cycle.add("B.co", "class : C {}\n");
  indirect_cycle.add("C.co", "class : A {}\n");
  indirect_cycle.add("A.co", "class : B {}\n");
  indirect_cycle.analyze();
  test.expect(indirect_cycle.has_diagnostic(
                  "inheritance cycle detected: A -> B -> C -> A"),
              "indirect inheritance cycle was not diagnosed deterministically");
  test.expect(
      !indirect_cycle.result->semantics.file(cloth::FileId{0}).is_valid &&
          !indirect_cycle.result->semantics.file(cloth::FileId{1}).is_valid &&
          !indirect_cycle.result->semantics.file(cloth::FileId{2}).is_valid,
      "cycle participants were not marked invalid");

  AnalyzedCompilation reordered_cycle;
  reordered_cycle.add("C.co", "class : A {}\n");
  reordered_cycle.add("A.co", "class : B {}\n");
  reordered_cycle.add("B.co", "class : C {}\n");
  reordered_cycle.analyze();
  test.expect(reordered_cycle.has_diagnostic(
                  "inheritance cycle detected: A -> B -> C -> A"),
              "cycle diagnostic changed with source registration order");

  AnalyzedCompilation inaccessible;
  inaccessible.add("secret.co", "class {}\n");
  inaccessible.add("Derived.co", "class : secret {}\n");
  inaccessible.add("Unknown.co", "class : Missing {}\n");
  inaccessible.analyze();
  test.expect(inaccessible.has_diagnostic("file class 'secret' is private"),
              "private base class was accepted");
  test.expect(inaccessible.has_diagnostic("unknown type 'Missing'"),
              "unknown base class was accepted");
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
  compilation.add("User.co", "static func hidden(): bool { return true; }\n");
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
                  "string Name;\n"
                  "User(string name) { Name = name; }\n");
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
                  "func Empty(): User? { return null; }\n"
                  "func Present(User value): User? { return value; }\n"
                  "func Missing(): User { return null; }\n"
                  "func Number(): int { return null; }\n");
  compilation.analyze();

  test.expect(!compilation.has_diagnostic(
                  "return value has type 'null'; expected 'User?'"),
              "null was rejected for a nullable reference type");
  test.expect(compilation.has_diagnostic(
                  "return value has type 'null'; expected 'User'"),
              "null was accepted for a non-null reference type");
  test.expect(compilation.has_diagnostic(
                  "return value has type 'null'; expected 'int32'"),
              "null was accepted for a value type");

  const cloth::SemanticModel& semantics = compilation.result->semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{1});
  const cloth::SemanticType& nullable =
      semantics.type(semantics.symbol(file.functions[0]).type);
  test.expect(
      nullable.kind == cloth::TypeKind::kNullable &&
          nullable.element_type == semantics.file(cloth::FileId{0}).type &&
          nullable.name == "User?",
      "nullable file-class type was not canonicalized");
}

void nullable_reference_shapes(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("User.co", "string? Name;\n");
  valid.add("Shapes.co",
            "func Values(User first): User?[] { return [first, null]; }\n"
            "func MaybeValues(): User[]? { return null; }\n"
            "func Both(): User?[]? { return null; }\n"
            "func Accept(User? value) {}\n"
            "func Widen(User value) { Accept(value); }\n"
            "func Text(string value): string? { return value; }\n"
            "func Iterate(User[] values) { "
            "for (User? value in values) {} }\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid nullable reference shapes produced semantic errors");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::TypeId user = semantics.file(cloth::FileId{0}).type;
  const cloth::FileSemantics& shapes = semantics.file(cloth::FileId{1});

  const cloth::SemanticType& values =
      semantics.type(semantics.symbol(shapes.functions[0]).type);
  test.expect(values.kind == cloth::TypeKind::kArray && values.element_type &&
                  semantics.type(*values.element_type).kind ==
                      cloth::TypeKind::kNullable &&
                  semantics.type(*values.element_type).element_type == user,
              "nullable array element type is structurally wrong");

  const cloth::SemanticType& maybe_values =
      semantics.type(semantics.symbol(shapes.functions[1]).type);
  test.expect(
      maybe_values.kind == cloth::TypeKind::kNullable &&
          maybe_values.element_type &&
          semantics.type(*maybe_values.element_type).kind ==
              cloth::TypeKind::kArray &&
          semantics.type(*maybe_values.element_type).element_type == user,
      "nullable array type is structurally wrong");

  const cloth::SemanticType& both =
      semantics.type(semantics.symbol(shapes.functions[2]).type);
  test.expect(
      both.kind == cloth::TypeKind::kNullable && both.element_type &&
          semantics.type(*both.element_type).kind == cloth::TypeKind::kArray &&
          semantics.type(*both.element_type).element_type &&
          semantics.type(*semantics.type(*both.element_type).element_type)
                  .kind == cloth::TypeKind::kNullable,
      "combined array nullability is structurally wrong");

  AnalyzedCompilation invalid;
  invalid.add("User.co", "string? Name;\n");
  invalid.add("BadNulls.co",
              "func Primitive(int32? value) {}\n"
              "func PrimitiveElements(int32?[] values) {}\n"
              "func BadVoid(): void? {}\n"
              "func Need(User value) {}\n"
              "func Reject(User? value) { Need(value); value.Name; }\n"
              "func Index(User[]? values) { values[0]; }\n"
              "func Iterate(User[]? values) { "
              "for (var value in values) {} }\n");
  invalid.analyze();

  test.expect(invalid.has_diagnostic(
                  "nullable marker requires a reference type; 'int32' is a "
                  "value type"),
              "nullable value type was accepted");
  test.expect(invalid.has_diagnostic("'void' cannot be nullable"),
              "nullable void type was accepted");
  test.expect(invalid.has_diagnostic("no matching overload"),
              "nullable argument was passed to a non-null parameter");
  test.expect(invalid.has_diagnostic(
                  "nullable type 'User?' has no members without narrowing"),
              "nullable member access was accepted without narrowing");
  test.expect(invalid.has_diagnostic(
                  "nullable array type 'User[]?' cannot be indexed without "
                  "narrowing"),
              "nullable array indexing was accepted without narrowing");
  test.expect(invalid.has_diagnostic("type 'User[]?' is not iterable"),
              "nullable array iteration was accepted without narrowing");
  test.expect(!invalid.has_diagnostic("internal"),
              "invalid nullable use leaked an internal diagnostic");
}

void nullable_flow_narrowing(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("User.co", "string Name = \"Ada\";\n");
  valid.add("Flow.co",
            "func Read(User? value): string? {\n"
            "  if (value != null) { return value.Name; }\n"
            "  return null;\n"
            "}\n"
            "func Guard(User? value): string {\n"
            "  if (value == null) { return \"missing\"; }\n"
            "  return value.Name;\n"
            "}\n"
            "func Negated(User? value): string? {\n"
            "  if (!(value == null)) { return value.Name; }\n"
            "  return null;\n"
            "}\n"
            "func Conjunction(User? value): bool {\n"
            "  return value != null && value.Name == \"Ada\";\n"
            "}\n"
            "func Disjunction(User? value): bool {\n"
            "  return value == null || value.Name == \"Ada\";\n"
            "}\n"
            "func Reverse(User? value): string? {\n"
            "  if (null != value) { return value.Name; }\n"
            "  return null;\n"
            "}\n"
            "func ArrayLength(int32[]? values): int32 {\n"
            "  if (values == null) { return 0; }\n"
            "  return values::length;\n"
            "}\n"
            "func First(int32[]? values): int32 {\n"
            "  if (values != null) { return values[0]; }\n"
            "  return 0;\n"
            "}\n"
            "func Sum(int32[]? values): int32 {\n"
            "  if (values == null) { return 0; }\n"
            "  int32 total = 0;\n"
            "  for (var value in values) { total = total + value; }\n"
            "  return total;\n"
            "}\n"
            "func Reset(User? value) {\n"
            "  if (value != null) { value = null; }\n"
            "}\n"
            "func Consume(User? value) {\n"
            "  while (value != null) {\n"
            "    println(value.Name);\n"
            "    value = null;\n"
            "  }\n"
            "}\n"
            "func Final(final User? value): string? {\n"
            "  if (value != null) { return value.Name; }\n"
            "  return null;\n"
            "}\n"
            "func Local(User? value): string? {\n"
            "  User? copy = value;\n"
            "  if (copy != null) { return copy.Name; }\n"
            "  return null;\n"
            "}\n"
            "func Both(User? left, User? right): bool {\n"
            "  return left != null && right != null &&\n"
            "      left.Name == right.Name;\n"
            "}\n"
            "func Neither(User? left, User? right): bool {\n"
            "  if (left == null || right == null) { return true; }\n"
            "  return left.Name == right.Name;\n"
            "}\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid nullable flow narrowing produced errors");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::SymbolId parameter =
      semantics.symbol(semantics.file(cloth::FileId{1}).functions[0])
          .parameter_symbols[0];
  const cloth::TypeId user_type = semantics.file(cloth::FileId{0}).type;
  bool found_narrowed_read = false;
  for (const cloth::HirExpression& expression :
       valid.result->hir.storage.expressions()) {
    const auto* symbol =
        std::get_if<cloth::HirSymbolExpression>(&expression.data);
    found_narrowed_read = found_narrowed_read ||
                          (symbol != nullptr && symbol->symbol == parameter &&
                           expression.type == user_type);
  }
  test.expect(found_narrowed_read,
              "HIR did not retain the narrowed non-null read type");

  AnalyzedCompilation invalid;
  invalid.add("User.co", "string Name = \"Ada\";\n");
  invalid.add("BadFlow.co",
              "User? Current;\n"
              "func Outside(User? value): string {\n"
              "  if (value != null) {}\n"
              "  return value.Name;\n"
              "}\n"
              "func Reassigned(User? value): string? {\n"
              "  if (value != null) {\n"
              "    value = null;\n"
              "    return value.Name;\n"
              "  }\n"
              "  return null;\n"
              "}\n"
              "func Joined(User? value, bool choose): string? {\n"
              "  if (value != null) {\n"
              "    if (choose) { value = null; }\n"
              "    return value.Name;\n"
              "  }\n"
              "  return null;\n"
              "}\n"
              "func Field(): string? {\n"
              "  if (Current != null) { return Current.Name; }\n"
              "  return null;\n"
              "}\n"
              "func ConditionWrite(User? value): bool {\n"
              "  return value != null &&\n"
              "      ((value = null) == null || value.Name == \"Ada\");\n"
              "}\n");
  invalid.analyze();

  test.expect(invalid.has_diagnostic(
                  "nullable type 'User?' has no members without narrowing"),
              "unsafe nullable read was accepted");
  test.expect(invalid.error_count() == 5,
              "nullable refinements survived an unsafe scope or write");
  test.expect(!invalid.has_diagnostic("internal"),
              "invalid nullable flow leaked an internal diagnostic");
}

void null_ergonomics(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("User.co",
            "string Name = \"Ada\";\n"
            "string? Alias;\n"
            "User? Manager;\n"
            "int32 Count;\n"
            "func Greet() {}\n");
  valid.add("NullErgonomics.co",
            "func Display(User? user, User? backup, bool enabled): string {\n"
            "  string? name = user?.Name;\n"
            "  string? alias = user?.Alias;\n"
            "  if (user) { println(user.Name); }\n"
            "  if (!user) { println(\"missing\"); }\n"
            "  while (backup) { println(backup.Name); backup = null; }\n"
            "  if (user && enabled) { println(user.Name); }\n"
            "  User actual = user!;\n"
            "  return user.Name;\n"
            "}\n"
            "func Required(User? user, User backup): User {\n"
            "  return user ?? backup;\n"
            "}\n"
            "func Optional(User? user, User? backup): User? {\n"
            "  return user ?? backup;\n"
            "}\n"
            "func Safe(User? user): string? { return user?.Name; }\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid null ergonomics produced semantic errors");
  bool found_presence_test = false;
  for (const cloth::ExpressionSemantics& expression :
       valid.result->semantics.file(cloth::FileId{1}).expressions) {
    found_presence_test = found_presence_test || expression.is_presence_test;
  }
  test.expect(found_presence_test,
              "nullable condition was not marked as a presence test");

  AnalyzedCompilation invalid;
  invalid.add("User.co",
              "string Name = \"Ada\";\n"
              "int32 Count;\n"
              "func Greet() {}\n");
  invalid.add(
      "BadNullErgonomics.co",
      "func NonNullableCondition(User value) { if (value) {} }\n"
      "func SafeNonNullable(User value): string? { return value?.Name; }\n"
      "func SafeValue(User? value): string? { value?.Count; return null; }\n"
      "func SafeCall(User? value) { value?.Greet(); }\n"
      "func BadCoalesce(User value): User { return value ?? value; }\n"
      "func WrongFallback(User? value): User { return value ?? \"x\"; }\n"
      "func BadAssert(User value): User { return value!; }\n"
      "func Assign(User? value) { value?.Name = \"x\"; }\n");
  invalid.analyze();

  test.expect(invalid.has_diagnostic(
                  "if condition uses a non-null reference and is always true"),
              "non-null reference condition was accepted");
  test.expect(invalid.has_diagnostic(
                  "safe member access requires a nullable reference"),
              "safe access accepted a non-null receiver");
  test.expect(invalid.has_diagnostic(
                  "safe access to value-type field 'Count' requires nullable "
                  "value types"),
              "safe access accepted a value-type field");
  test.expect(invalid.has_diagnostic(
                  "safe function calls are not implemented; narrow the "
                  "receiver first"),
              "safe access accepted an instance function call");
  test.expect(invalid.has_diagnostic(
                  "left operand of the null-coalescing operator must be "
                  "nullable"),
              "coalescing accepted a non-null left operand");
  test.expect(invalid.has_diagnostic(
                  "right operand of the null-coalescing operator has type "
                  "'string'"),
              "coalescing accepted an incompatible fallback");
  test.expect(invalid.has_diagnostic(
                  "non-null assertion requires a nullable reference"),
              "non-null assertion accepted a non-null operand");
  test.expect(invalid.has_diagnostic("assignment target is not mutable"),
              "safe member access was accepted as an assignment target");
  test.expect(!invalid.has_diagnostic("internal"),
              "invalid null ergonomics leaked an internal diagnostic");
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
            "  return values::length + values[0];\n"
            "}\n"
            "func Empty(): int32[]? { return null; }\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid array operations produced semantic errors");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  const cloth::TypeId return_type = semantics.symbol(file.functions[1]).type;
  const cloth::SemanticType& nullable_array = semantics.type(return_type);
  test.expect(nullable_array.kind == cloth::TypeKind::kNullable &&
                  nullable_array.element_type &&
                  semantics.type(*nullable_array.element_type).kind ==
                      cloth::TypeKind::kArray &&
                  semantics.type(*nullable_array.element_type).element_type ==
                      semantics.find_type("int32"),
              "nullable array return type was not canonicalized");

  AnalyzedCompilation invalid;
  invalid.add("BadArrays.co",
              "func Bad() {\n"
              "  int32[] empty = [];\n"
              "  int32[] mixed = [1, true];\n"
              "  int32 value = mixed[false];\n"
              "  int32 missing = mixed::Length;\n"
              "  int32 legacy = mixed.Length;\n"
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
  test.expect(invalid.has_diagnostic("has no meta query 'Length'"),
              "array meta query capitalization was ignored");
  test.expect(
      invalid.has_diagnostic("array length is a meta query; use '::length'"),
      "legacy array member syntax did not receive a migration hint");
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
            "func Read(string[] values): string {\n"
            "  string value = \"outer\";\n"
            "  for (var values in values) {\n"
            "    string copy = values;\n"
            "    values = copy;\n"
            "  }\n"
            "  for (string value in values) { value = \"changed\"; }\n"
            "  return value;\n"
            "}\n");
  valid.analyze();
  test.expect(valid.error_count() == 0,
              "valid reference iteration or loop shadowing failed");

  std::size_t string_bindings = 0;
  const cloth::TypeId string_type =
      *valid.result->semantics.find_type("string");
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
  compilation.add("User.co", "string Name = \"Ada\";\n");
  compilation.add("Reader.co",
                  "func Read(User value): string { return value.Name; }\n");
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

void static_member_contract(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add(
      "Statics.co",
      "static final int32 Version = 12;\n"
      "int32 value;\n"
      "static func Twice(int32 input): int32 { return input + input; }\n"
      "func Read(): int32 { return Twice(Version) + Statics.Version; }\n");
  compilation.analyze();

  test.expect(compilation.error_count() == 0,
              "valid static members produced semantic errors");
  const cloth::FileSemantics& file =
      compilation.result->semantics.file(cloth::FileId{0});
  test.expect(compilation.result->semantics.symbol(file.fields[0]).is_static &&
                  compilation.result->semantics.symbol(file.fields[0]).is_final,
              "static field metadata was not preserved");
  test.expect(
      compilation.result->semantics.symbol(file.functions[0]).is_static &&
          !compilation.result->semantics.symbol(file.functions[1]).is_static,
      "static function metadata was not preserved");
}

void invalid_static_members(TestContext& test) {
  AnalyzedCompilation compilation;
  compilation.add(
      "StaticErrors.co",
      "static int32 Mutable = 1;\n"
      "static final string Text = \"cloth\";\n"
      "static final int32 Missing;\n"
      "int32 value;\n"
      "func Read() {}\n"
      "static func Utility() {}\n"
      "static func Bad() { print(value); Read(); print(self); }\n"
      "func ThroughInstance(StaticErrors instance) { instance.Utility(); }\n"
      "func ThroughType() { StaticErrors.Read(); }\n"
      "func Main() {}\n");
  compilation.analyze();

  test.expect(compilation.has_diagnostic("must also be final"),
              "mutable static field was accepted");
  test.expect(compilation.has_diagnostic(
                  "static field initializer must be a scalar literal"),
              "reference-valued static field was accepted");
  test.expect(compilation.has_diagnostic("requires an initializer"),
              "uninitialized static field was accepted");
  test.expect(compilation.has_diagnostic(
                  "instance field 'value' is unavailable in a static context"),
              "static function accessed an implicit instance field");
  test.expect(
      compilation.has_diagnostic(
          "instance function 'Read' is unavailable in a static context"),
      "static function called an implicit instance function");
  test.expect(compilation.has_diagnostic("unknown name 'self'"),
              "static function received an implicit self binding");
  test.expect(
      compilation.has_diagnostic(
          "static function 'Utility' must be accessed through its file class"),
      "static function was accepted through an instance");
  test.expect(
      compilation.has_diagnostic("function 'Read' requires an instance"),
      "instance function was accepted through a file class");
  test.expect(
      compilation.has_diagnostic("entry point 'Main' must be declared static"),
      "instance Main declaration was accepted");
}

void string_value_semantics(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Strings.co",
            "func Inspect(string left, string right): int32 {\n"
            "  string joined = left + right;\n"
            "  bool equal = joined == \"cloth\";\n"
            "  bool different = left != right;\n"
            "  bool empty = joined::isEmpty;\n"
            "  if (equal && different && !empty) { return joined::length; }\n"
            "  return joined::byteLength;\n"
            "}\n"
            "func Narrow(string? value): int32 {\n"
            "  if (value) { return value::length; }\n"
            "  return 0;\n"
            "}\n"
            "func Assert(string? value): int32 { return value!::length; }\n"
            "func Literal(): int32 { return \"cloth\"::length; }\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid string operations produced semantic errors");
  bool found_length = false;
  bool found_byte_length = false;
  bool found_is_empty = false;
  for (const cloth::HirExpression& expression :
       valid.result->hir.storage.expressions()) {
    const auto* meta =
        std::get_if<cloth::HirStringMetaExpression>(&expression.data);
    if (meta == nullptr) {
      continue;
    }
    found_length =
        found_length || meta->query == cloth::StringMetaQuery::kLength;
    found_byte_length =
        found_byte_length || meta->query == cloth::StringMetaQuery::kByteLength;
    found_is_empty =
        found_is_empty || meta->query == cloth::StringMetaQuery::kIsEmpty;
  }
  test.expect(found_length && found_byte_length && found_is_empty,
              "string meta queries were not retained explicitly in HIR");

  AnalyzedCompilation uppercase_type;
  uppercase_type.add("String.co", "int32 Value = 1;\n");
  uppercase_type.add(
      "UsesString.co",
      "func Read(String value): int32 { return value.Value; }\n");
  uppercase_type.analyze();
  test.expect(uppercase_type.error_count() == 0,
              "a user-defined uppercase String type was shadowed by string");

  AnalyzedCompilation invalid;
  invalid.add("BadStrings.co",
              "String Legacy;\n"
              "func Subtract(string left, string right): string {\n"
              "  return left - right;\n"
              "}\n"
              "func WrongCase(string value): int32 { return value::Length; }\n"
              "func Legacy(string value): int32 { return value.Length; }\n"
              "func Nullable(string? value): int32 { return value::length; }\n"
              "func SafeNullable(string? value): int32 {\n"
              "  return value?.Length;\n"
              "}\n"
              "func Called(string value) { value::length(); }\n"
              "func Assigned(string value) { value::length = 1; }\n"
              "func Primitive(int32 value): int32 { return value::length; }\n");
  invalid.analyze();

  test.expect(invalid.has_diagnostic(
                  "unknown type 'String'; use the built-in type 'string'"),
              "legacy String spelling did not receive a migration diagnostic");
  test.expect(
      invalid.has_diagnostic(
          "operator 'minus' cannot be applied to 'string' and 'string'"),
      "string subtraction was accepted");
  test.expect(invalid.has_diagnostic("string has no meta query 'Length'"),
              "string meta query capitalization was ignored");
  test.expect(
      invalid.has_diagnostic("string length is a meta query; use '::length'"),
      "legacy string member syntax did not receive a migration hint");
  test.expect(invalid.has_diagnostic(
                  "nullable type 'string?' has no meta queries without "
                  "narrowing"),
              "nullable string meta query skipped narrowing");
  test.expect(invalid.has_diagnostic(
                  "safe meta queries are not supported; narrow the string and "
                  "use '::length'"),
              "safe string meta access produced the wrong diagnostic");
  test.expect(invalid.has_diagnostic("expression is not callable"),
              "a meta query was accepted as a method call");
  test.expect(invalid.has_diagnostic("assignment target is not mutable"),
              "a meta query was accepted as an assignment target");
  test.expect(invalid.has_diagnostic("type 'int32' has no Cloth meta queries"),
              "a meta query was accepted on an unsupported value type");

  std::string malformed_source = "func Broken(): string { return \"";
  malformed_source.push_back(static_cast<char>(0xC3));
  malformed_source += "\"; }\n";
  AnalyzedCompilation malformed;
  malformed.add("Malformed.co", std::move(malformed_source));
  malformed.analyze();
  test.expect(malformed.has_diagnostic("string literal is not valid UTF-8"),
              "malformed UTF-8 was accepted in a string literal");
}

void object_model_semantics(TestContext& test) {
  AnalyzedCompilation valid;
  valid.add("Objects.co",
            "Objects() {}\n"
            "static func Main() {\n"
            "  Objects instance = Objects();\n"
            "  object value = instance;\n"
            "  object? maybe = value;\n"
            "  bool matches = maybe is Objects;\n"
            "  Objects? cast = maybe as Objects?;\n"
            "  object[] values = [instance, \"cloth\", [1]];\n"
            "  string name = value::typeName;\n"
            "}\n");
  valid.analyze();

  test.expect(valid.error_count() == 0,
              "valid universal object program produced semantic errors");
  const cloth::SemanticModel& semantics = valid.result->semantics;
  test.expect(semantics.find_type("object") == semantics.object_type() &&
                  semantics.type(semantics.object_type()).kind ==
                      cloth::TypeKind::kObject,
              "object was not registered as the canonical universal type");
  bool found_object_array = false;
  bool found_checked_file_type = false;
  for (const cloth::SemanticType& type : semantics.types()) {
    found_object_array =
        found_object_array || (type.kind == cloth::TypeKind::kArray &&
                               type.element_type == semantics.object_type());
  }
  for (const cloth::ExpressionSemantics& expression :
       semantics.file(cloth::FileId{0}).expressions) {
    found_checked_file_type = found_checked_file_type ||
                              (expression.checked_type &&
                               semantics.type(*expression.checked_type).kind ==
                                   cloth::TypeKind::kFileClass);
  }
  test.expect(found_object_array,
              "heterogeneous managed references did not infer object[]");
  test.expect(found_checked_file_type,
              "checked target identity was not retained in semantics");

  AnalyzedCompilation invalid;
  invalid.add("ObjectErrors.co",
              "static func Main() {\n"
              "  int32 scalar = 1;\n"
              "  bool badSource = scalar is object;\n"
              "  object value = \"cloth\";\n"
              "  string badCast = value as string;\n"
              "  bool nullableTarget = value is string?;\n"
              "  bool erasedArray = value is int32[];\n"
              "  object boxed = scalar;\n"
              "}\n");
  invalid.analyze();
  test.expect(invalid.has_diagnostic(
                  "checked type operations require a managed reference"),
              "a primitive was accepted by a checked type operation");
  test.expect(
      invalid.has_diagnostic("the right operand of 'as' must be nullable"),
      "a non-nullable checked cast target was accepted");
  test.expect(
      invalid.has_diagnostic("the right operand of 'is' must be non-nullable"),
      "a nullable type-test target was accepted");
  test.expect(invalid.has_diagnostic(
                  "checked array casts require reified array type metadata"),
              "an erased array type was accepted as a checked target");
  test.expect(invalid.has_diagnostic("local initializer has type 'int32'; "
                                     "expected 'object'"),
              "implicit primitive boxing was accepted");
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
      {"non-null field initialization", non_null_field_initialization},
      {"core print intrinsic", core_print_intrinsic},
      {"cross-file binding", cross_file_binding},
      {"package imports", package_imports},
      {"inheritance graph", inheritance_graph},
      {"constructor initialization", constructor_initialization},
      {"inherited members and subtyping", inherited_members_and_subtyping},
      {"virtual override contract", virtual_override_contract},
      {"invalid inheritance graph", invalid_inheritance_graph},
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
      {"nullable reference shapes", nullable_reference_shapes},
      {"nullable flow narrowing", nullable_flow_narrowing},
      {"null ergonomics", null_ergonomics},
      {"assignment requires location", assignment_requires_location},
      {"array semantics", array_semantics},
      {"for iteration semantics", for_iteration_semantics},
      {"for binding scope and types", for_binding_scope_and_types},
      {"instance member binding", instance_member_binding},
      {"static member contract", static_member_contract},
      {"invalid static members", invalid_static_members},
      {"string value semantics", string_value_semantics},
      {"object model semantics", object_model_semantics},
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
