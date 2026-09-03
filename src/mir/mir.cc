// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/mir/mir.h"

#include "cloth/hir/hir.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/semantic_model.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

struct LoweredLocation {
  std::optional<SymbolId> symbol;
  std::optional<MirValueId> object;
  std::optional<MirValueId> index;
  TypeId type;
  bool is_member{false};
  bool is_array_element{false};
  std::optional<MirStoragePath> path{};
};

struct TransferTargets {
  std::optional<MirBlockId> continue_target;
  MirBlockId break_target;
};

class BodyBuilder {
 public:
  BodyBuilder(const HirModule& hir, const SemanticModel& semantics, FileId file,
              SourceRange range, TypeId return_type,
              bool suppress_self_virtual_dispatch)
      : hir_(hir),
        semantics_(semantics),
        file_(file),
        return_type_(return_type),
        suppress_self_virtual_dispatch_(suppress_self_virtual_dispatch),
        body_{range, MirBlockId{0}, {}, 0} {
    body_.entry = add_block(true);
    current_block_ = body_.entry;
  }

  MirBody lower_callable(HirBlockId block) {
    lower_block(block);
    if (current_block_) {
      if (return_type_ == semantics_.void_type()) {
        terminate(MirReturnTerminator{}, body_.range);
      } else {
        terminate(MirUnreachableTerminator{}, body_.range);
      }
    }
    finish_unterminated_blocks();
    body_.value_count = value_types_.size();
    return std::move(body_);
  }

  MirBody lower_abstract() {
    terminate(MirUnreachableTerminator{}, body_.range);
    finish_unterminated_blocks();
    body_.value_count = value_types_.size();
    return std::move(body_);
  }

  MirBody lower_constructor(
      const std::optional<HirConstructorInitializer>& initializer,
      HirBlockId block) {
    if (initializer) {
      const SemanticSymbol& base_constructor =
          semantics_.symbol(initializer->constructor);
      std::vector<MirValueId> arguments;
      arguments.reserve(initializer->arguments.size());
      for (std::size_t index = 0; index < initializer->arguments.size();
           ++index) {
        const HirExpressionId argument_id = initializer->arguments[index];
        const HirExpression& argument = hir_.storage.expression(argument_id);
        MirValueId value = require_value(lower_expression(argument_id),
                                         argument.type, argument.range);
        if (index < base_constructor.parameter_types.size()) {
          value = coerce(value, base_constructor.parameter_types[index],
                         argument.range);
        }
        arguments.push_back(value);
      }
      emit_void(initializer->range,
                MirCallInstruction{MirCallKind::kBaseConstructor,
                                   MirDispatchKind::kDirect, true,
                                   initializer->constructor, std::nullopt,
                                   std::move(arguments)});
    }
    emit_void(hir_.storage.block(block).range,
              MirInitializeFieldsInstruction{});
    return lower_callable(block);
  }

  MirBody lower_initializer(HirExpressionId expression, TypeId field_type) {
    const HirExpression& syntax = hir_.storage.expression(expression);
    const MirValueId value =
        require_value(lower_expression(expression), syntax.type, syntax.range);
    const MirValueId converted = coerce(value, field_type, syntax.range);
    terminate(MirReturnTerminator{converted}, syntax.range);
    finish_unterminated_blocks();
    body_.value_count = value_types_.size();
    return std::move(body_);
  }

 private:
  MirBlockId add_block(bool is_reachable) {
    const MirBlockId id{body_.blocks.size()};
    body_.blocks.push_back(
        MirBasicBlock{is_reachable,
                      {},
                      MirTerminator{body_.range, MirUnreachableTerminator{}}});
    terminated_.push_back(false);
    return id;
  }

  void ensure_current(SourceRange range) {
    if (!current_block_) {
      current_block_ = add_block(false);
      body_.blocks[current_block_->value].terminator.range = range;
    }
  }

  bool current_is_reachable() const {
    return current_block_ && body_.blocks[current_block_->value].is_reachable;
  }

  template <typename Data>
  MirValueId emit_value(TypeId type, SourceRange range, Data data) {
    ensure_current(range);
    const MirValueId value{value_types_.size()};
    value_types_.push_back(type);
    body_.blocks[current_block_->value].instructions.push_back(MirInstruction{
        value, type, range, MirInstructionData{std::move(data)}});
    return value;
  }

  template <typename Data>
  void emit_void(SourceRange range, Data data) {
    ensure_current(range);
    body_.blocks[current_block_->value].instructions.push_back(
        MirInstruction{std::nullopt, semantics_.void_type(), range,
                       MirInstructionData{std::move(data)}});
  }

  template <typename Data>
  void terminate(Data data, SourceRange range) {
    if (!current_block_) {
      return;
    }
    const std::size_t index = current_block_->value;
    body_.blocks[index].terminator =
        MirTerminator{range, MirTerminatorData{std::move(data)}};
    terminated_[index] = true;
    current_block_.reset();
  }

  void jump_to(MirBlockId target, SourceRange range) {
    terminate(MirJumpTerminator{target}, range);
  }

  MirValueId invalid_value(SourceRange range) {
    return emit_value(semantics_.error_type(), range, MirInvalidInstruction{});
  }

  MirValueId require_value(std::optional<MirValueId> value, TypeId,
                           SourceRange range) {
    if (value) {
      return *value;
    }
    return invalid_value(range);
  }

  TypeId value_type(MirValueId value) const {
    return value_types_.at(value.value);
  }

  MirValueId coerce(MirValueId value, TypeId expected, SourceRange range) {
    const TypeId actual = value_type(value);
    if (actual == expected || actual == semantics_.error_type() ||
        expected == semantics_.error_type()) {
      return value;
    }
    const SemanticType& target = semantics_.type(expected);
    if (can_widen_numeric(semantics_.type(actual).kind, target.kind)) {
      return emit_value(
          expected, range,
          MirConvertInstruction{value, MirConversionKind::kWidenNumeric});
    }
    if (can_widen_reference(actual, expected) &&
        is_non_null_reference(actual)) {
      return emit_value(
          expected, range,
          MirConvertInstruction{value, MirConversionKind::kWidenReference});
    }
    if (target.kind == TypeKind::kNullable && target.element_type) {
      if (actual == semantics_.null_type()) {
        return emit_value(
            expected, range,
            MirConvertInstruction{value, MirConversionKind::kToNullable});
      }
      const SemanticType& source = semantics_.type(actual);
      if (source.kind == TypeKind::kNullable && source.element_type &&
          can_widen_reference(*source.element_type, *target.element_type)) {
        return emit_value(
            expected, range,
            MirConvertInstruction{value, MirConversionKind::kWidenReference});
      }
      MirValueId converted = value;
      if (actual != *target.element_type) {
        converted = coerce(value, *target.element_type, range);
        if (value_type(converted) == semantics_.error_type()) {
          return converted;
        }
      }
      return emit_value(
          expected, range,
          MirConvertInstruction{converted, MirConversionKind::kToNullable});
    }
    return invalid_value(range);
  }

  bool is_non_null_reference(TypeId type) const {
    const TypeKind kind = semantics_.type(type).kind;
    return kind == TypeKind::kString || kind == TypeKind::kObject ||
           kind == TypeKind::kFileClass || kind == TypeKind::kInterface ||
           kind == TypeKind::kArray;
  }

  bool can_widen_reference(TypeId source, TypeId target) const {
    if (source == target) {
      return true;
    }
    if (target == semantics_.object_type()) {
      return is_non_null_reference(source);
    }
    const SemanticType& source_type = semantics_.type(source);
    const SemanticType& target_type = semantics_.type(target);
    if ((source_type.kind == TypeKind::kFileClass ||
         source_type.kind == TypeKind::kInterface) &&
        source_type.file && target_type.kind == TypeKind::kInterface &&
        target_type.file) {
      const std::vector<FileId>& interfaces =
          semantics_.file(*source_type.file).interfaces;
      return std::ranges::find(interfaces, *target_type.file) !=
             interfaces.end();
    }
    if (source_type.kind != TypeKind::kFileClass || !source_type.file ||
        target_type.kind != TypeKind::kFileClass || !target_type.file) {
      return false;
    }
    std::optional<FileId> current =
        semantics_.file(*source_type.file).base_file;
    for (std::size_t depth = 0; current && depth < semantics_.files().size();
         ++depth) {
      if (*current == *target_type.file) {
        return true;
      }
      current = semantics_.file(*current).base_file;
    }
    return false;
  }

  std::optional<TypeId> common_numeric_type(TypeId left, TypeId right) const {
    const TypeKind left_kind = semantics_.type(left).kind;
    const TypeKind right_kind = semantics_.type(right).kind;
    if (!is_numeric_type(left_kind) || !is_numeric_type(right_kind)) {
      return std::nullopt;
    }
    if (left == right) {
      return left;
    }
    if (can_widen_numeric(left_kind, right_kind)) {
      return right;
    }
    if (can_widen_numeric(right_kind, left_kind)) {
      return left;
    }
    return std::nullopt;
  }

  void lower_block(HirBlockId id) {
    const HirBlock& block = hir_.storage.block(id);
    for (const HirStatementId statement_id : block.statements) {
      const HirStatement& statement = hir_.storage.statement(statement_id);
      ensure_current(statement.range);
      lower_statement(statement);
    }
  }

  void lower_statement(const HirStatement& statement) {
    if (std::holds_alternative<HirInvalidStatement>(statement.data)) {
      emit_void(statement.range, MirInvalidInstruction{});
      return;
    }
    if (const auto* local = std::get_if<HirLocalStatement>(&statement.data)) {
      lower_local(*local, statement.range);
      return;
    }
    if (const auto* return_statement =
            std::get_if<HirReturnStatement>(&statement.data)) {
      lower_return(*return_statement, statement.range);
      return;
    }
    if (const auto* expression =
            std::get_if<HirExpressionStatement>(&statement.data)) {
      static_cast<void>(lower_expression(expression->expression));
      return;
    }
    if (const auto* if_statement =
            std::get_if<HirIfStatement>(&statement.data)) {
      lower_if(*if_statement, statement.range);
      return;
    }
    if (const auto* while_statement =
            std::get_if<HirWhileStatement>(&statement.data)) {
      lower_while(*while_statement, statement.range);
      return;
    }
    if (const auto* for_statement =
            std::get_if<HirForEachStatement>(&statement.data)) {
      lower_for_each(*for_statement, statement.range);
      return;
    }
    if (const auto* for_statement =
            std::get_if<HirForStatement>(&statement.data)) {
      lower_for(*for_statement, statement.range);
      return;
    }
    if (std::holds_alternative<HirBreakStatement>(statement.data)) {
      if (!transfer_targets_.empty()) {
        jump_to(transfer_targets_.back().break_target, statement.range);
      }
      return;
    }
    if (std::holds_alternative<HirContinueStatement>(statement.data)) {
      for (auto target = transfer_targets_.rbegin();
           target != transfer_targets_.rend(); ++target) {
        if (target->continue_target) {
          jump_to(*target->continue_target, statement.range);
          break;
        }
      }
      return;
    }
    if (const auto* selection =
            std::get_if<HirSwitchStatement>(&statement.data)) {
      lower_switch(*selection, statement.range);
      return;
    }
    const auto& nested = std::get<HirNestedBlockStatement>(statement.data);
    lower_block(nested.block);
  }

  void lower_local(const HirLocalStatement& local, SourceRange range) {
    std::optional<MirValueId> initializer;
    if (local.initializer) {
      const HirExpression& syntax = hir_.storage.expression(*local.initializer);
      MirValueId value = require_value(lower_expression(*local.initializer),
                                       syntax.type, syntax.range);
      if (local.symbol) {
        value =
            coerce(value, semantics_.symbol(*local.symbol).type, syntax.range);
      }
      initializer = value;
    }
    if (local.symbol) {
      emit_void(range, MirDeclareLocalInstruction{*local.symbol, initializer});
    } else {
      emit_void(range, MirInvalidInstruction{});
    }
  }

  void lower_return(const HirReturnStatement& return_statement,
                    SourceRange range) {
    std::optional<MirValueId> value;
    if (return_statement.value) {
      const HirExpression& syntax =
          hir_.storage.expression(*return_statement.value);
      MirValueId lowered = require_value(
          lower_expression(*return_statement.value), syntax.type, syntax.range);
      if (return_type_ != semantics_.void_type()) {
        lowered = coerce(lowered, return_type_, syntax.range);
        value = lowered;
      }
    }
    if (!value && return_type_ != semantics_.void_type()) {
      terminate(MirUnreachableTerminator{}, range);
      return;
    }
    terminate(MirReturnTerminator{value}, range);
  }

  void lower_if(const HirIfStatement& if_statement, SourceRange range) {
    const MirValueId condition = lower_condition(
        if_statement.condition, if_statement.condition_is_presence_test);
    const bool branch_reachable = current_is_reachable();
    const MirBlockId then_block = add_block(branch_reachable);
    const MirBlockId else_block = add_block(branch_reachable);
    terminate(MirBranchTerminator{condition, then_block, else_block}, range);

    current_block_ = then_block;
    lower_block(if_statement.then_block);
    const std::optional<MirBlockId> then_end = current_block_;

    current_block_ = else_block;
    if (if_statement.else_block) {
      lower_block(*if_statement.else_block);
    }
    const std::optional<MirBlockId> else_end = current_block_;

    if (!then_end && !else_end) {
      current_block_.reset();
      return;
    }
    const bool continuation_reachable =
        (then_end && body_.blocks[then_end->value].is_reachable) ||
        (else_end && body_.blocks[else_end->value].is_reachable);
    const MirBlockId continuation = add_block(continuation_reachable);
    if (then_end) {
      current_block_ = then_end;
      jump_to(continuation, range);
    }
    if (else_end) {
      current_block_ = else_end;
      jump_to(continuation, range);
    }
    current_block_ = continuation;
  }

  void lower_switch(const HirSwitchStatement& selection, SourceRange range) {
    const MirValueId selector = require_value(
        lower_expression(selection.selector), selection.selector_type, range);
    const bool reachable = current_is_reachable();
    const MirBlockId dispatch = *current_block_;
    const MirBlockId join = add_block(reachable);
    std::optional<MirBlockId> invalid;
    if (semantics_.type(selection.selector_type).kind == TypeKind::kEnum) {
      invalid = add_block(reachable);
      current_block_ = *invalid;
      terminate(MirTrapTerminator{}, range);
    }
    MirSwitchTerminator terminator{
        selector, selection.selector_type, {}, invalid.value_or(join), invalid};
    std::vector<MirBlockId> arms;
    arms.reserve(selection.arms.size());
    for (const auto& arm : selection.arms) {
      const MirBlockId target = add_block(reachable);
      arms.push_back(target);
      if (arm.is_default) terminator.default_block = target;
      for (const auto& label : arm.labels)
        terminator.cases.push_back(MirSwitchCase{label.value, target});
    }
    std::ranges::sort(terminator.cases, {}, [](const MirSwitchCase& entry) {
      return entry.value.bits;
    });
    current_block_ = dispatch;
    terminate(std::move(terminator), range);
    transfer_targets_.push_back(TransferTargets{std::nullopt, join});
    for (std::size_t index = 0; index < arms.size(); ++index) {
      current_block_ = arms[index];
      lower_block(selection.arms[index].body);
      if (current_block_) jump_to(join, range);
    }
    transfer_targets_.pop_back();
    current_block_ = join;
  }

  void lower_while(const HirWhileStatement& while_statement,
                   SourceRange range) {
    const bool loop_reachable = current_is_reachable();
    const MirBlockId condition_block = add_block(loop_reachable);
    const MirBlockId body_block = add_block(loop_reachable);
    const MirBlockId exit_block = add_block(loop_reachable);
    jump_to(condition_block, range);

    current_block_ = condition_block;
    const MirValueId condition = lower_condition(
        while_statement.condition, while_statement.condition_is_presence_test);
    terminate(MirBranchTerminator{condition, body_block, exit_block}, range);

    transfer_targets_.push_back(TransferTargets{condition_block, exit_block});
    current_block_ = body_block;
    lower_block(while_statement.body);
    if (current_block_) {
      jump_to(condition_block, range);
    }
    transfer_targets_.pop_back();
    current_block_ = exit_block;
  }

  void lower_for_each(const HirForEachStatement& for_statement,
                      SourceRange range) {
    const HirExpression& iterable =
        hir_.storage.expression(for_statement.iterable);
    const MirValueId array =
        require_value(lower_expression(for_statement.iterable), iterable.type,
                      iterable.range);
    const SemanticType& array_type = semantics_.type(iterable.type);
    if (array_type.kind != TypeKind::kArray || !array_type.element_type ||
        !for_statement.variable) {
      return;
    }
    const TypeId int32_type = *semantics_.find_type("int32");
    const MirValueId zero = emit_value(
        int32_type, range, MirLiteralInstruction{LiteralKind::kInteger, "0"});
    const MirBlockId preheader = *current_block_;

    const MirBlockId condition_block = add_block(true);
    const MirBlockId body_block = add_block(true);
    const MirBlockId latch_block = add_block(true);
    const MirBlockId exit_block = add_block(true);
    jump_to(condition_block, range);

    current_block_ = condition_block;
    const MirValueId index =
        emit_value(int32_type, range,
                   MirPhiInstruction{{MirPhiIncoming{preheader, zero}}});
    const std::size_t phi_instruction =
        body_.blocks[condition_block.value].instructions.size() - 1;
    const MirValueId length =
        emit_value(int32_type, range, MirArrayLengthInstruction{array});
    const MirValueId condition =
        emit_value(semantics_.bool_type(), range,
                   MirBinaryInstruction{index, TokenKind::kLess, length});
    terminate(MirBranchTerminator{condition, body_block, exit_block}, range);

    current_block_ = body_block;
    const TypeId variable_type =
        semantics_.symbol(*for_statement.variable).type;
    MirValueId element = emit_value(*array_type.element_type, range,
                                    MirArrayLoadInstruction{array, index});
    element = coerce(element, variable_type, range);
    emit_void(range,
              MirDeclareLocalInstruction{*for_statement.variable, element});

    transfer_targets_.push_back(TransferTargets{latch_block, exit_block});
    lower_block(for_statement.body);
    if (current_block_) {
      jump_to(latch_block, range);
    }
    transfer_targets_.pop_back();

    current_block_ = latch_block;
    const MirValueId one = emit_value(
        int32_type, range, MirLiteralInstruction{LiteralKind::kInteger, "1"});
    const MirValueId next_index = emit_value(
        int32_type, range, MirBinaryInstruction{index, TokenKind::kPlus, one});
    jump_to(condition_block, range);

    MirInstruction& instruction =
        body_.blocks[condition_block.value].instructions[phi_instruction];
    auto& phi = std::get<MirPhiInstruction>(instruction.data);
    phi.incoming.push_back(MirPhiIncoming{latch_block, next_index});
    current_block_ = exit_block;
  }

  void lower_for(const HirForStatement& for_statement, SourceRange range) {
    if (for_statement.initializer) {
      lower_statement(hir_.storage.statement(*for_statement.initializer));
    }

    const bool reachable = current_is_reachable();
    const MirBlockId condition_block = add_block(reachable);
    const MirBlockId body_block = add_block(reachable);
    const MirBlockId update_block = add_block(reachable);
    const MirBlockId exit_block = add_block(reachable);
    jump_to(condition_block, range);

    current_block_ = condition_block;
    if (for_statement.condition) {
      const MirValueId condition = lower_condition(
          *for_statement.condition, for_statement.condition_is_presence_test);
      terminate(MirBranchTerminator{condition, body_block, exit_block}, range);
    } else {
      jump_to(body_block, range);
    }

    current_block_ = body_block;
    transfer_targets_.push_back(TransferTargets{update_block, exit_block});
    lower_block(for_statement.body);
    if (current_block_) {
      jump_to(update_block, range);
    }
    transfer_targets_.pop_back();

    current_block_ = update_block;
    for (const HirExpressionId update : for_statement.updates) {
      static_cast<void>(lower_expression(update));
    }
    if (current_block_) {
      jump_to(condition_block, range);
    }
    current_block_ = exit_block;
  }

  std::optional<MirValueId> lower_expression(HirExpressionId id) {
    const HirExpression& expression = hir_.storage.expression(id);
    if (std::holds_alternative<HirInvalidExpression>(expression.data)) {
      return invalid_value(expression.range);
    }
    if (const auto* literal =
            std::get_if<HirLiteralExpression>(&expression.data)) {
      return emit_value(expression.type, expression.range,
                        MirLiteralInstruction{literal->kind, literal->lexeme});
    }
    if (const auto* symbol =
            std::get_if<HirSymbolExpression>(&expression.data)) {
      return lower_symbol(*symbol, expression);
    }
    if (std::holds_alternative<HirTypeExpression>(expression.data) ||
        std::holds_alternative<HirSuperExpression>(expression.data)) {
      return invalid_value(expression.range);
    }
    if (const auto* unary = std::get_if<HirUnaryExpression>(&expression.data)) {
      const HirExpression& operand_syntax =
          hir_.storage.expression(unary->operand);
      const MirValueId operand =
          unary->operation == TokenKind::kBang
              ? lower_condition(unary->operand, unary->operand_is_presence_test)
              : require_value(lower_expression(unary->operand),
                              operand_syntax.type, operand_syntax.range);
      return emit_value(expression.type, expression.range,
                        MirUnaryInstruction{unary->operation, operand});
    }
    if (const auto* update =
            std::get_if<HirUpdateExpression>(&expression.data)) {
      return lower_update(*update, expression);
    }
    if (const auto* binary =
            std::get_if<HirBinaryExpression>(&expression.data)) {
      if (binary->operation == TokenKind::kAmpersandAmpersand ||
          binary->operation == TokenKind::kPipePipe) {
        return lower_short_circuit(*binary, expression);
      }
      MirValueId left = require_value(
          lower_expression(binary->left),
          hir_.storage.expression(binary->left).type, expression.range);
      MirValueId right = require_value(
          lower_expression(binary->right),
          hir_.storage.expression(binary->right).type, expression.range);
      if (binary->operation != TokenKind::kShiftLeft &&
          binary->operation != TokenKind::kShiftRight) {
        if (const std::optional<TypeId> common =
                common_numeric_type(value_type(left), value_type(right))) {
          left = coerce(left, *common, expression.range);
          right = coerce(right, *common, expression.range);
        }
      }
      return emit_value(expression.type, expression.range,
                        MirBinaryInstruction{left, binary->operation, right});
    }
    if (const auto* test =
            std::get_if<HirTypeTestExpression>(&expression.data)) {
      const HirExpression& value_syntax = hir_.storage.expression(test->value);
      const MirValueId value = require_value(
          lower_expression(test->value), value_syntax.type, value_syntax.range);
      return emit_value(expression.type, expression.range,
                        MirTypeTestInstruction{value, test->target});
    }
    if (const auto* cast =
            std::get_if<HirCheckedCastExpression>(&expression.data)) {
      const HirExpression& value_syntax = hir_.storage.expression(cast->value);
      const MirValueId value = require_value(
          lower_expression(cast->value), value_syntax.type, value_syntax.range);
      return emit_value(expression.type, expression.range,
                        MirCheckedCastInstruction{value, cast->target});
    }
    if (const auto* conversion =
            std::get_if<HirNumericConversionExpression>(&expression.data)) {
      const HirExpression& value_syntax =
          hir_.storage.expression(conversion->value);
      const MirValueId value =
          require_value(lower_expression(conversion->value), value_syntax.type,
                        value_syntax.range);
      if (value_type(value) == expression.type) {
        return value;
      }
      return emit_value(
          expression.type, expression.range,
          MirConvertInstruction{value, MirConversionKind::kCheckedNumeric});
    }
    if (const auto* assignment =
            std::get_if<HirAssignmentExpression>(&expression.data)) {
      return lower_assignment(*assignment, expression);
    }
    if (const auto* member =
            std::get_if<HirMemberExpression>(&expression.data)) {
      return lower_member(*member, expression);
    }
    if (const auto* member =
            std::get_if<HirSafeMemberExpression>(&expression.data)) {
      return lower_safe_member(*member, expression);
    }
    if (const auto* coalesce =
            std::get_if<HirNullCoalesceExpression>(&expression.data)) {
      return lower_null_coalesce(*coalesce, expression);
    }
    if (const auto* assertion =
            std::get_if<HirNullAssertExpression>(&expression.data)) {
      return lower_null_assert(*assertion, expression);
    }
    if (std::holds_alternative<HirIntegerMetaExpression>(expression.data)) {
      return invalid_value(expression.range);
    }
    if (const auto* call =
            std::get_if<HirIntegerMetaCallExpression>(&expression.data)) {
      const HirExpression& object = hir_.storage.expression(call->object);
      const MirValueId lowered_object = require_value(
          lower_expression(call->object), object.type, object.range);
      const TypeId int32_type = *semantics_.find_type("int32");
      if (call->operation.kind == IntegerMetaOperationKind::kWrite) {
        if (call->arguments.size() != 2) {
          return invalid_value(expression.range);
        }
        const HirExpression& destination =
            hir_.storage.expression(call->arguments[0]);
        const HirExpression& offset =
            hir_.storage.expression(call->arguments[1]);
        const MirValueId lowered_destination =
            require_value(lower_expression(call->arguments[0]),
                          destination.type, destination.range);
        MirValueId lowered_offset = require_value(
            lower_expression(call->arguments[1]), offset.type, offset.range);
        lowered_offset = coerce(lowered_offset, int32_type, offset.range);
        emit_void(expression.range,
                  MirIntegerWriteInstruction{
                      lowered_object, lowered_destination, lowered_offset,
                      call->operation.byte_order});
        return std::nullopt;
      }
      if (call->arguments.size() != 1) {
        return invalid_value(expression.range);
      }
      const HirExpression& offset = hir_.storage.expression(call->arguments[0]);
      MirValueId lowered_offset = require_value(
          lower_expression(call->arguments[0]), offset.type, offset.range);
      lowered_offset = coerce(lowered_offset, int32_type, offset.range);
      return emit_value(
          expression.type, expression.range,
          MirIntegerReadInstruction{lowered_object, lowered_offset,
                                    call->operation.byte_order});
    }
    if (const auto* call = std::get_if<HirCallExpression>(&expression.data)) {
      return lower_call(*call, expression);
    }
    if (const auto* array =
            std::get_if<HirArrayLiteralExpression>(&expression.data)) {
      std::vector<MirValueId> elements;
      elements.reserve(array->elements.size());
      for (const HirExpressionId element_id : array->elements) {
        const HirExpression& element = hir_.storage.expression(element_id);
        MirValueId value = require_value(lower_expression(element_id),
                                         element.type, element.range);
        elements.push_back(coerce(value, array->element_type, element.range));
      }
      return emit_value(
          expression.type, expression.range,
          MirArrayLiteralInstruction{array->element_type, std::move(elements)});
    }
    if (const auto* index = std::get_if<HirIndexExpression>(&expression.data)) {
      const HirExpression& object = hir_.storage.expression(index->object);
      const HirExpression& subscript = hir_.storage.expression(index->index);
      const MirValueId array = require_value(lower_expression(index->object),
                                             object.type, object.range);
      MirValueId lowered_index = require_value(lower_expression(index->index),
                                               subscript.type, subscript.range);
      lowered_index = coerce(lowered_index, *semantics_.find_type("int32"),
                             subscript.range);
      const SemanticType& array_type = semantics_.type(object.type);
      if (array_type.kind != TypeKind::kArray || !array_type.element_type) {
        return invalid_value(expression.range);
      }
      return emit_value(expression.type, expression.range,
                        MirArrayLoadInstruction{array, lowered_index});
    }
    if (const auto* length =
            std::get_if<HirArrayLengthExpression>(&expression.data)) {
      const HirExpression& array = hir_.storage.expression(length->array);
      const MirValueId value = require_value(lower_expression(length->array),
                                             array.type, array.range);
      return emit_value(expression.type, expression.range,
                        MirArrayLengthInstruction{value});
    }
    if (const auto* meta =
            std::get_if<HirStringMetaExpression>(&expression.data)) {
      const HirExpression& string = hir_.storage.expression(meta->string);
      const MirValueId value = require_value(lower_expression(meta->string),
                                             string.type, string.range);
      return emit_value(expression.type, expression.range,
                        MirStringMetaInstruction{value, meta->query});
    }
    if (const auto* meta =
            std::get_if<HirObjectMetaExpression>(&expression.data)) {
      const HirExpression& object = hir_.storage.expression(meta->object);
      const MirValueId value = require_value(lower_expression(meta->object),
                                             object.type, object.range);
      return emit_value(expression.type, expression.range,
                        MirObjectMetaInstruction{value});
    }
    const auto& grouped = std::get<HirGroupedExpression>(expression.data);
    return lower_expression(grouped.expression);
  }

  MirValueId lower_condition(HirExpressionId id, bool is_presence_test) {
    const HirExpression& expression = hir_.storage.expression(id);
    const MirValueId value =
        require_value(lower_expression(id), expression.type, expression.range);
    if (!is_presence_test) {
      if (expression.type == semantics_.bool_type() ||
          expression.type == semantics_.error_type()) {
        return value;
      }
      return invalid_value(expression.range);
    }
    return emit_value(semantics_.bool_type(), expression.range,
                      MirIsNonNullInstruction{value});
  }

  std::optional<MirValueId> lower_safe_member(
      const HirSafeMemberExpression& member, const HirExpression& expression) {
    const HirExpression& object_syntax = hir_.storage.expression(member.object);
    const MirValueId object =
        require_value(lower_expression(member.object), object_syntax.type,
                      object_syntax.range);
    if (!member.member) {
      return invalid_value(expression.range);
    }
    const SemanticType& nullable_type = semantics_.type(object_syntax.type);
    if (nullable_type.kind != TypeKind::kNullable ||
        !nullable_type.element_type) {
      return invalid_value(expression.range);
    }

    const MirValueId has_object =
        emit_value(semantics_.bool_type(), object_syntax.range,
                   MirIsNonNullInstruction{object});
    const bool branch_reachable = current_is_reachable();
    const MirBlockId member_block = add_block(branch_reachable);
    const MirBlockId null_block = add_block(branch_reachable);
    const MirBlockId merge_block = add_block(branch_reachable);
    terminate(MirBranchTerminator{has_object, member_block, null_block},
              expression.range);

    current_block_ = member_block;
    const MirValueId narrowed_object = emit_value(
        *nullable_type.element_type, object_syntax.range,
        MirConvertInstruction{object, MirConversionKind::kFromNullable});
    const SemanticSymbol& symbol = semantics_.symbol(*member.member);
    MirValueId loaded =
        emit_value(symbol.type, expression.range,
                   MirLoadMemberInstruction{narrowed_object, *member.member});
    loaded = coerce(loaded, expression.type, expression.range);
    const MirBlockId member_end = *current_block_;
    jump_to(merge_block, expression.range);

    current_block_ = null_block;
    MirValueId null_value =
        emit_value(semantics_.null_type(), expression.range,
                   MirLiteralInstruction{LiteralKind::kNull, "null"});
    null_value = coerce(null_value, expression.type, expression.range);
    const MirBlockId null_end = *current_block_;
    jump_to(merge_block, expression.range);

    current_block_ = merge_block;
    std::vector<MirPhiIncoming> incoming;
    incoming.push_back(MirPhiIncoming{member_end, loaded});
    incoming.push_back(MirPhiIncoming{null_end, null_value});
    return emit_value(expression.type, expression.range,
                      MirPhiInstruction{std::move(incoming)});
  }

  std::optional<MirValueId> lower_null_coalesce(
      const HirNullCoalesceExpression& coalesce_expression,
      const HirExpression& expression) {
    const HirExpression& nullable_syntax =
        hir_.storage.expression(coalesce_expression.nullable);
    const MirValueId nullable =
        require_value(lower_expression(coalesce_expression.nullable),
                      nullable_syntax.type, nullable_syntax.range);
    const SemanticType& nullable_type = semantics_.type(nullable_syntax.type);
    if (nullable_type.kind != TypeKind::kNullable ||
        !nullable_type.element_type) {
      return invalid_value(expression.range);
    }

    const MirValueId has_value =
        emit_value(semantics_.bool_type(), nullable_syntax.range,
                   MirIsNonNullInstruction{nullable});
    const bool branch_reachable = current_is_reachable();
    const MirBlockId value_block = add_block(branch_reachable);
    const MirBlockId fallback_block = add_block(branch_reachable);
    const MirBlockId merge_block = add_block(branch_reachable);
    terminate(MirBranchTerminator{has_value, value_block, fallback_block},
              expression.range);

    current_block_ = value_block;
    MirValueId value = emit_value(
        *nullable_type.element_type, nullable_syntax.range,
        MirConvertInstruction{nullable, MirConversionKind::kFromNullable});
    value = coerce(value, expression.type, expression.range);
    const MirBlockId value_end = *current_block_;
    jump_to(merge_block, expression.range);

    current_block_ = fallback_block;
    const HirExpression& fallback_syntax =
        hir_.storage.expression(coalesce_expression.fallback);
    MirValueId fallback =
        require_value(lower_expression(coalesce_expression.fallback),
                      fallback_syntax.type, fallback_syntax.range);
    fallback = coerce(fallback, expression.type, fallback_syntax.range);
    const MirBlockId fallback_end = *current_block_;
    jump_to(merge_block, expression.range);

    current_block_ = merge_block;
    std::vector<MirPhiIncoming> incoming;
    incoming.push_back(MirPhiIncoming{value_end, value});
    incoming.push_back(MirPhiIncoming{fallback_end, fallback});
    return emit_value(expression.type, expression.range,
                      MirPhiInstruction{std::move(incoming)});
  }

  std::optional<MirValueId> lower_null_assert(
      const HirNullAssertExpression& assertion,
      const HirExpression& expression) {
    if (expression.type == semantics_.error_type()) {
      return invalid_value(expression.range);
    }
    const HirExpression& operand_syntax =
        hir_.storage.expression(assertion.operand);
    const MirValueId operand =
        require_value(lower_expression(assertion.operand), operand_syntax.type,
                      operand_syntax.range);
    return emit_value(expression.type, expression.range,
                      MirNullAssertInstruction{operand});
  }

  std::optional<MirValueId> lower_symbol(
      const HirSymbolExpression& symbol_expression,
      const HirExpression& expression) {
    const SemanticSymbol& symbol = semantics_.symbol(symbol_expression.symbol);
    if (symbol.kind == SymbolKind::kField) {
      if (symbol.is_static) {
        return emit_value(expression.type, expression.range,
                          MirLoadSymbolInstruction{symbol_expression.symbol});
      }
      if (semantics_.file(file_).kind == FileTypeKind::kStruct) {
        return emit_value(expression.type, expression.range,
                          MirLoadStorageInstruction{
                              MirStoragePath{semantics_.file(file_).self_symbol,
                                             std::nullopt,
                                             std::nullopt,
                                             {symbol_expression.symbol}}});
      }
      const MirValueId self = emit_self(expression.range);
      return emit_value(
          expression.type, expression.range,
          MirLoadMemberInstruction{self, symbol_expression.symbol});
    }
    if (symbol.kind == SymbolKind::kParameter ||
        symbol.kind == SymbolKind::kLocal || symbol.kind == SymbolKind::kSelf) {
      const MirValueId loaded =
          emit_value(symbol.type, expression.range,
                     MirLoadSymbolInstruction{symbol_expression.symbol});
      const SemanticType& declared = semantics_.type(symbol.type);
      if (declared.kind == TypeKind::kNullable &&
          declared.element_type == expression.type) {
        return emit_value(
            expression.type, expression.range,
            MirConvertInstruction{loaded, MirConversionKind::kFromNullable});
      }
      return loaded;
    }
    return invalid_value(expression.range);
  }

  std::optional<MirValueId> lower_member(const HirMemberExpression& member,
                                         const HirExpression& expression) {
    if (member.member) {
      const SemanticSymbol& symbol = semantics_.symbol(*member.member);
      if (symbol.kind == SymbolKind::kField && symbol.is_static) {
        return emit_value(expression.type, expression.range,
                          MirLoadSymbolInstruction{*member.member});
      }
    }
    const HirExpression& receiver = hir_.storage.expression(member.object);
    if (member.member &&
        semantics_.type(receiver.type).kind == TypeKind::kStruct &&
        receiver.category != ValueCategory::kValue) {
      MirStoragePath path = storage_path(lower_location(member.object));
      path.fields.push_back(*member.member);
      return emit_value(expression.type, expression.range,
                        MirLoadStorageInstruction{std::move(path)});
    }
    const MirValueId object = require_value(
        lower_expression(member.object),
        hir_.storage.expression(member.object).type, expression.range);
    if (!member.member) {
      return invalid_value(expression.range);
    }
    const SemanticSymbol& symbol = semantics_.symbol(*member.member);
    if (symbol.kind != SymbolKind::kField) {
      return invalid_value(expression.range);
    }
    return emit_value(expression.type, expression.range,
                      MirLoadMemberInstruction{object, *member.member});
  }

  std::optional<MirValueId> lower_assignment(
      const HirAssignmentExpression& assignment,
      const HirExpression& expression) {
    const LoweredLocation location = lower_location(assignment.target);
    const HirExpression& value_syntax =
        hir_.storage.expression(assignment.value);
    MirValueId value = require_value(lower_expression(assignment.value),
                                     value_syntax.type, value_syntax.range);
    if (assignment.operation == TokenKind::kEqual) {
      value = coerce(value, location.type, value_syntax.range);
    } else {
      const std::optional<TokenKind> operation =
          compound_binary_operation(assignment.operation);
      if (!operation) {
        emit_void(expression.range, MirInvalidInstruction{});
        return invalid_value(expression.range);
      }
      const MirValueId current = load_location(location, expression.range);
      if (*operation != TokenKind::kShiftLeft &&
          *operation != TokenKind::kShiftRight) {
        value = coerce(value, location.type, value_syntax.range);
      }
      value = emit_value(location.type, expression.range,
                         MirBinaryInstruction{current, *operation, value});
    }
    store_location(location, value, expression.range);
    return value;
  }

  std::optional<MirValueId> lower_update(const HirUpdateExpression& update,
                                         const HirExpression& expression) {
    const LoweredLocation location = lower_location(update.operand);
    const MirValueId previous = load_location(location, expression.range);
    const TypeKind kind = semantics_.type(location.type).kind;
    const bool is_float =
        kind == TypeKind::kFloat32 || kind == TypeKind::kFloat64;
    const MirValueId one =
        emit_value(location.type, expression.range,
                   MirLiteralInstruction{
                       is_float ? LiteralKind::kFloat : LiteralKind::kInteger,
                       is_float ? "1.0" : "1"});
    const TokenKind operation = update.operation == TokenKind::kPlusPlus
                                    ? TokenKind::kPlus
                                    : TokenKind::kMinus;
    const MirValueId updated =
        emit_value(location.type, expression.range,
                   MirBinaryInstruction{previous, operation, one});
    store_location(location, updated, expression.range);
    return update.is_postfix ? previous : updated;
  }

  static MirStoragePath storage_path(const LoweredLocation& location) {
    if (location.path) return *location.path;
    if (location.is_member) {
      return {std::nullopt, location.object, std::nullopt,
              location.symbol ? std::vector<SymbolId>{*location.symbol}
                              : std::vector<SymbolId>{}};
    }
    return {location.symbol, location.object, location.index, {}};
  }

  MirValueId load_location(const LoweredLocation& location, SourceRange range) {
    if (location.path) {
      return emit_value(location.type, range,
                        MirLoadStorageInstruction{*location.path});
    }
    if (location.is_array_element && location.object && location.index) {
      return emit_value(
          location.type, range,
          MirArrayLoadInstruction{*location.object, *location.index});
    }
    if (!location.symbol) {
      return invalid_value(range);
    }
    if (location.is_member && location.object) {
      return emit_value(
          location.type, range,
          MirLoadMemberInstruction{*location.object, *location.symbol});
    }
    return emit_value(location.type, range,
                      MirLoadSymbolInstruction{*location.symbol});
  }

  void store_location(const LoweredLocation& location, MirValueId value,
                      SourceRange range) {
    if (location.path) {
      emit_void(range, MirStoreStorageInstruction{*location.path, value});
      return;
    }
    if (location.is_array_element && location.object && location.index) {
      emit_void(range, MirArrayStoreInstruction{*location.object,
                                                *location.index, value});
      return;
    }
    if (!location.symbol) {
      emit_void(range, MirInvalidInstruction{});
      return;
    }
    if (location.is_member && location.object) {
      emit_void(range, MirStoreMemberInstruction{*location.object,
                                                 *location.symbol, value});
      return;
    }
    emit_void(range, MirStoreSymbolInstruction{*location.symbol, value});
  }

  static std::optional<TokenKind> compound_binary_operation(
      TokenKind operation) {
    switch (operation) {
      case TokenKind::kPlusEqual:
        return TokenKind::kPlus;
      case TokenKind::kMinusEqual:
        return TokenKind::kMinus;
      case TokenKind::kStarEqual:
        return TokenKind::kStar;
      case TokenKind::kSlashEqual:
        return TokenKind::kSlash;
      case TokenKind::kPercentEqual:
        return TokenKind::kPercent;
      case TokenKind::kShiftLeftEqual:
        return TokenKind::kShiftLeft;
      case TokenKind::kShiftRightEqual:
        return TokenKind::kShiftRight;
      case TokenKind::kAmpersandEqual:
        return TokenKind::kAmpersand;
      case TokenKind::kPipeEqual:
        return TokenKind::kPipe;
      case TokenKind::kCaretEqual:
        return TokenKind::kCaret;
      default:
        return std::nullopt;
    }
  }

  LoweredLocation lower_location(HirExpressionId id) {
    const HirExpression& expression = hir_.storage.expression(id);
    if (const auto* grouped =
            std::get_if<HirGroupedExpression>(&expression.data)) {
      return lower_location(grouped->expression);
    }
    if (const auto* symbol_expression =
            std::get_if<HirSymbolExpression>(&expression.data)) {
      const SemanticSymbol& symbol =
          semantics_.symbol(symbol_expression->symbol);
      if (symbol.kind == SymbolKind::kField) {
        if (symbol.is_static) {
          return LoweredLocation{symbol_expression->symbol,
                                 std::nullopt,
                                 std::nullopt,
                                 symbol.type,
                                 false,
                                 false};
        }
        if (semantics_.file(file_).kind == FileTypeKind::kStruct) {
          return LoweredLocation{
              std::nullopt,
              std::nullopt,
              std::nullopt,
              symbol.type,
              false,
              false,
              MirStoragePath{semantics_.file(file_).self_symbol,
                             std::nullopt,
                             std::nullopt,
                             {symbol_expression->symbol}}};
        }
        return LoweredLocation{symbol_expression->symbol,
                               emit_self(expression.range),
                               std::nullopt,
                               symbol.type,
                               true,
                               false};
      }
      if (symbol.kind == SymbolKind::kParameter ||
          symbol.kind == SymbolKind::kLocal ||
          symbol.kind == SymbolKind::kSelf) {
        return LoweredLocation{symbol_expression->symbol,
                               std::nullopt,
                               std::nullopt,
                               symbol.type,
                               false,
                               false};
      }
    }
    if (const auto* member =
            std::get_if<HirMemberExpression>(&expression.data)) {
      if (member->member) {
        const SemanticSymbol& symbol = semantics_.symbol(*member->member);
        if (symbol.kind == SymbolKind::kField && symbol.is_static) {
          return LoweredLocation{member->member, std::nullopt, std::nullopt,
                                 symbol.type,    false,        false};
        }
      }
      if (member->member &&
          semantics_.type(hir_.storage.expression(member->object).type).kind ==
              TypeKind::kStruct) {
        MirStoragePath path = storage_path(lower_location(member->object));
        path.fields.push_back(*member->member);
        return LoweredLocation{std::nullopt,    std::nullopt, std::nullopt,
                               expression.type, false,        false,
                               std::move(path)};
      }
      const MirValueId object = require_value(
          lower_expression(member->object),
          hir_.storage.expression(member->object).type, expression.range);
      if (member->member) {
        return LoweredLocation{
            member->member, object,
            std::nullopt,   semantics_.symbol(*member->member).type,
            true,           false};
      }
      return LoweredLocation{std::nullopt, std::nullopt,
                             std::nullopt, semantics_.error_type(),
                             false,        false};
    }
    if (const auto* index = std::get_if<HirIndexExpression>(&expression.data)) {
      const HirExpression& object = hir_.storage.expression(index->object);
      const HirExpression& subscript = hir_.storage.expression(index->index);
      const MirValueId array = require_value(lower_expression(index->object),
                                             object.type, object.range);
      MirValueId lowered_index = require_value(lower_expression(index->index),
                                               subscript.type, subscript.range);
      lowered_index = coerce(lowered_index, *semantics_.find_type("int32"),
                             subscript.range);
      const SemanticType& array_type = semantics_.type(object.type);
      if (array_type.kind != TypeKind::kArray || !array_type.element_type) {
        return LoweredLocation{std::nullopt, std::nullopt,
                               std::nullopt, semantics_.error_type(),
                               false,        false};
      }
      return LoweredLocation{std::nullopt,    array, lowered_index,
                             expression.type, false, true};
    }
    static_cast<void>(lower_expression(id));
    return LoweredLocation{std::nullopt, std::nullopt,
                           std::nullopt, semantics_.error_type(),
                           false,        false};
  }

  std::optional<MirValueId> lower_call(const HirCallExpression& call,
                                       const HirExpression& expression) {
    MirCallKind kind = MirCallKind::kUnqualified;
    std::optional<MirValueId> receiver;
    bool receiver_is_self = false;
    const HirExpression& callee = hir_.storage.expression(call.callee);
    if (call.is_base_qualified) {
      kind = MirCallKind::kBaseQualified;
      receiver_is_self = true;
    } else if (const auto* member =
                   std::get_if<HirMemberExpression>(&callee.data)) {
      const HirExpression& object = hir_.storage.expression(member->object);
      if (std::holds_alternative<HirTypeExpression>(object.data)) {
        kind = MirCallKind::kClassQualified;
      } else {
        kind = MirCallKind::kInstance;
        receiver_is_self = is_self_expression(member->object);
        receiver = require_value(lower_expression(member->object), object.type,
                                 object.range);
      }
    } else if (std::holds_alternative<HirTypeExpression>(callee.data)) {
      kind = MirCallKind::kConstructor;
    } else if (!std::holds_alternative<HirSymbolExpression>(callee.data) &&
               !std::holds_alternative<HirTypeExpression>(callee.data)) {
      static_cast<void>(lower_expression(call.callee));
    }

    if (call.struct_receiver == StructReceiverMode::kReadOnlyValue &&
        !receiver) {
      receiver = emit_self(expression.range);
      receiver_is_self = true;
    }
    std::vector<MirValueId> arguments;
    arguments.reserve(call.arguments.size());
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
      const HirExpressionId argument_id = call.arguments[index];
      const HirExpression& argument = hir_.storage.expression(argument_id);
      MirValueId value = require_value(lower_expression(argument_id),
                                       argument.type, argument.range);
      if (call.callable) {
        const auto& parameter_types =
            semantics_.symbol(*call.callable).parameter_types;
        if (index < parameter_types.size()) {
          value = coerce(value, parameter_types[index], argument.range);
        }
      }
      arguments.push_back(value);
    }

    if (!call.callable) {
      return invalid_value(expression.range);
    }
    const SemanticSymbol& callable = semantics_.symbol(*call.callable);
    receiver_is_self = receiver_is_self || (kind == MirCallKind::kUnqualified &&
                                            !callable.is_static);
    std::optional<std::size_t> interface_slot;
    if (call.interface_dispatch) {
      const std::vector<SymbolId>& functions =
          semantics_.file(*call.interface_dispatch).interface_functions;
      const auto slot = std::ranges::find(functions, *call.callable);
      if (slot != functions.end()) {
        interface_slot = static_cast<std::size_t>(slot - functions.begin());
      }
    }
    const MirDispatchKind dispatch =
        call.interface_dispatch ? MirDispatchKind::kInterface
        : callable.virtual_slot && !call.is_base_qualified &&
                !(suppress_self_virtual_dispatch_ && receiver_is_self)
            ? MirDispatchKind::kVirtual
            : MirDispatchKind::kDirect;
    MirCallInstruction instruction{kind,
                                   dispatch,
                                   receiver_is_self,
                                   *call.callable,
                                   receiver,
                                   std::move(arguments),
                                   call.interface_dispatch,
                                   interface_slot,
                                   call.struct_receiver};
    if (expression.type == semantics_.void_type()) {
      emit_void(expression.range, std::move(instruction));
      return std::nullopt;
    }
    return emit_value(expression.type, expression.range,
                      std::move(instruction));
  }

  std::optional<MirValueId> lower_short_circuit(
      const HirBinaryExpression& binary, const HirExpression& expression) {
    const MirValueId left =
        lower_condition(binary.left, binary.left_is_presence_test);
    const bool branch_reachable = current_is_reachable();
    const MirBlockId right_block = add_block(branch_reachable);
    const MirBlockId short_block = add_block(branch_reachable);
    const MirBlockId merge_block = add_block(branch_reachable);
    const bool is_and = binary.operation == TokenKind::kAmpersandAmpersand;
    const MirBlockId then_block = is_and ? right_block : short_block;
    const MirBlockId else_block = is_and ? short_block : right_block;
    terminate(MirBranchTerminator{left, then_block, else_block},
              expression.range);

    current_block_ = right_block;
    const MirValueId right =
        lower_condition(binary.right, binary.right_is_presence_test);
    const MirBlockId right_end = *current_block_;
    jump_to(merge_block, expression.range);

    current_block_ = short_block;
    const bool short_value = !is_and;
    const MirValueId short_result =
        emit_value(semantics_.bool_type(), expression.range,
                   MirLiteralInstruction{LiteralKind::kBoolean,
                                         short_value ? "true" : "false"});
    const MirBlockId short_end = *current_block_;
    jump_to(merge_block, expression.range);

    current_block_ = merge_block;
    std::vector<MirPhiIncoming> incoming;
    incoming.push_back(MirPhiIncoming{right_end, right});
    incoming.push_back(MirPhiIncoming{short_end, short_result});
    return emit_value(expression.type, expression.range,
                      MirPhiInstruction{std::move(incoming)});
  }

  bool is_self_expression(HirExpressionId id) const {
    const HirExpression& expression = hir_.storage.expression(id);
    if (const auto* symbol =
            std::get_if<HirSymbolExpression>(&expression.data)) {
      return symbol->symbol == semantics_.file(file_).self_symbol;
    }
    if (const auto* grouped =
            std::get_if<HirGroupedExpression>(&expression.data)) {
      return is_self_expression(grouped->expression);
    }
    return false;
  }

  MirValueId emit_self(SourceRange range) {
    const SymbolId self = semantics_.file(file_).self_symbol;
    return emit_value(semantics_.symbol(self).type, range,
                      MirLoadSymbolInstruction{self});
  }

  void finish_unterminated_blocks() {
    for (std::size_t index = 0; index < body_.blocks.size(); ++index) {
      if (!terminated_[index]) {
        body_.blocks[index].terminator =
            MirTerminator{body_.range, MirUnreachableTerminator{}};
        terminated_[index] = true;
      }
    }
    update_reachability();
    current_block_.reset();
  }

  void update_reachability() {
    for (MirBasicBlock& block : body_.blocks) {
      block.is_reachable = false;
    }
    std::vector<MirBlockId> worklist{body_.entry};
    while (!worklist.empty()) {
      const MirBlockId block_id = worklist.back();
      worklist.pop_back();
      MirBasicBlock& block = body_.blocks[block_id.value];
      if (block.is_reachable) {
        continue;
      }
      block.is_reachable = true;
      for (const auto successor : mir_successors(block.terminator))
        worklist.push_back(successor);
    }
  }

  const HirModule& hir_;
  const SemanticModel& semantics_;
  FileId file_;
  TypeId return_type_;
  bool suppress_self_virtual_dispatch_;
  MirBody body_;
  std::vector<TypeId> value_types_;
  std::vector<bool> terminated_;
  std::optional<MirBlockId> current_block_;
  std::vector<TransferTargets> transfer_targets_;
};

MirCallable lower_callable(const HirModule& hir, const SemanticModel& semantics,
                           FileId file, const HirCallable& callable) {
  const SemanticSymbol& symbol = semantics.symbol(callable.symbol);
  const SourceRange range = hir.storage.block(callable.body).range;
  const TypeId return_type = symbol.kind == SymbolKind::kConstructor
                                 ? semantics.void_type()
                                 : symbol.type;
  BodyBuilder builder{hir,         semantics,
                      file,        range,
                      return_type, symbol.kind == SymbolKind::kConstructor};
  MirBody body =
      symbol.is_abstract ? builder.lower_abstract()
      : symbol.kind == SymbolKind::kConstructor
          ? builder.lower_constructor(callable.initializer, callable.body)
          : builder.lower_callable(callable.body);
  return MirCallable{callable.symbol, symbol.parameter_symbols, std::move(body),
                     callable.struct_receiver};
}

}  // namespace

std::vector<MirBlockId> mir_successors(const MirTerminator& terminator) {
  if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator.data))
    return {jump->target};
  std::vector<MirBlockId> result;
  if (const auto* branch = std::get_if<MirBranchTerminator>(&terminator.data)) {
    result = {branch->then_block, branch->else_block};
  } else if (const auto* selection =
                 std::get_if<MirSwitchTerminator>(&terminator.data)) {
    result.reserve(selection->cases.size() + 2);
    for (const auto& entry : selection->cases) result.push_back(entry.target);
    result.push_back(selection->default_block);
    if (selection->invalid_block) result.push_back(*selection->invalid_block);
  }
  std::ranges::sort(result, {}, &MirBlockId::value);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

MirModule lower_to_mir(const HirModule& hir, const SemanticModel& semantics) {
  MirModule module;
  module.files.reserve(hir.files.size());
  for (const HirFileClass& hir_file : hir.files) {
    MirFileClass file{
        hir_file.file,        hir_file.symbol, hir_file.base_file, {}, {}, {},
        hir_file.member_order};
    file.fields.reserve(hir_file.fields.size());
    for (const HirField& hir_field : hir_file.fields) {
      std::optional<MirBody> initializer;
      if (hir_field.initializer &&
          !semantics.symbol(hir_field.symbol).is_static) {
        const SourceRange range =
            hir.storage.expression(*hir_field.initializer).range;
        initializer =
            BodyBuilder{hir,
                        semantics,
                        hir_file.file,
                        range,
                        semantics.symbol(hir_field.symbol).type,
                        true}
                .lower_initializer(*hir_field.initializer,
                                   semantics.symbol(hir_field.symbol).type);
      }
      file.fields.push_back(MirField{hir_field.symbol, std::move(initializer),
                                     hir_field.static_constant});
    }
    file.functions.reserve(hir_file.functions.size());
    for (const HirCallable& function : hir_file.functions) {
      file.functions.push_back(
          lower_callable(hir, semantics, hir_file.file, function));
    }
    file.constructors.reserve(hir_file.constructors.size());
    for (const HirCallable& constructor : hir_file.constructors) {
      file.constructors.push_back(
          lower_callable(hir, semantics, hir_file.file, constructor));
    }
    module.files.push_back(std::move(file));
  }
  return module;
}

}  // namespace cloth
