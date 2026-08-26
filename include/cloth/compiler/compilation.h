#ifndef CLOTH_COMPILER_COMPILATION_H_
#define CLOTH_COMPILER_COMPILATION_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/lexer/token.h"
#include "cloth/parser/parser.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cloth {

struct CompilationResult {
  SemanticModel semantics;
  HirModule hir;
  bool is_valid;
};

class Compilation {
 public:
  void add_source(SourceFile source);
  // Source ranges in the result refer to source storage owned here.
  [[nodiscard]] CompilationResult analyze(DiagnosticEngine& diagnostics);

  // Token and syntax access is available after analyze().
  [[nodiscard]] std::size_t source_count() const noexcept;
  [[nodiscard]] const SourceFile& source(std::size_t index) const;
  [[nodiscard]] std::span<const Token> tokens(std::size_t index) const;
  [[nodiscard]] const FileClassDecl& syntax(std::size_t index) const;

 private:
  struct Unit {
    SourceFile source;
    std::vector<Token> tokens;
    std::optional<ParseResult> parse_result;
  };

  std::vector<Unit> units_;
};

}  // namespace cloth

#endif  // CLOTH_COMPILER_COMPILATION_H_
