#ifndef CLOTH_SEMA_FINAL_FIELD_ANALYSIS_H_
#define CLOTH_SEMA_FINAL_FIELD_ANALYSIS_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"

namespace cloth {

void validate_final_fields(const FileClassDecl& file,
                           const SemanticModel& semantics, FileId file_id,
                           DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_SEMA_FINAL_FIELD_ANALYSIS_H_
