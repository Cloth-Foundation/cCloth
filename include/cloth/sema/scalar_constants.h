// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SEMA_SCALAR_CONSTANTS_H_
#define CLOTH_SEMA_SCALAR_CONSTANTS_H_

#include "cloth/lexer/token.h"
#include "cloth/sema/semantic_model.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace cloth {

inline constexpr std::size_t kMaxStaticConstants = 65536;
inline constexpr std::size_t kMaxConstantNodes = 65536;
inline constexpr std::size_t kMaxPackageConstantNodes = 1048576;
inline constexpr std::size_t kMaxConstantDepth = 256;
inline constexpr std::size_t kMaxConstantLiteralBytes = 4096;

// Checks declared type, canonical bits, and nominal enum ownership.
[[nodiscard]] bool is_valid_scalar_constant(ScalarConstant value,
                                            TypeId expected,
                                            const SemanticModel& semantics);
// Only for verified integer values; used by canonical artifact serialization.
[[nodiscard]] std::string scalar_integer_decimal(TypeKind type,
                                                 std::uint64_t bits);

enum class ConstantError {
  kInvalidLiteral,
  kOutOfRange,
  kOverflow,
  kZeroDivisor,
  kInvalidShift,
  kNonFinite,
  kInvalidOperation,
};

using ConstantBits = std::expected<std::uint64_t, ConstantError>;

[[nodiscard]] std::string_view constant_error_message(ConstantError error);
[[nodiscard]] bool is_scalar_constant_type(TypeKind type);
[[nodiscard]] bool is_valid_scalar_bits(TypeKind type, std::uint64_t bits);
// Literal conversions are destination-directed. A floating literal converted
// to an integer is first rounded to binary64, as in ordinary literal checking.
[[nodiscard]] ConstantBits scalar_literal(LiteralKind literal,
                                          std::string_view text,
                                          TypeKind target,
                                          bool negative = false);
[[nodiscard]] ConstantBits convert_scalar(std::uint64_t bits, TypeKind source,
                                          TypeKind target);
// Converts an integer by the explicit Stage 30 mode. The result is always the
// canonical target-width bit pattern.
[[nodiscard]] ConstantBits convert_integer_mode(std::uint64_t bits,
                                                TypeKind source,
                                                TypeKind target,
                                                IntegerConversionMode mode);
// Signs are in outermost-first source order. Only the innermost sign is part
// of literal formation (allowing signed minima); every outer sign is checked.
[[nodiscard]] ConstantBits scalar_signed_literal(
    LiteralKind literal, std::string_view text, TypeKind target,
    std::span<const TokenKind> signs);
[[nodiscard]] ConstantBits unary_scalar(TokenKind operation, TypeKind type,
                                        std::uint64_t bits);
// Non-shift operands already have a common type. Shift counts retain theirs.
// Comparison/logical operations return canonical boolean bits.
[[nodiscard]] ConstantBits binary_scalar(TokenKind operation, TypeKind type,
                                         std::uint64_t left,
                                         std::uint64_t right,
                                         TypeKind right_type);

}  // namespace cloth

#endif  // CLOTH_SEMA_SCALAR_CONSTANTS_H_
