// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_PARSER_CONSTANT_PARSE_BUDGET_H_
#define CLOTH_PARSER_CONSTANT_PARSE_BUDGET_H_

#include <cstddef>

namespace cloth {

// Shared by source files of one owning package, never by dependency packages.
struct ConstantParseBudget {
  std::size_t declarations{0};
  std::size_t expression_nodes{0};
};

}  // namespace cloth

#endif  // CLOTH_PARSER_CONSTANT_PARSE_BUDGET_H_
