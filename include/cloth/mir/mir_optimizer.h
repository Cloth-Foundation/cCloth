// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_MIR_MIR_OPTIMIZER_H_
#define CLOTH_MIR_MIR_OPTIMIZER_H_

#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

namespace cloth {

// Requires verified MIR. Folds target-independent scalar operations and
// canonically compacts control flow made unreachable by those folds.
void optimize_mir(MirModule& mir, const SemanticModel& semantics);

}  // namespace cloth

#endif  // CLOTH_MIR_MIR_OPTIMIZER_H_
