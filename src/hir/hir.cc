// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/hir/hir.h"

#include "cloth/lexer/literal.h"

#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

std::string hir_literal_lexeme(const LiteralExpression& literal) {
  if (literal.kind != LiteralKind::kInteger &&
      literal.kind != LiteralKind::kFloat) {
    return std::string{literal.lexeme};
  }
  const NumericLiteralSpelling spelling =
      parse_numeric_literal_spelling(literal.lexeme);
  if (spelling.error == NumericLiteralSpellingError::kNone) {
    return spelling.core;
  }
  return std::string{literal.lexeme};
}

}  // namespace

StructReceiverMode struct_receiver_mode(const SemanticSymbol& callable,
                                        const SemanticModel& semantics) {
  if (!callable.file || callable.is_static ||
      semantics.file(*callable.file).kind != FileTypeKind::kStruct) {
    return StructReceiverMode::kNone;
  }
  return callable.kind == SymbolKind::kConstructor
             ? StructReceiverMode::kConstruction
             : StructReceiverMode::kReadOnlyValue;
}

HirExpressionId HirStorage::add_expression(HirExpression expression) {
  const HirExpressionId id{expressions_.size()};
  expressions_.push_back(std::move(expression));
  return id;
}

HirStatementId HirStorage::add_statement(HirStatement statement) {
  const HirStatementId id{statements_.size()};
  statements_.push_back(std::move(statement));
  return id;
}

HirBlockId HirStorage::add_block(HirBlock block) {
  const HirBlockId id{blocks_.size()};
  blocks_.push_back(std::move(block));
  return id;
}

const HirExpression& HirStorage::expression(HirExpressionId id) const {
  return expressions_.at(id.value);
}

const HirStatement& HirStorage::statement(HirStatementId id) const {
  return statements_.at(id.value);
}

const HirBlock& HirStorage::block(HirBlockId id) const {
  return blocks_.at(id.value);
}

std::span<const HirExpression> HirStorage::expressions() const noexcept {
  return expressions_;
}

std::span<const HirStatement> HirStorage::statements() const noexcept {
  return statements_;
}

std::span<const HirBlock> HirStorage::blocks() const noexcept {
  return blocks_;
}

namespace {

class Lowerer {
 public:
  Lowerer(std::span<const FileClassDecl* const> files,
          const SemanticModel& semantics)
      : files_(files), semantics_(semantics) {}

  HirModule run() {
    for (std::size_t index = 0; index < files_.size(); ++index) {
      lower_file(FileId{index});
    }
    return std::move(module_);
  }

 private:
  void lower_file(FileId file_id) {
    current_file_ = file_id;
    expression_base_ = module_.storage.expressions().size();
    statement_base_ = module_.storage.statements().size();
    block_base_ = module_.storage.blocks().size();
    const FileClassDecl& syntax = *files_[file_id.value];

    for (std::size_t index = 0; index < syntax.storage.expressions().size();
         ++index) {
      lower_expression(ExpressionId{index});
    }
    for (std::size_t index = 0; index < syntax.storage.statements().size();
         ++index) {
      lower_statement(StatementId{index});
    }
    for (std::size_t index = 0; index < syntax.storage.blocks().size();
         ++index) {
      lower_block(BlockId{index});
    }

    const FileSemantics& semantic_file = semantics_.file(file_id);
    HirFileClass file{
        file_id, semantic_file.symbol, semantic_file.base_file, {}, {},
        {},      syntax.member_order};
    file.fields.reserve(syntax.fields.size());
    for (std::size_t index = 0; index < syntax.fields.size(); ++index) {
      const FieldDecl& field = syntax.fields[index];
      file.fields.push_back(HirField{
          semantic_file.fields[index],
          field.initializer
              ? std::optional<HirExpressionId>{expression(*field.initializer)}
              : std::nullopt,
          semantics_.symbol(semantic_file.fields[index]).static_constant});
    }
    file.functions.reserve(syntax.functions.size());
    for (std::size_t index = 0; index < syntax.functions.size(); ++index) {
      file.functions.push_back(HirCallable{
          semantic_file.functions[index], std::nullopt,
          block(syntax.functions[index].body),
          struct_receiver_mode(
              semantics_.symbol(semantic_file.functions[index]), semantics_)});
    }
    file.constructors.reserve(syntax.constructors.size());
    for (std::size_t index = 0; index < syntax.constructors.size(); ++index) {
      const ConstructorDecl& constructor = syntax.constructors[index];
      std::optional<HirConstructorInitializer> initializer;
      const SemanticSymbol& symbol =
          semantics_.symbol(semantic_file.constructors[index]);
      if (symbol.base_constructor) {
        std::vector<HirExpressionId> arguments;
        SourceRange range = constructor.range;
        if (constructor.initializer) {
          arguments.reserve(constructor.initializer->arguments.size());
          for (const ExpressionId argument :
               constructor.initializer->arguments) {
            arguments.push_back(expression(argument));
          }
          range = constructor.initializer->range;
        }
        initializer = HirConstructorInitializer{*symbol.base_constructor,
                                                std::move(arguments), range};
      }
      file.constructors.push_back(HirCallable{
          semantic_file.constructors[index], std::move(initializer),
          block(constructor.body), struct_receiver_mode(symbol, semantics_)});
    }
    module_.files.push_back(std::move(file));
  }

  void lower_expression(ExpressionId id) {
    const Expression& syntax =
        files_[current_file_.value]->storage.expression(id);
    const ExpressionSemantics& semantic =
        semantics_.file(current_file_).expressions.at(id.value);
    HirExpressionData data = HirInvalidExpression{};

    if (const auto* literal = std::get_if<LiteralExpression>(&syntax.data)) {
      const NumericLiteralSpelling spelling =
          parse_numeric_literal_spelling(literal->lexeme);
      if ((literal->kind != LiteralKind::kInteger &&
           literal->kind != LiteralKind::kFloat) ||
          (spelling.error == NumericLiteralSpellingError::kNone &&
           semantic.type != semantics_.error_type())) {
        data =
            HirLiteralExpression{literal->kind, hir_literal_lexeme(*literal)};
      }
    } else if (std::holds_alternative<IdentifierExpression>(syntax.data)) {
      if (semantic.category == ValueCategory::kType) {
        data = HirTypeExpression{semantic.type};
      } else if (semantic.symbol) {
        data = HirSymbolExpression{*semantic.symbol};
      }
    } else if (std::holds_alternative<SuperExpression>(syntax.data)) {
      data = HirSuperExpression{};
    } else if (const auto* thrown =
                   std::get_if<ThrowExpression>(&syntax.data)) {
      const TypeId error_type = semantics_.file(current_file_)
                                    .expressions.at(thrown->operand.value)
                                    .type;
      if (semantic.type == semantics_.bottom_type() &&
          error_type.value < semantics_.types().size() &&
          semantics_.type(error_type).kind == TypeKind::kErrorClass) {
        data = HirThrowExpression{expression(thrown->operand), error_type};
      }
    } else if (const auto* unary = std::get_if<UnaryExpression>(&syntax.data)) {
      const ExpressionSemantics& operand =
          semantics_.file(current_file_).expressions.at(unary->operand.value);
      if (semantic.type != semantics_.error_type() &&
          operand.type != semantics_.error_type()) {
        data = HirUnaryExpression{unary->operation, expression(unary->operand),
                                  operand.is_presence_test};
      }
    } else if (const auto* update =
                   std::get_if<UpdateExpression>(&syntax.data)) {
      data = HirUpdateExpression{update->operation, expression(update->operand),
                                 update->is_postfix};
    } else if (const auto* binary =
                   std::get_if<BinaryExpression>(&syntax.data)) {
      const FileSemantics& file = semantics_.file(current_file_);
      data = HirBinaryExpression{
          expression(binary->left),
          binary->operation,
          expression(binary->right),
          file.expressions.at(binary->left.value).is_presence_test,
          file.expressions.at(binary->right.value).is_presence_test,
          semantic.may_divide_by_zero};
    } else if (const auto* test =
                   std::get_if<TypeTestExpression>(&syntax.data)) {
      if (semantic.type != semantics_.error_type() && semantic.checked_type) {
        data = HirTypeTestExpression{expression(test->value),
                                     *semantic.checked_type};
      }
    } else if (const auto* cast =
                   std::get_if<CheckedCastExpression>(&syntax.data)) {
      if (semantic.type != semantics_.error_type() && semantic.checked_type) {
        data = HirCheckedCastExpression{expression(cast->value),
                                        *semantic.checked_type};
      }
    } else if (const auto* conversion =
                   std::get_if<NumericConversionExpression>(&syntax.data)) {
      if (semantic.type != semantics_.error_type()) {
        data = HirNumericConversionExpression{expression(conversion->value)};
      }
    } else if (const auto* conversion =
                   std::get_if<IntegerConversionExpression>(&syntax.data)) {
      if (semantic.type != semantics_.error_type()) {
        const IntegerConversionMode mode = conversion->operation == "wrap"
                                               ? IntegerConversionMode::kWrap
                                               : IntegerConversionMode::kSat;
        data =
            HirIntegerConversionExpression{expression(conversion->value), mode};
      }
    } else if (const auto* assignment =
                   std::get_if<AssignmentExpression>(&syntax.data)) {
      data = HirAssignmentExpression{
          expression(assignment->target), assignment->operation,
          expression(assignment->value), semantic.may_divide_by_zero};
    } else if (const auto* member =
                   std::get_if<MemberAccessExpression>(&syntax.data)) {
      if (semantic.symbol &&
          semantics_.symbol(*semantic.symbol).kind == SymbolKind::kEnumCase) {
        data = HirLiteralExpression{
            LiteralKind::kEnum,
            std::to_string(*semantics_.symbol(*semantic.symbol).enum_tag)};
      } else {
        data = HirMemberExpression{expression(member->object), semantic.symbol};
      }
    } else if (const auto* meta =
                   std::get_if<MetaAccessExpression>(&syntax.data)) {
      const TypeId object_type = semantics_.file(current_file_)
                                     .expressions.at(meta->object.value)
                                     .type;
      if (semantic.integer_meta_operation) {
        data = HirIntegerMetaExpression{expression(meta->object),
                                        *semantic.integer_meta_operation};
      } else if (meta->meta == "typeName" &&
                 semantic.type == semantics_.string_type()) {
        data = HirObjectMetaExpression{expression(meta->object)};
      } else if (semantics_.type(object_type).kind == TypeKind::kArray &&
                 meta->meta == "length" &&
                 semantic.type != semantics_.error_type()) {
        data = HirArrayLengthExpression{expression(meta->object)};
      } else if (semantics_.type(object_type).kind == TypeKind::kString &&
                 semantic.type != semantics_.error_type()) {
        StringMetaQuery query = StringMetaQuery::kLength;
        if (meta->meta == "byteLength") {
          query = StringMetaQuery::kByteLength;
        } else if (meta->meta == "isEmpty") {
          query = StringMetaQuery::kIsEmpty;
        }
        data = HirStringMetaExpression{expression(meta->object), query};
      }
    } else if (const auto* member =
                   std::get_if<SafeMemberAccessExpression>(&syntax.data)) {
      data =
          HirSafeMemberExpression{expression(member->object), semantic.symbol};
    } else if (const auto* coalesce =
                   std::get_if<NullCoalesceExpression>(&syntax.data)) {
      data = HirNullCoalesceExpression{expression(coalesce->nullable),
                                       expression(coalesce->fallback)};
    } else if (const auto* assertion =
                   std::get_if<NullAssertExpression>(&syntax.data)) {
      data = HirNullAssertExpression{expression(assertion->operand)};
    } else if (const auto* call = std::get_if<CallExpression>(&syntax.data)) {
      std::vector<HirExpressionId> arguments;
      arguments.reserve(call->arguments.size());
      for (const ExpressionId argument : call->arguments) {
        arguments.push_back(expression(argument));
      }
      if (semantic.integer_meta_operation) {
        const Expression& callee =
            files_[current_file_.value]->storage.expression(call->callee);
        if (const auto* meta =
                std::get_if<MetaAccessExpression>(&callee.data)) {
          data = HirIntegerMetaCallExpression{expression(meta->object),
                                              std::move(arguments),
                                              *semantic.integer_meta_operation};
        }
      } else {
        const bool is_base_qualified = semantics_.file(current_file_)
                                           .expressions.at(call->callee.value)
                                           .is_base_qualified;
        data = HirCallExpression{expression(call->callee), semantic.symbol,
                                 std::move(arguments), is_base_qualified,
                                 semantic.interface_dispatch};
      }
    } else if (const auto* array =
                   std::get_if<ArrayLiteralExpression>(&syntax.data)) {
      std::vector<HirExpressionId> elements;
      elements.reserve(array->elements.size());
      for (const ExpressionId element : array->elements) {
        elements.push_back(expression(element));
      }
      const SemanticType& type = semantics_.type(semantic.type);
      data = HirArrayLiteralExpression{
          type.element_type.value_or(semantics_.error_type()),
          std::move(elements)};
    } else if (const auto* index = std::get_if<IndexExpression>(&syntax.data)) {
      data = HirIndexExpression{expression(index->object),
                                expression(index->index)};
    } else if (const auto* grouped =
                   std::get_if<ParenthesizedExpression>(&syntax.data)) {
      data = HirGroupedExpression{expression(grouped->expression)};
    }

    if (auto* call = std::get_if<HirCallExpression>(&data);
        call && call->callable) {
      call->struct_receiver =
          struct_receiver_mode(semantics_.symbol(*call->callable), semantics_);
    }
    static_cast<void>(module_.storage.add_expression(HirExpression{
        semantic.type, syntax.range, std::move(data), semantic.category}));
  }

  void lower_statement(StatementId id) {
    const Statement& syntax =
        files_[current_file_.value]->storage.statement(id);
    HirStatementData data = HirInvalidStatement{};
    if (const auto* local = std::get_if<LocalVariableStatement>(&syntax.data)) {
      data = HirLocalStatement{
          semantics_.file(current_file_).statement_symbols.at(id.value),
          local->initializer
              ? std::optional<HirExpressionId>{expression(*local->initializer)}
              : std::nullopt};
    } else if (const auto* return_statement =
                   std::get_if<ReturnStatement>(&syntax.data)) {
      data = HirReturnStatement{return_statement->value
                                    ? std::optional<HirExpressionId>{expression(
                                          *return_statement->value)}
                                    : std::nullopt};
    } else if (const auto* expression_statement =
                   std::get_if<ExpressionStatement>(&syntax.data)) {
      data =
          HirExpressionStatement{expression(expression_statement->expression)};
    } else if (const auto* if_statement =
                   std::get_if<IfStatement>(&syntax.data)) {
      data = HirIfStatement{
          expression(if_statement->condition), block(if_statement->then_block),
          if_statement->else_block
              ? std::optional<HirBlockId>{block(*if_statement->else_block)}
              : std::nullopt,
          semantics_.file(current_file_)
              .expressions.at(if_statement->condition.value)
              .is_presence_test};
    } else if (const auto* while_statement =
                   std::get_if<WhileStatement>(&syntax.data)) {
      data = HirWhileStatement{
          expression(while_statement->condition), block(while_statement->body),
          semantics_.file(current_file_)
              .expressions.at(while_statement->condition.value)
              .is_presence_test};
    } else if (const auto* for_statement =
                   std::get_if<ForEachStatement>(&syntax.data)) {
      data = HirForEachStatement{
          semantics_.file(current_file_).statement_symbols.at(id.value),
          expression(for_statement->iterable), block(for_statement->body)};
    } else if (const auto* for_statement =
                   std::get_if<ForStatement>(&syntax.data)) {
      std::vector<HirExpressionId> updates;
      updates.reserve(for_statement->updates.size());
      for (const ExpressionId update : for_statement->updates) {
        updates.push_back(expression(update));
      }
      data = HirForStatement{
          for_statement->initializer ? std::optional<HirStatementId>{statement(
                                           *for_statement->initializer)}
                                     : std::nullopt,
          for_statement->condition ? std::optional<HirExpressionId>{expression(
                                         *for_statement->condition)}
                                   : std::nullopt,
          std::move(updates), block(for_statement->body),
          for_statement->condition &&
              semantics_.file(current_file_)
                  .expressions.at(for_statement->condition->value)
                  .is_presence_test};
    } else if (const auto* selection =
                   std::get_if<SwitchStatement>(&syntax.data)) {
      const auto& switches = semantics_.file(current_file_).switches;
      const auto checked = switches.find(id.value);
      HirSwitchStatement lowered{
          expression(selection->selector), semantics_.error_type(), {}, false};
      if (checked != switches.end()) {
        lowered.selector_type = checked->second.selector_type;
        lowered.is_exhaustive = checked->second.is_exhaustive;
      }
      for (std::size_t index = 0; index < selection->arms.size(); ++index) {
        const auto& arm = selection->arms[index];
        std::vector<SwitchLabel> labels;
        if (checked != switches.end() && index < checked->second.labels.size())
          labels = checked->second.labels[index];
        lowered.arms.push_back(HirSwitchArm{std::move(labels), block(arm.body),
                                            arm.range, arm.labels.empty()});
      }
      data = std::move(lowered);
    } else if (std::holds_alternative<BreakStatement>(syntax.data)) {
      data = HirBreakStatement{};
    } else if (std::holds_alternative<ContinueStatement>(syntax.data)) {
      data = HirContinueStatement{};
    } else if (const auto* nested =
                   std::get_if<NestedBlockStatement>(&syntax.data)) {
      data = HirNestedBlockStatement{block(nested->block)};
    }
    static_cast<void>(module_.storage.add_statement(
        HirStatement{syntax.range, std::move(data)}));
  }

  void lower_block(BlockId id) {
    const Block& syntax = files_[current_file_.value]->storage.block(id);
    std::vector<HirStatementId> statements;
    statements.reserve(syntax.statements.size());
    for (const StatementId statement_id : syntax.statements) {
      statements.push_back(statement(statement_id));
    }
    static_cast<void>(module_.storage.add_block(
        HirBlock{syntax.range, std::move(statements)}));
  }

  HirExpressionId expression(ExpressionId id) const noexcept {
    return HirExpressionId{expression_base_ + id.value};
  }

  HirStatementId statement(StatementId id) const noexcept {
    return HirStatementId{statement_base_ + id.value};
  }

  HirBlockId block(BlockId id) const noexcept {
    return HirBlockId{block_base_ + id.value};
  }

  std::span<const FileClassDecl* const> files_;
  const SemanticModel& semantics_;
  HirModule module_;
  FileId current_file_{0};
  std::size_t expression_base_{0};
  std::size_t statement_base_{0};
  std::size_t block_base_{0};
};

}  // namespace

HirModule lower_to_hir(std::span<const FileClassDecl* const> files,
                       const SemanticModel& semantics) {
  return Lowerer{files, semantics}.run();
}

}  // namespace cloth
