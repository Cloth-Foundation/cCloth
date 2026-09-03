// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SEMA_CONSTANT_EVALUATOR_H_
#define CLOTH_SEMA_CONSTANT_EVALUATOR_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"

#include <optional>
#include <span>

namespace cloth {

// Iterative, before recursive typing. Counts every source node, including
// parentheses and skipped expressions; rejects structurally ineligible forms.
[[nodiscard]] std::optional<std::size_t> preflight_constant_expression(
    const AstStorage& storage, ExpressionId root,
    DiagnosticEngine& diagnostics);
void evaluate_static_constants(std::span<const FileClassDecl* const> files,
                               SemanticModel& semantics,
                               DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_SEMA_CONSTANT_EVALUATOR_H_
