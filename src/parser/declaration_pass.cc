#include "cloth/parser/declaration_pass.h"

#include "cloth/parser/syntax_facts.h"
#include "cloth/sema/visibility.h"

#include <string>
#include <utility>

namespace cloth {
namespace {

SourceRange file_range(std::span<const Token> tokens) noexcept {
  return SourceRange{SourceLocation{tokens.back().range.begin.file, 0, 1, 1},
                     tokens.back().range.end};
}

bool same_signature(const MemberSymbol& left,
                    const MemberSymbol& right) noexcept {
  if (left.parameters.size() != right.parameters.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.parameters.size(); ++index) {
    if (left.parameters[index].type.name != right.parameters[index].type.name ||
        left.parameters[index].type.is_array !=
            right.parameters[index].type.is_array) {
      return false;
    }
  }
  return true;
}

}  // namespace

DeclarationPass::DeclarationPass(const SourceFile& source,
                                 std::span<const Token> tokens,
                                 DiagnosticEngine& diagnostics)
    : source_(source),
      tokens_(tokens),
      diagnostics_(diagnostics),
      symbols_(std::string{source.stem()}, infer_visibility(source.stem()),
               file_range(tokens)) {}

DeclarationPassResult DeclarationPass::run() {
  if (!is_valid_identifier(source_.stem())) {
    diagnostics_.error(point_range(symbols_.range().begin),
                       "source file stem '" + std::string{source_.stem()} +
                           "' is not a valid Cloth identifier");
    is_valid_ = false;
  }

  bool saw_member = false;
  while (!at_end()) {
    const std::size_t before = current_;
    if (current().kind == TokenKind::kKwImport) {
      if (saw_member) {
        diagnostics_.error(current().range,
                           "imports must appear before member declarations");
        is_valid_ = false;
      }
      parse_import();
    } else if (current().kind == TokenKind::kKwModule) {
      diagnostics_.error(
          current().range,
          "module declarations are unnecessary; the source path defines the "
          "package");
      is_valid_ = false;
      synchronize_member();
    } else if (current().kind == TokenKind::kKwFunc) {
      saw_member = true;
      parse_function();
    } else if (is_nested_type_keyword(current().kind)) {
      saw_member = true;
      skip_deferred_nested_type();
    } else if (current().kind == TokenKind::kIdentifier &&
               peek(1).kind == TokenKind::kLeftParen) {
      saw_member = true;
      parse_constructor();
    } else if (current().kind == TokenKind::kKwFinal ||
               can_start_type(current().kind)) {
      saw_member = true;
      parse_field();
    } else {
      diagnostics_.error(
          current().range,
          "expected a field, function, or constructor declaration");
      is_valid_ = false;
      synchronize_member();
    }

    if (current_ == before) {
      advance();
    }
  }

  return DeclarationPassResult{std::move(symbols_), std::move(imports_),
                               std::move(outlines_), is_valid_};
}

bool DeclarationPass::at_end() const noexcept {
  return current().kind == TokenKind::kEof;
}

const Token& DeclarationPass::current() const noexcept {
  return tokens_[current_];
}

const Token& DeclarationPass::peek(std::size_t lookahead) const noexcept {
  const std::size_t remaining = tokens_.size() - current_;
  if (lookahead >= remaining) {
    return tokens_.back();
  }
  return tokens_[current_ + lookahead];
}

const Token& DeclarationPass::advance() noexcept {
  const Token& token = current();
  if (!at_end()) {
    ++current_;
  }
  return token;
}

bool DeclarationPass::match(TokenKind kind) noexcept {
  if (current().kind != kind) {
    return false;
  }
  advance();
  return true;
}

std::optional<TypeSyntax> DeclarationPass::parse_type() {
  if (!can_start_type(current().kind)) {
    diagnostics_.error(current().range, "expected a type");
    is_valid_ = false;
    return std::nullopt;
  }
  const Token& token = advance();
  SourceRange range = token.range;
  bool is_array = false;
  while (match(TokenKind::kLeftBracket)) {
    if (is_array) {
      diagnostics_.error(tokens_[current_ - 1].range,
                         "multidimensional array types are not supported");
      is_valid_ = false;
    }
    is_array = true;
    if (match(TokenKind::kRightBracket)) {
      range.end = tokens_[current_ - 1].range.end;
    } else {
      diagnostics_.error(current().range, "expected ']' in array type");
      is_valid_ = false;
      break;
    }
  }
  return TypeSyntax{token.lexeme, is_primitive_type(token.kind), range,
                    is_array};
}

std::vector<ParameterSymbol> DeclarationPass::parse_parameters(
    std::string_view declaration_name) {
  std::vector<ParameterSymbol> parameters;
  if (!match(TokenKind::kLeftParen)) {
    diagnostics_.error(
        current().range,
        "expected '(' after '" + std::string{declaration_name} + "'");
    is_valid_ = false;
    return parameters;
  }

  while (!at_end() && current().kind != TokenKind::kRightParen &&
         current().kind != TokenKind::kLeftBrace) {
    const std::size_t parameter_begin = current_;
    const bool is_final = match(TokenKind::kKwFinal);
    auto type = parse_type();
    if (!type) {
      while (!at_end() && current().kind != TokenKind::kComma &&
             current().kind != TokenKind::kRightParen &&
             current().kind != TokenKind::kLeftBrace) {
        advance();
      }
      if (match(TokenKind::kComma)) {
        continue;
      }
      break;
    }

    if (current().kind != TokenKind::kIdentifier) {
      diagnostics_.error(current().range,
                         "expected parameter name after type '" +
                             std::string{type->name} + "'");
      is_valid_ = false;
      while (!at_end() && current().kind != TokenKind::kComma &&
             current().kind != TokenKind::kRightParen &&
             current().kind != TokenKind::kLeftBrace) {
        advance();
      }
      if (match(TokenKind::kComma)) {
        continue;
      }
      break;
    }

    const Token& name = advance();
    parameters.push_back(ParameterSymbol{
        *type, name.lexeme,
        SourceRange{tokens_[parameter_begin].range.begin, name.range.end},
        is_final});

    if (match(TokenKind::kComma)) {
      if (current().kind == TokenKind::kRightParen) {
        diagnostics_.error(current().range, "expected parameter after ','");
        is_valid_ = false;
      }
      continue;
    }
    if (current().kind != TokenKind::kRightParen) {
      diagnostics_.error(current().range,
                         "expected ',' or ')' after parameter");
      is_valid_ = false;
      if (current().kind == TokenKind::kKwFinal ||
          can_start_type(current().kind)) {
        continue;
      }
      while (!at_end() && current().kind != TokenKind::kComma &&
             current().kind != TokenKind::kRightParen &&
             current().kind != TokenKind::kLeftBrace) {
        advance();
      }
      match(TokenKind::kComma);
    }
  }

  if (!match(TokenKind::kRightParen)) {
    diagnostics_.error(current().range, "expected ')' after parameter list");
    is_valid_ = false;
  }
  return parameters;
}

std::optional<TokenIndexRange> DeclarationPass::locate_body(
    std::string_view declaration_name) {
  if (current().kind != TokenKind::kLeftBrace) {
    diagnostics_.error(current().range, "expected '{' to begin body of '" +
                                            std::string{declaration_name} +
                                            "'");
    is_valid_ = false;
    return std::nullopt;
  }

  const std::size_t body_begin = current_;
  std::size_t depth = 0;
  do {
    if (current().kind == TokenKind::kLeftBrace) {
      ++depth;
    } else if (current().kind == TokenKind::kRightBrace) {
      --depth;
    }
    advance();
  } while (!at_end() && depth != 0);

  if (depth != 0) {
    diagnostics_.error(
        tokens_[body_begin].range,
        "unterminated body for '" + std::string{declaration_name} + "'");
    is_valid_ = false;
    return TokenIndexRange{body_begin, tokens_.size() - 1};
  }
  return TokenIndexRange{body_begin, current_};
}

void DeclarationPass::parse_field() {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  const std::size_t begin = current_;
  const bool is_final = match(TokenKind::kKwFinal);
  auto type = parse_type();
  if (!type) {
    synchronize_member();
    return;
  }

  if (current().kind != TokenKind::kIdentifier) {
    diagnostics_.error(current().range, "expected field name after type '" +
                                            std::string{type->name} + "'");
    is_valid_ = false;
    synchronize_member();
    return;
  }
  const Token& name = advance();

  std::optional<TokenIndexRange> initializer;
  if (match(TokenKind::kEqual)) {
    const std::size_t initializer_begin = current_;
    std::size_t parenthesis_depth = 0;
    while (!at_end()) {
      if (current().kind == TokenKind::kLeftParen ||
          current().kind == TokenKind::kLeftBracket) {
        ++parenthesis_depth;
      } else if ((current().kind == TokenKind::kRightParen ||
                  current().kind == TokenKind::kRightBracket) &&
                 parenthesis_depth != 0) {
        --parenthesis_depth;
      }
      if (parenthesis_depth == 0 &&
          (current().kind == TokenKind::kSemicolon ||
           current().kind == TokenKind::kKwFunc ||
           is_nested_type_keyword(current().kind) ||
           (current().kind == TokenKind::kKwFinal &&
            can_start_type(peek(1).kind)) ||
           (current_ != initializer_begin && can_start_type(current().kind) &&
            peek(1).kind == TokenKind::kIdentifier))) {
        break;
      }
      advance();
    }
    initializer = TokenIndexRange{initializer_begin, current_};
    if (initializer_begin == current_) {
      diagnostics_.error(current().range,
                         "expected expression after '=' in field declaration");
      is_valid_ = false;
    }
  }

  if (!match(TokenKind::kSemicolon)) {
    diagnostics_.error(current().range, "expected ';' after field declaration");
    is_valid_ = false;
  }
  const bool declaration_valid =
      diagnostics_.diagnostics().size() == diagnostic_count;

  const std::size_t end = current_ == begin ? begin : current_ - 1;
  const SourceRange range = range_from(begin, end);
  MemberSymbol symbol{name.lexeme,
                      DeclarationKind::kField,
                      infer_visibility(name.lexeme),
                      range,
                      {},
                      *type,
                      declaration_valid};
  symbol.is_final = is_final;
  const std::size_t symbol_index = add_symbol(std::move(symbol));
  outlines_.push_back(MemberOutline{DeclarationKind::kField, symbol_index,
                                    begin, initializer, std::nullopt, range,
                                    declaration_valid});
}

void DeclarationPass::parse_function() {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  const std::size_t begin = current_;
  advance();
  if (current().kind != TokenKind::kIdentifier) {
    diagnostics_.error(current().range, "expected function name after 'func'");
    is_valid_ = false;
    synchronize_member();
    return;
  }
  const Token& name = advance();
  auto parameters = parse_parameters(name.lexeme);

  std::optional<TypeSyntax> return_type;
  if (match(TokenKind::kColon)) {
    return_type = parse_type();
  }

  const auto body = locate_body(name.lexeme);
  const std::size_t end = current_ == begin ? begin : current_ - 1;
  const SourceRange range = range_from(begin, end);
  const bool declaration_valid =
      body.has_value() && diagnostics_.diagnostics().size() == diagnostic_count;
  MemberSymbol symbol{name.lexeme,
                      DeclarationKind::kFunction,
                      infer_visibility(name.lexeme),
                      range,
                      std::move(parameters),
                      return_type,
                      declaration_valid};
  const std::size_t symbol_index = add_symbol(std::move(symbol));
  outlines_.push_back(MemberOutline{DeclarationKind::kFunction, symbol_index,
                                    begin, std::nullopt, body, range,
                                    declaration_valid});
}

void DeclarationPass::parse_constructor() {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  const std::size_t begin = current_;
  const Token& name = advance();
  auto parameters = parse_parameters(name.lexeme);
  const auto body = locate_body(name.lexeme);

  bool declaration_valid = body.has_value();
  if (name.lexeme != source_.stem()) {
    diagnostics_.error(name.range, "constructor '" + std::string{name.lexeme} +
                                       "' must match implicit class '" +
                                       std::string{source_.stem()} + "'");
    is_valid_ = false;
    declaration_valid = false;
  }
  declaration_valid = declaration_valid &&
                      diagnostics_.diagnostics().size() == diagnostic_count;

  const std::size_t end = current_ == begin ? begin : current_ - 1;
  const SourceRange range = range_from(begin, end);
  MemberSymbol symbol{name.lexeme,           DeclarationKind::kConstructor,
                      symbols_.visibility(), range,
                      std::move(parameters), std::nullopt,
                      declaration_valid};
  const std::size_t symbol_index = add_symbol(std::move(symbol));
  outlines_.push_back(MemberOutline{DeclarationKind::kConstructor, symbol_index,
                                    begin, std::nullopt, body, range,
                                    declaration_valid});
}

void DeclarationPass::parse_import() {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  const Token& keyword = advance();
  std::vector<std::string_view> package_segments;
  std::string type_name;
  std::string local_name;
  ImportKind kind = ImportKind::kType;

  if (current().kind != TokenKind::kIdentifier) {
    diagnostics_.error(current().range,
                       "expected a package or type name after 'import'");
    is_valid_ = false;
  } else {
    const Token& first = advance();
    if (match(TokenKind::kColonColon)) {
      package_segments.push_back(first.lexeme);
      if (current().kind == TokenKind::kIdentifier) {
        type_name = std::string{advance().lexeme};
      } else {
        diagnostics_.error(current().range, "expected a type name after '::'");
        is_valid_ = false;
      }
    } else if (match(TokenKind::kDot)) {
      package_segments.push_back(first.lexeme);
      while (!at_end()) {
        if (match(TokenKind::kStar)) {
          kind = ImportKind::kWildcard;
          break;
        }
        if (current().kind != TokenKind::kIdentifier) {
          diagnostics_.error(current().range,
                             "expected a package name or '*' after '.'");
          is_valid_ = false;
          break;
        }
        package_segments.push_back(advance().lexeme);
        if (match(TokenKind::kColonColon)) {
          if (current().kind == TokenKind::kIdentifier) {
            type_name = std::string{advance().lexeme};
          } else {
            diagnostics_.error(current().range,
                               "expected a type name after '::'");
            is_valid_ = false;
          }
          break;
        }
        if (!match(TokenKind::kDot)) {
          diagnostics_.error(
              current().range,
              "expected '::' for a type or '.*' for a wildcard import");
          is_valid_ = false;
          break;
        }
      }
    } else {
      type_name = std::string{first.lexeme};
    }
  }

  if (kind == ImportKind::kType && match(TokenKind::kKwAs)) {
    if (current().kind == TokenKind::kIdentifier) {
      local_name = std::string{advance().lexeme};
    } else {
      diagnostics_.error(current().range, "expected an alias name after 'as'");
      is_valid_ = false;
    }
  } else if (kind == ImportKind::kWildcard &&
             current().kind == TokenKind::kKwAs) {
    diagnostics_.error(current().range,
                       "wildcard imports cannot have an alias");
    is_valid_ = false;
    advance();
    if (current().kind == TokenKind::kIdentifier) {
      advance();
    }
  }

  SourceLocation end = current().range.begin;
  if (match(TokenKind::kSemicolon)) {
    end = tokens_[current_ - 1].range.end;
  } else {
    diagnostics_.error(current().range, "expected ';' after import");
    is_valid_ = false;
    while (!at_end() && !match(TokenKind::kSemicolon)) {
      advance();
    }
    if (current_ != 0) {
      end = tokens_[current_ - 1].range.end;
    }
  }

  std::string package_name;
  for (const std::string_view segment : package_segments) {
    if (!package_name.empty()) {
      package_name += '.';
    }
    package_name += segment;
  }
  if (local_name.empty()) {
    local_name = type_name;
  }
  const bool import_valid =
      diagnostics_.diagnostics().size() == diagnostic_count &&
      (kind == ImportKind::kWildcard || !type_name.empty());
  imports_.push_back(ImportDecl{kind, std::move(package_name),
                                std::move(type_name), std::move(local_name),
                                SourceRange{keyword.range.begin, end},
                                import_valid});
}

void DeclarationPass::skip_deferred_nested_type() {
  diagnostics_.error(
      current().range,
      "nested type declarations are reserved but not supported in Stage 1.0");
  is_valid_ = false;
  advance();
  if (current().kind == TokenKind::kIdentifier) {
    advance();
  }
  if (current().kind == TokenKind::kLeftBrace) {
    static_cast<void>(locate_body("nested type"));
  } else {
    while (!at_end() && !match(TokenKind::kSemicolon)) {
      advance();
    }
  }
}

void DeclarationPass::synchronize_member() {
  while (!at_end()) {
    if (match(TokenKind::kSemicolon)) {
      return;
    }
    if (looks_like_member_start(current_)) {
      return;
    }
    advance();
  }
}

std::size_t DeclarationPass::add_symbol(MemberSymbol symbol) {
  if (has_duplicate(symbol)) {
    symbol.is_valid = false;
    is_valid_ = false;
  }
  return symbols_.add(std::move(symbol));
}

bool DeclarationPass::has_duplicate(const MemberSymbol& symbol) {
  for (const MemberSymbol& previous : symbols_.members()) {
    if (previous.name != symbol.name) {
      continue;
    }

    const bool overloadable = previous.kind == symbol.kind &&
                              (symbol.kind == DeclarationKind::kFunction ||
                               symbol.kind == DeclarationKind::kConstructor);
    if (overloadable && !same_signature(previous, symbol)) {
      continue;
    }

    std::string message;
    if (overloadable) {
      message = "duplicate " + std::string{declaration_kind_name(symbol.kind)} +
                " signature for '" + std::string{symbol.name} + "'";
    } else {
      message =
          "member '" + std::string{symbol.name} + "' conflicts with previous " +
          std::string{declaration_kind_name(previous.kind)} + " declaration";
    }
    diagnostics_.error(symbol.range, std::move(message));
    diagnostics_.note(previous.range, "previous declaration is here");
    return true;
  }
  return false;
}

bool DeclarationPass::looks_like_member_start(
    std::size_t index) const noexcept {
  const TokenKind kind = tokens_[index].kind;
  if (kind == TokenKind::kKwFunc || kind == TokenKind::kKwFinal ||
      is_nested_type_keyword(kind)) {
    return true;
  }
  if (!can_start_type(kind)) {
    return false;
  }
  const std::size_t next = index + 1;
  if (next >= tokens_.size()) {
    return false;
  }
  return tokens_[next].kind == TokenKind::kIdentifier ||
         (tokens_[next].kind == TokenKind::kLeftBracket &&
          next + 2 < tokens_.size() &&
          tokens_[next + 1].kind == TokenKind::kRightBracket &&
          tokens_[next + 2].kind == TokenKind::kIdentifier) ||
         (kind == TokenKind::kIdentifier &&
          tokens_[next].kind == TokenKind::kLeftParen);
}

SourceRange DeclarationPass::range_from(std::size_t begin,
                                        std::size_t end) const noexcept {
  return SourceRange{tokens_[begin].range.begin, tokens_[end].range.end};
}

}  // namespace cloth
