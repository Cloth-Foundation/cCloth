// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/lexer/literal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cloth {

NumericLiteralSpelling parse_numeric_literal_spelling(
    std::string_view spelling) noexcept {
  std::size_t offset = 0;
  while (offset < spelling.size() && spelling[offset] >= '0' &&
         spelling[offset] <= '9') {
    ++offset;
  }
  if (offset == 0) {
    return {{},
            spelling,
            NumericLiteralSuffix::kNone,
            NumericLiteralSpellingError::kInvalidCore,
            false,
            false};
  }

  bool core_is_floating = false;
  if (offset < spelling.size() && spelling[offset] == '.') {
    core_is_floating = true;
    ++offset;
    const std::size_t fraction_begin = offset;
    while (offset < spelling.size() && spelling[offset] >= '0' &&
           spelling[offset] <= '9') {
      ++offset;
    }
    if (offset == fraction_begin) {
      return {spelling.substr(0, offset),
              spelling.substr(offset),
              NumericLiteralSuffix::kNone,
              NumericLiteralSpellingError::kInvalidCore,
              true,
              true};
    }
  }

  const std::string_view core = spelling.substr(0, offset);
  const std::string_view suffix = spelling.substr(offset);
  NumericLiteralSuffix suffix_kind = NumericLiteralSuffix::kNone;
  if (suffix == "i8")
    suffix_kind = NumericLiteralSuffix::kInt8;
  else if (suffix == "i16")
    suffix_kind = NumericLiteralSuffix::kInt16;
  else if (suffix == "i32")
    suffix_kind = NumericLiteralSuffix::kInt32;
  else if (suffix == "i64")
    suffix_kind = NumericLiteralSuffix::kInt64;
  else if (suffix == "u8")
    suffix_kind = NumericLiteralSuffix::kUint8;
  else if (suffix == "u16")
    suffix_kind = NumericLiteralSuffix::kUint16;
  else if (suffix == "u32")
    suffix_kind = NumericLiteralSuffix::kUint32;
  else if (suffix == "u64")
    suffix_kind = NumericLiteralSuffix::kUint64;
  else if (suffix == "f32")
    suffix_kind = NumericLiteralSuffix::kFloat32;
  else if (suffix == "f64")
    suffix_kind = NumericLiteralSuffix::kFloat64;
  else if (!suffix.empty())
    return {core,
            suffix,
            NumericLiteralSuffix::kNone,
            NumericLiteralSpellingError::kInvalidSuffix,
            core_is_floating,
            core_is_floating};

  const bool floating_suffix = suffix_kind == NumericLiteralSuffix::kFloat32 ||
                               suffix_kind == NumericLiteralSuffix::kFloat64;
  const bool integer_suffix =
      suffix_kind != NumericLiteralSuffix::kNone && !floating_suffix;
  if (core_is_floating && integer_suffix) {
    return {
        core,        suffix,
        suffix_kind, NumericLiteralSpellingError::kIntegerSuffixOnFloatingCore,
        true,        true};
  }
  return {core,
          suffix,
          suffix_kind,
          NumericLiteralSpellingError::kNone,
          core_is_floating,
          core_is_floating || floating_suffix};
}

std::string_view numeric_literal_suffix_type_name(
    NumericLiteralSuffix suffix) noexcept {
  switch (suffix) {
    case NumericLiteralSuffix::kInt8:
      return "int8";
    case NumericLiteralSuffix::kInt16:
      return "int16";
    case NumericLiteralSuffix::kInt32:
      return "int32";
    case NumericLiteralSuffix::kInt64:
      return "int64";
    case NumericLiteralSuffix::kUint8:
      return "uint8";
    case NumericLiteralSuffix::kUint16:
      return "uint16";
    case NumericLiteralSuffix::kUint32:
      return "uint32";
    case NumericLiteralSuffix::kUint64:
      return "uint64";
    case NumericLiteralSuffix::kFloat32:
      return "float32";
    case NumericLiteralSuffix::kFloat64:
      return "float64";
    case NumericLiteralSuffix::kNone:
      return {};
  }
  return {};
}

char decode_escape_character(char character) noexcept {
  switch (character) {
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    case '0':
      return '\0';
    case '\\':
      return '\\';
    case '\'':
      return '\'';
    case '"':
      return '"';
    default:
      return character;
  }
}

std::string decode_string_literal(std::string_view lexeme) {
  std::string value;
  if (lexeme.size() < 2) {
    return value;
  }
  value.reserve(lexeme.size() - 2);
  for (std::size_t index = 1; index + 1 < lexeme.size(); ++index) {
    if (lexeme[index] == '\\' && index + 2 < lexeme.size()) {
      ++index;
      value.push_back(decode_escape_character(lexeme[index]));
    } else {
      value.push_back(lexeme[index]);
    }
  }
  return value;
}

std::optional<std::size_t> utf8_scalar_count(std::string_view text) noexcept {
  std::size_t scalar_count = 0;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto first = static_cast<std::uint8_t>(text[offset]);
    std::size_t width = 0;
    if (first <= 0x7F) {
      width = 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
      width = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
      width = 3;
    } else if (first >= 0xF0 && first <= 0xF4) {
      width = 4;
    } else {
      return std::nullopt;
    }

    if (width > text.size() - offset) {
      return std::nullopt;
    }
    for (std::size_t index = 1; index < width; ++index) {
      const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
      if ((continuation & 0xC0U) != 0x80U) {
        return std::nullopt;
      }
    }

    if (width == 3) {
      const auto second = static_cast<std::uint8_t>(text[offset + 1]);
      if ((first == 0xE0 && second < 0xA0) ||
          (first == 0xED && second >= 0xA0)) {
        return std::nullopt;
      }
    } else if (width == 4) {
      const auto second = static_cast<std::uint8_t>(text[offset + 1]);
      if ((first == 0xF0 && second < 0x90) ||
          (first == 0xF4 && second > 0x8F)) {
        return std::nullopt;
      }
    }

    offset += width;
    ++scalar_count;
  }
  return scalar_count;
}

}  // namespace cloth
