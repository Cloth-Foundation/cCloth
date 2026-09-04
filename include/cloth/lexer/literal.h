// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_LEXER_LITERAL_H_
#define CLOTH_LEXER_LITERAL_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cloth {

enum class NumericLiteralSuffix {
  kNone,
  kInt8,
  kInt16,
  kInt32,
  kInt64,
  kUint8,
  kUint16,
  kUint32,
  kUint64,
  kFloat32,
  kFloat64,
};

enum class NumericLiteralSpellingError {
  kNone,
  kInvalidCore,
  kInvalidSuffix,
  kIntegerSuffixOnFloatingCore,
};

struct NumericLiteralSpelling {
  std::string_view core;
  std::string_view suffix;
  NumericLiteralSuffix suffix_kind;
  NumericLiteralSpellingError error;
  bool core_is_floating;
  bool is_floating;
};

// Splits a complete decimal numeric token. This validates source spelling but
// does not perform type-specific range checking.
[[nodiscard]] NumericLiteralSpelling parse_numeric_literal_spelling(
    std::string_view spelling) noexcept;

// Returns the canonical Cloth type name selected by a non-empty suffix.
[[nodiscard]] std::string_view numeric_literal_suffix_type_name(
    NumericLiteralSuffix suffix) noexcept;

[[nodiscard]] char decode_escape_character(char character) noexcept;

[[nodiscard]] std::string decode_string_literal(std::string_view lexeme);

[[nodiscard]] std::optional<std::size_t> utf8_scalar_count(
    std::string_view text) noexcept;

}  // namespace cloth

#endif  // CLOTH_LEXER_LITERAL_H_
