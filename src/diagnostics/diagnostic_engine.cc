// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/diagnostics/diagnostic_engine.h"

#include <utility>

namespace cloth {

std::string_view diagnostic_severity_name(
    DiagnosticSeverity severity) noexcept {
  switch (severity) {
    case DiagnosticSeverity::kError:
      return "error";
    case DiagnosticSeverity::kWarning:
      return "warning";
    case DiagnosticSeverity::kNote:
      return "note";
  }
  return "unknown";
}

void DiagnosticEngine::report(DiagnosticSeverity severity,
                              SourceLocation location, std::string message) {
  report(severity, point_range(location), std::move(message));
}

void DiagnosticEngine::report(DiagnosticSeverity severity, SourceRange range,
                              std::string message) {
  if (severity == DiagnosticSeverity::kError) {
    has_errors_ = true;
  }
  diagnostics_.push_back(Diagnostic{severity, range, std::move(message)});
}

void DiagnosticEngine::error(SourceLocation location, std::string message) {
  report(DiagnosticSeverity::kError, location, std::move(message));
}

void DiagnosticEngine::error(SourceRange range, std::string message) {
  report(DiagnosticSeverity::kError, range, std::move(message));
}

void DiagnosticEngine::warning(SourceLocation location, std::string message) {
  report(DiagnosticSeverity::kWarning, location, std::move(message));
}

void DiagnosticEngine::warning(SourceRange range, std::string message) {
  report(DiagnosticSeverity::kWarning, range, std::move(message));
}

void DiagnosticEngine::note(SourceLocation location, std::string message) {
  report(DiagnosticSeverity::kNote, location, std::move(message));
}

void DiagnosticEngine::note(SourceRange range, std::string message) {
  report(DiagnosticSeverity::kNote, range, std::move(message));
}

bool DiagnosticEngine::has_errors() const noexcept { return has_errors_; }

std::span<const Diagnostic> DiagnosticEngine::diagnostics() const noexcept {
  return diagnostics_;
}

}  // namespace cloth
