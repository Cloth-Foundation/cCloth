#include "cloth/parser/expression_parser.h"

#include <utility>

namespace cloth {
namespace {

SourceRange join_ranges(SourceRange left, SourceRange right) noexcept {
  return SourceRange{left.begin, right.end};
}

}  // namespace

ExpressionParser::ExpressionParser(std::span<const Token> tokens,
                                   std::size_t& current, std::size_t limit,
                                   AstStorage& storage,
                                   DiagnosticEngine& diagnostics) noexcept
    : tokens_(tokens),
      current_(current),
      limit_(limit),
      storage_(storage),
      diagnostics_(diagnostics) {}

ExpressionId ExpressionParser::parse_expression(int minimum_precedence) {
  ExpressionId left = parse_unary_expression();
  while (!at_limit()) {
    const TokenKind operation = current().kind;
    const int precedence = binary_precedence(operation);
    if (precedence < minimum_precedence) {
      break;
    }
    advance();
    const int next_precedence =
        is_right_associative(operation) ? precedence : precedence + 1;
    const ExpressionId right = parse_expression(next_precedence);
    const SourceRange range =
        join_ranges(expression_range(left), expression_range(right));
    if (operation == TokenKind::kEqual) {
      left = storage_.add_expression(
          Expression{range, AssignmentExpression{left, operation, right}});
    } else {
      left = storage_.add_expression(
          Expression{range, BinaryExpression{left, operation, right}});
    }
  }
  return left;
}

bool ExpressionParser::at_limit() const noexcept { return current_ >= limit_; }

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
  if (kind == TokenKind::kBang || kind == TokenKind::kMinus ||
      kind == TokenKind::kPlus || kind == TokenKind::kTilde) {
    const Token& operation = advance();
    const ExpressionId operand = parse_unary_expression();
    return storage_.add_expression(Expression{
        SourceRange{operation.range.begin, expression_range(operand).end},
        UnaryExpression{operation.kind, operand}});
  }
  return parse_postfix_expression();
}

ExpressionId ExpressionParser::parse_postfix_expression() {
  ExpressionId expression = parse_primary_expression();
  while (!at_limit()) {
    if (current().kind == TokenKind::kLeftParen) {
      expression = parse_call_expression(expression);
      continue;
    }
    if (match(TokenKind::kDot)) {
      if (current().kind != TokenKind::kIdentifier) {
        diagnostics_.error(current().range, "expected member name after '.'");
        break;
      }
      const Token& member = advance();
      const SourceRange range{expression_range(expression).begin,
                              member.range.end};
      expression = storage_.add_expression(
          Expression{range, MemberAccessExpression{expression, member.lexeme}});
      continue;
    }
    break;
  }
  return expression;
}

ExpressionId ExpressionParser::parse_primary_expression() {
  if (at_limit()) {
    diagnostics_.error(current().range, "expected expression");
    return make_invalid_expression(point_range(current().range.begin));
  }

  const Token& token = current();
  if (match(TokenKind::kIdentifier)) {
    return storage_.add_expression(
        Expression{token.range, IdentifierExpression{token.lexeme}});
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
    return storage_.add_expression(
        Expression{token.range, LiteralExpression{literal_kind, token.lexeme}});
  }

  if (match(TokenKind::kLeftParen)) {
    const SourceLocation begin = token.range.begin;
    const ExpressionId expression = parse_expression();
    SourceLocation end = expression_range(expression).end;
    if (match(TokenKind::kRightParen)) {
      end = tokens_[current_ - 1].range.end;
    } else {
      diagnostics_.error(current().range,
                         "expected ')' after parenthesized expression");
    }
    return storage_.add_expression(Expression{
        SourceRange{begin, end}, ParenthesizedExpression{expression}});
  }

  diagnostics_.error(token.range, "expected expression");
  if (token.kind != TokenKind::kSemicolon &&
      token.kind != TokenKind::kRightParen && token.kind != TokenKind::kComma &&
      token.kind != TokenKind::kRightBrace) {
    advance();
  }
  return make_invalid_expression(token.range);
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
  return storage_.add_expression(Expression{
      SourceRange{begin, end}, CallExpression{callee, std::move(arguments)}});
}

ExpressionId ExpressionParser::make_invalid_expression(SourceRange range) {
  return storage_.add_expression(Expression{range, InvalidExpression{}});
}

int ExpressionParser::binary_precedence(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::kEqual:
      return 1;
    case TokenKind::kPipePipe:
      return 2;
    case TokenKind::kAmpersandAmpersand:
      return 3;
    case TokenKind::kEqualEqual:
    case TokenKind::kBangEqual:
      return 4;
    case TokenKind::kLess:
    case TokenKind::kLessEqual:
    case TokenKind::kGreater:
    case TokenKind::kGreaterEqual:
      return 5;
    case TokenKind::kPlus:
    case TokenKind::kMinus:
      return 6;
    case TokenKind::kStar:
    case TokenKind::kSlash:
    case TokenKind::kPercent:
      return 7;
    default:
      return 0;
  }
}

bool ExpressionParser::is_right_associative(TokenKind kind) noexcept {
  return kind == TokenKind::kEqual;
}

SourceRange ExpressionParser::expression_range(ExpressionId id) const {
  return storage_.expression(id).range;
}

}  // namespace cloth
