#ifndef CLOTH_LEXER_TOKEN_H_
#define CLOTH_LEXER_TOKEN_H_

#include "cloth/source/source_range.h"

#include <string_view>

namespace cloth {

enum class TokenKind {
  kEof,

  kIdentifier,
  kIntegerLiteral,
  kFloatLiteral,
  kStringLiteral,
  kCharacterLiteral,

  kLeftParen,
  kRightParen,
  kLeftBrace,
  kRightBrace,
  kLeftBracket,
  kRightBracket,
  kComma,
  kSemicolon,
  kColon,
  kColonColon,
  kDot,
  kQuestion,

  kPlus,
  kMinus,
  kStar,
  kSlash,
  kPercent,
  kEqual,
  kEqualEqual,
  kBangEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kAmpersandAmpersand,
  kPipePipe,
  kBang,
  kAmpersand,
  kPipe,
  kCaret,
  kTilde,
  kPlusPlus,
  kMinusMinus,
  kPlusEqual,
  kMinusEqual,
  kStarEqual,
  kSlashEqual,
  kPercentEqual,
  kShiftLeft,
  kShiftRight,
  kShiftLeftEqual,
  kShiftRightEqual,
  kAmpersandEqual,
  kPipeEqual,
  kCaretEqual,

  kKwFunc,
  kKwReturn,
  kKwIf,
  kKwElse,
  kKwWhile,
  kKwFor,
  kKwBreak,
  kKwContinue,
  kKwStruct,
  kKwClass,
  kKwEnum,
  kKwTrait,
  kKwImplements,
  kKwLet,
  kKwVar,
  kKwConst,
  kKwTrue,
  kKwFalse,
  kKwNull,
  kKwExtern,
  kKwUnsafe,
  kKwImport,
  kKwAs,
  kKwModule,
  kKwMatch,
  kKwInt,
  kKwInt8,
  kKwInt16,
  kKwInt32,
  kKwInt64,
  kKwUint,
  kKwUint8,
  kKwUint16,
  kKwUint32,
  kKwUint64,
  kKwFloat,
  kKwFloat32,
  kKwFloat64,
  kKwBool,
  kKwChar,
  kKwByte,
};

struct Token {
  TokenKind kind;
  std::string_view lexeme;
  SourceRange range;
};

[[nodiscard]] std::string_view token_kind_name(TokenKind kind) noexcept;

}  // namespace cloth

#endif  // CLOTH_LEXER_TOKEN_H_
