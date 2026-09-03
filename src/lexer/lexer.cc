// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/lexer/lexer.h"

#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace cloth {
namespace {

using Keyword = std::pair<std::string_view, TokenKind>;

constexpr std::array kKeywords{
    Keyword{"func", TokenKind::kKwFunc},
    Keyword{"return", TokenKind::kKwReturn},
    Keyword{"if", TokenKind::kKwIf},
    Keyword{"else", TokenKind::kKwElse},
    Keyword{"while", TokenKind::kKwWhile},
    Keyword{"for", TokenKind::kKwFor},
    Keyword{"in", TokenKind::kKwIn},
    Keyword{"break", TokenKind::kKwBreak},
    Keyword{"continue", TokenKind::kKwContinue},
    Keyword{"switch", TokenKind::kKwSwitch},
    Keyword{"case", TokenKind::kKwCase},
    Keyword{"default", TokenKind::kKwDefault},
    Keyword{"struct", TokenKind::kKwStruct},
    Keyword{"class", TokenKind::kKwClass},
    Keyword{"interface", TokenKind::kKwInterface},
    Keyword{"abstract", TokenKind::kKwAbstract},
    Keyword{"sealed", TokenKind::kKwSealed},
    Keyword{"enum", TokenKind::kKwEnum},
    Keyword{"trait", TokenKind::kKwTrait},
    Keyword{"let", TokenKind::kKwLet},
    Keyword{"var", TokenKind::kKwVar},
    Keyword{"const", TokenKind::kKwConst},
    Keyword{"final", TokenKind::kKwFinal},
    Keyword{"static", TokenKind::kKwStatic},
    Keyword{"override", TokenKind::kKwOverride},
    Keyword{"super", TokenKind::kKwSuper},
    Keyword{"true", TokenKind::kKwTrue},
    Keyword{"false", TokenKind::kKwFalse},
    Keyword{"null", TokenKind::kKwNull},
    Keyword{"extern", TokenKind::kKwExtern},
    Keyword{"unsafe", TokenKind::kKwUnsafe},
    Keyword{"import", TokenKind::kKwImport},
    Keyword{"is", TokenKind::kKwIs},
    Keyword{"as", TokenKind::kKwAs},
    Keyword{"match", TokenKind::kKwMatch},
    Keyword{"int", TokenKind::kKwInt},
    Keyword{"int8", TokenKind::kKwInt8},
    Keyword{"int16", TokenKind::kKwInt16},
    Keyword{"int32", TokenKind::kKwInt32},
    Keyword{"int64", TokenKind::kKwInt64},
    Keyword{"uint", TokenKind::kKwUint},
    Keyword{"uint8", TokenKind::kKwUint8},
    Keyword{"uint16", TokenKind::kKwUint16},
    Keyword{"uint32", TokenKind::kKwUint32},
    Keyword{"uint64", TokenKind::kKwUint64},
    Keyword{"float", TokenKind::kKwFloat},
    Keyword{"float32", TokenKind::kKwFloat32},
    Keyword{"float64", TokenKind::kKwFloat64},
    Keyword{"bool", TokenKind::kKwBool},
    Keyword{"char", TokenKind::kKwChar},
    Keyword{"byte", TokenKind::kKwByte},
    Keyword{"void", TokenKind::kKwVoid},
    Keyword{"object", TokenKind::kKwObject},
};

constexpr bool is_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

constexpr bool is_identifier_start(char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') || character == '_';
}

constexpr bool is_identifier_continue(char character) noexcept {
  return is_identifier_start(character) || is_digit(character);
}

constexpr bool is_supported_escape(char character) noexcept {
  return character == 'n' || character == 'r' || character == 't' ||
         character == '\\' || character == '"' || character == '\'' ||
         character == '0';
}

std::string describe_character(char character) {
  const auto value = static_cast<unsigned char>(character);
  if (std::isprint(value) != 0) {
    return std::string{"'"} + character + "'";
  }

  std::ostringstream output;
  output << "byte 0x" << std::uppercase << std::hex << std::setw(2)
         << std::setfill('0') << static_cast<unsigned int>(value);
  return output.str();
}

}  // namespace

TokenKind identifier_token_kind(std::string_view lexeme) noexcept {
  for (const auto& [keyword, kind] : kKeywords) {
    if (lexeme == keyword) {
      return kind;
    }
  }
  return TokenKind::kIdentifier;
}

Lexer::Lexer(const SourceFile& source, DiagnosticEngine& diagnostics) noexcept
    : source_(source), diagnostics_(diagnostics), input_(source.contents()) {}

std::vector<Token> Lexer::lex() {
  std::vector<Token> tokens;

  while (true) {
    skip_ignored();

    const auto start = current_;
    const auto location = current_location();
    if (at_end()) {
      tokens.push_back(make_token(TokenKind::kEof, start, location));
      break;
    }

    const char character = advance();
    if (is_identifier_start(character)) {
      tokens.push_back(scan_identifier(start, location));
      continue;
    }
    if (is_digit(character)) {
      tokens.push_back(scan_number(start, location));
      continue;
    }

    auto add = [&](TokenKind kind) {
      tokens.push_back(make_token(kind, start, location));
    };

    switch (character) {
      case '(':
        add(TokenKind::kLeftParen);
        break;
      case ')':
        add(TokenKind::kRightParen);
        break;
      case '{':
        add(TokenKind::kLeftBrace);
        break;
      case '}':
        add(TokenKind::kRightBrace);
        break;
      case '[':
        add(TokenKind::kLeftBracket);
        break;
      case ']':
        add(TokenKind::kRightBracket);
        break;
      case ',':
        add(TokenKind::kComma);
        break;
      case ';':
        add(TokenKind::kSemicolon);
        break;
      case ':':
        add(match(':') ? TokenKind::kColonColon : TokenKind::kColon);
        break;
      case '.':
        add(TokenKind::kDot);
        break;
      case '?':
        add(match('?')   ? TokenKind::kQuestionQuestion
            : match('.') ? TokenKind::kQuestionDot
                         : TokenKind::kQuestion);
        break;
      case '+':
        add(match('+')   ? TokenKind::kPlusPlus
            : match('=') ? TokenKind::kPlusEqual
                         : TokenKind::kPlus);
        break;
      case '-':
        add(match('-')   ? TokenKind::kMinusMinus
            : match('=') ? TokenKind::kMinusEqual
                         : TokenKind::kMinus);
        break;
      case '*':
        add(match('=') ? TokenKind::kStarEqual : TokenKind::kStar);
        break;
      case '/':
        add(match('=') ? TokenKind::kSlashEqual : TokenKind::kSlash);
        break;
      case '%':
        add(match('=') ? TokenKind::kPercentEqual : TokenKind::kPercent);
        break;
      case '=':
        add(match('=') ? TokenKind::kEqualEqual : TokenKind::kEqual);
        break;
      case '!':
        add(match('=') ? TokenKind::kBangEqual : TokenKind::kBang);
        break;
      case '<':
        if (match('<')) {
          add(match('=') ? TokenKind::kShiftLeftEqual : TokenKind::kShiftLeft);
        } else {
          add(match('=') ? TokenKind::kLessEqual : TokenKind::kLess);
        }
        break;
      case '>':
        if (match('>')) {
          add(match('=') ? TokenKind::kShiftRightEqual
                         : TokenKind::kShiftRight);
        } else {
          add(match('=') ? TokenKind::kGreaterEqual : TokenKind::kGreater);
        }
        break;
      case '&':
        add(match('&')   ? TokenKind::kAmpersandAmpersand
            : match('=') ? TokenKind::kAmpersandEqual
                         : TokenKind::kAmpersand);
        break;
      case '|':
        add(match('|')   ? TokenKind::kPipePipe
            : match('=') ? TokenKind::kPipeEqual
                         : TokenKind::kPipe);
        break;
      case '^':
        add(match('=') ? TokenKind::kCaretEqual : TokenKind::kCaret);
        break;
      case '~':
        add(TokenKind::kTilde);
        break;
      case '"':
        tokens.push_back(scan_string(start, location));
        break;
      case '\'':
        tokens.push_back(scan_character(start, location));
        break;
      default:
        diagnostics_.error(
            location, "unexpected character " + describe_character(character));
        break;
    }
  }

  return tokens;
}

bool Lexer::at_end() const noexcept { return current_ >= input_.size(); }

char Lexer::peek(std::size_t lookahead) const noexcept {
  if (lookahead >= input_.size() - current_) {
    return '\0';
  }
  return input_[current_ + lookahead];
}

char Lexer::advance() noexcept {
  const char character = input_[current_++];
  if (character == '\r') {
    ++line_;
    column_ = 1;
    previous_was_carriage_return_ = true;
  } else if (character == '\n') {
    if (!previous_was_carriage_return_) {
      ++line_;
    }
    column_ = 1;
    previous_was_carriage_return_ = false;
  } else {
    ++column_;
    previous_was_carriage_return_ = false;
  }
  return character;
}

bool Lexer::match(char expected) noexcept {
  if (at_end() || peek() != expected) {
    return false;
  }
  advance();
  return true;
}

SourceLocation Lexer::current_location() const noexcept {
  return SourceLocation{source_.display_path(), current_, line_, column_};
}

Token Lexer::make_token(TokenKind kind, std::size_t start,
                        SourceLocation location) const noexcept {
  return Token{kind, input_.substr(start, current_ - start),
               SourceRange{location, current_location()}};
}

void Lexer::skip_ignored() {
  while (!at_end()) {
    switch (peek()) {
      case ' ':
      case '\t':
      case '\r':
      case '\n':
        advance();
        continue;
      case '/':
        if (peek(1) == '/') {
          advance();
          advance();
          while (!at_end() && peek() != '\r' && peek() != '\n') {
            advance();
          }
          continue;
        }
        if (peek(1) == '*') {
          const auto comment_location = current_location();
          advance();
          advance();
          while (!at_end() && !(peek() == '*' && peek(1) == '/')) {
            advance();
          }
          if (at_end()) {
            diagnostics_.error(comment_location, "unterminated block comment");
            return;
          }
          advance();
          advance();
          continue;
        }
        return;
      default:
        return;
    }
  }
}

Token Lexer::scan_identifier(std::size_t start, SourceLocation location) {
  while (is_identifier_continue(peek())) {
    advance();
  }
  const auto lexeme = input_.substr(start, current_ - start);
  return Token{identifier_token_kind(lexeme), lexeme,
               SourceRange{location, current_location()}};
}

Token Lexer::scan_number(std::size_t start, SourceLocation location) {
  while (is_digit(peek())) {
    advance();
  }

  auto kind = TokenKind::kIntegerLiteral;
  if (peek() == '.' && is_digit(peek(1))) {
    kind = TokenKind::kFloatLiteral;
    advance();
    while (is_digit(peek())) {
      advance();
    }
  }

  return make_token(kind, start, location);
}

Token Lexer::scan_string(std::size_t start, SourceLocation location) {
  while (!at_end()) {
    if (peek() == '"') {
      advance();
      return make_token(TokenKind::kStringLiteral, start, location);
    }
    if (peek() == '\r' || peek() == '\n') {
      diagnostics_.error(location, "unterminated string literal");
      return make_token(TokenKind::kStringLiteral, start, location);
    }
    if (peek() == '\\') {
      const auto escape_location = current_location();
      advance();
      if (at_end() || peek() == '\r' || peek() == '\n') {
        diagnostics_.error(location, "unterminated string literal");
        return make_token(TokenKind::kStringLiteral, start, location);
      }
      const char escaped = advance();
      if (!is_supported_escape(escaped)) {
        report_invalid_escape(escape_location, escaped);
      }
      continue;
    }
    advance();
  }

  diagnostics_.error(location, "unterminated string literal");
  return make_token(TokenKind::kStringLiteral, start, location);
}

Token Lexer::scan_character(std::size_t start, SourceLocation location) {
  if (at_end() || peek() == '\r' || peek() == '\n') {
    diagnostics_.error(location, "unterminated character literal");
    return make_token(TokenKind::kCharacterLiteral, start, location);
  }

  if (peek() == '\'') {
    advance();
    diagnostics_.error(location, "empty character literal");
    return make_token(TokenKind::kCharacterLiteral, start, location);
  }

  if (peek() == '\\') {
    const auto escape_location = current_location();
    advance();
    if (at_end() || peek() == '\r' || peek() == '\n') {
      diagnostics_.error(location, "unterminated character literal");
      return make_token(TokenKind::kCharacterLiteral, start, location);
    }
    const char escaped = advance();
    if (!is_supported_escape(escaped)) {
      report_invalid_escape(escape_location, escaped);
    }
  } else {
    advance();
  }

  if (match('\'')) {
    return make_token(TokenKind::kCharacterLiteral, start, location);
  }

  if (at_end() || peek() == '\r' || peek() == '\n') {
    diagnostics_.error(location, "unterminated character literal");
    return make_token(TokenKind::kCharacterLiteral, start, location);
  }

  diagnostics_.error(location,
                     "character literal must contain exactly one character");
  while (!at_end() && peek() != '\'' && peek() != '\r' && peek() != '\n') {
    advance();
  }
  if (peek() == '\'') {
    advance();
  }
  return make_token(TokenKind::kCharacterLiteral, start, location);
}

void Lexer::report_invalid_escape(SourceLocation location, char escaped) {
  diagnostics_.error(location,
                     "unknown escape sequence \\" + std::string{escaped});
}

}  // namespace cloth
