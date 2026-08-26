#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/flow/control_flow.h"
#include "cloth/hir/hir.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
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

class CompiledSources {
 public:
  void add(std::filesystem::path path, std::string text) {
    compilation_.add_source(
        cloth::SourceFile::from_memory(std::move(path), std::move(text)));
  }

  void compile() { result.emplace(compilation_.analyze(diagnostics)); }

  [[nodiscard]] bool has_diagnostic(std::string_view text) const {
    for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
      if (diagnostic.message.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;

 private:
  cloth::Compilation compilation_;
};

template <typename Instruction>
bool body_has_instruction(const cloth::MirBody& body) {
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<Instruction>(instruction.data)) {
        return true;
      }
    }
  }
  return false;
}

bool has_diagnostic(const cloth::DiagnosticEngine& diagnostics,
                    std::string_view text) {
  for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void straight_line_body(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Math.co",
                  "function Add(int a, int b): int {\n"
                  "  int sum = a + b;\n"
                  "  return sum;\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid straight-line program failed");
  const cloth::MirCallable& function =
      compilation.result->mir.files[0].functions[0];
  test.expect(function.parameters.size() == 2, "MIR lost parameter symbols");
  test.expect(function.body.blocks.size() == 1,
              "straight-line function created extra blocks");
  test.expect(std::holds_alternative<cloth::MirReturnTerminator>(
                  function.body.blocks[0].terminator.data),
              "function does not end in an explicit return");
  test.expect(body_has_instruction<cloth::MirBinaryInstruction>(function.body),
              "binary expression was not lowered");
  test.expect(
      body_has_instruction<cloth::MirDeclareLocalInstruction>(function.body),
      "local declaration was not lowered");
}

void branching_returns(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Branches.co",
                  "function Choose(bool flag): int {\n"
                  "  if (flag) { return 1; } else { return 2; }\n"
                  "}\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(compilation.result->is_valid,
              "complete branching function failed");
  test.expect(body.blocks.size() == 3,
              "complete if/else should produce three blocks");
  test.expect(std::holds_alternative<cloth::MirBranchTerminator>(
                  body.blocks[0].terminator.data),
              "entry block does not branch");
  test.expect(!compilation.result->control_flow.callables[0].can_fall_through,
              "complete if/else was marked as falling through");
}

void branch_continuation(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Continuation.co",
                  "function Choose(bool flag): int {\n"
                  "  if (flag) { return 1; }\n"
                  "  return 2;\n"
                  "}\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(compilation.result->is_valid,
              "fallthrough branch did not join its continuation");
  test.expect(body.blocks.size() == 4,
              "if without else has the wrong CFG shape");
  bool found_jump = false;
  for (const cloth::MirBasicBlock& block : body.blocks) {
    found_jump = found_jump || std::holds_alternative<cloth::MirJumpTerminator>(
                                   block.terminator.data);
  }
  test.expect(found_jump, "fallthrough edge is not an explicit jump");
}

void short_circuit_control_flow(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Logic.co",
                  "function Both(bool left, bool right): bool {\n"
                  "  return left && right;\n"
                  "}\n"
                  "function Either(bool left, bool right): bool {\n"
                  "  return left || right;\n"
                  "}\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(compilation.result->is_valid,
              "short-circuit expression failed to lower");
  test.expect(body.blocks.size() == 4,
              "short-circuit expression has the wrong CFG shape");
  test.expect(std::holds_alternative<cloth::MirBranchTerminator>(
                  body.blocks[0].terminator.data),
              "short-circuit left operand does not branch");
  test.expect(body_has_instruction<cloth::MirPhiInstruction>(body),
              "short-circuit merge has no phi value");
  const cloth::MirBody& either =
      compilation.result->mir.files[0].functions[1].body;
  const auto* either_branch = std::get_if<cloth::MirBranchTerminator>(
      &either.blocks[0].terminator.data);
  test.expect(either_branch != nullptr &&
                  either_branch->then_block == cloth::MirBlockId{2} &&
                  either_branch->else_block == cloth::MirBlockId{1},
              "logical-or does not short-circuit its true branch");
  test.expect(body_has_instruction<cloth::MirPhiInstruction>(either),
              "logical-or merge has no phi value");
}

void unreachable_statements(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Dead.co",
                  "function Stop(): int {\n"
                  "  return 1;\n"
                  "  return 2;\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "unreachable-code warning invalidated compilation");
  test.expect(compilation.has_diagnostic("unreachable statement"),
              "unreachable statement was not reported");
  const cloth::CallableControlFlow& flow =
      compilation.result->control_flow.callables[0];
  test.expect(flow.unreachable_statements == 1,
              "unreachable statement count is wrong");
  bool found_dead_block = false;
  for (const cloth::MirBasicBlock& block :
       compilation.result->mir.files[0].functions[0].body.blocks) {
    found_dead_block = found_dead_block || !block.is_reachable;
  }
  test.expect(found_dead_block, "unreachable code was not isolated in MIR");
}

void incomplete_return_flow(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Returns.co",
                  "function Maybe(bool flag): int {\n"
                  "  if (flag) { return 1; }\n"
                  "}\n");
  compilation.compile();

  test.expect(
      compilation.has_diagnostic("does not return a value on every path"),
      "control-flow analysis missed an incomplete return");
  test.expect(compilation.result->control_flow.callables[0].can_fall_through,
              "incomplete return was marked complete");
  bool found_unreachable = false;
  for (const cloth::MirBasicBlock& block :
       compilation.result->mir.files[0].functions[0].body.blocks) {
    found_unreachable = found_unreachable ||
                        std::holds_alternative<cloth::MirUnreachableTerminator>(
                            block.terminator.data);
  }
  test.expect(found_unreachable,
              "invalid value fallthrough lacks an unreachable terminator");
}

void no_value_fallthrough(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Actions.co", "function Run() { int value = 1; }\n");
  compilation.compile();

  const cloth::MirTerminator& terminator =
      compilation.result->mir.files[0].functions[0].body.blocks[0].terminator;
  const auto* return_terminator =
      std::get_if<cloth::MirReturnTerminator>(&terminator.data);
  test.expect(compilation.result->is_valid,
              "no-value fallthrough should be valid");
  test.expect(return_terminator != nullptr && !return_terminator->value,
              "no-value fallthrough lacks an implicit return");
}

void field_initializer_body(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Fields.co", "int Count = 1;\n");
  compilation.compile();

  const cloth::MirField& field = compilation.result->mir.files[0].fields[0];
  test.expect(compilation.result->is_valid, "field initializer failed");
  test.expect(field.initializer.has_value(),
              "field initializer has no MIR body");
  if (field.initializer) {
    const auto* return_terminator = std::get_if<cloth::MirReturnTerminator>(
        &field.initializer->blocks[0].terminator.data);
    test.expect(return_terminator != nullptr && return_terminator->value,
                "field initializer does not return its value");
  }
}

void member_store(TestContext& test) {
  CompiledSources compilation;
  compilation.add("User.co",
                  "String Name;\n"
                  "User(String name) { self.Name = name; }\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].constructors[0].body;
  test.expect(compilation.result->is_valid,
              "member assignment failed to lower");
  test.expect(body_has_instruction<cloth::MirStoreMemberInstruction>(body),
              "member assignment is not an explicit store");
}

void nullable_conversion(TestContext& test) {
  CompiledSources compilation;
  compilation.add("User.co", "");
  compilation.add("Factory.co", "function Empty(): User { return null; }\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[1].functions[0].body;
  test.expect(compilation.result->is_valid, "nullable reference return failed");
  test.expect(body_has_instruction<cloth::MirConvertInstruction>(body),
              "nullable return lacks an explicit conversion");
}

void call_receivers(TestContext& test) {
  CompiledSources compilation;
  compilation.add("User.co",
                  "User() {}\n"
                  "function Echo(int value): int { return value; }\n"
                  "function Forward(int value): int { return Echo(value); }\n");
  compilation.add("Calls.co",
                  "function Static(User user): int { return User.Echo(1); }\n"
                  "function Instance(User user): int { "
                  "return user.Echo(1); }\n"
                  "function Make(): User { return User(); }\n");
  compilation.compile();

  test.expect(compilation.result->is_valid, "valid calls failed to lower");
  const auto call_in =
      [](const cloth::MirBody& body) -> const cloth::MirCallInstruction* {
    for (const cloth::MirBasicBlock& block : body.blocks) {
      for (const cloth::MirInstruction& instruction : block.instructions) {
        if (const auto* call =
                std::get_if<cloth::MirCallInstruction>(&instruction.data)) {
          return call;
        }
      }
    }
    return nullptr;
  };
  const cloth::MirFileClass& calls = compilation.result->mir.files[1];
  const cloth::MirCallInstruction* class_call =
      call_in(calls.functions[0].body);
  const cloth::MirCallInstruction* instance_call =
      call_in(calls.functions[1].body);
  const cloth::MirCallInstruction* unqualified_call =
      call_in(compilation.result->mir.files[0].functions[1].body);
  const cloth::MirCallInstruction* constructor_call =
      call_in(calls.functions[2].body);
  test.expect(class_call != nullptr && !class_call->receiver &&
                  class_call->kind == cloth::MirCallKind::kClassQualified,
              "class-qualified call lost its call kind");
  test.expect(instance_call != nullptr && instance_call->receiver &&
                  instance_call->kind == cloth::MirCallKind::kInstance,
              "instance-qualified call lost its receiver or call kind");
  test.expect(unqualified_call != nullptr && !unqualified_call->receiver &&
                  unqualified_call->kind == cloth::MirCallKind::kUnqualified,
              "unqualified call lost its call kind");
  test.expect(constructor_call != nullptr && !constructor_call->receiver &&
                  constructor_call->kind == cloth::MirCallKind::kConstructor,
              "constructor call lost its call kind");
}

void verifiers_reject_corruption(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Verify.co", "function Value(): int { return 1; }\n");
  compilation.compile();

  cloth::MirModule broken_mir = compilation.result->mir;
  broken_mir.files[0].functions[0].body.blocks[0].terminator.data =
      cloth::MirJumpTerminator{cloth::MirBlockId{999}};
  cloth::DiagnosticEngine mir_diagnostics;
  test.expect(!cloth::verify_mir(broken_mir, compilation.result->semantics,
                                 mir_diagnostics),
              "MIR verifier accepted an invalid block target");
  test.expect(has_diagnostic(mir_diagnostics, "unknown basic block"),
              "MIR verifier reported the wrong invariant failure");

  cloth::HirModule broken_hir;
  const cloth::SourceRange range =
      compilation.result->semantics
          .symbol(compilation.result->semantics.file(cloth::FileId{0}).symbol)
          .range;
  static_cast<void>(broken_hir.storage.add_expression(cloth::HirExpression{
      compilation.result->semantics.bool_type(), range,
      cloth::HirUnaryExpression{cloth::TokenKind::kBang,
                                cloth::HirExpressionId{999}}}));
  cloth::DiagnosticEngine hir_diagnostics;
  test.expect(!cloth::verify_hir(broken_hir, compilation.result->semantics,
                                 hir_diagnostics),
              "HIR verifier accepted an invalid expression target");
  test.expect(has_diagnostic(hir_diagnostics, "unknown expression"),
              "HIR verifier reported the wrong invariant failure");
}

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string_view name;
  TestFunction function;
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"straight-line body", straight_line_body},
      {"branching returns", branching_returns},
      {"branch continuation", branch_continuation},
      {"short-circuit control flow", short_circuit_control_flow},
      {"unreachable statements", unreachable_statements},
      {"incomplete return flow", incomplete_return_flow},
      {"no-value fallthrough", no_value_fallthrough},
      {"field initializer body", field_initializer_body},
      {"member store", member_store},
      {"nullable conversion", nullable_conversion},
      {"call receivers", call_receivers},
      {"verifiers reject corruption", verifiers_reject_corruption},
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
