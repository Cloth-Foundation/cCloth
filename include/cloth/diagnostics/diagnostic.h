// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_DIAGNOSTICS_DIAGNOSTIC_H_
#define CLOTH_DIAGNOSTICS_DIAGNOSTIC_H_

#include "cloth/source/source_range.h"

#include <string>
#include <string_view>

namespace cloth {

enum class DiagnosticSeverity {
  kError,
  kWarning,
  kNote,
};

struct Diagnostic {
  DiagnosticSeverity severity;
  SourceRange range;
  std::string message;
};

[[nodiscard]] std::string_view diagnostic_severity_name(
    DiagnosticSeverity severity) noexcept;

}  // namespace cloth

#endif  // CLOTH_DIAGNOSTICS_DIAGNOSTIC_H_
