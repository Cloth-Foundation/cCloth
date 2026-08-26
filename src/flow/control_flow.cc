#include "cloth/flow/control_flow.h"

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"

#include <string>
#include <variant>

namespace cloth {
namespace {

class CallableAnalyzer {
 public:
  CallableAnalyzer(const HirModule& hir, DiagnosticEngine& diagnostics)
      : hir_(hir), diagnostics_(diagnostics) {}

  CallableControlFlow analyze(const HirCallable& callable) {
    const bool definitely_returns = analyze_block(callable.body);
    return CallableControlFlow{callable.symbol, !definitely_returns,
                               reachable_statements_, unreachable_statements_};
  }

 private:
  bool analyze_block(HirBlockId id) {
    const HirBlock& block = hir_.storage.block(id);
    bool terminated = false;
    for (const HirStatementId statement_id : block.statements) {
      const HirStatement& statement = hir_.storage.statement(statement_id);
      if (terminated) {
        ++unreachable_statements_;
        diagnostics_.warning(statement.range, "unreachable statement");
        continue;
      }
      ++reachable_statements_;
      terminated = analyze_statement(statement);
    }
    return terminated;
  }

  bool analyze_statement(const HirStatement& statement) {
    if (std::holds_alternative<HirReturnStatement>(statement.data)) {
      return true;
    }
    if (const auto* if_statement =
            std::get_if<HirIfStatement>(&statement.data)) {
      const bool then_returns = analyze_block(if_statement->then_block);
      const bool else_returns = if_statement->else_block
                                    ? analyze_block(*if_statement->else_block)
                                    : false;
      return if_statement->else_block && then_returns && else_returns;
    }
    if (const auto* nested =
            std::get_if<HirNestedBlockStatement>(&statement.data)) {
      return analyze_block(nested->block);
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
        CallableControlFlow flow =
            CallableAnalyzer{hir, diagnostics}.analyze(function);
        const SemanticSymbol& symbol = semantics.symbol(function.symbol);
        if (symbol.is_valid && symbol.type != semantics.no_value_type() &&
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
