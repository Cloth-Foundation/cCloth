#ifndef CLOTH_COMPILER_COMPILATION_H_
#define CLOTH_COMPILER_COMPILATION_H_

#include "cloth/abi/abi.h"
#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/flow/control_flow.h"
#include "cloth/hir/hir.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir.h"
#include "cloth/parser/parser.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cloth {

struct CompilationDependency {
  std::string owner;
  std::string alias;
  std::string target;
};

struct CompilationResult {
  SemanticModel semantics;
  HirModule hir;
  ControlFlowAnalysis control_flow;
  MirModule mir;
  AbiModule abi;
  bool is_valid;
};

class Compilation {
 public:
  Compilation();
  explicit Compilation(TargetDataLayout target);

  void set_source_root(std::filesystem::path source_root,
                       bool discover_package_sources = false);
  void set_package_dependencies(
      std::vector<CompilationDependency> dependencies);
  void add_source(SourceFile source, std::string package_name = {});
  void add_package_source(SourceFile source, std::string owning_package,
                          std::string source_package);
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
    std::string package_name;
    std::string owning_package;
    std::string qualified_name;
    std::vector<Token> tokens;
    std::optional<ParseResult> parse_result;
  };

  void prepare_source_graph(DiagnosticEngine& diagnostics);

  std::vector<Unit> units_;
  std::optional<std::filesystem::path> source_root_;
  bool discover_package_sources_{false};
  std::vector<CompilationDependency> package_dependencies_;
  TargetDataLayout target_;
};

}  // namespace cloth

#endif  // CLOTH_COMPILER_COMPILATION_H_
