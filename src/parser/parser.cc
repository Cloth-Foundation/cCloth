// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/parser/parser.h"

#include "cloth/parser/declaration_pass.h"
#include "cloth/parser/definition_pass.h"

#include <string>
#include <utility>

namespace cloth {

Parser::Parser(const SourceFile& source, std::span<const Token> tokens,
               DiagnosticEngine& diagnostics) noexcept
    : source_(source), tokens_(tokens), diagnostics_(diagnostics) {}

ParseResult Parser::parse() {
  if (tokens_.empty() || tokens_.back().kind != TokenKind::kEof) {
    const SourceLocation origin{source_.display_path(), 0, 1, 1};
    const SourceRange range = point_range(origin);
    diagnostics_.error(range,
                       "internal parser error: token stream must end with eof");
    FileClassSymbols symbols{std::string{source_.stem()},
                             infer_visibility(source_.stem()), range};
    FileClassDecl file_class{std::string{source_.stem()},
                             {},
                             std::string{source_.stem()},
                             source_.display_path(),
                             infer_visibility(source_.stem()),
                             range,
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             false};
    return ParseResult{std::move(file_class), std::move(symbols), false};
  }

  DeclarationPassResult declarations =
      DeclarationPass{source_, tokens_, diagnostics_}.run();
  FileClassDecl file_class =
      DefinitionPass{source_,
                     tokens_,
                     declarations.symbols,
                     declarations.imports,
                     declarations.outlines,
                     declarations.base_class,
                     declarations.has_explicit_class_declaration,
                     declarations.is_abstract,
                     declarations.is_sealed,
                     declarations.file_type_kind,
                     declarations.interfaces,
                     diagnostics_}
          .run();
  file_class.is_valid = file_class.is_valid && declarations.is_valid;
  const bool is_valid = file_class.is_valid && !diagnostics_.has_errors();
  return ParseResult{std::move(file_class), std::move(declarations.symbols),
                     is_valid};
}

}  // namespace cloth
