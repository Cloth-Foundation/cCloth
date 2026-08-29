#include "cloth/flow/control_flow.h"

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"

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
  CallableAnalyzer(const HirModule& hir, DiagnosticEngine& diagnostics)
      : hir_(hir), diagnostics_(diagnostics) {}

  CallableControlFlow analyze(const HirCallable& callable) {
    const FlowResult flow = analyze_block(callable.body);
    return CallableControlFlow{callable.symbol, flow.can_fall_through,
                               reachable_statements_, unreachable_statements_};
  }

 private:
  FlowResult analyze_block(HirBlockId id) {
    const HirBlock& block = hir_.storage.block(id);
    FlowResult block_flow;
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
    if (const auto* if_statement =
            std::get_if<HirIfStatement>(&statement.data)) {
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
      const FlowResult body_flow = analyze_block(while_statement->body);
      return FlowResult{
          !is_true(while_statement->condition) || body_flow.has_break, false};
    }
    if (const auto* for_statement =
            std::get_if<HirForEachStatement>(&statement.data)) {
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
    for (const MemberReference& member : file.member_order) {
      if (member.kind == DeclarationKind::kFunction) {
        const HirCallable& function = file.functions.at(member.index);
        const SemanticSymbol& symbol = semantics.symbol(function.symbol);
        CallableControlFlow flow =
            symbol.is_abstract
                ? CallableControlFlow{function.symbol, false, 0, 0}
                : CallableAnalyzer{hir, diagnostics}.analyze(function);
        if (symbol.is_valid && symbol.type != semantics.void_type() &&
            symbol.type != semantics.error_type() && flow.can_fall_through) {
          diagnostics.error(symbol.range, "function '" + symbol.name +
                                              "' does not return a value on "
                                              "every path");
        }
        analysis.callables.push_back(flow);
      } else if (member.kind == DeclarationKind::kConstructor) {
        const HirCallable& constructor = file.constructors.at(member.index);
        analysis.callables.push_back(
            CallableAnalyzer{hir, diagnostics}.analyze(constructor));
      }
    }
  }
  return analysis;
}

}  // namespace cloth
