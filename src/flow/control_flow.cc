// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/flow/control_flow.h"

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"

#include <algorithm>
#include <string>
#include <variant>

namespace cloth {
namespace {

struct FlowResult {
  bool can_fall_through{true};
  bool has_break{false};
};

class CallableAnalyzer {
 public:
  CallableAnalyzer(const HirModule& hir, const SemanticModel& semantics,
                   DiagnosticEngine& diagnostics)
      : hir_(hir), semantics_(semantics), diagnostics_(diagnostics) {}

  CallableControlFlow analyze(const HirCallable& callable,
                              bool body_is_reachable = true) {
    const FlowResult flow = analyze_block(callable.body, body_is_reachable);
    return CallableControlFlow{callable.symbol, flow.can_fall_through,
                               reachable_statements_, unreachable_statements_};
  }

 private:
  FlowResult analyze_block(HirBlockId id, bool is_reachable = true) {
    const HirBlock& block = hir_.storage.block(id);
    FlowResult block_flow{is_reachable, false};
    for (const HirStatementId statement_id : block.statements) {
      const HirStatement& statement = hir_.storage.statement(statement_id);
      if (!block_flow.can_fall_through) {
        ++unreachable_statements_;
        diagnostics_.warning(statement.range, "unreachable statement");
        continue;
      }
      ++reachable_statements_;
      const FlowResult statement_flow = analyze_statement(statement);
      block_flow.can_fall_through = statement_flow.can_fall_through;
      block_flow.has_break = block_flow.has_break || statement_flow.has_break;
    }
    return block_flow;
  }

  FlowResult analyze_statement(const HirStatement& statement) {
    if (std::holds_alternative<HirReturnStatement>(statement.data)) {
      return FlowResult{false, false};
    }
    if (std::holds_alternative<HirBreakStatement>(statement.data)) {
      return FlowResult{false, true};
    }
    if (std::holds_alternative<HirContinueStatement>(statement.data)) {
      return FlowResult{false, false};
    }
    if (const auto* local = std::get_if<HirLocalStatement>(&statement.data);
        local && local->initializer && never_completes(*local->initializer)) {
      return FlowResult{false, false};
    }
    if (const auto* expression =
            std::get_if<HirExpressionStatement>(&statement.data);
        expression && never_completes(expression->expression)) {
      return FlowResult{false, false};
    }
    if (const auto* if_statement =
            std::get_if<HirIfStatement>(&statement.data)) {
      if (never_completes(if_statement->condition)) {
        return FlowResult{false, false};
      }
      const FlowResult then_flow = analyze_block(if_statement->then_block);
      const FlowResult else_flow =
          if_statement->else_block ? analyze_block(*if_statement->else_block)
                                   : FlowResult{};
      return FlowResult{
          then_flow.can_fall_through || else_flow.can_fall_through,
          then_flow.has_break || else_flow.has_break};
    }
    if (const auto* while_statement =
            std::get_if<HirWhileStatement>(&statement.data)) {
      if (never_completes(while_statement->condition)) {
        return FlowResult{false, false};
      }
      const FlowResult body_flow = analyze_block(while_statement->body);
      return FlowResult{
          !is_true(while_statement->condition) || body_flow.has_break, false};
    }
    if (const auto* selection =
            std::get_if<HirSwitchStatement>(&statement.data)) {
      if (never_completes(selection->selector)) {
        return FlowResult{false, false};
      }
      bool can_fall_through = !selection->is_exhaustive;
      for (const auto& arm : selection->arms) {
        const FlowResult flow = analyze_block(arm.body);
        can_fall_through =
            can_fall_through || flow.can_fall_through || flow.has_break;
      }
      // Breaks in arms belong to this switch, not an enclosing loop.
      return FlowResult{can_fall_through, false};
    }
    if (const auto* for_statement =
            std::get_if<HirForEachStatement>(&statement.data)) {
      if (never_completes(for_statement->iterable)) {
        return FlowResult{false, false};
      }
      static_cast<void>(analyze_block(for_statement->body));
      return FlowResult{};
    }
    if (const auto* for_statement =
            std::get_if<HirForStatement>(&statement.data)) {
      if (for_statement->initializer) {
        const FlowResult initializer = analyze_statement(
            hir_.storage.statement(*for_statement->initializer));
        if (!initializer.can_fall_through) {
          return initializer;
        }
      }
      if (for_statement->condition &&
          never_completes(*for_statement->condition)) {
        return FlowResult{false, false};
      }
      const FlowResult body_flow = analyze_block(for_statement->body);
      const bool condition_can_exit =
          for_statement->condition && !is_true(*for_statement->condition);
      return FlowResult{condition_can_exit || body_flow.has_break, false};
    }
    if (const auto* nested =
            std::get_if<HirNestedBlockStatement>(&statement.data)) {
      return analyze_block(nested->block);
    }
    return FlowResult{};
  }

  bool never_completes(HirExpressionId id) const {
    return hir_.storage.expression(id).type == semantics_.bottom_type();
  }

  bool is_true(HirExpressionId id) const {
    const HirExpression& expression = hir_.storage.expression(id);
    if (const auto* literal =
            std::get_if<HirLiteralExpression>(&expression.data)) {
      return literal->kind == LiteralKind::kBoolean &&
             literal->lexeme == "true";
    }
    if (const auto* grouped =
            std::get_if<HirGroupedExpression>(&expression.data)) {
      return is_true(grouped->expression);
    }
    return false;
  }

  const HirModule& hir_;
  const SemanticModel& semantics_;
  DiagnosticEngine& diagnostics_;
  std::size_t reachable_statements_{0};
  std::size_t unreachable_statements_{0};
};

}  // namespace

ControlFlowAnalysis analyze_control_flow(const HirModule& hir,
                                         const SemanticModel& semantics,
                                         DiagnosticEngine& diagnostics) {
  ControlFlowAnalysis analysis;
  for (const HirFileClass& file : hir.files) {
    const bool field_initializers_complete =
        std::ranges::none_of(file.fields, [&](const HirField& field) {
          return !semantics.symbol(field.symbol).is_static &&
                 field.initializer &&
                 hir.storage.expression(*field.initializer).type ==
                     semantics.bottom_type();
        });
    for (const MemberReference& member : file.member_order) {
      if (member.kind == DeclarationKind::kFunction) {
        const HirCallable& function = file.functions.at(member.index);
        const SemanticSymbol& symbol = semantics.symbol(function.symbol);
        CallableControlFlow flow =
            symbol.is_abstract
                ? CallableControlFlow{function.symbol, false, 0, 0}
                : CallableAnalyzer{hir, semantics, diagnostics}.analyze(
                      function);
        if (symbol.is_valid && symbol.type != semantics.void_type() &&
            symbol.type != semantics.error_type() && flow.can_fall_through) {
          diagnostics.error(symbol.range, "function '" + symbol.name +
                                              "' does not return a value on "
                                              "every path");
        }
        analysis.callables.push_back(flow);
      } else if (member.kind == DeclarationKind::kConstructor) {
        const HirCallable& constructor = file.constructors.at(member.index);
        const bool initializer_completes =
            !constructor.initializer ||
            std::ranges::none_of(
                constructor.initializer->arguments,
                [&](HirExpressionId argument) {
                  return hir.storage.expression(argument).type ==
                         semantics.bottom_type();
                });
        analysis.callables.push_back(
            CallableAnalyzer{hir, semantics, diagnostics}.analyze(
                constructor,
                initializer_completes && field_initializers_complete));
      }
    }
  }
  return analysis;
}

}  // namespace cloth
