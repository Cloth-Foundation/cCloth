// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_HIR_HIR_VERIFIER_H_
#define CLOTH_HIR_HIR_VERIFIER_H_

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"

namespace cloth {

// Checks compiler invariants. Recovered invalid HIR nodes are structurally
// valid and do not cause verification failure.
[[nodiscard]] bool verify_hir(const HirModule& hir,
                              const SemanticModel& semantics,
                              DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_HIR_HIR_VERIFIER_H_
