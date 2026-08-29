#ifndef CLOTH_PARSER_SYNTAX_FACTS_H_
#define CLOTH_PARSER_SYNTAX_FACTS_H_

#include "cloth/lexer/token.h"

namespace cloth {

[[nodiscard]] constexpr bool is_primitive_type(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::kKwInt:
    case TokenKind::kKwInt8:
    case TokenKind::kKwInt16:
    case TokenKind::kKwInt32:
    case TokenKind::kKwInt64:
    case TokenKind::kKwUint:
    case TokenKind::kKwUint8:
    case TokenKind::kKwUint16:
    case TokenKind::kKwUint32:
    case TokenKind::kKwUint64:
    case TokenKind::kKwFloat:
    case TokenKind::kKwFloat32:
    case TokenKind::kKwFloat64:
    case TokenKind::kKwBool:
    case TokenKind::kKwChar:
    case TokenKind::kKwByte:
    case TokenKind::kKwVoid:
    case TokenKind::kKwObject:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] constexpr bool can_start_type(TokenKind kind) noexcept {
  return kind == TokenKind::kIdentifier || is_primitive_type(kind);
}

[[nodiscard]] constexpr bool is_nested_type_keyword(TokenKind kind) noexcept {
  return kind == TokenKind::kKwStruct || kind == TokenKind::kKwClass ||
         kind == TokenKind::kKwInterface || kind == TokenKind::kKwEnum;
}

}  // namespace cloth

#endif  // CLOTH_PARSER_SYNTAX_FACTS_H_
