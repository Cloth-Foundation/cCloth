// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_PARSER_EXPRESSION_PARSER_H_
#define CLOTH_PARSER_EXPRESSION_PARSER_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/token.h"

#include <cstddef>
#include <span>

namespace cloth {

class ExpressionParser {
 public:
  ExpressionParser(std::span<const Token> tokens, std::size_t& current,
                   std::size_t limit, AstStorage& storage,
                   DiagnosticEngine& diagnostics) noexcept;

  [[nodiscard]] ExpressionId parse_expression(int minimum_precedence = 1);

 private:
  [[nodiscard]] bool at_limit() const noexcept;
  [[nodiscard]] const Token& current() const noexcept;
  const Token& advance() noexcept;
  bool match(TokenKind kind) noexcept;

  [[nodiscard]] ExpressionId parse_unary_expression();
  [[nodiscard]] ExpressionId parse_postfix_expression();
  [[nodiscard]] ExpressionId parse_primary_expression();
  [[nodiscard]] ExpressionId parse_numeric_conversion_expression();
  [[nodiscard]] ExpressionId parse_call_expression(ExpressionId callee);
  [[nodiscard]] ExpressionId parse_array_literal_expression();
  [[nodiscard]] ExpressionId parse_index_expression(ExpressionId object);
  [[nodiscard]] TypeSyntax parse_checked_type();
  [[nodiscard]] ExpressionId make_invalid_expression(SourceRange range);

  [[nodiscard]] static int binary_precedence(TokenKind kind) noexcept;
  [[nodiscard]] static bool is_assignment_operator(TokenKind kind) noexcept;
  [[nodiscard]] static bool is_right_associative(TokenKind kind) noexcept;
  [[nodiscard]] SourceRange expression_range(ExpressionId id) const;

  std::span<const Token> tokens_;
  std::size_t& current_;
  std::size_t limit_;
  AstStorage& storage_;
  DiagnosticEngine& diagnostics_;
};

}  // namespace cloth

#endif  // CLOTH_PARSER_EXPRESSION_PARSER_H_
