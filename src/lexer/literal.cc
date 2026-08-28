#include "cloth/lexer/literal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cloth {

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
