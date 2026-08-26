#include "cloth/compiler/compilation.h"

#include "cloth/lexer/lexer.h"
#include "cloth/parser/parser.h"
#include "cloth/sema/semantic_analyzer.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace cloth {

void Compilation::add_source(SourceFile source) {
  units_.push_back(Unit{std::move(source), {}, std::nullopt});
}

CompilationResult Compilation::analyze(DiagnosticEngine& diagnostics) {
  std::vector<const FileClassDecl*> files;
  files.reserve(units_.size());
  for (Unit& unit : units_) {
    unit.tokens = Lexer{unit.source, diagnostics}.lex();
    unit.parse_result.emplace(
        Parser{unit.source, unit.tokens, diagnostics}.parse());
    files.push_back(&unit.parse_result->file_class);
  }

  SemanticAnalysisResult semantic_result =
      analyze_semantics(files, diagnostics);
  HirModule hir = lower_to_hir(files, semantic_result.model);
  return CompilationResult{std::move(semantic_result.model), std::move(hir),
                           semantic_result.is_valid};
}

std::size_t Compilation::source_count() const noexcept { return units_.size(); }

const SourceFile& Compilation::source(std::size_t index) const {
  return units_.at(index).source;
}

std::span<const Token> Compilation::tokens(std::size_t index) const {
  return units_.at(index).tokens;
}

const FileClassDecl& Compilation::syntax(std::size_t index) const {
  return units_.at(index).parse_result.value().file_class;
}

}  // namespace cloth
