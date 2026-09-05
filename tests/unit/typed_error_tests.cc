// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_verifier.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

class CheckedCompilation {
 public:
  void add(std::string name, std::string source) {
    compilation_.add_source(
        cloth::SourceFile::from_memory(std::move(name), std::move(source)));
  }

  void analyze() {
    result_.emplace(compilation_.analyze_frontend(diagnostics_));
  }

  [[nodiscard]] bool has_diagnostic(std::string_view text) const {
    return std::ranges::any_of(diagnostics_.diagnostics(),
                               [text](const cloth::Diagnostic& diagnostic) {
                                 return diagnostic.message.find(text) !=
                                        std::string::npos;
                               });
  }

  [[nodiscard]] std::string messages() const {
    std::string result;
    for (const cloth::Diagnostic& diagnostic : diagnostics_.diagnostics()) {
      result += diagnostic.message;
      result += '\n';
    }
    return result;
  }

  [[nodiscard]] const cloth::FrontendResult& result() const { return *result_; }

  [[nodiscard]] const cloth::FileClassDecl& syntax(std::size_t index) const {
    return compilation_.syntax(index);
  }

  [[nodiscard]] std::span<const cloth::Token> tokens(std::size_t index) const {
    return compilation_.tokens(index);
  }

 private:
  cloth::Compilation compilation_;
  cloth::DiagnosticEngine diagnostics_;
  std::optional<cloth::FrontendResult> result_;
};

void syntax_and_hir(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Problem.co", R"(
    error {
      Problem(string message): Error(message) {}

      func Raise() throws Problem {
        throw self;
      }
    }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;

  const cloth::FileClassDecl& syntax = compilation.syntax(0);
  test.expect(syntax.kind == cloth::FileTypeKind::kError &&
                  syntax.functions.size() == 1 &&
                  syntax.functions[0].has_explicit_throws &&
                  syntax.functions[0].throws_types.size() == 1,
              "error declaration or throws syntax was not retained");
  test.expect(
      std::ranges::any_of(compilation.tokens(0),
                          [](const cloth::Token& token) {
                            return token.kind == cloth::TokenKind::kKwError;
                          }) &&
          std::ranges::any_of(compilation.tokens(0),
                              [](const cloth::Token& token) {
                                return token.kind == cloth::TokenKind::kKwThrow;
                              }) &&
          std::ranges::any_of(compilation.tokens(0),
                              [](const cloth::Token& token) {
                                return token.kind ==
                                       cloth::TokenKind::kKwThrows;
                              }),
      "typed-error keywords were not classified by the lexer");
  const auto& semantics = compilation.result().semantics;
  const auto& function =
      semantics.symbol(semantics.file(cloth::FileId{0}).functions[0]);
  test.expect(
      function.thrown_types.size() == 1 &&
          function.thrown_types[0] == semantics.file(cloth::FileId{0}).type,
      "semantic throws contract did not retain the canonical error");
  test.expect(std::ranges::any_of(
                  compilation.result().hir.storage.expressions(),
                  [](const cloth::HirExpression& expression) {
                    return std::holds_alternative<cloth::HirThrowExpression>(
                        expression.data);
                  }),
              "throw expression was not lowered to HIR");
}

void interface_contract_intersections(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", "error { Failure() {} }");
  compilation.add("Other.co", "error { Other() {} }");
  compilation.add("General.co", "interface { func Run() throws Error; }");
  compilation.add("Specific.co", "interface { func Run() throws Failure; }");
  compilation.add("Combined.co", "interface : General, Specific {}");
  compilation.add("Left.co", "interface { func Stop() throws Failure; }");
  compilation.add("Right.co", "interface { func Stop() throws Other; }");
  compilation.add("Disjoint.co", "interface : Left, Right {}");
  compilation.add("Worker.co", R"(
    class is Combined, Disjoint {
      override func Run() throws Failure {}
      override func Stop() {}
    }
  )");
  compilation.add("Use.co", R"(
    func Invoke(Combined value) throws Failure { value.Run(); }
    func Halt(Disjoint value) { value.Stop(); }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;

  const auto& semantics = compilation.result().semantics;
  const auto& combined = semantics.file(cloth::FileId{4});
  const auto& disjoint = semantics.file(cloth::FileId{7});
  test.expect(
      combined.interface_functions.size() == 1 &&
          semantics.symbol(combined.interface_functions[0]).thrown_types ==
              std::vector<cloth::TypeId>{semantics.file(cloth::FileId{0}).type},
      "comparable interface throws contracts did not intersect");
  test.expect(disjoint.interface_functions.size() == 1 &&
                  semantics.symbol(disjoint.interface_functions[0])
                      .thrown_types.empty(),
              "disjoint interface throws contracts did not become empty");
}

void hierarchy_and_interface_contracts(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Renderable.co",
                  "interface { func Render(): string throws Error; }");
  compilation.add("BaseFailure.co", R"(
    abstract error is Renderable {
      BaseFailure(): Error() {}
      abstract override func Render(): string throws Error;
    }
  )");
  compilation.add("SpecificFailure.co", R"(
    sealed error : BaseFailure {
      SpecificFailure(): BaseFailure() {}
      override func Render(): string throws SpecificFailure {
        return Message;
      }
    }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;
  const auto& semantics = compilation.result().semantics;
  const auto& derived = semantics.file(cloth::FileId{2});
  test.expect(derived.kind == cloth::FileTypeKind::kError &&
                  derived.base_file == cloth::FileId{1} &&
                  derived.interfaces.size() == 1,
              "error inheritance or interface closure is incomplete");
}

void error_values_and_effect_sets(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Rooted.co", R"(
    abstract error {
      int32 Code;
      Rooted(): Error() { Code = 0; }
      Rooted(string message): Error(message) { Code = 1; }
    }
  )");
  compilation.add("Specific.co", R"(
    sealed error : Rooted {
      Specific(): Rooted("specific") {}
    }
  )");
  compilation.add("Use.co", R"(
    func Widen(Specific value): Error { return value; }
    func Matches(Error value): bool { return value is Specific; }
    func Checked(Error value): Specific? { return value as Specific?; }
    func Nullable(Specific value): Specific? { return value; }
    func Covered() throws Rooted { throw Specific(); }
    func Multiple() throws Specific, DivisionByZero {}
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;

  const auto& semantics = compilation.result().semantics;
  const auto& rooted = semantics.file(cloth::FileId{0});
  const auto& specific = semantics.file(cloth::FileId{1});
  const auto& use = semantics.file(cloth::FileId{2});
  test.expect(rooted.kind == cloth::FileTypeKind::kError &&
                  rooted.is_abstract && rooted.constructors.size() == 2 &&
                  specific.kind == cloth::FileTypeKind::kError &&
                  specific.is_sealed && specific.base_file == cloth::FileId{0},
              "error value hierarchy or constructors were not retained");
  test.expect(semantics.symbol(use.functions[4]).thrown_types ==
                      std::vector<cloth::TypeId>{rooted.type} &&
                  semantics.symbol(use.functions[5]).thrown_types ==
                      std::vector<cloth::TypeId>{
                          specific.type, semantics.division_by_zero_type()},
              "valid base-covered or multiple throws sets changed");
}

void imported_error_constructor_flow(TestContext& test) {
  CheckedCompilation valid;
  valid.add("Failure.co",
            "error { Failure(string message): Error(message) {} }");
  valid.add("Box.co", R"(
    import Failure;
    class {
      string Value;
      Box(bool fail) throws Failure {
        if (fail) { throw Failure("failed"); }
        Value = "ready";
      }
    }
  )");
  valid.add("Use.co", R"(
    import Box;
    import Failure;
    func Build(bool fail): Box throws Failure { return Box(fail); }
  )");
  valid.analyze();
  test.expect(valid.result().is_valid, valid.messages());

  CheckedCompilation invalid;
  invalid.add("Failure.co", "error { Failure() {} }");
  invalid.add("Incomplete.co", R"(
    import Failure;
    class {
      string Value;
      Incomplete(bool fail) throws Failure {
        if (fail) { throw Failure(); }
      }
    }
  )");
  invalid.analyze();
  test.expect(!invalid.result().is_valid &&
                  invalid.has_diagnostic(
                      "constructor exits before non-null field 'Value' is "
                      "initialized"),
              invalid.messages());
}

void private_fixed_point(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", "error { Failure() {} }");
  compilation.add("Application.co", R"(
    static func Main() throws Failure { first(true); }

    static func first(bool again) {
      if (again) { second(false); }
    }

    static func second(bool again) {
      if (again) { first(false); }
      throw Failure();
    }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;
  const auto& semantics = compilation.result().semantics;
  const auto& file = semantics.file(cloth::FileId{1});
  const cloth::TypeId failure = semantics.file(cloth::FileId{0}).type;
  test.expect(semantics.symbol(file.functions[1]).thrown_types ==
                      std::vector<cloth::TypeId>{failure} &&
                  semantics.symbol(file.functions[2]).thrown_types ==
                      std::vector<cloth::TypeId>{failure},
              "recursive private throws inference did not reach a fixed point");
}

void coalescing_bottom_type(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", "error { Failure() {} }");
  compilation.add("Require.co", R"(
    func Require(Failure? value): Failure throws Failure {
      return value ?? throw Failure();
    }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;
  const auto& semantics = compilation.result().semantics;
  const auto& syntax = compilation.syntax(1);
  const auto& returned = std::get<cloth::ReturnStatement>(
      syntax.storage
          .statement(
              syntax.storage.block(syntax.functions[0].body).statements[0])
          .data);
  test.expect(returned.value.has_value(), "return expression is missing");
  if (!returned.value) return;
  test.expect(semantics.file(cloth::FileId{1})
                      .expressions[returned.value->value]
                      .type == semantics.file(cloth::FileId{0}).type,
              "nullable coalescing with throw did not produce non-null type");
}

void bottom_effect_reachability(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", "error { Failure() {} }");
  compilation.add("Other.co", "error { Other() {} }");
  compilation.add("Application.co", R"(
    static func Consume(int32 first, int32 second) {}
    static func Later(): int32 throws Other { throw Other(); }

    static func Main() throws Failure {
      Consume(throw Failure(), Later());
      Later();
    }
  )");
  compilation.add("Holder.co", R"(
    class {
      int32 First = throw Failure();
      int32 Second = Later();

      Holder() throws Failure { Later(); }
      holder(bool ignored) {}
      static func Later(): int32 throws Other { throw Other(); }
    }
  )");
  compilation.add("Base.co", "Base(int32 value) {}");
  compilation.add("Derived.co", R"(
    class : Base {
      Derived() throws Failure: Base(throw Failure()) { Later(); }
      static func Later(): int32 throws Other { throw Other(); }
    }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;

  const auto& semantics = compilation.result().semantics;
  const cloth::TypeId failure = semantics.file(cloth::FileId{0}).type;
  const auto& application = semantics.file(cloth::FileId{2});
  const auto& holder = semantics.file(cloth::FileId{3});
  const auto& derived = semantics.file(cloth::FileId{5});
  test.expect(semantics.symbol(application.functions[2]).thrown_types ==
                      std::vector<cloth::TypeId>{failure} &&
                  semantics.symbol(holder.constructors[0]).thrown_types ==
                      std::vector<cloth::TypeId>{failure} &&
                  semantics.symbol(holder.constructors[1]).thrown_types ==
                      std::vector<cloth::TypeId>{failure} &&
                  semantics.symbol(derived.constructors[0]).thrown_types ==
                      std::vector<cloth::TypeId>{failure},
              "unreachable expressions widened a throws contract");
  test.expect(
      compilation.result().hir.files[5].constructors[0].initializer.has_value(),
      "bottom-typed base argument lost its constructor binding");
}

void bottom_control_flow(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", "error { Failure() {} }");
  compilation.add("Other.co", "error { Other() {} }");
  compilation.add("Flows.co", R"(
    static func Later() throws Other { throw Other(); }

    static func IfCase() throws Failure {
      if (throw Failure()) { Later(); }
    }

    static func WhileCase() throws Failure {
      while (throw Failure()) { Later(); }
    }

    static func EachCase() throws Failure {
      for (int32 value in throw Failure()) { Later(); }
    }

    static func ForCase() throws Failure {
      for (; throw Failure(); Later()) { Later(); }
    }

    static func SwitchCase() throws Failure {
      switch (throw Failure()) { default: { Later(); } }
    }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;

  const auto& semantics = compilation.result().semantics;
  const cloth::TypeId failure = semantics.file(cloth::FileId{0}).type;
  const auto& flows = semantics.file(cloth::FileId{2});
  for (std::size_t index = 1; index < flows.functions.size(); ++index) {
    test.expect(semantics.symbol(flows.functions[index]).thrown_types ==
                    std::vector<cloth::TypeId>{failure},
                "bottom control flow retained an unreachable effect");
  }
}

void invalid_effect_contracts(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", "error { Failure() {} }");
  compilation.add("Specific.co",
                  "error : Failure { Specific(): Failure() {} }");
  compilation.add("hidden.co", "error { hidden() {} }");
  compilation.add("Invalid.co", R"(
    func Scalar() throws int32 {}
    func Nullable() throws Failure? {}
    func Duplicate() throws Failure, Failure {}
    func Redundant() throws Error, Specific {}
    func Exposed() throws hidden {}
    func Uncovered() { throw Failure(); }
  )");
  compilation.analyze();
  test.expect(!compilation.result().is_valid,
              "invalid throws contracts were accepted");
  test.expect(compilation.has_diagnostic("is not a non-null error") &&
                  compilation.has_diagnostic("duplicate error type") &&
                  compilation.has_diagnostic("is already covered") &&
                  compilation.has_diagnostic(
                      "public throws clause exposes private error") &&
                  compilation.has_diagnostic("is not covered by its throws") &&
                  !compilation.has_diagnostic("internal HIR verification"),
              compilation.messages());
}

void parser_recovery(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Malformed.co", R"(
    func Missing() throws {}
    func Trailing() throws Error, {}
    func Recovered() {}
  )");
  compilation.analyze();
  test.expect(!compilation.result().is_valid,
              "malformed throws clauses were accepted");
  test.expect(
      compilation.has_diagnostic("expected an error type after 'throws'") &&
          compilation.has_diagnostic(
              "expected an error type after ',' in throws clause") &&
          compilation.syntax(0).functions.size() == 3,
      compilation.messages());
}

void invalid_inheritance_and_throw_operands(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Plain.co", "class {}");
  compilation.add("BadError.co", R"(
    error : Plain {
      BadError(): Plain() {}
      func Raise() throws Error { throw null; }
    }
  )");
  compilation.add("BadClass.co", "class : Error {}");
  compilation.analyze();
  test.expect(!compilation.result().is_valid,
              "invalid error inheritance or throw operand was accepted");
  test.expect(compilation.has_diagnostic("must be an error") &&
                  compilation.has_diagnostic(
                      "throw operand must be a non-null error") &&
                  compilation.has_diagnostic("must be a file class") &&
                  !compilation.has_diagnostic("internal HIR verification"),
              compilation.messages());
}

void reserved_error_members_and_names(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Error.co", "class {}");
  compilation.add("DivisionByZero.co", "error {}");
  compilation.add("Shadow.co", R"(
    error {
      string Message;
      Shadow() {}
    }
  )");
  compilation.add("Construct.co", R"(
    func Build(): Error { return Error(); }
  )");
  compilation.add("AliasTarget.co", "error { AliasTarget() {} }");
  compilation.add("AliasUse.co", "import AliasTarget as Error;");
  compilation.analyze();
  test.expect(!compilation.result().is_valid,
              "compiler-owned error declarations were shadowed");
  test.expect(
      compilation.has_diagnostic("conflicts with a core type") &&
          compilation.has_diagnostic("cannot hide inherited final field") &&
          compilation.has_diagnostic(
              "abstract compiler error 'Error' cannot be constructed") &&
          compilation.has_diagnostic(
              "import name 'Error' conflicts with a core type"),
      compilation.messages());
}

void compiler_error_surface(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Use.co", R"(
    func Fail() throws DivisionByZero { throw DivisionByZero(); }
    func Describe(Error value): string { return value.Message; }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;

  const auto& semantics = compilation.result().semantics;
  const auto& use = semantics.file(cloth::FileId{0});
  test.expect(semantics.symbol(use.functions[0]).thrown_types ==
                  std::vector<cloth::TypeId>{semantics.division_by_zero_type()},
              "compiler-provided DivisionByZero contract was not resolved");
}

void widening_contracts(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", "error { Failure() {} }");
  compilation.add("Base.co",
                  "abstract class { abstract func Run() throws Failure; }");
  compilation.add("Derived.co", R"(
    class : Base {
      override func Run() throws Error {}
    }
  )");
  compilation.add("Action.co", "interface { func Act() throws Failure; }");
  compilation.add("Worker.co", R"(
    class is Action {
      override func Act() throws Error {}
    }
  )");
  compilation.analyze();
  test.expect(!compilation.result().is_valid,
              "widening override or interface implementation was accepted");
  test.expect(compilation.has_diagnostic("widens the inherited throws") &&
                  compilation.has_diagnostic("widens its throws contract"),
              compilation.messages());
}

void mir_and_abi_lowering(TestContext& test) {
  cloth::Compilation compilation;
  compilation.add_source(cloth::SourceFile::from_memory("Failure.co", R"(
        error {
          final string detail;
          Failure(string message): Error(message) {
            self.detail = message;
          }
          func Detail(): string { return self.detail; }
        }
      )"));
  compilation.add_source(cloth::SourceFile::from_memory(
      "Use.co",
      "func Raise(): int32 throws Failure { throw Failure(\"broken\"); }"));
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, "typed error lowering failed");
  if (!result.is_valid) return;
  const auto& failure_abi = result.abi.files.at(0);
  test.expect(failure_abi.type_descriptor &&
                  failure_abi.type_descriptor->kind ==
                      cloth::AbiHeapObjectKind::kError &&
                  failure_abi.type_descriptor->parent_is_error_root,
              "error descriptor did not retain its compiler-root parent");
  test.expect(result.abi.files.at(1).functions.at(0).uses_error_abi &&
                  result.abi.files.at(1).functions.at(0).return_mode ==
                      cloth::AbiReturnMode::kIndirect,
              "throwing result did not use the result/error ABI");
  test.expect(std::ranges::any_of(
                  result.mir.files.at(1).functions.at(0).body.blocks,
                  [](const cloth::MirBasicBlock& block) {
                    return std::holds_alternative<cloth::MirErrorTerminator>(
                        block.terminator.data);
                  }),
              "throw did not lower to a MIR error edge");
}

void malformed_hir_is_rejected(TestContext& test) {
  CheckedCompilation compilation;
  compilation.add("Failure.co", R"(
    error {
      Failure() {}
      func Raise() throws Failure { throw self; }
    }
  )");
  compilation.analyze();
  test.expect(compilation.result().is_valid, compilation.messages());
  if (!compilation.result().is_valid) return;

  cloth::HirModule malformed_hir = compilation.result().hir;
  bool mutated = false;
  for (const cloth::HirExpression& stored :
       malformed_hir.storage.expressions()) {
    auto& expression = const_cast<cloth::HirExpression&>(stored);
    if (auto* thrown =
            std::get_if<cloth::HirThrowExpression>(&expression.data)) {
      thrown->error_type = compilation.result().semantics.string_type();
      mutated = true;
      break;
    }
  }
  cloth::DiagnosticEngine hir_diagnostics;
  test.expect(mutated && !cloth::verify_hir(malformed_hir,
                                            compilation.result().semantics,
                                            hir_diagnostics),
              "HIR verifier accepted malformed throw metadata");

  cloth::SemanticModel malformed_semantics = compilation.result().semantics;
  const cloth::SymbolId function =
      malformed_semantics.file(cloth::FileId{0}).functions[0];
  auto& symbol =
      const_cast<cloth::SemanticSymbol&>(malformed_semantics.symbol(function));
  symbol.thrown_types.push_back(malformed_semantics.error_root_type());
  cloth::DiagnosticEngine contract_diagnostics;
  test.expect(!cloth::verify_hir(compilation.result().hir, malformed_semantics,
                                 contract_diagnostics),
              "HIR verifier accepted a redundant throws contract");
}

void malformed_mir_and_abi_are_rejected(TestContext& test) {
  cloth::Compilation compilation;
  compilation.add_source(cloth::SourceFile::from_memory(
      "Failure.co", "error { Failure(): Error() {} }"));
  compilation.add_source(cloth::SourceFile::from_memory("Use.co", R"(
        static func Raise(): int32 throws Failure { throw Failure(); }
        static func Forward(): int32 throws Failure { return Raise(); }
      )"));
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, "malformed-state fixture failed to lower");
  if (!result.is_valid) return;

  cloth::MirModule broken_call = result.mir;
  bool changed_call = false;
  for (auto& function : broken_call.files[1].functions) {
    for (auto& block : function.body.blocks) {
      for (auto& instruction : block.instructions) {
        if (auto* call =
                std::get_if<cloth::MirCallInstruction>(&instruction.data);
            call && call->error_result) {
          call->error_result.reset();
          changed_call = true;
          break;
        }
      }
      if (changed_call) break;
    }
    if (changed_call) break;
  }
  cloth::DiagnosticEngine call_diagnostics;
  test.expect(changed_call && !cloth::verify_mir(broken_call, result.semantics,
                                                 call_diagnostics),
              "MIR verifier accepted a throwing call without an error result");

  cloth::MirModule broken_error = result.mir;
  bool changed_error = false;
  for (auto& function : broken_error.files[1].functions) {
    for (auto& block : function.body.blocks) {
      if (auto* error =
              std::get_if<cloth::MirErrorTerminator>(&block.terminator.data)) {
        error->error = cloth::MirValueId{function.body.value_count + 1};
        changed_error = true;
        break;
      }
    }
    if (changed_error) break;
  }
  cloth::DiagnosticEngine error_diagnostics;
  test.expect(
      changed_error &&
          !cloth::verify_mir(broken_error, result.semantics, error_diagnostics),
      "MIR verifier accepted an invalid error terminator value");

  cloth::AbiModule broken_abi = result.abi;
  broken_abi.files[1].functions[0].uses_error_abi = false;
  cloth::DiagnosticEngine abi_diagnostics;
  test.expect(!cloth::verify_abi(broken_abi, result.mir, result.semantics,
                                 abi_diagnostics),
              "ABI verifier accepted a throwing callable with the plain ABI");
}

}  // namespace

int main() {
  const TestCase tests[]{
      {"syntax and HIR", syntax_and_hir},
      {"hierarchy and interface contracts", hierarchy_and_interface_contracts},
      {"error values and effect sets", error_values_and_effect_sets},
      {"imported error constructor flow", imported_error_constructor_flow},
      {"interface contract intersections", interface_contract_intersections},
      {"private fixed point", private_fixed_point},
      {"coalescing bottom type", coalescing_bottom_type},
      {"bottom effect reachability", bottom_effect_reachability},
      {"bottom control flow", bottom_control_flow},
      {"invalid effect contracts", invalid_effect_contracts},
      {"parser recovery", parser_recovery},
      {"invalid inheritance and throw operands",
       invalid_inheritance_and_throw_operands},
      {"reserved error members and names", reserved_error_members_and_names},
      {"compiler error surface", compiler_error_surface},
      {"widening contracts", widening_contracts},
      {"MIR and ABI lowering", mir_and_abi_lowering},
      {"malformed HIR is rejected", malformed_hir_is_rejected},
      {"malformed MIR and ABI are rejected",
       malformed_mir_and_abi_are_rejected},
  };
  return cloth::test::run_tests(tests);
}
