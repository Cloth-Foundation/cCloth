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
  void error(SourceLocation location, std::string message);
  void warning(SourceLocation location, std::string message);
  void note(SourceLocation location, std::string message);

  [[nodiscard]] bool has_errors() const noexcept;
  [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept;

 private:
  std::vector<Diagnostic> diagnostics_;
  bool has_errors_{false};
};

}  // namespace cloth

#endif  // CLOTH_DIAGNOSTICS_DIAGNOSTIC_ENGINE_H_
