#ifndef CLOTH_FLOW_CONTROL_FLOW_H_
#define CLOTH_FLOW_CONTROL_FLOW_H_

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"

#include <cstddef>
#include <vector>

namespace cloth {

struct CallableControlFlow {
  SymbolId symbol;
  bool can_fall_through;
  std::size_t reachable_statements;
  std::size_t unreachable_statements;
};

struct ControlFlowAnalysis {
  std::vector<CallableControlFlow> callables;
};

[[nodiscard]] ControlFlowAnalysis analyze_control_flow(
    const HirModule& hir, const SemanticModel& semantics,
    DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_FLOW_CONTROL_FLOW_H_
