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

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

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
                  "  return values::length + values[0];\n"
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
                  "  bool empty = joined::isEmpty;\n"
                  "  if (equal && different && !empty) {\n"
                  "    return joined::length;\n"
                  "  }\n"
                  "  return joined::byteLength;\n"
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
      if (const auto* meta =
              std::get_if<cloth::MirStringMetaInstruction>(&instruction.data)) {
        found_length =
            found_length || meta->query == cloth::StringMetaQuery::kLength;
        found_byte_length = found_byte_length ||
                            meta->query == cloth::StringMetaQuery::kByteLength;
        found_is_empty =
            found_is_empty || meta->query == cloth::StringMetaQuery::kIsEmpty;
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
              "string meta queries were not lowered explicitly");
  test.expect(found_concat && equality_count == 2,
              "string value operators were not retained in MIR");
}

void object_model_instructions(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Objects.co",
                  "Objects() {}\n"
                  "static func Main() {\n"
                  "  Objects instance = Objects();\n"
                  "  object value = instance;\n"
                  "  bool matches = value is Objects;\n"
                  "  Objects? cast = value as Objects?;\n"
                  "  object[] values = [instance, \"cloth\"];\n"
                  "  string name = value::typeName;\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid object operations failed to lower");
  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(
      body_has_conversion(body, cloth::MirConversionKind::kWidenReference),
      "object widening was not explicit in MIR");
  test.expect(body_has_instruction<cloth::MirTypeTestInstruction>(body) &&
                  body_has_instruction<cloth::MirCheckedCastInstruction>(body),
              "checked object operations were not lowered explicitly");
  test.expect(body_has_instruction<cloth::MirObjectMetaInstruction>(body),
              "object typeName was not lowered explicitly");
  test.expect(body_has_instruction<cloth::MirArrayLiteralInstruction>(body),
              "heterogeneous object array was not retained in MIR");
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

void classical_for_and_update_lowering(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Updates.co",
                  "func Sum(): int32 {\n"
                  "  int32 total = 0;\n"
                  "  int32 index = 0;\n"
                  "  int32[] values = [5, 9];\n"
                  "  values[index++] += 2;\n"
                  "  for (int32 i = 0; i < 4; i++) {\n"
                  "    if (i == 1) { continue; }\n"
                  "    total += i;\n"
                  "  }\n"
                  "  return total + values[0] + index;\n"
                  "}\n");
  compilation.compile();
  test.expect(compilation.result->is_valid,
              "classical for or update lowering failed verification");
  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  const cloth::SemanticModel& semantics = compilation.result->semantics;

  std::size_t index_stores = 0;
  std::size_t array_loads = 0;
  std::size_t array_stores = 0;
  std::optional<cloth::MirBlockId> update_block;
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    for (const cloth::MirInstruction& instruction :
         body.blocks[block_index].instructions) {
      if (const auto* store = std::get_if<cloth::MirStoreSymbolInstruction>(
              &instruction.data)) {
        const std::string& name = semantics.symbol(store->symbol).name;
        index_stores += name == "index" ? 1U : 0U;
        if (name == "i") {
          update_block = cloth::MirBlockId{block_index};
        }
      }
      array_loads += std::holds_alternative<cloth::MirArrayLoadInstruction>(
                         instruction.data)
                         ? 1U
                         : 0U;
      array_stores += std::holds_alternative<cloth::MirArrayStoreInstruction>(
                          instruction.data)
                          ? 1U
                          : 0U;
    }
  }
  test.expect(index_stores == 1,
              "an indexed compound target was evaluated more than once");
  test.expect(array_loads >= 2 && array_stores == 1,
              "array compound assignment did not load and store its element");
  test.expect(update_block.has_value(),
              "classical for update block was not lowered");
  if (!update_block) {
    return;
  }

  std::size_t update_edges = 0;
  for (const cloth::MirBasicBlock& block : body.blocks) {
    const auto* jump =
        std::get_if<cloth::MirJumpTerminator>(&block.terminator.data);
    update_edges += jump != nullptr && jump->target == *update_block ? 1U : 0U;
  }
  test.expect(update_edges >= 2,
              "continue or body fallthrough bypasses the for update clause");
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

void numeric_widening_conversion(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Numbers.co",
                  "func Expand(int16 signedSmall, uint16 unsignedSmall, "
                  "float32 single): float64 {\n"
                  "  int32 signedWide = signedSmall;\n"
                  "  int32 unsignedWide = unsignedSmall;\n"
                  "  int32[] values = [1];\n"
                  "  int32 first = values[signedSmall];\n"
                  "  signedWide += signedSmall;\n"
                  "  return single;\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid numeric widening failed MIR lowering");
  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[0].body;
  test.expect(
      body_has_conversion(body, cloth::MirConversionKind::kWidenNumeric),
      "numeric widening was not explicit in MIR");

  std::size_t widening_count = 0;
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      const auto* conversion =
          std::get_if<cloth::MirConvertInstruction>(&instruction.data);
      widening_count +=
          conversion != nullptr &&
                  conversion->kind == cloth::MirConversionKind::kWidenNumeric
              ? 1U
              : 0U;
    }
  }
  test.expect(widening_count == 5,
              "numeric widening did not cover locals, compound assignment, "
              "array indexing, and return");

  cloth::MirModule broken = compilation.result->mir;
  for (cloth::MirBasicBlock& block : broken.files[0].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      auto* conversion =
          std::get_if<cloth::MirConvertInstruction>(&instruction.data);
      if (conversion != nullptr &&
          conversion->kind == cloth::MirConversionKind::kWidenNumeric) {
        conversion->kind = cloth::MirConversionKind::kWidenReference;
        break;
      }
    }
  }
  cloth::DiagnosticEngine diagnostics;
  test.expect(
      !cloth::verify_mir(broken, compilation.result->semantics, diagnostics),
      "MIR verifier accepted a mislabeled numeric conversion");
  test.expect(has_diagnostic(diagnostics,
                             "reference widening consumes incompatible types"),
              "mislabeled numeric conversion produced the wrong diagnostic");
}

void overload_directed_literal_lowering(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Calls.co",
                  "func Take(uint value): uint { return value; }\n"
                  "func Use(): uint { return Take(4294967295); }\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "overload-directed literal failed MIR lowering");
  const cloth::MirBody& body =
      compilation.result->mir.files[0].functions[1].body;
  const cloth::MirCallInstruction* selected_call = nullptr;
  std::optional<cloth::MirValueId> argument;
  for (const cloth::MirBasicBlock& block : body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (const auto* call =
              std::get_if<cloth::MirCallInstruction>(&instruction.data);
          call != nullptr && !call->arguments.empty()) {
        selected_call = call;
        argument = call->arguments[0];
      }
    }
  }
  bool argument_is_uint32_literal = false;
  if (argument) {
    for (const cloth::MirBasicBlock& block : body.blocks) {
      for (const cloth::MirInstruction& instruction : block.instructions) {
        argument_is_uint32_literal =
            argument_is_uint32_literal ||
            (instruction.result == argument &&
             instruction.type ==
                 *compilation.result->semantics.find_type("uint32") &&
             std::holds_alternative<cloth::MirLiteralInstruction>(
                 instruction.data));
      }
    }
  }
  test.expect(selected_call != nullptr && argument_is_uint32_literal,
              "call argument did not retain its contextual literal type");
  test.expect(
      !body_has_conversion(body, cloth::MirConversionKind::kWidenNumeric),
      "contextual call literal was lowered as a typed-value conversion");
}

void checked_numeric_conversion_lowering(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Conversions.co",
                  "func Narrow(int32 value): int8 { return int8(value); }\n"
                  "func Constant(): int8 { return int8(12); }\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "checked numeric conversion failed MIR lowering");
  const cloth::MirBody& runtime =
      compilation.result->mir.files[0].functions[0].body;
  const cloth::MirBody& constant =
      compilation.result->mir.files[0].functions[1].body;
  test.expect(
      body_has_conversion(runtime, cloth::MirConversionKind::kCheckedNumeric),
      "runtime numeric conversion is not explicit in MIR");
  test.expect(
      !body_has_conversion(constant, cloth::MirConversionKind::kCheckedNumeric),
      "constant numeric conversion emitted an unnecessary runtime check");

  cloth::MirModule broken = compilation.result->mir;
  bool corrupted = false;
  for (cloth::MirBasicBlock& block : broken.files[0].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      const auto* conversion =
          std::get_if<cloth::MirConvertInstruction>(&instruction.data);
      if (conversion != nullptr &&
          conversion->kind == cloth::MirConversionKind::kCheckedNumeric) {
        instruction.type = compilation.result->semantics.string_type();
        corrupted = true;
      }
    }
  }
  cloth::DiagnosticEngine diagnostics;
  test.expect(
      corrupted && !cloth::verify_mir(broken, compilation.result->semantics,
                                      diagnostics),
      "MIR verifier accepted an invalid checked numeric conversion");
  test.expect(
      has_diagnostic(diagnostics,
                     "checked numeric conversion consumes incompatible types"),
      "invalid checked numeric conversion produced the wrong diagnostic");
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
                  class_call->kind == cloth::MirCallKind::kClassQualified &&
                  class_call->dispatch == cloth::MirDispatchKind::kDirect,
              "class-qualified call lost its call kind");
  test.expect(instance_call != nullptr && instance_call->receiver &&
                  instance_call->kind == cloth::MirCallKind::kInstance &&
                  instance_call->dispatch == cloth::MirDispatchKind::kVirtual,
              "instance-qualified call lost its receiver or call kind");
  test.expect(
      unqualified_call != nullptr && !unqualified_call->receiver &&
          unqualified_call->kind == cloth::MirCallKind::kUnqualified &&
          unqualified_call->dispatch == cloth::MirDispatchKind::kVirtual,
      "unqualified call lost its call kind");
  test.expect(constructor_call != nullptr && !constructor_call->receiver &&
                  constructor_call->kind == cloth::MirCallKind::kConstructor &&
                  constructor_call->dispatch == cloth::MirDispatchKind::kDirect,
              "constructor call lost its call kind");
}

void constructor_initialization_order(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Base.co", "int32 BaseValue = 1;\nBase(int32 value) {}\n");
  compilation.add("Derived.co",
                  "class : Base {\n"
                  "  int32 DerivedValue = 2;\n"
                  "  Derived(int32 value): Base(value) {}\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid constructor chain failed MIR lowering");
  const cloth::MirCallable& constructor =
      compilation.result->mir.files[1].constructors[0];
  bool found_base_call = false;
  bool found_field_initialization = false;
  for (const cloth::MirBasicBlock& block : constructor.body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (const auto* call =
              std::get_if<cloth::MirCallInstruction>(&instruction.data);
          call != nullptr &&
          call->kind == cloth::MirCallKind::kBaseConstructor) {
        found_base_call = true;
        test.expect(!found_field_initialization && call->arguments.size() == 1,
                    "base constructor call has the wrong position or args");
      }
      if (std::holds_alternative<cloth::MirInitializeFieldsInstruction>(
              instruction.data)) {
        test.expect(found_base_call,
                    "derived fields initialize before the base constructor");
        found_field_initialization = true;
      }
    }
  }
  test.expect(found_base_call && found_field_initialization,
              "constructor MIR lost an initialization phase");
}

void inherited_reference_widening(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Base.co",
                  "string Name;\n"
                  "Base(string name) { Name = name; }\n"
                  "func Read(): string { return Name; }\n");
  compilation.add("Derived.co",
                  "class : Base {\n"
                  "  Derived(string name): Base(name) {}\n"
                  "  func Own(): string { return Read(); }\n"
                  "}\n");
  compilation.add("Other.co", "int32 Wrong;\n");
  compilation.add("Use.co",
                  "func Upcast(Derived value): Base { return value; }\n"
                  "func Maybe(Derived? value): Base? { return value; }\n"
                  "func Field(Derived value): string {\n"
                  "  value.Name = \"updated\";\n"
                  "  return value.Read();\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid inherited references failed MIR lowering");
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  const cloth::TypeId base_type = semantics.file(cloth::FileId{0}).type;
  const cloth::TypeId other_type = semantics.file(cloth::FileId{2}).type;
  const cloth::SymbolId base_name = semantics.file(cloth::FileId{0}).fields[0];
  const cloth::SymbolId base_read =
      semantics.file(cloth::FileId{0}).functions[0];
  const cloth::MirFileClass& use = compilation.result->mir.files[3];

  bool found_base_widening = false;
  bool found_nullable_base_widening = false;
  for (const cloth::MirCallable& callable : use.functions) {
    for (const cloth::MirBasicBlock& block : callable.body.blocks) {
      for (const cloth::MirInstruction& instruction : block.instructions) {
        const auto* conversion =
            std::get_if<cloth::MirConvertInstruction>(&instruction.data);
        if (conversion == nullptr ||
            conversion->kind != cloth::MirConversionKind::kWidenReference) {
          continue;
        }
        found_base_widening =
            found_base_widening || instruction.type == base_type;
        if (instruction.type.value < semantics.types().size()) {
          const cloth::SemanticType& target = semantics.type(instruction.type);
          found_nullable_base_widening =
              found_nullable_base_widening ||
              (target.kind == cloth::TypeKind::kNullable &&
               target.element_type == base_type);
        }
      }
    }
  }
  test.expect(found_base_widening && found_nullable_base_widening,
              "base-reference widenings were not explicit in MIR");

  bool found_inherited_store = false;
  bool found_inherited_call = false;
  for (const cloth::MirBasicBlock& block : use.functions[2].body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (const auto* store = std::get_if<cloth::MirStoreMemberInstruction>(
              &instruction.data)) {
        found_inherited_store =
            found_inherited_store || store->member == base_name;
      }
      if (const auto* call =
              std::get_if<cloth::MirCallInstruction>(&instruction.data)) {
        found_inherited_call =
            found_inherited_call ||
            (call->callable == base_read &&
             call->kind == cloth::MirCallKind::kInstance &&
             call->dispatch == cloth::MirDispatchKind::kVirtual);
      }
    }
  }
  test.expect(found_inherited_store && found_inherited_call,
              "inherited member symbols were not preserved in MIR");

  const cloth::MirCallInstruction* unqualified = nullptr;
  for (const cloth::MirBasicBlock& block :
       compilation.result->mir.files[1].functions[0].body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (const auto* call =
              std::get_if<cloth::MirCallInstruction>(&instruction.data)) {
        unqualified = call;
      }
    }
  }
  test.expect(unqualified != nullptr && unqualified->callable == base_read &&
                  unqualified->kind == cloth::MirCallKind::kUnqualified &&
                  unqualified->dispatch == cloth::MirDispatchKind::kVirtual,
              "unqualified inherited call lost its base virtual slot");

  cloth::MirModule broken = compilation.result->mir;
  for (cloth::MirBasicBlock& block : broken.files[3].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (const auto* conversion =
              std::get_if<cloth::MirConvertInstruction>(&instruction.data);
          conversion != nullptr &&
          conversion->kind == cloth::MirConversionKind::kWidenReference) {
        instruction.type = other_type;
      }
    }
  }
  cloth::DiagnosticEngine diagnostics;
  test.expect(!cloth::verify_mir(broken, semantics, diagnostics),
              "MIR verifier accepted an unrelated reference widening");
  test.expect(has_diagnostic(diagnostics,
                             "reference widening consumes incompatible types"),
              "invalid inherited widening produced the wrong diagnostic");

  cloth::MirModule broken_member = compilation.result->mir;
  const cloth::SymbolId unrelated_field =
      semantics.file(cloth::FileId{2}).fields[0];
  for (cloth::MirBasicBlock& block :
       broken_member.files[3].functions[2].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (auto* store = std::get_if<cloth::MirStoreMemberInstruction>(
              &instruction.data)) {
        store->member = unrelated_field;
      }
    }
  }
  cloth::DiagnosticEngine member_diagnostics;
  test.expect(!cloth::verify_mir(broken_member, semantics, member_diagnostics),
              "MIR verifier accepted an unrelated member receiver");
  test.expect(
      has_diagnostic(member_diagnostics,
                     "member receiver is unrelated to its declaring class"),
      "invalid inherited member produced the wrong diagnostic");
}

void virtual_dispatch_lowering(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Base.co",
                  "int32 Marker = Hook();\n"
                  "Base() { self.Hook(); }\n"
                  "Base(Base other) { other.Hook(); }\n"
                  "func Hook(): int32 { return 1; }\n");
  compilation.add("Derived.co",
                  "class : Base {\n"
                  "  Derived(): Base() {}\n"
                  "  override func Hook(): int32 { return 2; }\n"
                  "}\n");
  compilation.add("Use.co",
                  "func ThroughBase(Base value): int32 { "
                  "return value.Hook(); }\n"
                  "func ThroughDerived(Derived value): int32 { "
                  "return value.Hook(); }\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid virtual calls failed MIR lowering");
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  const cloth::SymbolId base_hook =
      semantics.file(cloth::FileId{0}).functions[0];
  const cloth::SymbolId derived_hook =
      semantics.file(cloth::FileId{1}).functions[0];
  const auto first_call =
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

  const cloth::MirCallInstruction* through_base =
      first_call(compilation.result->mir.files[2].functions[0].body);
  const cloth::MirCallInstruction* through_derived =
      first_call(compilation.result->mir.files[2].functions[1].body);
  test.expect(through_base != nullptr && through_base->callable == base_hook &&
                  through_base->dispatch == cloth::MirDispatchKind::kVirtual,
              "base-typed call lost virtual dispatch");
  test.expect(through_derived != nullptr &&
                  through_derived->callable == derived_hook &&
                  through_derived->dispatch == cloth::MirDispatchKind::kVirtual,
              "derived-typed call lost its override slot");

  const cloth::MirCallInstruction* initializer_call =
      first_call(*compilation.result->mir.files[0].fields[0].initializer);
  const cloth::MirCallInstruction* constructor_call =
      first_call(compilation.result->mir.files[0].constructors[0].body);
  const cloth::MirCallInstruction* other_receiver_call =
      first_call(compilation.result->mir.files[0].constructors[1].body);
  test.expect(
      initializer_call != nullptr &&
          initializer_call->dispatch == cloth::MirDispatchKind::kDirect &&
          constructor_call != nullptr &&
          constructor_call->dispatch == cloth::MirDispatchKind::kDirect,
      "construction-time calls were lowered virtually");
  test.expect(
      other_receiver_call != nullptr &&
          !other_receiver_call->receiver_is_self &&
          other_receiver_call->dispatch == cloth::MirDispatchKind::kVirtual,
      "constructor suppressed dispatch on an unrelated receiver");

  cloth::MirModule broken = compilation.result->mir;
  bool corrupted = false;
  for (cloth::MirBasicBlock& block : broken.files[2].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (auto* call =
              std::get_if<cloth::MirCallInstruction>(&instruction.data)) {
        call->dispatch = cloth::MirDispatchKind::kDirect;
        corrupted = true;
      }
    }
  }
  cloth::DiagnosticEngine diagnostics;
  test.expect(corrupted && !cloth::verify_mir(broken, semantics, diagnostics),
              "MIR verifier accepted a direct virtual-function call");
  test.expect(has_diagnostic(diagnostics,
                             "virtual function was lowered as a direct call"),
              "invalid virtual dispatch produced the wrong diagnostic");
}

void base_qualified_call_lowering(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Base.co", "func Value(): int32 { return 1; }\n");
  compilation.add("Derived.co",
                  "class : Base {\n"
                  "  override func Value(): int32 {\n"
                  "    return super.Value() + 1;\n"
                  "  }\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid base-qualified call failed MIR lowering");
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  const cloth::SymbolId base_value =
      semantics.file(cloth::FileId{0}).functions[0];
  const cloth::SymbolId derived_value =
      semantics.file(cloth::FileId{1}).functions[0];
  cloth::MirCallInstruction* base_call = nullptr;
  cloth::MirModule broken = compilation.result->mir;
  for (cloth::MirBasicBlock& block : broken.files[1].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      auto* call = std::get_if<cloth::MirCallInstruction>(&instruction.data);
      if (call != nullptr && call->kind == cloth::MirCallKind::kBaseQualified) {
        base_call = call;
      }
    }
  }
  test.expect(base_call != nullptr && base_call->callable == base_value &&
                  base_call->dispatch == cloth::MirDispatchKind::kDirect &&
                  base_call->receiver_is_self && !base_call->receiver,
              "base-qualified call lost its direct self dispatch contract");

  if (base_call != nullptr) {
    base_call->callable = derived_value;
  }
  cloth::DiagnosticEngine diagnostics;
  test.expect(base_call != nullptr &&
                  !cloth::verify_mir(broken, semantics, diagnostics),
              "MIR verifier accepted a base call to the derived override");
  test.expect(has_diagnostic(
                  diagnostics,
                  "base-qualified call does not target a direct-base member"),
              "invalid base-qualified call produced the wrong diagnostic");
}

void interface_dispatch_lowering(TestContext& test) {
  CompiledSources compilation;
  compilation.add("Renderable.co", "interface { func Render(): string; }\n");
  compilation.add("Widget.co",
                  "class is Renderable {\n"
                  "  func Render(): string { return \"widget\"; }\n"
                  "}\n");
  compilation.add("Use.co",
                  "func Upcast(Widget value): Renderable { return value; }\n"
                  "func Read(Renderable value): string {\n"
                  "  return value.Render();\n"
                  "}\n");
  compilation.compile();

  test.expect(compilation.result->is_valid,
              "valid interface program failed MIR lowering");
  const cloth::MirFileClass& use = compilation.result->mir.files[2];
  test.expect(body_has_conversion(use.functions[0].body,
                                  cloth::MirConversionKind::kWidenReference),
              "class-to-interface conversion was not explicit in MIR");
  const cloth::MirCallInstruction* interface_call = nullptr;
  for (const cloth::MirBasicBlock& block : use.functions[1].body.blocks) {
    for (const cloth::MirInstruction& instruction : block.instructions) {
      if (const auto* call =
              std::get_if<cloth::MirCallInstruction>(&instruction.data)) {
        interface_call = call;
      }
    }
  }
  test.expect(
      interface_call != nullptr && interface_call->receiver &&
          interface_call->dispatch == cloth::MirDispatchKind::kInterface &&
          interface_call->interface_file == cloth::FileId{0} &&
          interface_call->interface_slot == 0,
      "interface call lost its receiver, identity, or contract slot");
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

  CompiledSources inheritance;
  inheritance.add("Base.co", "Base() {}\n");
  inheritance.add("Derived.co", "class : Base { Derived(): Base() {} }\n");
  inheritance.compile();
  cloth::HirModule broken_base_hir = inheritance.result->hir;
  broken_base_hir.files[1].base_file.reset();
  cloth::DiagnosticEngine base_hir_diagnostics;
  test.expect(!cloth::verify_hir(broken_base_hir, inheritance.result->semantics,
                                 base_hir_diagnostics),
              "HIR verifier accepted a missing base identity");
  test.expect(
      has_diagnostic(base_hir_diagnostics, "base does not match semantics"),
      "HIR base corruption produced the wrong diagnostic");

  cloth::MirModule broken_base_mir = inheritance.result->mir;
  broken_base_mir.files[1].base_file = cloth::FileId{999};
  cloth::DiagnosticEngine base_mir_diagnostics;
  test.expect(!cloth::verify_mir(broken_base_mir, inheritance.result->semantics,
                                 base_mir_diagnostics),
              "MIR verifier accepted an invalid base identity");
  test.expect(
      has_diagnostic(base_mir_diagnostics, "base does not match semantics") &&
          has_diagnostic(base_mir_diagnostics, "invalid base FileId"),
      "MIR base corruption produced the wrong diagnostics");

  cloth::HirModule broken_initializer_hir = inheritance.result->hir;
  broken_initializer_hir.files[1].constructors[0].initializer.reset();
  cloth::DiagnosticEngine initializer_hir_diagnostics;
  test.expect(
      !cloth::verify_hir(broken_initializer_hir, inheritance.result->semantics,
                         initializer_hir_diagnostics),
      "HIR verifier accepted a missing constructor initializer");
  test.expect(has_diagnostic(initializer_hir_diagnostics,
                             "lost its semantic base initializer"),
              "HIR initializer corruption produced the wrong diagnostic");

  cloth::MirModule broken_initializer_mir = inheritance.result->mir;
  for (cloth::MirBasicBlock& block :
       broken_initializer_mir.files[1].constructors[0].body.blocks) {
    std::erase_if(block.instructions, [](const cloth::MirInstruction& value) {
      return std::holds_alternative<cloth::MirInitializeFieldsInstruction>(
          value.data);
    });
  }
  cloth::DiagnosticEngine initializer_mir_diagnostics;
  test.expect(
      !cloth::verify_mir(broken_initializer_mir, inheritance.result->semantics,
                         initializer_mir_diagnostics),
      "MIR verifier accepted a missing field-initialization phase");
  test.expect(has_diagnostic(initializer_mir_diagnostics,
                             "exactly one field initialization"),
              "MIR initializer corruption produced the wrong diagnostic");

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
      range, cloth::HirForEachStatement{cloth::SymbolId{999},
                                        cloth::HirExpressionId{999},
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
      {"object model instructions", object_model_instructions},
      {"for iteration control flow", for_iteration_control_flow},
      {"for terminating body reachability", for_terminating_body_reachability},
      {"classical for and update lowering", classical_for_and_update_lowering},
      {"nullable conversion", nullable_conversion},
      {"numeric widening conversion", numeric_widening_conversion},
      {"overload-directed literal lowering",
       overload_directed_literal_lowering},
      {"checked numeric conversion lowering",
       checked_numeric_conversion_lowering},
      {"nullable narrowing conversion", nullable_narrowing_conversion},
      {"null ergonomics lowering", null_ergonomics_lowering},
      {"call receivers", call_receivers},
      {"constructor initialization order", constructor_initialization_order},
      {"inherited reference widening", inherited_reference_widening},
      {"virtual dispatch lowering", virtual_dispatch_lowering},
      {"base-qualified call lowering", base_qualified_call_lowering},
      {"interface dispatch lowering", interface_dispatch_lowering},
      {"verifiers reject corruption", verifiers_reject_corruption},
  };

  return cloth::test::run_tests(tests);
}
