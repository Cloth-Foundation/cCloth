#ifndef CLOTH_LEXER_LITERAL_H_
#define CLOTH_LEXER_LITERAL_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cloth {

[[nodiscard]] char decode_escape_character(char character) noexcept;

[[nodiscard]] std::string decode_string_literal(std::string_view lexeme);

[[nodiscard]] std::optional<std::size_t> utf8_scalar_count(
    std::string_view text) noexcept;

}  // namespace cloth

#endif  // CLOTH_LEXER_LITERAL_H_
