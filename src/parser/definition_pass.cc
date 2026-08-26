#include "cloth/parser/definition_pass.h"

#include "cloth/parser/expression_parser.h"
#include "cloth/parser/syntax_facts.h"

#include <string>
#include <utility>

namespace cloth {
namespace {

SourceRange range_ending_at(SourceRange begin, SourceLocation end) noexcept {
  return SourceRange{begin.begin, end};
}

}  // namespace

DefinitionPass::DefinitionPass(const SourceFile& source,
                               std::span<const Token> tokens,
                               const FileClassSymbols& symbols,
                               std::span<const MemberOutline> outlines,
                               DiagnosticEngine& diagnostics)
    : tokens_(tokens),
      symbols_(symbols),
      outlines_(outlines),
      diagnostics_(diagnostics),
      file_class_{symbols.name(),
                  source.display_path(),
                  symbols.visibility(),
                  symbols.range(),
                  {},
                  {},
                  {},
                  {},
                  {},
                  true} {}

FileClassDecl DefinitionPass::run() {
  for (const MemberOutline& outline : outlines_) {
    const MemberSymbol& symbol = symbols_.member(outline.symbol_index);
    switch (outline.kind) {
      case DeclarationKind::kField:
        build_field(outline, symbol);
        break;
      case DeclarationKind::kFunction:
        build_function(outline, symbol);
        break;
      case DeclarationKind::kConstructor:
        build_constructor(outline, symbol);
        break;
      case DeclarationKind::kNestedType:
        break;
    }
  }
  return std::move(file_class_);
}

bool DefinitionPass::at_limit() const noexcept { return current_ >= limit_; }

const Token& DefinitionPass::current() const noexcept {
  const std::size_t index =
      current_ < tokens_.size() ? current_ : tokens_.size() - 1;
  return tokens_[index];
}

const Token& DefinitionPass::peek(std::size_t lookahead) const noexcept {
  if (lookahead >= limit_ - current_) {
    const std::size_t index =
        limit_ < tokens_.size() ? limit_ : tokens_.size() - 1;
    return tokens_[index];
  }
  return tokens_[current_ + lookahead];
}

const Token& DefinitionPass::advance() noexcept {
  const Token& token = current();
  if (!at_limit()) {
    ++current_;
  }
  return token;
}

bool DefinitionPass::match(TokenKind kind) noexcept {
  if (at_limit() || current().kind != kind) {
    return false;
  }
  advance();
  return true;
}

bool DefinitionPass::expect(TokenKind kind, std::string_view message) {
  if (match(kind)) {
    return true;
  }
  diagnostics_.error(current().range, std::string{message});
  return false;
}

void DefinitionPass::build_field(const MemberOutline& outline,
                                 const MemberSymbol& symbol) {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  std::optional<ExpressionId> initializer;
  if (outline.initializer_tokens) {
    current_ = outline.initializer_tokens->begin;
    limit_ = outline.initializer_tokens->end;
    if (current_ < limit_) {
      initializer = parse_expression();
      if (!at_limit()) {
        diagnostics_.error(current().range,
                           "unexpected token in field initializer");
      }
    }
  }

  const TypeSyntax type = symbol.declared_type.value_or(
      TypeSyntax{"<invalid>", false, point_range(outline.range.begin)});
  const bool is_valid =
      symbol.is_valid && diagnostics_.diagnostics().size() == diagnostic_count;
  const std::size_t index = file_class_.fields.size();
  file_class_.fields.push_back(FieldDecl{type, symbol.name, symbol.visibility,
                                         initializer, outline.range, is_valid});
  file_class_.member_order.push_back(
      MemberReference{DeclarationKind::kField, index});
  file_class_.is_valid = file_class_.is_valid && is_valid;
}

void DefinitionPass::build_function(const MemberOutline& outline,
                                    const MemberSymbol& symbol) {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  BlockId body = file_class_.storage.add_block(
      Block{point_range(outline.range.end), {}, false});
  if (outline.body_tokens) {
    current_ = outline.body_tokens->begin;
    limit_ = outline.body_tokens->end;
    body = parse_block();
  }

  const bool is_valid = symbol.is_valid &&
                        file_class_.storage.block(body).is_valid &&
                        diagnostics_.diagnostics().size() == diagnostic_count;
  const std::size_t index = file_class_.functions.size();
  file_class_.functions.push_back(
      FunctionDecl{symbol.name, symbol.visibility, copy_parameters(symbol),
                   symbol.declared_type, body, outline.range, is_valid});
  file_class_.member_order.push_back(
      MemberReference{DeclarationKind::kFunction, index});
  file_class_.is_valid = file_class_.is_valid && is_valid;
}

void DefinitionPass::build_constructor(const MemberOutline& outline,
                                       const MemberSymbol& symbol) {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  BlockId body = file_class_.storage.add_block(
      Block{point_range(outline.range.end), {}, false});
  if (outline.body_tokens) {
    current_ = outline.body_tokens->begin;
    limit_ = outline.body_tokens->end;
    body = parse_block();
  }

  const bool is_valid = symbol.is_valid &&
                        file_class_.storage.block(body).is_valid &&
                        diagnostics_.diagnostics().size() == diagnostic_count;
  const std::size_t index = file_class_.constructors.size();
  file_class_.constructors.push_back(
      ConstructorDecl{symbol.name, symbol.visibility, copy_parameters(symbol),
                      body, outline.range, is_valid});
  file_class_.member_order.push_back(
      MemberReference{DeclarationKind::kConstructor, index});
  file_class_.is_valid = file_class_.is_valid && is_valid;
}

std::vector<ParameterDecl> DefinitionPass::copy_parameters(
    const MemberSymbol& symbol) const {
  std::vector<ParameterDecl> parameters;
  parameters.reserve(symbol.parameters.size());
  for (const ParameterSymbol& parameter : symbol.parameters) {
    parameters.push_back(
        ParameterDecl{parameter.type, parameter.name, parameter.range});
  }
  return parameters;
}

BlockId DefinitionPass::parse_block() {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  if (!match(TokenKind::kLeftBrace)) {
    diagnostics_.error(current().range, "expected '{' to begin block");
    return file_class_.storage.add_block(
        Block{point_range(current().range.begin), {}, false});
  }
  const SourceRange open_range = tokens_[current_ - 1].range;
  std::vector<StatementId> statements;

  while (!at_limit() && current().kind != TokenKind::kRightBrace) {
    const std::size_t before = current_;
    statements.push_back(parse_statement());
    if (current_ == before) {
      advance();
    }
  }

  SourceLocation end = current().range.begin;
  bool has_close = false;
  if (match(TokenKind::kRightBrace)) {
    end = tokens_[current_ - 1].range.end;
    has_close = true;
  }
  const bool is_valid =
      has_close && diagnostics_.diagnostics().size() == diagnostic_count;
  return file_class_.storage.add_block(
      Block{range_ending_at(open_range, end), std::move(statements), is_valid});
}

StatementId DefinitionPass::parse_statement() {
  if (current().kind == TokenKind::kLeftBrace) {
    const BlockId block = parse_block();
    return file_class_.storage.add_statement(Statement{
        file_class_.storage.block(block).range, NestedBlockStatement{block}});
  }
  if (current().kind == TokenKind::kKwReturn) {
    return parse_return_statement();
  }
  if (current().kind == TokenKind::kKwIf) {
    return parse_if_statement();
  }
  if (can_start_type(current().kind) &&
      peek(1).kind == TokenKind::kIdentifier) {
    return parse_local_variable();
  }
  return parse_expression_statement();
}

StatementId DefinitionPass::parse_local_variable() {
  const Token& type_token = advance();
  const TypeSyntax type{type_token.lexeme, is_primitive_type(type_token.kind),
                        type_token.range};
  const Token& name = advance();
  std::optional<ExpressionId> initializer;
  if (match(TokenKind::kEqual)) {
    initializer = parse_expression();
  }

  SourceLocation end =
      initializer ? expression_range(*initializer).end : name.range.end;
  if (match(TokenKind::kSemicolon)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range,
                       "expected ';' after local variable declaration");
  }

  const SourceRange range{type_token.range.begin, end};
  return file_class_.storage.add_statement(
      Statement{range, LocalVariableStatement{type, name.lexeme, initializer}});
}

StatementId DefinitionPass::parse_return_statement() {
  const Token& keyword = advance();
  std::optional<ExpressionId> value;
  if (current().kind != TokenKind::kSemicolon &&
      current().kind != TokenKind::kRightBrace && !at_limit()) {
    value = parse_expression();
  }

  SourceLocation end = value ? expression_range(*value).end : keyword.range.end;
  if (match(TokenKind::kSemicolon)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range, "expected ';' after return statement");
  }
  return file_class_.storage.add_statement(
      Statement{SourceRange{keyword.range.begin, end}, ReturnStatement{value}});
}

StatementId DefinitionPass::parse_if_statement() {
  const Token& keyword = advance();
  expect(TokenKind::kLeftParen, "expected '(' after 'if'");
  const ExpressionId condition = parse_expression();
  expect(TokenKind::kRightParen, "expected ')' after if condition");

  BlockId then_block = file_class_.storage.add_block(
      Block{point_range(current().range.begin), {}, false});
  if (current().kind == TokenKind::kLeftBrace) {
    then_block = parse_block();
  } else {
    diagnostics_.error(current().range, "expected '{' to begin if body");
  }

  std::optional<BlockId> else_block;
  if (match(TokenKind::kKwElse)) {
    if (current().kind == TokenKind::kLeftBrace) {
      else_block = parse_block();
    } else {
      diagnostics_.error(current().range, "expected '{' to begin else body");
    }
  }

  SourceLocation end = else_block
                           ? file_class_.storage.block(*else_block).range.end
                           : file_class_.storage.block(then_block).range.end;
  return file_class_.storage.add_statement(
      Statement{SourceRange{keyword.range.begin, end},
                IfStatement{condition, then_block, else_block}});
}

StatementId DefinitionPass::parse_expression_statement() {
  const SourceLocation begin = current().range.begin;
  const ExpressionId expression = parse_expression();
  SourceLocation end = expression_range(expression).end;
  if (match(TokenKind::kSemicolon)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range,
                       "expected ';' after expression statement");
    synchronize_statement();
  }
  return file_class_.storage.add_statement(
      Statement{SourceRange{begin, end}, ExpressionStatement{expression}});
}

void DefinitionPass::synchronize_statement() {
  while (!at_limit() && current().kind != TokenKind::kRightBrace) {
    if (match(TokenKind::kSemicolon)) {
      return;
    }
    if (current().kind == TokenKind::kKwReturn ||
        current().kind == TokenKind::kKwIf ||
        current().kind == TokenKind::kLeftBrace ||
        (can_start_type(current().kind) &&
         peek(1).kind == TokenKind::kIdentifier)) {
      return;
    }
    advance();
  }
}

ExpressionId DefinitionPass::parse_expression() {
  return ExpressionParser{tokens_, current_, limit_, file_class_.storage,
                          diagnostics_}
      .parse_expression();
}

SourceRange DefinitionPass::expression_range(ExpressionId id) const {
  return file_class_.storage.expression(id).range;
}

}  // namespace cloth
