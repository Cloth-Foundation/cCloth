// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/parser/declaration_pass.h"

#include "cloth/parser/syntax_facts.h"
#include "cloth/sema/visibility.h"

#include <string>
#include <unordered_map>
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

bool is_file_class_modifier(TokenKind kind) noexcept {
  return kind == TokenKind::kKwAbstract || kind == TokenKind::kKwSealed;
}

bool is_function_modifier(TokenKind kind) noexcept {
  return kind == TokenKind::kKwAbstract || kind == TokenKind::kKwOverride ||
         kind == TokenKind::kKwStatic || kind == TokenKind::kKwFinal;
}

bool looks_like_file_type_declaration(std::span<const Token> tokens,
                                      std::size_t index) noexcept {
  while (index < tokens.size() && is_file_class_modifier(tokens[index].kind)) {
    ++index;
  }
  return index < tokens.size() &&
         (tokens[index].kind == TokenKind::kKwClass ||
          tokens[index].kind == TokenKind::kKwInterface ||
          tokens[index].kind == TokenKind::kKwEnum ||
          tokens[index].kind == TokenKind::kKwStruct);
}

bool looks_like_function_declaration(std::span<const Token> tokens,
                                     std::size_t index) noexcept {
  while (index < tokens.size() && is_function_modifier(tokens[index].kind)) {
    ++index;
  }
  return index < tokens.size() && tokens[index].kind == TokenKind::kKwFunc;
}

std::string lowercase_constructor_name(std::string_view class_name) {
  std::string result{class_name};
  if (!result.empty() && result.front() >= 'A' && result.front() <= 'Z') {
    result.front() = static_cast<char>(result.front() - 'A' + 'a');
  }
  return result;
}

bool is_constructor_name(std::string_view name, std::string_view class_name) {
  if (name == class_name || name == lowercase_constructor_name(class_name)) {
    return true;
  }
  return !class_name.starts_with('_') && name == "_" + std::string{class_name};
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
    if (class_body_started_ && current().kind == TokenKind::kRightBrace) {
      advance();
      class_body_closed_ = true;
      if (!at_end()) {
        diagnostics_.error(current().range,
                           "expected end of file after file type declaration");
        is_valid_ = false;
        while (!at_end()) {
          advance();
        }
      }
      break;
    }
    if (current().kind == TokenKind::kKwImport) {
      if (saw_member || has_explicit_class_declaration_) {
        diagnostics_.error(
            current().range,
            has_explicit_class_declaration_
                ? "imports must appear before the file type declaration"
                : "imports must appear before member declarations");
        is_valid_ = false;
      }
      parse_import();
    } else if (!saw_member && !has_explicit_class_declaration_ &&
               looks_like_file_type_declaration(tokens_, current_)) {
      parse_file_type_declaration();
    } else if (looks_like_function_declaration(tokens_, current_)) {
      saw_member = true;
      parse_function();
    } else if (is_nested_type_keyword(current().kind)) {
      saw_member = true;
      skip_deferred_nested_type();
    } else if (current().kind == TokenKind::kIdentifier &&
               peek(1).kind == TokenKind::kLeftParen) {
      saw_member = true;
      if (file_type_kind_ == FileTypeKind::kInterface) {
        diagnostics_.error(current().range,
                           "interfaces cannot declare constructors");
        is_valid_ = false;
      }
      parse_constructor();
    } else if (current().kind == TokenKind::kKwStatic ||
               current().kind == TokenKind::kKwFinal ||
               can_start_type(current().kind)) {
      saw_member = true;
      if (file_type_kind_ == FileTypeKind::kInterface) {
        diagnostics_.error(current().range, "interfaces cannot declare fields");
        is_valid_ = false;
      }
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

  if (class_body_started_ && !class_body_closed_) {
    diagnostics_.error(current().range,
                       "expected '}' to close file type declaration");
    is_valid_ = false;
  }

  return DeclarationPassResult{std::move(symbols_),
                               std::move(imports_),
                               std::move(outlines_),
                               base_class_,
                               has_explicit_class_declaration_,
                               class_is_abstract_,
                               class_is_sealed_,
                               file_type_kind_,
                               std::move(interfaces_),
                               is_valid_,
                               std::move(enum_cases_)};
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
  const bool inner_nullable = match(TokenKind::kQuestion);
  if (inner_nullable) {
    range.end = tokens_[current_ - 1].range.end;
  }
  bool is_array = false;
  while (match(TokenKind::kLeftBracket)) {
    if (is_array) {
      diagnostics_.error(
          tokens_[current_ - 1].range,
          "multidimensional array types are not supported");  // TODO: Support
                                                              // multidimensional
                                                              // array types in
                                                              // the future.
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
  const bool is_static = match(TokenKind::kKwStatic);
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
           (class_body_started_ && current().kind == TokenKind::kRightBrace) ||
           current().kind == TokenKind::kKwFunc ||
           current().kind == TokenKind::kKwStatic ||
           current().kind == TokenKind::kKwOverride ||
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
  symbol.is_static = is_static;
  const std::size_t symbol_index = add_symbol(std::move(symbol));
  outlines_.push_back(MemberOutline{DeclarationKind::kField, symbol_index,
                                    begin, initializer, std::nullopt,
                                    std::nullopt, range, declaration_valid});
}

void DeclarationPass::parse_function() {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  const std::size_t begin = current_;
  bool is_static = false;
  bool is_override = false;
  bool is_abstract = false;
  bool is_final = false;
  while (is_function_modifier(current().kind)) {
    const Token& modifier = advance();
    bool* present = &is_abstract;
    if (modifier.kind == TokenKind::kKwStatic) {
      present = &is_static;
    } else if (modifier.kind == TokenKind::kKwOverride) {
      present = &is_override;
    } else if (modifier.kind == TokenKind::kKwFinal) {
      present = &is_final;
    }
    if (*present) {
      diagnostics_.error(
          modifier.range,
          "duplicate '" + std::string{modifier.lexeme} + "' function modifier");
      is_valid_ = false;
    }
    *present = true;
  }
  const bool is_interface_member = file_type_kind_ == FileTypeKind::kInterface;
  if (is_interface_member &&
      (is_static || is_override || is_abstract || is_final)) {
    diagnostics_.error(
        tokens_[begin].range,
        "interface function contracts do not accept function modifiers");
    is_valid_ = false;
  }
  is_abstract = is_abstract || is_interface_member;
  if (!match(TokenKind::kKwFunc)) {
    diagnostics_.error(current().range,
                       "expected 'func' after function modifiers");
    is_valid_ = false;
    synchronize_member();
    return;
  }
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

  std::optional<TokenIndexRange> body;
  bool has_abstract_terminator = false;
  if (is_abstract) {
    if (match(TokenKind::kSemicolon)) {
      has_abstract_terminator = true;
    } else if (current().kind == TokenKind::kLeftBrace) {
      diagnostics_.error(current().range, "abstract function '" +
                                              std::string{name.lexeme} +
                                              "' cannot have a body");
      is_valid_ = false;
      body = locate_body(name.lexeme);
    } else {
      diagnostics_.error(current().range,
                         "expected ';' after abstract function declaration");
      is_valid_ = false;
      synchronize_member();
    }
  } else {
    body = locate_body(name.lexeme);
  }
  const std::size_t end = current_ == begin ? begin : current_ - 1;
  const SourceRange range = range_from(begin, end);
  const bool declaration_valid =
      (is_abstract ? has_abstract_terminator : body.has_value()) &&
      diagnostics_.diagnostics().size() == diagnostic_count;
  MemberSymbol symbol{name.lexeme,
                      DeclarationKind::kFunction,
                      infer_visibility(name.lexeme),
                      range,
                      std::move(parameters),
                      return_type,
                      declaration_valid};
  symbol.is_static = is_static;
  symbol.is_override = is_override;
  symbol.is_abstract = is_abstract;
  symbol.is_final = is_final;
  const std::size_t symbol_index = add_symbol(std::move(symbol));
  outlines_.push_back(MemberOutline{DeclarationKind::kFunction, symbol_index,
                                    begin, std::nullopt, std::nullopt, body,
                                    range, declaration_valid});
}

void DeclarationPass::parse_constructor() {
  const std::size_t diagnostic_count = diagnostics_.diagnostics().size();
  const std::size_t begin = current_;
  const Token& name = advance();
  auto parameters = parse_parameters(name.lexeme);
  std::optional<TokenIndexRange> initializer;
  if (match(TokenKind::kColon)) {
    const std::size_t initializer_begin = current_;
    std::size_t parenthesis_depth = 0;
    while (!at_end()) {
      if (current().kind == TokenKind::kLeftBrace && parenthesis_depth == 0) {
        break;
      }
      if (current().kind == TokenKind::kLeftParen) {
        ++parenthesis_depth;
      } else if (current().kind == TokenKind::kRightParen &&
                 parenthesis_depth != 0) {
        --parenthesis_depth;
      }
      advance();
    }
    initializer = TokenIndexRange{initializer_begin, current_};
    if (initializer_begin == current_) {
      diagnostics_.error(current().range,
                         "expected base constructor after ':'");
      is_valid_ = false;
    }
  }
  const auto body = locate_body(name.lexeme);

  bool declaration_valid = body.has_value();
  if (!is_constructor_name(name.lexeme, source_.stem())) {
    const std::string lowercase = lowercase_constructor_name(source_.stem());
    diagnostics_.error(name.range, "constructor '" + std::string{name.lexeme} +
                                       "' must use a class-derived name: '" +
                                       std::string{source_.stem()} + "', '" +
                                       lowercase + "', or '_" +
                                       std::string{source_.stem()} + "'");
    is_valid_ = false;
    declaration_valid = false;
  }
  declaration_valid = declaration_valid &&
                      diagnostics_.diagnostics().size() == diagnostic_count;

  const std::size_t end = current_ == begin ? begin : current_ - 1;
  const SourceRange range = range_from(begin, end);
  MemberSymbol symbol{name.lexeme,
                      DeclarationKind::kConstructor,
                      infer_visibility(name.lexeme),
                      range,
                      std::move(parameters),
                      std::nullopt,
                      declaration_valid};
  const std::size_t symbol_index = add_symbol(std::move(symbol));
  outlines_.push_back(MemberOutline{DeclarationKind::kConstructor, symbol_index,
                                    begin, std::nullopt, initializer, body,
                                    range, declaration_valid});
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

void DeclarationPass::parse_file_type_declaration() {
  has_explicit_class_declaration_ = true;
  while (is_file_class_modifier(current().kind)) {
    const Token& modifier = advance();
    bool& present = modifier.kind == TokenKind::kKwAbstract ? class_is_abstract_
                                                            : class_is_sealed_;
    if (present) {
      diagnostics_.error(
          modifier.range,
          "duplicate '" + std::string{modifier.lexeme} + "' class modifier");
      is_valid_ = false;
    }
    present = true;
  }
  const bool is_interface = match(TokenKind::kKwInterface);
  const bool is_enum = !is_interface && match(TokenKind::kKwEnum);
  const bool is_struct =
      !is_interface && !is_enum && match(TokenKind::kKwStruct);
  if (!is_interface && !is_enum && !is_struct && !match(TokenKind::kKwClass)) {
    diagnostics_.error(current().range,
                       "expected 'class' or 'interface' after file type "
                       "modifiers");
    is_valid_ = false;
    return;
  }
  file_type_kind_ = is_struct      ? FileTypeKind::kStruct
                    : is_enum      ? FileTypeKind::kEnum
                    : is_interface ? FileTypeKind::kInterface
                                   : FileTypeKind::kClass;
  if ((is_interface || is_enum || is_struct) &&
      (class_is_abstract_ || class_is_sealed_)) {
    diagnostics_.error(
        tokens_[current_ - 1].range,
        "interfaces, enums, and structs cannot be declared abstract or sealed");
    is_valid_ = false;
  }

  if (current().kind == TokenKind::kIdentifier) {
    diagnostics_.error(current().range,
                       "the source file already defines implicit type '" +
                           std::string{source_.stem()} +
                           "'; do not repeat its name");
    is_valid_ = false;
    advance();
  }

  if (is_interface && match(TokenKind::kColon)) {
    parse_interface_list(interfaces_, "interface inheritance clause");
  } else if (!is_interface && !is_enum && match(TokenKind::kColon)) {
    if (current().kind == TokenKind::kIdentifier) {
      const Token& base = advance();
      base_class_ = TypeSyntax{base.lexeme, false, base.range};
    } else {
      diagnostics_.error(current().range,
                         "expected a file class name after ':'");
      is_valid_ = false;
    }
  }
  if (!is_interface && !is_enum && match(TokenKind::kKwIs)) {
    parse_interface_list(interfaces_, "class conformance clause");
  }

  if (match(TokenKind::kLeftBrace)) {
    class_body_started_ = true;
    if (is_enum) {
      parse_enum_cases();
    }
    return;
  }
  diagnostics_.error(current().range,
                     "expected '{' after file type declaration");
  is_valid_ = false;
}

void DeclarationPass::parse_enum_cases() {
  std::unordered_map<std::string_view, SourceRange> names;
  while (!at_end() && current().kind != TokenKind::kRightBrace) {
    if (current().kind == TokenKind::kIdentifier) {
      const Token& name = advance();
      if (const auto [previous, inserted] =
              names.emplace(name.lexeme, name.range);
          !inserted) {
        diagnostics_.error(name.range, "duplicate enum case '" +
                                           std::string{name.lexeme} + "'");
        diagnostics_.note(previous->second, "previous case is here");
        is_valid_ = false;
      } else if (enum_cases_.size() == kMaxEnumCases) {
        diagnostics_.error(name.range, "enum exceeds the 65536-case limit");
        is_valid_ = false;
      } else {
        enum_cases_.push_back(EnumCaseDecl{name.lexeme, name.range});
      }
      if (current().kind == TokenKind::kRightBrace ||
          match(TokenKind::kComma)) {
        continue;
      }
      diagnostics_.error(current().range,
                         "expected ',' or '}' after enum case");
    } else {
      diagnostics_.error(current().range, "expected an enum case name");
    }
    is_valid_ = false;
    std::size_t braces = 0;
    while (!at_end()) {
      if (braces == 0 && (current().kind == TokenKind::kComma ||
                          current().kind == TokenKind::kRightBrace)) {
        break;
      }
      if (current().kind == TokenKind::kLeftBrace) ++braces;
      if (current().kind == TokenKind::kRightBrace && braces != 0) --braces;
      advance();
    }
    match(TokenKind::kComma);
  }
  if (enum_cases_.empty()) {
    diagnostics_.error(current().range, "enum must declare at least one case");
    is_valid_ = false;
  }
}

void DeclarationPass::parse_interface_list(std::vector<TypeSyntax>& interfaces,
                                           std::string_view context) {
  bool expects_name = true;
  while (!at_end()) {
    if (current().kind != TokenKind::kIdentifier) {
      diagnostics_.error(current().range, "expected an interface name in " +
                                              std::string{context});
      is_valid_ = false;
      return;
    }
    const Token& interface_name = advance();
    interfaces.push_back(
        TypeSyntax{interface_name.lexeme, false, interface_name.range});
    expects_name = false;
    if (!match(TokenKind::kComma)) {
      return;
    }
    expects_name = true;
  }
  if (expects_name) {
    diagnostics_.error(current().range, "expected an interface name after ','");
    is_valid_ = false;
  }
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
    if (class_body_started_ && current().kind == TokenKind::kRightBrace) {
      return;
    }
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
    const bool constructor_pair =
        previous.kind == DeclarationKind::kConstructor &&
        symbol.kind == DeclarationKind::kConstructor;
    if (!constructor_pair && previous.name != symbol.name) {
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
      kind == TokenKind::kKwStatic || kind == TokenKind::kKwOverride ||
      kind == TokenKind::kKwAbstract || kind == TokenKind::kKwSealed ||
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
