// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_PARSER_EXPRESSION_PARSER_H_
#define CLOTH_PARSER_EXPRESSION_PARSER_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/token.h"
#include "cloth/parser/constant_parse_budget.h"

#include <cstddef>
#include <span>

namespace cloth {

class ExpressionParser {
 public:
  ExpressionParser(std::span<const Token> tokens, std::size_t& current,
                   std::size_t limit, AstStorage& storage,
                   DiagnosticEngine& diagnostics, bool constant_context = false,
                   ConstantParseBudget* constant_budget = nullptr) noexcept;

  [[nodiscard]] ExpressionId parse_expression(int minimum_precedence = 1);

 private:
  [[nodiscard]] bool at_limit() const noexcept;
  [[nodiscard]] const Token& current() const noexcept;
  const Token& advance() noexcept;
  bool match(TokenKind kind) noexcept;

  [[nodiscard]] ExpressionId parse_unary_expression();
  [[nodiscard]] ExpressionId parse_throw_expression();
  [[nodiscard]] ExpressionId build_unary_expression(const Token& operation,
                                                    ExpressionId operand);
  [[nodiscard]] ExpressionId build_numeric_conversion(const Token& target,
                                                      ExpressionId value,
                                                      SourceLocation end);
  [[nodiscard]] ExpressionId build_integer_conversion(const Token& target,
                                                      const Token& operation,
                                                      ExpressionId value,
                                                      SourceLocation end);
  [[nodiscard]] ExpressionId parse_postfix_expression();
  [[nodiscard]] ExpressionId parse_primary_expression();
  [[nodiscard]] bool at_primitive_parse_target() const noexcept;
  [[nodiscard]] ExpressionId parse_primitive_meta_target();
  [[nodiscard]] ExpressionId parse_atom_expression();
  [[nodiscard]] ExpressionId parse_grouped_expression();
  [[nodiscard]] ExpressionId parse_postfix_suffixes(ExpressionId expression);
  [[nodiscard]] ExpressionId parse_access_suffix(ExpressionId expression);
  [[nodiscard]] ExpressionId parse_type_relation(ExpressionId left,
                                                 TokenKind operation);
  [[nodiscard]] ExpressionId build_binary_expression(ExpressionId left,
                                                     TokenKind operation,
                                                     ExpressionId right);
  [[nodiscard]] ExpressionId parse_numeric_conversion_expression();
  [[nodiscard]] ExpressionId parse_integer_conversion_expression();
  [[nodiscard]] ExpressionId parse_call_expression(ExpressionId callee);
  [[nodiscard]] ExpressionId parse_array_literal_expression();
  [[nodiscard]] ExpressionId parse_index_expression(ExpressionId object);
  [[nodiscard]] TypeSyntax parse_checked_type();
  [[nodiscard]] ExpressionId make_invalid_expression(SourceRange range);

  [[nodiscard]] static int binary_precedence(TokenKind kind) noexcept;
  [[nodiscard]] static bool is_assignment_operator(TokenKind kind) noexcept;
  [[nodiscard]] static bool is_right_associative(TokenKind kind) noexcept;
  [[nodiscard]] SourceRange expression_range(ExpressionId id) const;
  bool enter_constant_expression();
  ExpressionId add_expression(Expression expression);

  std::span<const Token> tokens_;
  std::size_t& current_;
  std::size_t limit_;
  AstStorage& storage_;
  DiagnosticEngine& diagnostics_;
  bool constant_context_{false};
  std::size_t expression_base_{0};
  std::size_t nesting_{0};
  ConstantParseBudget* constant_budget_;
  std::optional<ExpressionId> limit_error_;
};

}  // namespace cloth

#endif  // CLOTH_PARSER_EXPRESSION_PARSER_H_
