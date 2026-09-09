// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/lexer.h"
#include "cloth/lexer/token.h"
#include "cloth/source/source_file.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kLastTokenKind = static_cast<int>(cloth::TokenKind::kKwObject);
static_assert(kLastTokenKind == 108);

[[nodiscard]] bool starts_with(std::string_view text,
                               std::string_view prefix) noexcept {
  return text.starts_with(prefix);
}

[[nodiscard]] std::optional<int> diagnostic_kind(
    std::string_view message) noexcept {
  if (starts_with(message, "unexpected character ")) return 0;
  if (message == "unterminated block comment") return 1;
  if (starts_with(message, "invalid numeric literal '")) return 2;
  if (starts_with(message, "invalid numeric suffix '")) return 3;
  if (starts_with(message, "integer suffix '")) return 4;
  if (starts_with(message, "numeric base prefix requires digits")) return 5;
  if (starts_with(message, "invalid digit in base-")) return 6;
  if (starts_with(message, "numeric base prefix must be lowercase")) return 7;
  if (starts_with(message, "unknown numeric base prefix")) return 8;
  if (starts_with(message, "invalid digit separator")) return 9;
  if (starts_with(message, "numeric exponent requires digits")) return 10;
  if (starts_with(message, "floating suffix '")) return 11;
  if (message == "unterminated string literal") return 12;
  if (message == "unterminated character literal") return 13;
  if (message == "malformed string literal") return 14;
  if (message == "malformed character literal") return 15;
  if (message.ends_with(" literal is not valid UTF-8")) return 16;
  if (starts_with(message, "unknown escape sequence")) return 17;
  if (message == "invalid Unicode escape sequence") return 18;
  if (message == "Unicode escape does not name a scalar value") return 19;
  if (message == "empty character literal") return 20;
  if (message == "character literal must contain exactly one character") {
    return 21;
  }
  return std::nullopt;
}

void write_record(char category, int kind, std::size_t span_begin,
                  std::size_t span_end, const cloth::SourceRange& range,
                  std::string_view contents) {
  std::cout << category << '|' << kind << '|' << span_begin << '|' << span_end
            << '|' << range.begin.byte_offset << '|' << range.begin.line << '|'
            << range.begin.column << '|' << range.end.byte_offset << '|'
            << range.end.line << '|' << range.end.column << '|';
  for (std::size_t offset = span_begin; offset < span_end; ++offset) {
    if (offset != span_begin) std::cout << ',';
    std::cout << static_cast<unsigned int>(
        static_cast<unsigned char>(contents[offset]));
  }
  std::cout << '\n';
}

[[nodiscard]] std::size_t diagnostic_span_end(
    const cloth::Diagnostic& diagnostic,
    const std::vector<cloth::Token>& tokens,
    std::string_view contents) noexcept {
  const std::size_t begin = diagnostic.range.begin.byte_offset;
  if (starts_with(diagnostic.message, "unexpected character ")) {
    return std::min(begin + 1, contents.size());
  }
  if (diagnostic.message == "unterminated block comment") {
    return contents.size();
  }
  if (diagnostic.message == "unterminated string literal" ||
      diagnostic.message == "unterminated character literal") {
    const auto token = std::find_if(
        tokens.begin(), tokens.end(), [begin](const cloth::Token& candidate) {
          return candidate.range.begin.byte_offset == begin;
        });
    if (token != tokens.end()) return token->range.end.byte_offset;
  }
  return diagnostic.range.end.byte_offset;
}

int record(const std::filesystem::path& path) {
  auto loaded = cloth::SourceFile::load(path);
  if (!loaded) {
    std::cerr << path.string() << ": " << loaded.error().message << '\n';
    return 1;
  }

  cloth::SourceFile source = std::move(*loaded);
  cloth::DiagnosticEngine diagnostics;
  const std::vector<cloth::Token> tokens =
      cloth::Lexer{source, diagnostics}.lex();
  const std::string_view contents = source.contents();
  for (const auto& token : tokens) {
    write_record('T', static_cast<int>(token.kind),
                 token.range.begin.byte_offset, token.range.end.byte_offset,
                 token.range, contents);
  }
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    const auto kind = diagnostic_kind(diagnostic.message);
    if (!kind) {
      std::cerr << "unclassified lexer diagnostic: " << diagnostic.message
                << '\n';
      return 1;
    }
    write_record('D', *kind, diagnostic.range.begin.byte_offset,
                 diagnostic_span_end(diagnostic, tokens, contents),
                 diagnostic.range, contents);
  }
  return 0;
}

bool write_bytes(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

int generate(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::cerr << directory.string() << ": " << error.message() << '\n';
    return 1;
  }

  constexpr std::array<std::pair<std::string_view, std::string_view>, 17>
      kFixedInputs{{
          {"empty.co", ""},
          {"trivia.co", " \t\r\n// line\r\n/* block */"},
          {"identifiers.co", "alpha _beta Name9"},
          {"keywords.co",
           "func return if else while for in break continue switch case "
           "default struct class interface abstract sealed enum error throw "
           "throws trait let var const final static override super true false "
           "null extern unsafe import is as match int int8 int16 int32 int64 "
           "uint uint8 uint16 uint32 uint64 float float32 float64 bool char "
           "byte void object"},
          {"punctuation.co", "(){}[],;::: .?"},
          {"operators.co",
           "+ - * / % = == != < <= > >= && || ! & | ^ ~ ++ -- += -= *= "
           "/= %= << >> <<= >>= &= |= ^= ?. ??"},
          {"numbers.co",
           "0 10 1_000 3.14 1e3 1E+003 0b1010 0o17 0xFF i32 u64 f32"},
          {"bad_numbers.co",
           "0b 0b2 0o8 0xG 0B1 0q1 1e 1e+ 1_ 1__0 1.0i32 0xfff32 "
           "1wat"},
          {"strings.co", "\"hello\" \"line\\nnext\" 'a' '\\u{1F9F5}'"},
          {"bad_text.co", "\"\\u{}\" \"\\q\" '\\u{110000}' '' 'ab' '\\q'"},
          {"unterminated.co", "\"text\nname 'x\rnext"},
          {"comments.co", "one/* two\r\nthree */four// five\nsix"},
          {"unterminated_comment.co", "name /* never closed"},
          {"unexpected.co", "@ $ `"},
          {"cr.co", "one\rtwo"},
          {"lf.co", "one\ntwo"},
          {"crlf.co", "one\r\ntwo"},
      }};

  for (const auto& [name, bytes] : kFixedInputs) {
    if (!write_bytes(directory / name, bytes)) {
      std::cerr << "could not write generated input " << name << '\n';
      return 1;
    }
  }

  for (unsigned int value = 0; value <= 255; ++value) {
    std::string bytes(1, static_cast<char>(value));
    const std::string name = "byte_" + std::to_string(value) + ".bin";
    if (!write_bytes(directory / name, bytes)) return 1;
  }

  std::uint32_t state = UINT32_C(0x434C4F54);
  for (unsigned int input = 0; input < 96; ++input) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const std::size_t length = 1 + state % 64;
    std::string bytes(length, '\0');
    for (char& value : bytes) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      value = static_cast<char>(state & 0xFFU);
    }
    const std::string name = "random_" + std::to_string(input) + ".bin";
    if (!write_bytes(directory / name, bytes)) return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: cloth_lexer_parity_oracle <record|generate> <path>\n";
    return 2;
  }
  const std::string_view command = argv[1];
  if (command == "record") return record(argv[2]);
  if (command == "generate") return generate(argv[2]);
  std::cerr << "unknown command: " << command << '\n';
  return 2;
}
