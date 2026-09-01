// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_ABI_ABI_PRINTER_H_
#define CLOTH_ABI_ABI_PRINTER_H_

#include "cloth/abi/abi.h"
#include "cloth/sema/semantic_model.h"

#include <iosfwd>

namespace cloth {

void print_abi_summary(const AbiModule& abi, const SemanticModel& semantics,
                       std::ostream& output);

}  // namespace cloth

#endif  // CLOTH_ABI_ABI_PRINTER_H_
