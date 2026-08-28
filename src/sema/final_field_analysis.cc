#include "cloth/sema/final_field_analysis.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

struct AssignmentCount {
  unsigned int minimum{0};
  unsigned int maximum{0};
};

struct FlowState {
  std::vector<AssignmentCount> assignments;
  bool can_fall_through{true};
};

class FinalFieldAnalyzer {
 public:
  FinalFieldAnalyzer(const FileClassDecl& file, const SemanticModel& semantics,
                     FileId file_id, DiagnosticEngine& diagnostics)
      : file_(file),
        semantics_(semantics),
        file_id_(file_id),
        diagnostics_(diagnostics) {}

  void run() {
    validate_initializers();
    if (file_.constructors.empty()) {
      validate_fields_without_constructor();
      return;
    }
    for (const ConstructorDecl& constructor : file_.constructors) {
      validate_constructor(constructor);
    }
  }

 private:
  FlowState begin_analysis(bool include_declaration_initializers) {
    final_fields_.clear();
    FlowState flow;
    for (std::size_t index = 0; index < file_.fields.size(); ++index) {
      const FieldDecl& field = file_.fields[index];
      if (!field.is_valid || !field.is_final || field.is_static) {
        continue;
      }
      final_fields_.push_back(semantics_.file(file_id_).fields[index]);
      const unsigned int initialized =
          include_declaration_initializers && field.initializer ? 1U : 0U;
      flow.assignments.push_back(AssignmentCount{initialized, initialized});
    }
    return flow;
  }

  void validate_initializers() {
    FlowState flow = begin_analysis(false);
    if (final_fields_.empty()) {
      return;
    }

    for (std::size_t index = 0; index < file_.fields.size(); ++index) {
      const FieldDecl& field = file_.fields[index];
      if (!field.initializer) {
        continue;
      }
      analyze_expression(*field.initializer, flow.assignments, false);
      if (!field.is_valid || !field.is_final || field.is_static) {
        continue;
      }
      const SymbolId symbol = semantics_.file(file_id_).fields[index];
      for (std::size_t final_index = 0; final_index < final_fields_.size();
           ++final_index) {
        if (final_fields_[final_index] == symbol) {
          flow.assignments[final_index] = AssignmentCount{1U, 1U};
          break;
        }
      }
    }
    final_fields_.clear();
  }

  void validate_fields_without_constructor() {
    for (const FieldDecl& field : file_.fields) {
      if (field.is_valid && field.is_final && !field.is_static &&
          !field.initializer) {
        diagnostics_.error(field.range, "final field '" +
                                            std::string{field.name} +
                                            "' requires an initializer or a "
                                            "constructor");
      }
    }
  }

  void validate_constructor(const ConstructorDecl& constructor) {
    FlowState flow = begin_analysis(true);
    if (final_fields_.empty()) {
      return;
    }

    flow = analyze_block(constructor.body, std::move(flow), false);
    if (flow.can_fall_through) {
      const Block& body = file_.storage.block(constructor.body);
      require_initialized(flow.assignments, point_range(body.range.end));
    }
    final_fields_.clear();
  }

  FlowState analyze_block(BlockId id, FlowState flow, bool inside_loop) {
    const Block& block = file_.storage.block(id);
    for (const StatementId statement : block.statements) {
      if (!flow.can_fall_through) {
        break;
      }
      flow = analyze_statement(statement, std::move(flow), inside_loop);
    }
    return flow;
  }

  FlowState analyze_statement(StatementId id, FlowState flow,
                              bool inside_loop) {
    const Statement& statement = file_.storage.statement(id);
    if (const auto* local =
            std::get_if<LocalVariableStatement>(&statement.data)) {
      if (local->initializer) {
        analyze_expression(*local->initializer, flow.assignments, false);
      }
      return flow;
    }
    if (const auto* return_statement =
            std::get_if<ReturnStatement>(&statement.data)) {
      if (return_statement->value) {
        analyze_expression(*return_statement->value, flow.assignments, false);
      }
      require_initialized(flow.assignments, statement.range);
      flow.can_fall_through = false;
      return flow;
    }
    if (const auto* expression_statement =
            std::get_if<ExpressionStatement>(&statement.data)) {
      const Expression& expression =
          file_.storage.expression(expression_statement->expression);
      if (const auto* assignment =
              std::get_if<AssignmentExpression>(&expression.data)) {
        if (const std::optional<std::size_t> field =
                final_field_index(assignment->target)) {
          analyze_expression(assignment->target, flow.assignments, true);
          analyze_expression(assignment->value, flow.assignments, false);
          assign_field(*field, expression_range(assignment->target),
                       flow.assignments, inside_loop);
          return flow;
        }
      }
      analyze_expression(expression_statement->expression, flow.assignments,
                         false);
      return flow;
    }
    if (const auto* if_statement = std::get_if<IfStatement>(&statement.data)) {
      analyze_expression(if_statement->condition, flow.assignments, false);
      FlowState then_flow =
          analyze_block(if_statement->then_block, flow, inside_loop);
      FlowState else_flow =
          if_statement->else_block
              ? analyze_block(*if_statement->else_block, flow, inside_loop)
              : flow;
      return merge_flows(std::move(then_flow), std::move(else_flow));
    }
    if (const auto* while_statement =
            std::get_if<WhileStatement>(&statement.data)) {
      analyze_expression(while_statement->condition, flow.assignments, false);
      static_cast<void>(analyze_block(while_statement->body, flow, true));
      return flow;
    }
    if (const auto* for_statement =
            std::get_if<ForStatement>(&statement.data)) {
      analyze_expression(for_statement->iterable, flow.assignments, false);
      static_cast<void>(analyze_block(for_statement->body, flow, true));
      return flow;
    }
    if (std::holds_alternative<BreakStatement>(statement.data) ||
        std::holds_alternative<ContinueStatement>(statement.data)) {
      flow.can_fall_through = false;
      return flow;
    }
    if (const auto* nested =
            std::get_if<NestedBlockStatement>(&statement.data)) {
      return analyze_block(nested->block, std::move(flow), inside_loop);
    }
    return flow;
  }

  FlowState merge_flows(FlowState left, FlowState right) const {
    if (!left.can_fall_through) {
      return right;
    }
    if (!right.can_fall_through) {
      return left;
    }
    for (std::size_t index = 0; index < left.assignments.size(); ++index) {
      left.assignments[index].minimum = std::min(
          left.assignments[index].minimum, right.assignments[index].minimum);
      left.assignments[index].maximum = std::max(
          left.assignments[index].maximum, right.assignments[index].maximum);
    }
    return left;
  }

  void analyze_expression(ExpressionId id,
                          std::vector<AssignmentCount>& assignments,
                          bool is_assignment_target) {
    const Expression& expression = file_.storage.expression(id);
    if (std::holds_alternative<IdentifierExpression>(expression.data)) {
      if (!is_assignment_target) {
        check_read(id, assignments);
      }
      return;
    }
    if (std::holds_alternative<InvalidExpression>(expression.data) ||
        std::holds_alternative<LiteralExpression>(expression.data)) {
      return;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data)) {
      analyze_expression(unary->operand, assignments, false);
      return;
    }
    if (const auto* binary = std::get_if<BinaryExpression>(&expression.data)) {
      analyze_expression(binary->left, assignments, false);
      analyze_expression(binary->right, assignments, false);
      return;
    }
    if (const auto* assignment =
            std::get_if<AssignmentExpression>(&expression.data)) {
      analyze_expression(assignment->target, assignments, true);
      analyze_expression(assignment->value, assignments, false);
      if (const std::optional<std::size_t> field =
              final_field_index(assignment->target)) {
        diagnostics_.error(
            expression.range,
            "final field '" + semantics_.symbol(final_fields_[*field]).name +
                "' initialization must be a direct constructor statement");
      }
      return;
    }
    if (const auto* member =
            std::get_if<MemberAccessExpression>(&expression.data)) {
      analyze_expression(member->object, assignments, false);
      if (!is_assignment_target) {
        check_read(id, assignments);
      }
      return;
    }
    if (const auto* call = std::get_if<CallExpression>(&expression.data)) {
      analyze_expression(call->callee, assignments, false);
      for (const ExpressionId argument : call->arguments) {
        analyze_expression(argument, assignments, false);
      }
      return;
    }
    if (const auto* array =
            std::get_if<ArrayLiteralExpression>(&expression.data)) {
      for (const ExpressionId element : array->elements) {
        analyze_expression(element, assignments, false);
      }
      return;
    }
    if (const auto* index = std::get_if<IndexExpression>(&expression.data)) {
      analyze_expression(index->object, assignments, false);
      analyze_expression(index->index, assignments, false);
      return;
    }
    const auto& grouped = std::get<ParenthesizedExpression>(expression.data);
    analyze_expression(grouped.expression, assignments, is_assignment_target);
  }

  void assign_field(std::size_t index, SourceRange range,
                    std::vector<AssignmentCount>& assignments,
                    bool inside_loop) {
    const std::string& name = semantics_.symbol(final_fields_[index]).name;
    if (inside_loop) {
      diagnostics_.error(range, "final field '" + name +
                                    "' cannot be initialized inside a loop");
      return;
    }
    AssignmentCount& count = assignments[index];
    if (count.maximum != 0) {
      diagnostics_.error(
          range, "final field '" + name + "' may only be initialized once");
    }
    count.minimum = std::min(2U, count.minimum + 1U);
    count.maximum = std::min(2U, count.maximum + 1U);
  }

  void check_read(ExpressionId id,
                  const std::vector<AssignmentCount>& assignments) {
    const std::optional<std::size_t> field = final_field_index(id);
    if (!field || assignments[*field].minimum != 0) {
      return;
    }
    diagnostics_.error(expression_range(id),
                       "final field '" +
                           semantics_.symbol(final_fields_[*field]).name +
                           "' is read before it is initialized");
  }

  std::optional<std::size_t> final_field_index(ExpressionId id) const {
    const ExpressionSemantics& expression =
        semantics_.file(file_id_).expressions.at(id.value);
    if (!expression.symbol ||
        !is_self_field_reference(id, *expression.symbol)) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < final_fields_.size(); ++index) {
      if (final_fields_[index] == *expression.symbol) {
        return index;
      }
    }
    return std::nullopt;
  }

  bool is_self_field_reference(ExpressionId id, SymbolId symbol) const {
    const Expression& expression = file_.storage.expression(id);
    if (std::holds_alternative<IdentifierExpression>(expression.data)) {
      return true;
    }
    if (const auto* member =
            std::get_if<MemberAccessExpression>(&expression.data)) {
      const ExpressionSemantics& object =
          semantics_.file(file_id_).expressions.at(member->object.value);
      return object.symbol == semantics_.file(file_id_).self_symbol;
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return is_self_field_reference(grouped->expression, symbol);
    }
    return false;
  }

  void require_initialized(const std::vector<AssignmentCount>& assignments,
                           SourceRange range) {
    for (std::size_t index = 0; index < assignments.size(); ++index) {
      if (assignments[index].minimum == 0) {
        diagnostics_.error(range,
                           "constructor exits before final field '" +
                               semantics_.symbol(final_fields_[index]).name +
                               "' is initialized");
      }
    }
  }

  SourceRange expression_range(ExpressionId id) const {
    return file_.storage.expression(id).range;
  }

  const FileClassDecl& file_;
  const SemanticModel& semantics_;
  FileId file_id_;
  DiagnosticEngine& diagnostics_;
  std::vector<SymbolId> final_fields_;
};

}  // namespace

void validate_final_fields(const FileClassDecl& file,
                           const SemanticModel& semantics, FileId file_id,
                           DiagnosticEngine& diagnostics) {
  FinalFieldAnalyzer{file, semantics, file_id, diagnostics}.run();
}

}  // namespace cloth
