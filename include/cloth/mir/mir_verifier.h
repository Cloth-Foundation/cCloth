// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_MIR_MIR_VERIFIER_H_
#define CLOTH_MIR_MIR_VERIFIER_H_

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

namespace cloth {

[[nodiscard]] bool verify_mir(const MirModule& mir,
                              const SemanticModel& semantics,
                              DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_MIR_MIR_VERIFIER_H_
