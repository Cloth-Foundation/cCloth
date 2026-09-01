// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_DIAGNOSTICS_DIAGNOSTIC_ENGINE_H_
#define CLOTH_DIAGNOSTICS_DIAGNOSTIC_ENGINE_H_

#include "cloth/diagnostics/diagnostic.h"

#include <span>
#include <string>
#include <vector>

namespace cloth {

class DiagnosticEngine {
 public:
  void report(DiagnosticSeverity severity, SourceLocation location,
              std::string message);
  void report(DiagnosticSeverity severity, SourceRange range,
              std::string message);
  void error(SourceLocation location, std::string message);
  void error(SourceRange range, std::string message);
  void warning(SourceLocation location, std::string message);
  void warning(SourceRange range, std::string message);
  void note(SourceLocation location, std::string message);
  void note(SourceRange range, std::string message);

  [[nodiscard]] bool has_errors() const noexcept;
  [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept;

 private:
  std::vector<Diagnostic> diagnostics_;
  bool has_errors_{false};
};

}  // namespace cloth

#endif  // CLOTH_DIAGNOSTICS_DIAGNOSTIC_ENGINE_H_
