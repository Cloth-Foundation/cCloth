// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/mir/mir_optimizer.h"

#include "cloth/ast/ast.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/scalar_constants.h"
#include "cloth/sema/semantic_model.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

enum class ScalarStateKind { kUnknown, kExact, kVarying };

struct ScalarState {
  ScalarStateKind kind{ScalarStateKind::kUnknown};
  std::optional<ScalarConstant> value{};
};

ScalarState varying() { return {ScalarStateKind::kVarying, std::nullopt}; }

ScalarState exact(ScalarConstant value) {
  return {ScalarStateKind::kExact, value};
}

std::vector<MirValueId> fold_operands(const MirInstruction& instruction) {
  if (const auto* unary = std::get_if<MirUnaryInstruction>(&instruction.data)) {
    return {unary->operand};
  }
  if (const auto* binary =
          std::get_if<MirBinaryInstruction>(&instruction.data)) {
    return {binary->left, binary->right};
  }
  if (const auto* conversion =
          std::get_if<MirConvertInstruction>(&instruction.data)) {
    return {conversion->value};
  }
  if (const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data)) {
    std::vector<MirValueId> operands;
    operands.reserve(phi->incoming.size());
    for (const MirPhiIncoming& incoming : phi->incoming) {
      operands.push_back(incoming.value);
    }
    return operands;
  }
  return {};
}

struct Definition {
  const MirInstruction* instruction{};
  std::size_t block{};
};

enum class WorkKind { kValue, kTerminator };

enum class AliasState { kUnseen, kVisiting, kSafe, kUnsafe };

struct WorkItem {
  WorkKind kind;
  std::size_t index;
};

struct ScalarAnalysis {
  std::vector<ScalarState> states;
  std::vector<bool> executable_blocks;
  std::set<std::pair<std::size_t, std::size_t>> executable_edges;
};

class BodyScalarAnalysis {
 public:
  BodyScalarAnalysis(const MirBody& body, const SemanticModel& semantics)
      : body_(body),
        semantics_(semantics),
        result_{std::vector<ScalarState>(body.value_count),
                std::vector<bool>(body.blocks.size()),
                {}},
        definitions_(body.value_count),
        dependents_(body.value_count),
        terminator_dependents_(body.value_count),
        block_values_(body.blocks.size()),
        block_phis_(body.blocks.size()),
        queued_values_(body.value_count),
        queued_terminators_(body.blocks.size()) {}

  ScalarAnalysis run() {
    collect_dependencies();
    if (body_.entry.value < body_.blocks.size()) {
      mark_block(body_.entry.value);
    }
    while (!worklist_.empty()) {
      const WorkItem item = worklist_.front();
      worklist_.pop_front();
      if (item.kind == WorkKind::kValue) {
        queued_values_[item.index] = false;
        process_value(MirValueId{item.index});
      } else {
        queued_terminators_[item.index] = false;
        process_terminator(item.index);
      }
    }
    return std::move(result_);
  }

 private:
  void collect_dependencies() {
    for (std::size_t block_index = 0; block_index < body_.blocks.size();
         ++block_index) {
      const MirBasicBlock& block = body_.blocks[block_index];
      for (const MirInstruction& instruction : block.instructions) {
        if (!instruction.result ||
            instruction.result->value >= definitions_.size()) {
          continue;
        }
        const MirValueId value = *instruction.result;
        definitions_[value.value] = Definition{&instruction, block_index};
        block_values_[block_index].push_back(value);
        if (std::holds_alternative<MirPhiInstruction>(instruction.data)) {
          block_phis_[block_index].push_back(value);
        }
        for (const MirValueId operand : fold_operands(instruction)) {
          if (operand.value < dependents_.size()) {
            dependents_[operand.value].push_back(value);
          }
        }
      }
      const MirTerminator& terminator = block.terminator;
      if (const auto* branch =
              std::get_if<MirBranchTerminator>(&terminator.data);
          branch != nullptr && branch->condition.value < body_.value_count) {
        terminator_dependents_[branch->condition.value].push_back(block_index);
      } else if (const auto* selection =
                     std::get_if<MirSwitchTerminator>(&terminator.data);
                 selection != nullptr &&
                 selection->selector.value < body_.value_count) {
        terminator_dependents_[selection->selector.value].push_back(
            block_index);
      }
    }
  }

  void enqueue_value(MirValueId value) {
    if (value.value >= queued_values_.size() || queued_values_[value.value]) {
      return;
    }
    queued_values_[value.value] = true;
    worklist_.push_back({WorkKind::kValue, value.value});
  }

  void enqueue_terminator(std::size_t block) {
    if (block >= queued_terminators_.size() || queued_terminators_[block]) {
      return;
    }
    queued_terminators_[block] = true;
    worklist_.push_back({WorkKind::kTerminator, block});
  }

  void mark_block(std::size_t block) {
    if (block >= result_.executable_blocks.size() ||
        result_.executable_blocks[block]) {
      return;
    }
    result_.executable_blocks[block] = true;
    for (const MirValueId value : block_values_[block]) {
      enqueue_value(value);
    }
    enqueue_terminator(block);
  }

  void mark_edge(std::size_t predecessor, MirBlockId target) {
    if (target.value >= body_.blocks.size()) {
      return;
    }
    const auto [unused, inserted] =
        result_.executable_edges.emplace(predecessor, target.value);
    static_cast<void>(unused);
    if (!inserted) {
      return;
    }
    const bool was_executable = result_.executable_blocks[target.value];
    mark_block(target.value);
    if (was_executable) {
      for (const MirValueId phi : block_phis_[target.value]) {
        enqueue_value(phi);
      }
    }
  }

  ScalarState operand(MirValueId value) const {
    return value.value < result_.states.size() ? result_.states[value.value]
                                               : varying();
  }

  ScalarState constant_result(const MirInstruction& instruction,
                              ConstantBits bits) const {
    if (!bits) {
      return varying();
    }
    const ScalarConstant value{instruction.type, *bits};
    return is_valid_scalar_constant(value, instruction.type, semantics_)
               ? exact(value)
               : varying();
  }

  ScalarState evaluate_literal(const MirInstruction& instruction,
                               const MirLiteralInstruction& literal) const {
    if (instruction.type.value >= semantics_.types().size()) {
      return varying();
    }
    const TypeKind type = semantics_.type(instruction.type).kind;
    if (!is_scalar_constant_type(type)) {
      return varying();
    }
    if (literal.kind == LiteralKind::kEnum) {
      const auto tag =
          enum_constant_tag(literal.lexeme, instruction.type, semantics_);
      return tag ? constant_result(instruction, ConstantBits{*tag}) : varying();
    }
    return constant_result(instruction,
                           scalar_literal(literal.kind, literal.lexeme, type));
  }

  ScalarState evaluate_unary(const MirInstruction& instruction,
                             const MirUnaryInstruction& unary) const {
    const ScalarState input = operand(unary.operand);
    if (input.kind != ScalarStateKind::kExact) {
      return input.kind == ScalarStateKind::kUnknown ? input : varying();
    }
    const TypeKind type = semantics_.type(input.value->type).kind;
    return constant_result(
        instruction, unary_scalar(unary.operation, type, input.value->bits));
  }

  ScalarState evaluate_binary(const MirInstruction& instruction,
                              const MirBinaryInstruction& binary) const {
    const ScalarState left = operand(binary.left);
    const ScalarState right = operand(binary.right);
    if (left.kind == ScalarStateKind::kVarying ||
        right.kind == ScalarStateKind::kVarying) {
      return varying();
    }
    if (left.kind == ScalarStateKind::kUnknown ||
        right.kind == ScalarStateKind::kUnknown) {
      return {};
    }
    const TypeKind left_type = semantics_.type(left.value->type).kind;
    const TypeKind right_type = semantics_.type(right.value->type).kind;
    return constant_result(
        instruction,
        binary_scalar(binary.operation, left_type, left.value->bits,
                      right.value->bits, right_type));
  }

  ScalarState evaluate_conversion(
      const MirInstruction& instruction,
      const MirConvertInstruction& conversion) const {
    const ScalarState input = operand(conversion.value);
    if (input.kind != ScalarStateKind::kExact) {
      return input.kind == ScalarStateKind::kUnknown ? input : varying();
    }
    const TypeKind source = semantics_.type(input.value->type).kind;
    const TypeKind target = semantics_.type(instruction.type).kind;
    switch (conversion.kind) {
      case MirConversionKind::kWidenNumeric:
      case MirConversionKind::kCheckedNumeric:
        return constant_result(
            instruction, convert_scalar(input.value->bits, source, target));
      case MirConversionKind::kWrapInteger:
        return constant_result(
            instruction, convert_integer_mode(input.value->bits, source, target,
                                              IntegerConversionMode::kWrap));
      case MirConversionKind::kSaturateInteger:
        return constant_result(
            instruction, convert_integer_mode(input.value->bits, source, target,
                                              IntegerConversionMode::kSat));
      case MirConversionKind::kWidenReference:
      case MirConversionKind::kToNullable:
      case MirConversionKind::kFromNullable:
        return varying();
    }
    return varying();
  }

  ScalarState evaluate_phi(const MirInstruction& instruction,
                           const MirPhiInstruction& phi,
                           std::size_t block) const {
    std::optional<ScalarConstant> value;
    for (const MirPhiIncoming& incoming : phi.incoming) {
      if (!result_.executable_edges.contains(
              {incoming.predecessor.value, block})) {
        continue;
      }
      const ScalarState state = operand(incoming.value);
      if (state.kind == ScalarStateKind::kVarying) {
        return varying();
      }
      if (state.kind == ScalarStateKind::kExact) {
        if (value && *value != *state.value) {
          return varying();
        }
        value = state.value;
      }
    }
    if (value) {
      return is_valid_scalar_constant(*value, instruction.type, semantics_)
                 ? exact(*value)
                 : varying();
    }
    return {};
  }

  ScalarState evaluate(const Definition& definition) const {
    const MirInstruction& instruction = *definition.instruction;
    if (const auto* constant =
            std::get_if<MirScalarConstantInstruction>(&instruction.data)) {
      return is_valid_scalar_constant(constant->value, instruction.type,
                                      semantics_)
                 ? exact(constant->value)
                 : varying();
    }
    if (const auto* literal =
            std::get_if<MirLiteralInstruction>(&instruction.data)) {
      return evaluate_literal(instruction, *literal);
    }
    if (const auto* load =
            std::get_if<MirLoadSymbolInstruction>(&instruction.data)) {
      if (load->symbol.value >= semantics_.symbols().size()) {
        return varying();
      }
      const SemanticSymbol& symbol = semantics_.symbol(load->symbol);
      if (symbol.kind != SymbolKind::kField || !symbol.is_static ||
          !symbol.is_final || !symbol.static_constant ||
          !is_valid_scalar_constant(*symbol.static_constant, instruction.type,
                                    semantics_)) {
        return varying();
      }
      return exact(*symbol.static_constant);
    }
    if (const auto* unary =
            std::get_if<MirUnaryInstruction>(&instruction.data)) {
      return evaluate_unary(instruction, *unary);
    }
    if (const auto* binary =
            std::get_if<MirBinaryInstruction>(&instruction.data)) {
      return evaluate_binary(instruction, *binary);
    }
    if (const auto* conversion =
            std::get_if<MirConvertInstruction>(&instruction.data)) {
      return evaluate_conversion(instruction, *conversion);
    }
    if (const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data)) {
      return evaluate_phi(instruction, *phi, definition.block);
    }
    return varying();
  }

  bool update_state(MirValueId value, ScalarState candidate) {
    ScalarState& current = result_.states[value.value];
    if (current.kind == ScalarStateKind::kVarying ||
        candidate.kind == ScalarStateKind::kUnknown) {
      return false;
    }
    if (current.kind == ScalarStateKind::kUnknown) {
      current = candidate;
      return true;
    }
    if (candidate.kind == ScalarStateKind::kVarying ||
        *current.value != *candidate.value) {
      current = varying();
      return true;
    }
    return false;
  }

  void process_value(MirValueId value) {
    if (value.value >= definitions_.size()) {
      return;
    }
    const Definition& definition = definitions_[value.value];
    if (definition.instruction == nullptr ||
        !result_.executable_blocks[definition.block] ||
        !update_state(value, evaluate(definition))) {
      return;
    }
    for (const MirValueId dependent : dependents_[value.value]) {
      enqueue_value(dependent);
    }
    for (const std::size_t block : terminator_dependents_[value.value]) {
      enqueue_terminator(block);
    }
  }

  void process_terminator(std::size_t block_index) {
    if (block_index >= body_.blocks.size() ||
        !result_.executable_blocks[block_index]) {
      return;
    }
    const MirTerminator& terminator = body_.blocks[block_index].terminator;
    if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator.data)) {
      mark_edge(block_index, jump->target);
    } else if (const auto* branch =
                   std::get_if<MirBranchTerminator>(&terminator.data)) {
      const ScalarState condition = operand(branch->condition);
      if (condition.kind == ScalarStateKind::kExact) {
        mark_edge(block_index, condition.value->bits != 0 ? branch->then_block
                                                          : branch->else_block);
      } else if (condition.kind == ScalarStateKind::kVarying) {
        mark_edge(block_index, branch->then_block);
        mark_edge(block_index, branch->else_block);
      }
    } else if (const auto* selection =
                   std::get_if<MirSwitchTerminator>(&terminator.data)) {
      const ScalarState selector = operand(selection->selector);
      if (selector.kind == ScalarStateKind::kExact) {
        MirBlockId target = selection->default_block;
        for (const MirSwitchCase& entry : selection->cases) {
          if (entry.value == *selector.value) {
            target = entry.target;
            break;
          }
        }
        mark_edge(block_index, target);
      } else if (selector.kind == ScalarStateKind::kVarying) {
        for (const MirBlockId successor : mir_successors(terminator)) {
          mark_edge(block_index, successor);
        }
      }
    }
  }

  const MirBody& body_;
  const SemanticModel& semantics_;
  ScalarAnalysis result_;
  std::vector<Definition> definitions_;
  std::vector<std::vector<MirValueId>> dependents_;
  std::vector<std::vector<std::size_t>> terminator_dependents_;
  std::vector<std::vector<MirValueId>> block_values_;
  std::vector<std::vector<MirValueId>> block_phis_;
  std::vector<bool> queued_values_;
  std::vector<bool> queued_terminators_;
  std::deque<WorkItem> worklist_;
};

bool is_foldable(const MirInstruction& instruction) {
  return std::holds_alternative<MirLiteralInstruction>(instruction.data) ||
         std::holds_alternative<MirLoadSymbolInstruction>(instruction.data) ||
         std::holds_alternative<MirUnaryInstruction>(instruction.data) ||
         std::holds_alternative<MirBinaryInstruction>(instruction.data) ||
         std::holds_alternative<MirConvertInstruction>(instruction.data) ||
         std::holds_alternative<MirPhiInstruction>(instruction.data);
}

std::size_t executable_phi_edges(const MirPhiInstruction& phi,
                                 std::size_t block,
                                 const ScalarAnalysis& analysis) {
  std::size_t result = 0;
  for (const MirPhiIncoming& incoming : phi.incoming) {
    result +=
        analysis.executable_edges.contains({incoming.predecessor.value, block});
  }
  return result;
}

void rewrite_constants(MirBody& body, const ScalarAnalysis& analysis) {
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    if (!analysis.executable_blocks[block_index]) {
      continue;
    }
    for (MirInstruction& instruction : body.blocks[block_index].instructions) {
      if (!instruction.result ||
          instruction.result->value >= analysis.states.size()) {
        continue;
      }
      const ScalarState& state = analysis.states[instruction.result->value];
      if (state.kind != ScalarStateKind::kExact || !is_foldable(instruction)) {
        continue;
      }
      if (const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data);
          phi != nullptr &&
          executable_phi_edges(*phi, block_index, analysis) <= 1) {
        continue;
      }
      instruction.data = MirScalarConstantInstruction{*state.value};
    }
  }
}

void rewrite_terminators(MirBody& body, const ScalarAnalysis& analysis) {
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    if (!analysis.executable_blocks[block_index]) {
      continue;
    }
    MirTerminator& terminator = body.blocks[block_index].terminator;
    if (const auto* branch =
            std::get_if<MirBranchTerminator>(&terminator.data)) {
      if (branch->condition.value >= analysis.states.size()) {
        continue;
      }
      const ScalarState& condition = analysis.states[branch->condition.value];
      if (condition.kind == ScalarStateKind::kExact) {
        terminator.data =
            MirJumpTerminator{condition.value->bits != 0 ? branch->then_block
                                                         : branch->else_block};
      }
    } else if (const auto* selection =
                   std::get_if<MirSwitchTerminator>(&terminator.data)) {
      if (selection->selector.value >= analysis.states.size()) {
        continue;
      }
      const ScalarState& selector = analysis.states[selection->selector.value];
      if (selector.kind != ScalarStateKind::kExact) {
        continue;
      }
      MirBlockId target = selection->default_block;
      for (const MirSwitchCase& entry : selection->cases) {
        if (entry.value == *selector.value) {
          target = entry.target;
          break;
        }
      }
      terminator.data = MirJumpTerminator{target};
    }
  }
}

std::vector<bool> reachable_blocks(const MirBody& body) {
  std::vector<bool> reachable(body.blocks.size());
  if (body.entry.value >= body.blocks.size()) {
    return reachable;
  }
  std::vector<MirBlockId> pending{body.entry};
  while (!pending.empty()) {
    const MirBlockId block = pending.back();
    pending.pop_back();
    if (block.value >= body.blocks.size() || reachable[block.value]) {
      continue;
    }
    reachable[block.value] = true;
    for (const MirBlockId successor :
         mir_successors(body.blocks[block.value].terminator)) {
      pending.push_back(successor);
    }
  }
  return reachable;
}

MirValueId resolve_alias(
    MirValueId value, const std::vector<std::optional<MirValueId>>& aliases) {
  for (std::size_t count = 0; value.value < aliases.size() &&
                              aliases[value.value] && count < aliases.size();
       ++count) {
    value = *aliases[value.value];
  }
  return value;
}

void remap_value(MirValueId& value,
                 const std::vector<std::optional<MirValueId>>& aliases,
                 const std::vector<std::optional<MirValueId>>& values) {
  const MirValueId resolved = resolve_alias(value, aliases);
  if (resolved.value < values.size() && values[resolved.value]) {
    value = *values[resolved.value];
  }
}

void remap_optional_value(
    std::optional<MirValueId>& value,
    const std::vector<std::optional<MirValueId>>& aliases,
    const std::vector<std::optional<MirValueId>>& values) {
  if (value) {
    remap_value(*value, aliases, values);
  }
}

void remap_storage_path(MirStoragePath& path,
                        const std::vector<std::optional<MirValueId>>& aliases,
                        const std::vector<std::optional<MirValueId>>& values) {
  remap_optional_value(path.object, aliases, values);
  remap_optional_value(path.index, aliases, values);
}

void remap_instruction(MirInstruction& instruction,
                       const std::vector<std::optional<MirValueId>>& aliases,
                       const std::vector<std::optional<MirValueId>>& values) {
  if (auto* load = std::get_if<MirLoadStorageInstruction>(&instruction.data)) {
    remap_storage_path(load->path, aliases, values);
  } else if (auto* store =
                 std::get_if<MirStoreStorageInstruction>(&instruction.data)) {
    remap_storage_path(store->path, aliases, values);
    remap_value(store->value, aliases, values);
  } else if (auto* declaration =
                 std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
    remap_optional_value(declaration->initializer, aliases, values);
  } else if (auto* store =
                 std::get_if<MirStoreSymbolInstruction>(&instruction.data)) {
    remap_value(store->value, aliases, values);
  } else if (auto* load =
                 std::get_if<MirLoadMemberInstruction>(&instruction.data)) {
    remap_value(load->object, aliases, values);
  } else if (auto* store =
                 std::get_if<MirStoreMemberInstruction>(&instruction.data)) {
    remap_value(store->object, aliases, values);
    remap_value(store->value, aliases, values);
  } else if (auto* array =
                 std::get_if<MirArrayLiteralInstruction>(&instruction.data)) {
    for (MirValueId& element : array->elements) {
      remap_value(element, aliases, values);
    }
  } else if (auto* load =
                 std::get_if<MirArrayLoadInstruction>(&instruction.data)) {
    remap_value(load->array, aliases, values);
    remap_value(load->index, aliases, values);
  } else if (auto* store =
                 std::get_if<MirArrayStoreInstruction>(&instruction.data)) {
    remap_value(store->array, aliases, values);
    remap_value(store->index, aliases, values);
    remap_value(store->value, aliases, values);
  } else if (auto* length =
                 std::get_if<MirArrayLengthInstruction>(&instruction.data)) {
    remap_value(length->array, aliases, values);
  } else if (auto* meta =
                 std::get_if<MirStringMetaInstruction>(&instruction.data)) {
    remap_value(meta->string, aliases, values);
  } else if (auto* meta =
                 std::get_if<MirObjectMetaInstruction>(&instruction.data)) {
    remap_value(meta->object, aliases, values);
  } else if (auto* write =
                 std::get_if<MirIntegerWriteInstruction>(&instruction.data)) {
    remap_value(write->value, aliases, values);
    remap_value(write->destination, aliases, values);
    remap_value(write->offset, aliases, values);
  } else if (auto* read =
                 std::get_if<MirIntegerReadInstruction>(&instruction.data)) {
    remap_value(read->source, aliases, values);
    remap_value(read->offset, aliases, values);
  } else if (auto* unary =
                 std::get_if<MirUnaryInstruction>(&instruction.data)) {
    remap_value(unary->operand, aliases, values);
  } else if (auto* binary =
                 std::get_if<MirBinaryInstruction>(&instruction.data)) {
    remap_value(binary->left, aliases, values);
    remap_value(binary->right, aliases, values);
  } else if (auto* conversion =
                 std::get_if<MirConvertInstruction>(&instruction.data)) {
    remap_value(conversion->value, aliases, values);
  } else if (auto* test =
                 std::get_if<MirIsNonNullInstruction>(&instruction.data)) {
    remap_value(test->value, aliases, values);
  } else if (auto* assertion =
                 std::get_if<MirNullAssertInstruction>(&instruction.data)) {
    remap_value(assertion->value, aliases, values);
  } else if (auto* test =
                 std::get_if<MirTypeTestInstruction>(&instruction.data)) {
    remap_value(test->value, aliases, values);
  } else if (auto* cast =
                 std::get_if<MirCheckedCastInstruction>(&instruction.data)) {
    remap_value(cast->value, aliases, values);
  } else if (auto* call = std::get_if<MirCallInstruction>(&instruction.data)) {
    remap_optional_value(call->receiver, aliases, values);
    for (MirValueId& argument : call->arguments) {
      remap_value(argument, aliases, values);
    }
  } else if (auto* phi = std::get_if<MirPhiInstruction>(&instruction.data)) {
    for (MirPhiIncoming& incoming : phi->incoming) {
      remap_value(incoming.value, aliases, values);
    }
  }
}

void remap_block(MirBlockId& block,
                 const std::vector<std::optional<MirBlockId>>& blocks) {
  if (block.value < blocks.size() && blocks[block.value]) {
    block = *blocks[block.value];
  }
}

void remap_terminator(MirTerminator& terminator,
                      const std::vector<std::optional<MirBlockId>>& blocks,
                      const std::vector<std::optional<MirValueId>>& aliases,
                      const std::vector<std::optional<MirValueId>>& values) {
  if (auto* jump = std::get_if<MirJumpTerminator>(&terminator.data)) {
    remap_block(jump->target, blocks);
  } else if (auto* branch =
                 std::get_if<MirBranchTerminator>(&terminator.data)) {
    remap_value(branch->condition, aliases, values);
    remap_block(branch->then_block, blocks);
    remap_block(branch->else_block, blocks);
  } else if (auto* selection =
                 std::get_if<MirSwitchTerminator>(&terminator.data)) {
    remap_value(selection->selector, aliases, values);
    for (MirSwitchCase& entry : selection->cases) {
      remap_block(entry.target, blocks);
    }
    remap_block(selection->default_block, blocks);
    if (selection->invalid_block) {
      remap_block(*selection->invalid_block, blocks);
    }
  } else if (auto* returned =
                 std::get_if<MirReturnTerminator>(&terminator.data)) {
    remap_optional_value(returned->value, aliases, values);
  }
}

void compact_body(MirBody& body) {
  const std::vector<bool> reachable = reachable_blocks(body);
  std::set<std::pair<std::size_t, std::size_t>> live_edges;
  for (std::size_t predecessor = 0; predecessor < body.blocks.size();
       ++predecessor) {
    if (!reachable[predecessor]) {
      continue;
    }
    for (const MirBlockId target :
         mir_successors(body.blocks[predecessor].terminator)) {
      if (target.value < reachable.size() && reachable[target.value]) {
        live_edges.emplace(predecessor, target.value);
      }
    }
  }
  std::vector<std::optional<MirBlockId>> block_ids(body.blocks.size());
  std::size_t block_count = 0;
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (reachable[index]) {
      block_ids[index] = MirBlockId{block_count++};
    }
  }

  std::vector<std::optional<MirValueId>> candidates(body.value_count);
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    if (!reachable[block_index]) {
      continue;
    }
    for (const MirInstruction& instruction :
         body.blocks[block_index].instructions) {
      const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data);
      if (phi == nullptr || !instruction.result ||
          instruction.result->value >= candidates.size()) {
        continue;
      }
      std::optional<MirValueId> incoming_value;
      std::size_t incoming_count = 0;
      for (const MirPhiIncoming& incoming : phi->incoming) {
        if (live_edges.contains({incoming.predecessor.value, block_index})) {
          incoming_value = incoming.value;
          ++incoming_count;
        }
      }
      if (incoming_count == 1) {
        candidates[instruction.result->value] = incoming_value;
      }
    }
  }

  std::vector<AliasState> alias_states(candidates.size());
  for (std::size_t start = 0; start < candidates.size(); ++start) {
    if (!candidates[start] || alias_states[start] != AliasState::kUnseen) {
      continue;
    }
    std::vector<std::size_t> path;
    std::size_t current = start;
    while (current < candidates.size() && candidates[current] &&
           alias_states[current] == AliasState::kUnseen) {
      alias_states[current] = AliasState::kVisiting;
      path.push_back(current);
      current = candidates[current]->value;
    }
    const bool is_safe =
        current < candidates.size() &&
        (!candidates[current] || alias_states[current] == AliasState::kSafe);
    for (const std::size_t value : path) {
      alias_states[value] = is_safe ? AliasState::kSafe : AliasState::kUnsafe;
    }
  }

  std::vector<std::optional<MirValueId>> aliases(candidates.size());
  for (std::size_t value = 0; value < candidates.size(); ++value) {
    if (alias_states[value] == AliasState::kSafe) {
      aliases[value] = candidates[value];
    }
  }

  std::vector<std::optional<MirValueId>> value_ids(body.value_count);
  std::size_t value_count = 0;
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    if (!reachable[block_index]) {
      continue;
    }
    for (const MirInstruction& instruction :
         body.blocks[block_index].instructions) {
      if (!instruction.result ||
          instruction.result->value >= value_ids.size() ||
          aliases[instruction.result->value]) {
        continue;
      }
      value_ids[instruction.result->value] = MirValueId{value_count++};
    }
  }

  std::vector<MirBasicBlock> blocks;
  blocks.reserve(block_count);
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    if (!reachable[block_index]) {
      continue;
    }
    MirBasicBlock block = body.blocks[block_index];
    block.is_reachable = true;
    std::vector<MirInstruction> instructions;
    instructions.reserve(block.instructions.size());
    for (MirInstruction instruction : block.instructions) {
      if (const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data)) {
        std::vector<MirPhiIncoming> incoming;
        incoming.reserve(phi->incoming.size());
        for (MirPhiIncoming edge : phi->incoming) {
          if (live_edges.contains({edge.predecessor.value, block_index})) {
            incoming.push_back(edge);
          }
        }
        if (incoming.size() == 1) {
          continue;
        }
        std::get<MirPhiInstruction>(instruction.data).incoming =
            std::move(incoming);
      }
      if (instruction.result && instruction.result->value < value_ids.size() &&
          value_ids[instruction.result->value]) {
        instruction.result = *value_ids[instruction.result->value];
      }
      remap_instruction(instruction, aliases, value_ids);
      if (auto* phi = std::get_if<MirPhiInstruction>(&instruction.data)) {
        for (MirPhiIncoming& edge : phi->incoming) {
          remap_block(edge.predecessor, block_ids);
        }
      }
      instructions.push_back(std::move(instruction));
    }
    block.instructions = std::move(instructions);
    remap_terminator(block.terminator, block_ids, aliases, value_ids);
    blocks.push_back(std::move(block));
  }

  remap_block(body.entry, block_ids);
  body.blocks = std::move(blocks);
  body.value_count = value_count;
}

void optimize_body(MirBody& body, const SemanticModel& semantics) {
  if (body.blocks.empty()) {
    return;
  }
  const ScalarAnalysis analysis = BodyScalarAnalysis{body, semantics}.run();
  rewrite_constants(body, analysis);
  rewrite_terminators(body, analysis);
  compact_body(body);
}

}  // namespace

void optimize_mir(MirModule& mir, const SemanticModel& semantics) {
  for (MirFileClass& file : mir.files) {
    for (MirField& field : file.fields) {
      if (field.initializer) {
        optimize_body(*field.initializer, semantics);
      }
    }
    for (MirCallable& function : file.functions) {
      optimize_body(function.body, semantics);
    }
    for (MirCallable& constructor : file.constructors) {
      optimize_body(constructor.body, semantics);
    }
  }
}

}  // namespace cloth
