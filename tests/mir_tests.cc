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

bool body_has_conversion(const cloth::MirBody& body,
                         cloth::MirConversionKind kind) {
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      const auto* conversion =
          std::get_if<cloth::MirConvertInstruction>(&instruction.data);
      if (conversion != nullptr && conversion->kind == kind) {
        return true;
      }
    }
  }
  return false;
}

std::optional<cloth::MirBlockId> for_latch_block(const cloth::MirBody& body) {
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      const auto* phi =
          std::get_if<cloth::MirPhiInstruction>(&instruction.data);
      if (phi != nullptr && phi->incoming.size() == 2) {
        return phi->incoming[1].predecessor;
      }
    }
  }
  return std::nullopt;
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
                  "func Add(int a, int b): int {\n"
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
              "func does not end in an explicit return");
  test.expect(body_has_instruction<cloth::MirBinaryInstruction>(function.body),
              "binary expression was not lowered");
  test.expect(
      body_has_instruction<cloth::MirDeclareLocalInstruction>(function.body),
      "local declaration was not lowered");
}

void branching_returns(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Branches.co",
                  "func Choose(bool flag): int {\n"
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
                  "func Choose(bool flag): int {\n"
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
                  "func Both(bool left, bool right): bool {\n"
                  "  return left && right;\n"
                  "}\n"
                  "func Either(bool left, bool right): bool {\n"
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

void structured_loop_control_flow(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Loop.co",
                  "func Run() {\n"
                  "  int value = 0;\n"
                  "  while (value < 4) {\n"
                  "    value = value + 1;\n"
                  "    if (value == 2) { continue; }\n"
                  "    if (value == 3) { break; }\n"
                  "  }\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid, "structured loop failed to lower");
  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  const auto* entry_jump = std::get_if<cloth::MirJumpTerminator>(
      &body.blocks[body.entry.value].terminator.data);
  test.expect(entry_jump != nullptr,
              "loop entry does not jump to its condition");
  if (entry_jump == nullptr || entry_jump->target.value >= body.blocks.size()) {
    return;
  }

  const cloth::MirBlockId condition_block = entry_jump->target;
  const auto* condition_branch = std::get_if<cloth::MirBranchTerminator>(
      &body.blocks[condition_block.value].terminator.data);
  test.expect(condition_branch != nullptr,
              "loop condition does not branch to body and exit blocks");
  if (condition_branch == nullptr) {
    return;
  }

  std::size_t continue_edges = 0;
  std::size_t break_edges = 0;
  for (const cloth::MirBasicBlock& block : body.blocks) {
    const auto* jump =
        std::get_if<cloth::MirJumpTerminator>(&block.terminator.data);
    if (jump == nullptr) {
      continue;
    }
    if (jump->target == condition_block) {
      ++continue_edges;
    }
    if (jump->target == condition_branch->else_block) {
      ++break_edges;
    }
  }
  test.expect(continue_edges >= 2,
              "continue or loop backedge does not target the condition");
  test.expect(break_edges >= 1, "break does not target the loop exit");
  test.expect(compilation.result->control_flow.callables[0].can_fall_through,
              "loop with a reachable break was marked non-falling");
}

void unreachable_statements(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Dead.co",
                  "func Stop(): int {\n"
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
                  "func Maybe(bool flag): int {\n"
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

void void_fallthrough(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Actions.co",
                  "func Explicit(): void { return; }\n"
                  "func Implicit() { Explicit(); }\n");
  compilation.compile();

  test.expect(compilation.result->is_valid, "void fallthrough should be valid");
  for (const cloth::MirCallable& function :
       compilation.result->mir.files[0].functions) {
    const cloth::MirTerminator& terminator = function.body.blocks[0].terminator;
    const auto* return_terminator =
        std::get_if<cloth::MirReturnTerminator>(&terminator.data);
    test.expect(return_terminator != nullptr && !return_terminator->value,
                "void function lacks a valueless return");
  }
  const cloth::MirBasicBlock& implicit_body =
      compilation.result->mir.files[0].functions[1].body.blocks[0];
  bool found_void_call = false;
  for (const cloth::MirInstruction& instruction : implicit_body.instructions) {
    found_void_call =
        found_void_call ||
        (std::holds_alternative<cloth::MirCallInstruction>(instruction.data) &&
         !instruction.result &&
         instruction.type == compilation.result->semantics.void_type());
  }
  test.expect(found_void_call,
              "void call incorrectly materialized a MIR value");
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
                  "string Name;\n"
                  "User(string name) { self.Name = name; }\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].constructors[0].body;
  test.expect(compilation.result->is_valid,
              "member assignment failed to lower");
  test.expect(body_has_instruction<cloth::MirStoreMemberInstruction>(body),
              "member assignment is not an explicit store");
}

void array_instructions(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Arrays.co",
                  "func Sum(): int32 {\n"
                  "  int32[] values = [1, 2, 3];\n"
                  "  values[1] = 4;\n"
                  "  return values.Length + values[0];\n"
                  "}\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(compilation.result->is_valid,
              "valid arrays failed MIR verification");
  test.expect(body_has_instruction<cloth::MirArrayLiteralInstruction>(body),
              "array literal was not lowered explicitly");
  test.expect(body_has_instruction<cloth::MirArrayStoreInstruction>(body),
              "array assignment was not lowered explicitly");
  test.expect(body_has_instruction<cloth::MirArrayLoadInstruction>(body),
              "array indexing was not lowered explicitly");
  test.expect(body_has_instruction<cloth::MirArrayLengthInstruction>(body),
              "array Length was not lowered explicitly");
}

void string_instructions(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Strings.co",
                  "func Inspect(string left, string right): int32 {\n"
                  "  string joined = left + right;\n"
                  "  bool equal = joined == \"cloth\";\n"
                  "  bool different = left != right;\n"
                  "  bool empty = joined.IsEmpty;\n"
                  "  if (equal && different && !empty) {\n"
                  "    return joined.Length;\n"
                  "  }\n"
                  "  return joined.ByteLength;\n"
                  "}\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(compilation.result->is_valid,
              "valid string operations failed MIR verification");

  bool found_length = false;
  bool found_byte_length = false;
  bool found_is_empty = false;
  bool found_concat = false;
  std::size_t equality_count = 0;
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (const auto* property =
              std::get_if<cloth::MirStringPropertyInstruction>(
                  &instruction.data)) {
        found_length = found_length ||
                       property->property == cloth::StringProperty::kLength;
        found_byte_length =
            found_byte_length ||
            property->property == cloth::StringProperty::kByteLength;
        found_is_empty = found_is_empty ||
                         property->property == cloth::StringProperty::kIsEmpty;
      }
      if (const auto* binary =
              std::get_if<cloth::MirBinaryInstruction>(&instruction.data)) {
        found_concat =
            found_concat || binary->operation == cloth::TokenKind::kPlus;
        if (binary->operation == cloth::TokenKind::kEqualEqual ||
            binary->operation == cloth::TokenKind::kBangEqual) {
          ++equality_count;
        }
      }
    }
  }
  test.expect(found_length && found_byte_length && found_is_empty,
              "string properties were not lowered explicitly");
  test.expect(found_concat && equality_count == 2,
              "string value operators were not retained in MIR");
}

void for_iteration_control_flow(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Iteration.co",
                  "func Sum(int32[] values): int32 {\n"
                  "  int32 total = 0;\n"
                  "  for (var value in values) {\n"
                  "    if (value == 2) { continue; }\n"
                  "    if (value == 3) { break; }\n"
                  "    total = total + value;\n"
                  "  }\n"
                  "  return total;\n"
                  "}\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(compilation.result->is_valid,
              "valid for loop failed MIR verification");
  test.expect(body_has_instruction<cloth::MirPhiInstruction>(body),
              "for index is not represented by a phi value");
  test.expect(body_has_instruction<cloth::MirArrayLengthInstruction>(body) &&
                  body_has_instruction<cloth::MirArrayLoadInstruction>(body),
              "for loop does not use explicit array operations");
  test.expect(body_has_instruction<cloth::MirDeclareLocalInstruction>(body),
              "iteration variable was not declared in MIR");
  test.expect(body.blocks.size() >= 7,
              "for loop did not create condition, body, latch, and exit flow");

  std::optional<cloth::MirBlockId> condition_block;
  const cloth::MirPhiInstruction* index_phi = nullptr;
  std::optional<cloth::MirValueId> index;
  std::optional<cloth::MirValueId> array;
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    for (const cloth::MirInstruction& instruction :
         body.blocks[block_index].instructions) {
      if (const auto* phi =
              std::get_if<cloth::MirPhiInstruction>(&instruction.data)) {
        condition_block = cloth::MirBlockId{block_index};
        index_phi = phi;
        index = instruction.result;
      } else if (const auto* length =
                     std::get_if<cloth::MirArrayLengthInstruction>(
                         &instruction.data)) {
        array = length->array;
      }
    }
  }
  test.expect(condition_block && index_phi != nullptr && index && array,
              "for condition does not contain its canonical values");
  if (!condition_block || index_phi == nullptr || !index || !array ||
      index_phi->incoming.size() != 2) {
    return;
  }

  const cloth::MirBlockId preheader = index_phi->incoming[0].predecessor;
  const cloth::MirBlockId latch = index_phi->incoming[1].predecessor;
  test.expect(preheader == body.entry,
              "for phi does not receive zero from the preheader");
  const auto* preheader_jump = std::get_if<cloth::MirJumpTerminator>(
      &body.blocks[preheader.value].terminator.data);
  const auto* latch_jump = std::get_if<cloth::MirJumpTerminator>(
      &body.blocks[latch.value].terminator.data);
  test.expect(
      preheader_jump != nullptr && preheader_jump->target == *condition_block,
      "for preheader does not enter the condition");
  test.expect(latch_jump != nullptr && latch_jump->target == *condition_block,
              "for latch does not return to the condition");

  const auto* condition_branch = std::get_if<cloth::MirBranchTerminator>(
      &body.blocks[condition_block->value].terminator.data);
  test.expect(condition_branch != nullptr,
              "for condition does not branch to body and exit");
  if (condition_branch == nullptr) {
    return;
  }

  bool indexed_with_phi = false;
  for (const cloth::MirInstruction& instruction :
       body.blocks[condition_branch->then_block.value].instructions) {
    const auto* load =
        std::get_if<cloth::MirArrayLoadInstruction>(&instruction.data);
    indexed_with_phi =
        indexed_with_phi ||
        (load != nullptr && load->array == *array && load->index == *index);
  }
  test.expect(indexed_with_phi,
              "for body does not load the indexed element from its iterable");

  std::size_t latch_edges = 0;
  std::size_t condition_edges = 0;
  std::size_t exit_edges = 0;
  for (const cloth::MirBasicBlock& block : body.blocks) {
    const auto* jump =
        std::get_if<cloth::MirJumpTerminator>(&block.terminator.data);
    if (jump == nullptr) {
      continue;
    }
    latch_edges += jump->target == latch ? 1U : 0U;
    condition_edges += jump->target == *condition_block ? 1U : 0U;
    exit_edges += jump->target == condition_branch->else_block ? 1U : 0U;
  }
  test.expect(latch_edges >= 2,
              "continue or body fallthrough does not target the latch");
  test.expect(condition_edges == 2,
              "an edge other than preheader or latch bypasses the for index");
  test.expect(exit_edges >= 1, "break does not target the for exit");
}

void for_terminating_body_reachability(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Terminating.co",
                  "func BreakFirst(int32[] values): int32 {\n"
                  "  for (var value in values) { break; }\n"
                  "  return 1;\n"
                  "}\n"
                  "func ContinueAll(int32[] values): int32 {\n"
                  "  int32 total = 0;\n"
                  "  for (var value in values) {\n"
                  "    total = total + value;\n"
                  "    continue;\n"
                  "  }\n"
                  "  return total;\n"
                  "}\n"
                  "func ReturnFirst(int32[] values): int32 {\n"
                  "  for (var value in values) { return value; }\n"
                  "  return 0;\n"
                  "}\n");
  compilation.compile();
  test.expect(compilation.result->is_valid,
              "terminating for bodies failed MIR verification");

  const std::vector<cloth::MirCallable>& functions =
      compilation.result->mir.files[0].functions;
  test.expect(functions.size() == 3,
              "terminating body fixture has the wrong function count");
  if (functions.size() != 3) {
    return;
  }

  const std::optional<cloth::MirBlockId> break_latch =
      for_latch_block(functions[0].body);
  const std::optional<cloth::MirBlockId> continue_latch =
      for_latch_block(functions[1].body);
  const std::optional<cloth::MirBlockId> return_latch =
      for_latch_block(functions[2].body);
  test.expect(break_latch && continue_latch && return_latch,
              "terminating body fixture lost a for latch");
  if (!break_latch || !continue_latch || !return_latch) {
    return;
  }
  test.expect(!functions[0].body.blocks[break_latch->value].is_reachable,
              "unconditional break left the latch reachable");
  test.expect(functions[1].body.blocks[continue_latch->value].is_reachable,
              "unconditional continue did not reach the latch");
  test.expect(!functions[2].body.blocks[return_latch->value].is_reachable,
              "unconditional return left the latch reachable");
}

void nullable_conversion(TestContext& test) {
  CompiledSources compilation;
  compilation.add("User.co", "");
  compilation.add("Factory.co",
                  "func Empty(): User? { return null; }\n"
                  "func Present(User value): User? { return value; }\n");
  compilation.compile();

  const cloth::MirBody& empty =
      compilation.result->mir.files[1].functions[0].body;
  const cloth::MirBody& present =
      compilation.result->mir.files[1].functions[1].body;
  test.expect(compilation.result->is_valid, "nullable reference return failed");
  test.expect(body_has_conversion(empty, cloth::MirConversionKind::kToNullable),
              "null-to-nullable return lacks an explicit conversion");
  test.expect(
      body_has_conversion(present, cloth::MirConversionKind::kToNullable),
      "non-null-to-nullable return lacks an explicit conversion");
}

void nullable_narrowing_conversion(TestContext& test) {
  CompiledSources compilation;
  compilation.add("User.co", "string Name = \"Ada\";\n");
  compilation.add("Flow.co",
                  "func Read(User? value): string? {\n"
                  "  if (value != null) { return value.Name; }\n"
                  "  return null;\n"
                  "}\n");
  compilation.compile();

  const cloth::MirBody& body =
      compilation.result->mir.files[1].functions[0].body;
  test.expect(compilation.result->is_valid,
              "flow-narrowed reference failed MIR verification");
  test.expect(
      body_has_conversion(body, cloth::MirConversionKind::kFromNullable),
      "narrowed read lacks an explicit MIR conversion");
  test.expect(body_has_conversion(body, cloth::MirConversionKind::kToNullable),
              "narrowed return lost its nullable widening");

  cloth::MirModule broken = compilation.result->mir;
  for (cloth::MirBasicBlock& block : broken.files[1].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      auto* conversion =
          std::get_if<cloth::MirConvertInstruction>(&instruction.data);
      if (conversion != nullptr &&
          conversion->kind == cloth::MirConversionKind::kFromNullable) {
        conversion->kind = cloth::MirConversionKind::kToNullable;
      }
    }
  }
  cloth::DiagnosticEngine diagnostics;
  test.expect(
      !cloth::verify_mir(broken, compilation.result->semantics, diagnostics),
      "MIR verifier accepted a mislabeled narrowing conversion");
  test.expect(
      has_diagnostic(diagnostics,
                     "nullable widening does not produce a nullable type"),
      "MIR verifier reported the wrong conversion invariant");
}

void null_ergonomics_lowering(TestContext& test) {
  CompiledSources compilation;
  compilation.add("User.co", "string Name = \"Ada\";\n");
  compilation.add("NullErgonomics.co",
                  "func Display(User? user): string {\n"
                  "  if (user) { return user.Name; }\n"
                  "  return user?.Name ?? \"Unknown\";\n"
                  "}\n"
                  "func Assert(User? user): User { return user!; }\n"
                  "func Both(User? user, bool enabled): bool {\n"
                  "  return user && enabled;\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid null ergonomics failed MIR verification");
  const cloth::MirFileClass& file = compilation.result->mir.files[1];
  const cloth::MirBody& display = file.functions[0].body;
  const cloth::MirBody& assertion = file.functions[1].body;
  const cloth::MirBody& conjunction = file.functions[2].body;
  test.expect(
      body_has_instruction<cloth::MirIsNonNullInstruction>(display) &&
          body_has_instruction<cloth::MirPhiInstruction>(display),
      "safe access and coalescing did not lower to guarded control flow");
  test.expect(body_has_instruction<cloth::MirNullAssertInstruction>(assertion),
              "non-null assertion lacks an explicit MIR guard");
  test.expect(
      body_has_instruction<cloth::MirIsNonNullInstruction>(conjunction) &&
          body_has_instruction<cloth::MirPhiInstruction>(conjunction),
      "nullable short-circuit operand was not lowered as a presence test");

  cloth::MirModule broken = compilation.result->mir;
  bool corrupted = false;
  for (cloth::MirBasicBlock& block : broken.files[1].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<cloth::MirIsNonNullInstruction>(
              instruction.data)) {
        instruction.type =
            compilation.result->semantics.file(cloth::FileId{0}).type;
        corrupted = true;
        break;
      }
    }
    if (corrupted) {
      break;
    }
  }
  cloth::DiagnosticEngine diagnostics;
  test.expect(
      corrupted && !cloth::verify_mir(broken, compilation.result->semantics,
                                      diagnostics),
      "MIR verifier accepted a non-bool presence test");
  test.expect(
      has_diagnostic(diagnostics, "non-null test does not have type bool"),
      "MIR verifier reported the wrong presence-test invariant");
}

void call_receivers(TestContext& test) {
  CompiledSources compilation;
  compilation.add(
      "User.co",
      "User() {}\n"
      "static func Echo(int value): int { return value; }\n"
      "func InstanceEcho(int value): int { return value; }\n"
      "func Forward(int value): int { return InstanceEcho(value); }\n");
  compilation.add(
      "Calls.co",
      "static func Static(User user): int { return User.Echo(1); }\n"
      "func Instance(User user): int { "
      "return user.InstanceEcho(1); }\n"
      "func Make(): User { return User(); }\n");
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
      call_in(compilation.result->mir.files[0].functions[2].body);
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
  compilation.add("Verify.co", "func Value(): int { return 1; }\n");
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

  CompiledSources nullable;
  nullable.add("User.co", "");
  nullable.add("Factory.co", "func Empty(): User? { return null; }\n");
  nullable.compile();
  cloth::MirModule broken_conversion = nullable.result->mir;
  for (cloth::MirBasicBlock& block :
       broken_conversion.files[1].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<cloth::MirConvertInstruction>(
              instruction.data)) {
        instruction.type = cloth::TypeId{999};
      }
    }
  }
  cloth::DiagnosticEngine conversion_diagnostics;
  test.expect(!cloth::verify_mir(broken_conversion, nullable.result->semantics,
                                 conversion_diagnostics),
              "MIR verifier accepted an invalid nullable conversion type");
  test.expect(has_diagnostic(conversion_diagnostics, "unknown type"),
              "invalid nullable conversion did not report its type");

  cloth::HirModule broken_hir;
  const cloth::SourceRange range =
      compilation.result->semantics
          .symbol(compilation.result->semantics.file(cloth::FileId{0}).symbol)
          .range;
  static_cast<void>(broken_hir.storage.add_expression(cloth::HirExpression{
      compilation.result->semantics.bool_type(), range,
      cloth::HirUnaryExpression{cloth::TokenKind::kBang,
                                cloth::HirExpressionId{999}}}));
  static_cast<void>(broken_hir.storage.add_expression(cloth::HirExpression{
      compilation.result->semantics.void_type(), range,
      cloth::HirLiteralExpression{cloth::LiteralKind::kInteger, "1"}}));
  static_cast<void>(broken_hir.storage.add_statement(cloth::HirStatement{
      range,
      cloth::HirForStatement{cloth::SymbolId{999}, cloth::HirExpressionId{999},
                             cloth::HirBlockId{999}}}));
  cloth::DiagnosticEngine hir_diagnostics;
  test.expect(!cloth::verify_hir(broken_hir, compilation.result->semantics,
                                 hir_diagnostics),
              "HIR verifier accepted an invalid expression target");
  test.expect(has_diagnostic(hir_diagnostics, "unknown expression"),
              "HIR verifier reported the wrong invariant failure");
  test.expect(has_diagnostic(hir_diagnostics, "unknown symbol") &&
                  has_diagnostic(hir_diagnostics, "unknown block"),
              "HIR verifier did not validate for statement references");
  test.expect(has_diagnostic(hir_diagnostics, "void expression"),
              "HIR verifier accepted a fabricated void value");
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
      {"structured loop control flow", structured_loop_control_flow},
      {"unreachable statements", unreachable_statements},
      {"incomplete return flow", incomplete_return_flow},
      {"void fallthrough", void_fallthrough},
      {"field initializer body", field_initializer_body},
      {"member store", member_store},
      {"array instructions", array_instructions},
      {"string instructions", string_instructions},
      {"for iteration control flow", for_iteration_control_flow},
      {"for terminating body reachability", for_terminating_body_reachability},
      {"nullable conversion", nullable_conversion},
      {"nullable narrowing conversion", nullable_narrowing_conversion},
      {"null ergonomics lowering", null_ergonomics_lowering},
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
