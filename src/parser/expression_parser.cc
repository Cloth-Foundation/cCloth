// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/parser/expression_parser.h"

#include "cloth/parser/syntax_facts.h"
#include "cloth/sema/scalar_constants.h"

#include <utility>

namespace cloth {
namespace {

SourceRange join_ranges(SourceRange left, SourceRange right) noexcept {
  return SourceRange{left.begin, right.end};
}

}  // namespace

ExpressionParser::ExpressionParser(
    std::span<const Token> tokens, std::size_t& current, std::size_t limit,
    AstStorage& storage, DiagnosticEngine& diagnostics, bool constant_context,
    ConstantParseBudget* constant_budget) noexcept
    : tokens_(tokens),
      current_(current),
      limit_(limit),
      storage_(storage),
      diagnostics_(diagnostics),
      constant_context_(constant_context),
      expression_base_(storage.expressions().size()),
      constant_budget_(constant_budget) {}

ExpressionId ExpressionParser::add_expression(Expression expression) {
  if (constant_context_) {
    if (limit_error_) return *limit_error_;
    std::string_view error;
    if (storage_.expressions().size() - expression_base_ >= kMaxConstantNodes)
      error = "constant initializer exceeds 65536 expression nodes";
    else if (constant_budget_ &&
             constant_budget_->expression_nodes >= kMaxPackageConstantNodes)
      error = "package exceeds 1048576 constant expression nodes";
    if (!error.empty()) {
      diagnostics_.error(expression.range, std::string{error});
      current_ = limit_;
      limit_error_ = storage_.add_expression(
          Expression{expression.range, InvalidExpression{}});
      return *limit_error_;
    }
    if (constant_budget_) ++constant_budget_->expression_nodes;
  }
  return storage_.add_expression(std::move(expression));
}

bool ExpressionParser::enter_constant_expression() {
  if (!constant_context_) return true;
  std::string_view error;
  if (nesting_ >= kMaxConstantDepth)
    error = "constant expression exceeds nesting depth 256";
  if (error.empty()) return true;
  diagnostics_.error(current().range, std::string{error});
  current_ = limit_;
  return false;
}

ExpressionId ExpressionParser::parse_expression(int minimum_precedence) {
  if (!enter_constant_expression())
    return make_invalid_expression(current().range);
  ++nesting_;
  ExpressionId left = parse_unary_expression();
  while (!at_limit()) {
    const TokenKind operation = current().kind;
    const int precedence = binary_precedence(operation);
    if (precedence < minimum_precedence) {
      break;
    }
    advance();
    if (operation == TokenKind::kKwIs || operation == TokenKind::kKwAs) {
      left = parse_type_relation(left, operation);
      continue;
    }
    const int next_precedence =
        is_right_associative(operation) ? precedence : precedence + 1;
    const ExpressionId right = parse_expression(next_precedence);
    left = build_binary_expression(left, operation, right);
  }
  --nesting_;
  return left;
}

bool ExpressionParser::at_limit() const noexcept { return current_ >= limit_; }

ExpressionId ExpressionParser::parse_type_relation(ExpressionId left,
                                                   TokenKind operation) {
  const TypeSyntax target = parse_checked_type();
  const SourceRange range{expression_range(left).begin, target.range.end};
  if (operation == TokenKind::kKwIs)
    return add_expression(Expression{range, TypeTestExpression{left, target}});
  return add_expression(Expression{range, CheckedCastExpression{left, target}});
}

ExpressionId ExpressionParser::build_binary_expression(ExpressionId left,
                                                       TokenKind operation,
                                                       ExpressionId right) {
  const auto range =
      join_ranges(expression_range(left), expression_range(right));
  if (is_assignment_operator(operation))
    return add_expression(
        Expression{range, AssignmentExpression{left, operation, right}});
  if (operation == TokenKind::kQuestionQuestion)
    return add_expression(
        Expression{range, NullCoalesceExpression{left, right}});
  return add_expression(
      Expression{range, BinaryExpression{left, operation, right}});
}

const Token& ExpressionParser::current() const noexcept {
  const std::size_t index =
      current_ < tokens_.size() ? current_ : tokens_.size() - 1;
  return tokens_[index];
}

const Token& ExpressionParser::advance() noexcept {
  const Token& token = current();
  if (!at_limit()) {
    ++current_;
  }
  return token;
}

bool ExpressionParser::match(TokenKind kind) noexcept {
  if (at_limit() || current().kind != kind) {
    return false;
  }
  advance();
  return true;
}

ExpressionId ExpressionParser::parse_unary_expression() {
  const TokenKind kind = current().kind;
  if (kind == TokenKind::kPlusPlus || kind == TokenKind::kMinusMinus ||
      kind == TokenKind::kBang || kind == TokenKind::kMinus ||
      kind == TokenKind::kPlus || kind == TokenKind::kTilde) {
    const Token& operation = advance();
    if (!enter_constant_expression())
      return make_invalid_expression(operation.range);
    ++nesting_;
    const ExpressionId operand = parse_unary_expression();
    --nesting_;
    return build_unary_expression(operation, operand);
  }
  return parse_postfix_expression();
}

ExpressionId ExpressionParser::build_unary_expression(const Token& operation,
                                                      ExpressionId operand) {
  const SourceRange range{operation.range.begin, expression_range(operand).end};
  if (operation.kind == TokenKind::kPlusPlus ||
      operation.kind == TokenKind::kMinusMinus)
    return add_expression(
        Expression{range, UpdateExpression{operation.kind, operand, false}});
  return add_expression(
      Expression{range, UnaryExpression{operation.kind, operand}});
}

ExpressionId ExpressionParser::parse_postfix_expression() {
  return parse_postfix_suffixes(parse_primary_expression());
}

ExpressionId ExpressionParser::parse_postfix_suffixes(ExpressionId expression) {
  while (!at_limit()) {
    const auto kind = current().kind;
    if (kind != TokenKind::kLeftParen && kind != TokenKind::kLeftBracket &&
        kind != TokenKind::kColonColon && kind != TokenKind::kDot &&
        kind != TokenKind::kQuestionDot && kind != TokenKind::kBang &&
        kind != TokenKind::kPlusPlus && kind != TokenKind::kMinusMinus)
      break;
    if (!enter_constant_expression())
      return make_invalid_expression(current().range);
    if (kind == TokenKind::kLeftParen)
      expression = parse_call_expression(expression);
    else if (kind == TokenKind::kLeftBracket)
      expression = parse_index_expression(expression);
    else {
      const auto previous = expression;
      expression = parse_access_suffix(expression);
      if (expression == previous || kind == TokenKind::kPlusPlus ||
          kind == TokenKind::kMinusMinus)
        break;
    }
  }
  return expression;
}

ExpressionId ExpressionParser::parse_access_suffix(ExpressionId expression) {
  const auto& operation = advance();
  if (operation.kind == TokenKind::kColonColon ||
      operation.kind == TokenKind::kDot ||
      operation.kind == TokenKind::kQuestionDot) {
    if (current().kind != TokenKind::kIdentifier) {
      const auto message = operation.kind == TokenKind::kColonColon
                               ? "expected meta query name after '::'"
                           : operation.kind == TokenKind::kDot
                               ? "expected member name after '.'"
                               : "expected member name after '?.'";
      diagnostics_.error(current().range, message);
      return expression;
    }
    const auto& member = advance();
    const SourceRange range{expression_range(expression).begin,
                            member.range.end};
    if (operation.kind == TokenKind::kColonColon)
      return add_expression(
          Expression{range, MetaAccessExpression{expression, member.lexeme}});
    if (operation.kind == TokenKind::kDot)
      return add_expression(
          Expression{range, MemberAccessExpression{expression, member.lexeme}});
    return add_expression(Expression{
        range, SafeMemberAccessExpression{expression, member.lexeme}});
  }
  const SourceRange range{expression_range(expression).begin,
                          operation.range.end};
  if (operation.kind == TokenKind::kBang)
    return add_expression(Expression{range, NullAssertExpression{expression}});
  return add_expression(
      Expression{range, UpdateExpression{operation.kind, expression, true}});
}

ExpressionId ExpressionParser::parse_primary_expression() {
  // Keep recursive dispatch frames separate from AST-construction temporaries.
  // This preserves the depth contract even on sanitizer builds with small
  // stacks.
  if (!at_limit()) {
    if (current().kind == TokenKind::kLeftParen)
      return parse_grouped_expression();
    if (is_primitive_type(current().kind) && current_ + 1 < limit_ &&
        tokens_[current_ + 1].kind == TokenKind::kColonColon) {
      return parse_integer_conversion_expression();
    }
    if (is_numeric_type_token(current().kind)) {
      return parse_numeric_conversion_expression();
    }
    if (current().kind == TokenKind::kLeftBracket)
      return parse_array_literal_expression();
  }
  return parse_atom_expression();
}

ExpressionId ExpressionParser::parse_grouped_expression() {
  const auto begin = advance().range.begin;
  const auto expression = parse_expression();
  auto end = expression_range(expression).end;
  if (match(TokenKind::kRightParen))
    end = tokens_[current_ - 1].range.end;
  else
    diagnostics_.error(current().range,
                       "expected ')' after parenthesized expression");
  return add_expression(
      Expression{SourceRange{begin, end}, ParenthesizedExpression{expression}});
}

ExpressionId ExpressionParser::parse_atom_expression() {
  if (at_limit()) {
    diagnostics_.error(current().range, "expected expression");
    return make_invalid_expression(point_range(current().range.begin));
  }

  const Token& token = current();
  if (constant_context_ &&
      (token.kind == TokenKind::kIntegerLiteral ||
       token.kind == TokenKind::kFloatLiteral) &&
      token.lexeme.size() > kMaxConstantLiteralBytes) {
    diagnostics_.error(token.range,
                       "constant numeric literal exceeds 4096 bytes");
    current_ = limit_;
    return make_invalid_expression(token.range);
  }
  if (match(TokenKind::kIdentifier)) {
    return add_expression(
        Expression{token.range, IdentifierExpression{token.lexeme}});
  }
  if (match(TokenKind::kKwSuper)) {
    return add_expression(Expression{token.range, SuperExpression{}});
  }

  LiteralKind literal_kind = LiteralKind::kInteger;
  bool is_literal = true;
  switch (token.kind) {
    case TokenKind::kIntegerLiteral:
      literal_kind = LiteralKind::kInteger;
      break;
    case TokenKind::kFloatLiteral:
      literal_kind = LiteralKind::kFloat;
      break;
    case TokenKind::kStringLiteral:
      literal_kind = LiteralKind::kString;
      break;
    case TokenKind::kCharacterLiteral:
      literal_kind = LiteralKind::kCharacter;
      break;
    case TokenKind::kKwTrue:
    case TokenKind::kKwFalse:
      literal_kind = LiteralKind::kBoolean;
      break;
    case TokenKind::kKwNull:
      literal_kind = LiteralKind::kNull;
      break;
    default:
      is_literal = false;
      break;
  }
  if (is_literal) {
    advance();
    return add_expression(
        Expression{token.range, LiteralExpression{literal_kind, token.lexeme}});
  }

  diagnostics_.error(token.range, "expected expression");
  if (token.kind != TokenKind::kSemicolon &&
      token.kind != TokenKind::kRightParen && token.kind != TokenKind::kComma &&
      token.kind != TokenKind::kRightBracket &&
      token.kind != TokenKind::kRightBrace) {
    advance();
  }
  return make_invalid_expression(token.range);
}

ExpressionId ExpressionParser::parse_numeric_conversion_expression() {
  const Token& target = advance();
  if (!match(TokenKind::kLeftParen)) {
    diagnostics_.error(current().range,
                       "expected '(' after numeric conversion type");
    return make_invalid_expression(target.range);
  }

  ExpressionId value{};
  if (current().kind == TokenKind::kRightParen) {
    value = make_invalid_expression(point_range(current().range.begin));
    diagnostics_.error(current().range,
                       "expected a value in numeric conversion");
  } else {
    value = parse_expression();
  }

  if (match(TokenKind::kComma)) {
    diagnostics_.error(tokens_[current_ - 1].range,
                       "numeric conversion requires exactly one value");
    while (!at_limit() && current().kind != TokenKind::kRightParen) {
      static_cast<void>(parse_expression());
      if (!match(TokenKind::kComma)) {
        break;
      }
    }
  }

  SourceLocation end = expression_range(value).end;
  if (match(TokenKind::kRightParen)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range,
                       "expected ')' after numeric conversion");
  }
  return build_numeric_conversion(target, value, end);
}

ExpressionId ExpressionParser::build_numeric_conversion(const Token& target,
                                                        ExpressionId value,
                                                        SourceLocation end) {
  const TypeSyntax target_type{target.lexeme, true, target.range};
  return add_expression(
      Expression{SourceRange{target.range.begin, end},
                 NumericConversionExpression{target_type, value}});
}

ExpressionId ExpressionParser::parse_integer_conversion_expression() {
  const Token& target = advance();
  advance();  // The caller established the `::` token.
  if (current().kind != TokenKind::kIdentifier) {
    diagnostics_.error(current().range,
                       "expected integer conversion mode after '::'");
    return make_invalid_expression(
        SourceRange{target.range.begin, current().range.end});
  }
  const Token& operation = advance();
  if (!match(TokenKind::kLeftParen)) {
    diagnostics_.error(current().range,
                       "expected '(' after integer conversion mode");
    return make_invalid_expression(
        SourceRange{target.range.begin, operation.range.end});
  }

  ExpressionId value{};
  if (current().kind == TokenKind::kRightParen) {
    value = make_invalid_expression(point_range(current().range.begin));
    diagnostics_.error(current().range,
                       "expected a value in integer conversion");
  } else {
    value = parse_expression();
  }

  if (match(TokenKind::kComma)) {
    diagnostics_.error(tokens_[current_ - 1].range,
                       "integer conversion requires exactly one value");
    while (!at_limit() && current().kind != TokenKind::kRightParen) {
      static_cast<void>(parse_expression());
      if (!match(TokenKind::kComma)) {
        break;
      }
    }
  }

  SourceLocation end = expression_range(value).end;
  if (match(TokenKind::kRightParen)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range,
                       "expected ')' after integer conversion");
  }
  return build_integer_conversion(target, operation, value, end);
}

ExpressionId ExpressionParser::build_integer_conversion(const Token& target,
                                                        const Token& operation,
                                                        ExpressionId value,
                                                        SourceLocation end) {
  const TypeSyntax target_type{target.lexeme, true, target.range};
  return add_expression(Expression{
      SourceRange{target.range.begin, end},
      IntegerConversionExpression{target_type, operation.lexeme, value}});
}

ExpressionId ExpressionParser::parse_call_expression(ExpressionId callee) {
  const SourceLocation begin = expression_range(callee).begin;
  advance();
  std::vector<ExpressionId> arguments;
  while (!at_limit() && current().kind != TokenKind::kRightParen) {
    arguments.push_back(parse_expression());
    if (match(TokenKind::kComma)) {
      if (current().kind == TokenKind::kRightParen) {
        diagnostics_.error(current().range, "expected argument after ','");
      }
      continue;
    }
    if (current().kind != TokenKind::kRightParen) {
      diagnostics_.error(current().range, "expected ',' or ')' after argument");
      while (!at_limit() && current().kind != TokenKind::kComma &&
             current().kind != TokenKind::kRightParen) {
        advance();
      }
      match(TokenKind::kComma);
    }
  }

  SourceLocation end = current().range.begin;
  if (match(TokenKind::kRightParen)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range, "expected ')' after arguments");
  }
  return add_expression(Expression{
      SourceRange{begin, end}, CallExpression{callee, std::move(arguments)}});
}

ExpressionId ExpressionParser::parse_array_literal_expression() {
  const SourceLocation begin = advance().range.begin;
  std::vector<ExpressionId> elements;
  while (!at_limit() && current().kind != TokenKind::kRightBracket) {
    elements.push_back(parse_expression());
    if (match(TokenKind::kComma)) {
      if (current().kind == TokenKind::kRightBracket) {
        diagnostics_.error(current().range, "expected array element after ','");
      }
      continue;
    }
    if (current().kind != TokenKind::kRightBracket) {
      diagnostics_.error(current().range,
                         "expected ',' or ']' after array element");
      while (!at_limit() && current().kind != TokenKind::kComma &&
             current().kind != TokenKind::kRightBracket) {
        advance();
      }
      match(TokenKind::kComma);
    }
  }

  SourceLocation end = current().range.begin;
  if (match(TokenKind::kRightBracket)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range, "expected ']' after array literal");
  }
  return add_expression(Expression{
      SourceRange{begin, end}, ArrayLiteralExpression{std::move(elements)}});
}

ExpressionId ExpressionParser::parse_index_expression(ExpressionId object) {
  const SourceLocation begin = expression_range(object).begin;
  advance();
  ExpressionId index{};
  if (current().kind == TokenKind::kRightBracket) {
    index = make_invalid_expression(point_range(current().range.begin));
    diagnostics_.error(current().range, "expected index expression after '['");
  } else {
    index = parse_expression();
  }

  SourceLocation end = expression_range(index).end;
  if (match(TokenKind::kRightBracket)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range, "expected ']' after index expression");
  }
  return add_expression(
      Expression{SourceRange{begin, end}, IndexExpression{object, index}});
}

TypeSyntax ExpressionParser::parse_checked_type() {
  if (at_limit() || !can_start_type(current().kind)) {
    diagnostics_.error(current().range,
                       "expected a type after checked type operator");
    return TypeSyntax{"<invalid>", false, point_range(current().range.begin)};
  }

  const Token& token = advance();
  SourceRange range = token.range;
  const bool inner_nullable = match(TokenKind::kQuestion);
  if (inner_nullable) {
    range.end = tokens_[current_ - 1].range.end;
  }
  bool is_array = false;
  while (match(TokenKind::kLeftBracket)) {
    if (is_array) {
      diagnostics_.error(tokens_[current_ - 1].range,
                         "multidimensional array types are not supported");
    }
    is_array = true;
    if (match(TokenKind::kRightBracket)) {
      range.end = tokens_[current_ - 1].range.end;
    } else {
      diagnostics_.error(current().range, "expected ']' in checked type");
      break;
    }
  }
  const bool outer_nullable = is_array && match(TokenKind::kQuestion);
  if (outer_nullable) {
    range.end = tokens_[current_ - 1].range.end;
  }
  return TypeSyntax{token.lexeme,
                    is_primitive_type(token.kind),
                    range,
                    is_array,
                    is_array ? outer_nullable : inner_nullable,
                    is_array && inner_nullable};
}

ExpressionId ExpressionParser::make_invalid_expression(SourceRange range) {
  return add_expression(Expression{range, InvalidExpression{}});
}

int ExpressionParser::binary_precedence(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::kEqual:
    case TokenKind::kPlusEqual:
    case TokenKind::kMinusEqual:
    case TokenKind::kStarEqual:
    case TokenKind::kSlashEqual:
    case TokenKind::kPercentEqual:
    case TokenKind::kShiftLeftEqual:
    case TokenKind::kShiftRightEqual:
    case TokenKind::kAmpersandEqual:
    case TokenKind::kPipeEqual:
    case TokenKind::kCaretEqual:
      return 1;
    case TokenKind::kQuestionQuestion:
      return 2;
    case TokenKind::kPipePipe:
      return 3;
    case TokenKind::kAmpersandAmpersand:
      return 4;
    case TokenKind::kPipe:
      return 5;
    case TokenKind::kCaret:
      return 6;
    case TokenKind::kAmpersand:
      return 7;
    case TokenKind::kEqualEqual:
    case TokenKind::kBangEqual:
      return 8;
    case TokenKind::kLess:
    case TokenKind::kLessEqual:
    case TokenKind::kGreater:
    case TokenKind::kGreaterEqual:
    case TokenKind::kKwIs:
    case TokenKind::kKwAs:
      return 9;
    case TokenKind::kShiftLeft:
    case TokenKind::kShiftRight:
      return 10;
    case TokenKind::kPlus:
    case TokenKind::kMinus:
      return 11;
    case TokenKind::kStar:
    case TokenKind::kSlash:
    case TokenKind::kPercent:
      return 12;
    default:
      return 0;
  }
}

bool ExpressionParser::is_assignment_operator(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::kEqual:
    case TokenKind::kPlusEqual:
    case TokenKind::kMinusEqual:
    case TokenKind::kStarEqual:
    case TokenKind::kSlashEqual:
    case TokenKind::kPercentEqual:
    case TokenKind::kShiftLeftEqual:
    case TokenKind::kShiftRightEqual:
    case TokenKind::kAmpersandEqual:
    case TokenKind::kPipeEqual:
    case TokenKind::kCaretEqual:
      return true;
    default:
      return false;
  }
}

bool ExpressionParser::is_right_associative(TokenKind kind) noexcept {
  return is_assignment_operator(kind) || kind == TokenKind::kQuestionQuestion;
}

SourceRange ExpressionParser::expression_range(ExpressionId id) const {
  return storage_.expression(id).range;
}

}  // namespace cloth
