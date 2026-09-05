// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

[[nodiscard]] constexpr bool is_numeric_type_token(TokenKind kind) noexcept {
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
    case TokenKind::kKwByte:
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
         kind == TokenKind::kKwInterface || kind == TokenKind::kKwEnum ||
         kind == TokenKind::kKwError;
}

}  // namespace cloth

#endif  // CLOTH_PARSER_SYNTAX_FACTS_H_
