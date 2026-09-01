// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SEMA_FIELD_INITIALIZATION_ANALYSIS_H_
#define CLOTH_SEMA_FIELD_INITIALIZATION_ANALYSIS_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"

namespace cloth {

void validate_field_initialization(const FileClassDecl& file,
                                   const SemanticModel& semantics,
                                   FileId file_id,
                                   DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_SEMA_FIELD_INITIALIZATION_ANALYSIS_H_
