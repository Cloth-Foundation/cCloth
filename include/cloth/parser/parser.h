#ifndef CLOTH_PARSER_PARSER_H_
#define CLOTH_PARSER_PARSER_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/token.h"
#include "cloth/sema/file_class_symbols.h"
#include "cloth/source/source_file.h"

#include <span>

namespace cloth {

struct ParseResult {
  FileClassDecl file_class;
  FileClassSymbols symbols;
  bool is_valid;
};

class Parser {
 public:
  Parser(const SourceFile& source, std::span<const Token> tokens,
         DiagnosticEngine& diagnostics) noexcept;

  [[nodiscard]] ParseResult parse();

 private:
  const SourceFile& source_;
  std::span<const Token> tokens_;
  DiagnosticEngine& diagnostics_;
};

}  // namespace cloth

#endif  // CLOTH_PARSER_PARSER_H_
