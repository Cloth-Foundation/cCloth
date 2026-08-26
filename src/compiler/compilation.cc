#include "cloth/compiler/compilation.h"

#include "cloth/abi/abi.h"
#include "cloth/abi/abi_verifier.h"
#include "cloth/flow/control_flow.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/lexer/lexer.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/parser/parser.h"
#include "cloth/sema/semantic_analyzer.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace cloth {

Compilation::Compilation() : Compilation(TargetDataLayout::llvm_x86_64()) {}

Compilation::Compilation(TargetDataLayout target)
    : target_(std::move(target)) {}

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
  const bool hir_is_valid = verify_hir(hir, semantic_result.model, diagnostics);
  ControlFlowAnalysis control_flow;
  MirModule mir;
  AbiModule abi{target_, {}, {}};
  bool mir_is_valid = false;
  bool abi_is_valid = false;
  if (hir_is_valid) {
    control_flow =
        analyze_control_flow(hir, semantic_result.model, diagnostics);
    mir = lower_to_mir(hir, semantic_result.model);
    mir_is_valid = verify_mir(mir, semantic_result.model, diagnostics);
    if (mir_is_valid) {
      abi = lower_to_abi(mir, semantic_result.model, target_);
      abi_is_valid = verify_abi(abi, mir, semantic_result.model, diagnostics);
    }
  }
  const bool is_valid = !diagnostics.has_errors() && hir_is_valid &&
                        mir_is_valid && abi_is_valid &&
                        semantic_result.is_valid;
  return CompilationResult{std::move(semantic_result.model),
                           std::move(hir),
                           std::move(control_flow),
                           std::move(mir),
                           std::move(abi),
                           is_valid};
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
