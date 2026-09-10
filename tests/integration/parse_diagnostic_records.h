// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_TESTS_INTEGRATION_PARSE_DIAGNOSTIC_RECORDS_H_
#define CLOTH_TESTS_INTEGRATION_PARSE_DIAGNOSTIC_RECORDS_H_

#include "cloth/diagnostics/diagnostic.h"

#include <iosfwd>
#include <span>
#include <string_view>

namespace cloth::test {

[[nodiscard]] bool write_parse_diagnostic_records(
    std::span<const Diagnostic> diagnostics, std::string_view tag,
    std::ostream& output, std::ostream& errors, std::string_view context);

}  // namespace cloth::test

#endif  // CLOTH_TESTS_INTEGRATION_PARSE_DIAGNOSTIC_RECORDS_H_
