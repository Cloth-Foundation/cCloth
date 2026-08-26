#ifndef CLOTH_SEMA_SEMANTIC_ANALYZER_H_
#define CLOTH_SEMA_SEMANTIC_ANALYZER_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"

#include <span>

namespace cloth {

struct SemanticAnalysisResult {
  SemanticModel model;
  bool is_valid;
};

[[nodiscard]] SemanticAnalysisResult analyze_semantics(
    std::span<const FileClassDecl* const> files, DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_SEMA_SEMANTIC_ANALYZER_H_
