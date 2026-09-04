// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/lexer/literal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cloth {
namespace {

constexpr std::int64_t kSaturatedExponent = 1000000;

bool is_decimal_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

int digit_value(char character) noexcept {
  if (is_decimal_digit(character)) return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

NumericLiteralSuffix parse_suffix(std::string_view suffix) noexcept {
  if (suffix == "i8") return NumericLiteralSuffix::kInt8;
  if (suffix == "i16") return NumericLiteralSuffix::kInt16;
  if (suffix == "i32") return NumericLiteralSuffix::kInt32;
  if (suffix == "i64") return NumericLiteralSuffix::kInt64;
  if (suffix == "u8") return NumericLiteralSuffix::kUint8;
  if (suffix == "u16") return NumericLiteralSuffix::kUint16;
  if (suffix == "u32") return NumericLiteralSuffix::kUint32;
  if (suffix == "u64") return NumericLiteralSuffix::kUint64;
  if (suffix == "f32") return NumericLiteralSuffix::kFloat32;
  if (suffix == "f64") return NumericLiteralSuffix::kFloat64;
  return NumericLiteralSuffix::kNone;
}

bool is_floating_suffix(NumericLiteralSuffix suffix) noexcept {
  return suffix == NumericLiteralSuffix::kFloat32 ||
         suffix == NumericLiteralSuffix::kFloat64;
}

bool is_valid_separator_run(std::string_view run) noexcept {
  if (run.empty() || run.front() == '_' || run.back() == '_') return false;
  for (std::size_t index = 1; index < run.size(); ++index) {
    if (run[index] == '_' && run[index - 1] == '_') return false;
  }
  return true;
}

std::string digits_without_separators(std::string_view run) {
  std::string result;
  result.reserve(run.size());
  for (const char character : run) {
    if (character != '_') result.push_back(character);
  }
  return result;
}

std::string canonical_decimal_integer(std::string digits) {
  const std::size_t first = digits.find_first_not_of('0');
  if (first == std::string::npos) return "0";
  digits.erase(0, first);
  return digits;
}

std::int64_t clamp_exponent(std::int64_t value) noexcept {
  return std::clamp(value, -kSaturatedExponent, kSaturatedExponent);
}

std::int64_t parse_exponent(std::string_view digits, bool negative) noexcept {
  std::int64_t value = 0;
  for (const char character : digits) {
    const int digit = character - '0';
    if (value > (kSaturatedExponent - digit) / 10) {
      value = kSaturatedExponent;
      break;
    }
    value = value * 10 + digit;
  }
  return negative ? -value : value;
}

std::int64_t subtract_size(std::int64_t value, std::size_t amount) noexcept {
  const auto bounded = static_cast<std::int64_t>(
      std::min<std::size_t>(amount, kSaturatedExponent));
  return clamp_exponent(value - bounded);
}

std::int64_t add_size(std::int64_t value, std::size_t amount) noexcept {
  const auto bounded = static_cast<std::int64_t>(
      std::min<std::size_t>(amount, kSaturatedExponent));
  return clamp_exponent(value + bounded);
}

std::string canonical_decimal_float(std::string integer_digits,
                                    std::string fractional_digits,
                                    std::int64_t exponent) {
  std::string coefficient = std::move(integer_digits);
  coefficient += fractional_digits;
  const std::size_t first = coefficient.find_first_not_of('0');
  if (first == std::string::npos) return "0e0";
  coefficient.erase(0, first);
  exponent = subtract_size(exponent, fractional_digits.size());
  const std::size_t last = coefficient.find_last_not_of('0');
  const std::size_t trailing = coefficient.size() - last - 1;
  coefficient.erase(last + 1);
  exponent = add_size(exponent, trailing);
  return coefficient + 'e' + std::to_string(exponent);
}

NumericLiteralSpelling parse_base_literal(std::string_view spelling,
                                          NumericLiteralBase base) {
  NumericLiteralSpelling result;
  result.base = base;
  const int radix = static_cast<int>(base);
  std::size_t offset = 2;
  while (offset < spelling.size()) {
    const int value = digit_value(spelling[offset]);
    if ((value >= 0 && value < radix) || spelling[offset] == '_') {
      ++offset;
      continue;
    }
    break;
  }

  const std::string_view digit_run = spelling.substr(2, offset - 2);
  const std::string_view tail = spelling.substr(offset);
  if (digit_run.empty()) {
    result.error = tail.empty()
                       ? NumericLiteralSpellingError::kMissingBaseDigits
                       : NumericLiteralSpellingError::kInvalidBaseDigit;
    return result;
  }
  if (!is_valid_separator_run(digit_run)) {
    result.error = NumericLiteralSpellingError::kInvalidSeparator;
    return result;
  }

  result.suffix = tail;
  result.suffix_kind = parse_suffix(tail);
  if (!tail.empty() && result.suffix_kind == NumericLiteralSuffix::kNone) {
    result.error = is_decimal_digit(tail.front())
                       ? NumericLiteralSpellingError::kInvalidBaseDigit
                       : NumericLiteralSpellingError::kInvalidSuffix;
    return result;
  }
  if (is_floating_suffix(result.suffix_kind)) {
    result.error = NumericLiteralSpellingError::kFloatingSuffixOnNonDecimalCore;
    return result;
  }

  const std::string digits = digits_without_separators(digit_run);
  std::uint64_t value = 0;
  bool overflow = false;
  for (const char character : digits) {
    const auto digit = static_cast<std::uint64_t>(digit_value(character));
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) /
                    static_cast<std::uint64_t>(radix)) {
      overflow = true;
      break;
    }
    value = value * static_cast<std::uint64_t>(radix) + digit;
  }
  result.core = overflow ? "18446744073709551616" : std::to_string(value);
  return result;
}

}  // namespace

NumericLiteralSpelling parse_numeric_literal_spelling(
    std::string_view spelling) {
  NumericLiteralSpelling result;
  if (spelling.empty() || !is_decimal_digit(spelling.front())) {
    result.error = NumericLiteralSpellingError::kInvalidCore;
    return result;
  }

  if (spelling.size() >= 2 && spelling.front() == '0') {
    switch (spelling[1]) {
      case 'b':
        return parse_base_literal(spelling, NumericLiteralBase::kBinary);
      case 'o':
        return parse_base_literal(spelling, NumericLiteralBase::kOctal);
      case 'x':
        return parse_base_literal(spelling, NumericLiteralBase::kHexadecimal);
      case 'B':
      case 'O':
      case 'X':
        result.error = NumericLiteralSpellingError::kInvalidBasePrefix;
        return result;
      default:
        if (((spelling[1] >= 'a' && spelling[1] <= 'z') ||
             (spelling[1] >= 'A' && spelling[1] <= 'Z')) &&
            spelling[1] != 'e' && spelling[1] != 'E' && spelling[1] != 'f' &&
            spelling[1] != 'F' && spelling[1] != 'i' && spelling[1] != 'I' &&
            spelling[1] != 'u' && spelling[1] != 'U') {
          result.error = NumericLiteralSpellingError::kUnknownBasePrefix;
          return result;
        }
        break;
    }
  }

  std::size_t offset = 0;
  while (offset < spelling.size() &&
         (is_decimal_digit(spelling[offset]) || spelling[offset] == '_')) {
    ++offset;
  }
  const std::string_view integer_run = spelling.substr(0, offset);
  bool invalid_separator = !is_valid_separator_run(integer_run);

  bool has_fraction = false;
  std::string_view fractional_run;
  if (offset < spelling.size() && spelling[offset] == '.') {
    has_fraction = true;
    ++offset;
    const std::size_t begin = offset;
    while (offset < spelling.size() &&
           (is_decimal_digit(spelling[offset]) || spelling[offset] == '_')) {
      ++offset;
    }
    fractional_run = spelling.substr(begin, offset - begin);
    if (fractional_run.empty()) {
      result.error = NumericLiteralSpellingError::kInvalidCore;
      result.core_is_floating = true;
      result.is_floating = true;
      return result;
    }
    invalid_separator =
        invalid_separator || !is_valid_separator_run(fractional_run);
  }

  bool has_exponent = false;
  bool exponent_negative = false;
  std::string_view exponent_run;
  if (offset < spelling.size() &&
      (spelling[offset] == 'e' || spelling[offset] == 'E')) {
    has_exponent = true;
    ++offset;
    if (offset < spelling.size() &&
        (spelling[offset] == '+' || spelling[offset] == '-')) {
      exponent_negative = spelling[offset] == '-';
      ++offset;
    }
    const std::size_t begin = offset;
    while (offset < spelling.size() &&
           (is_decimal_digit(spelling[offset]) || spelling[offset] == '_')) {
      ++offset;
    }
    exponent_run = spelling.substr(begin, offset - begin);
    if (exponent_run.empty() ||
        std::ranges::none_of(exponent_run, is_decimal_digit)) {
      result.error = NumericLiteralSpellingError::kMissingExponentDigits;
      result.core_is_floating = true;
      result.is_floating = true;
      return result;
    }
    invalid_separator =
        invalid_separator || !is_valid_separator_run(exponent_run);
  }

  result.core_is_floating = has_fraction || has_exponent;
  result.suffix = spelling.substr(offset);
  result.suffix_kind = parse_suffix(result.suffix);
  if (invalid_separator) {
    result.error = NumericLiteralSpellingError::kInvalidSeparator;
    result.is_floating = result.core_is_floating;
    return result;
  }
  if (!result.suffix.empty() &&
      result.suffix_kind == NumericLiteralSuffix::kNone) {
    result.error = NumericLiteralSpellingError::kInvalidSuffix;
    result.is_floating = result.core_is_floating;
    return result;
  }

  const bool floating_suffix = is_floating_suffix(result.suffix_kind);
  const bool integer_suffix =
      result.suffix_kind != NumericLiteralSuffix::kNone && !floating_suffix;
  result.is_floating = result.core_is_floating || floating_suffix;
  if (result.core_is_floating && integer_suffix) {
    result.error = NumericLiteralSpellingError::kIntegerSuffixOnFloatingCore;
    return result;
  }

  std::string integer_digits = digits_without_separators(integer_run);
  if (!result.is_floating) {
    result.core = canonical_decimal_integer(std::move(integer_digits));
    return result;
  }

  std::int64_t exponent = 0;
  if (has_exponent) {
    exponent = parse_exponent(digits_without_separators(exponent_run),
                              exponent_negative);
  }
  result.core = canonical_decimal_float(
      std::move(integer_digits), digits_without_separators(fractional_run),
      exponent);
  return result;
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
