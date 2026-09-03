// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SEMA_NUMERIC_TYPES_H_
#define CLOTH_SEMA_NUMERIC_TYPES_H_

#include "cloth/sema/semantic_model.h"

#include <cstdint>
#include <optional>

namespace cloth {

enum class NumericCategory {
  kSignedInteger,
  kUnsignedInteger,
  kFloatingPoint,
};

struct NumericTypeProperties {
  NumericCategory category;
  std::uint32_t bit_width;
};

[[nodiscard]] std::optional<NumericTypeProperties> numeric_type_properties(
    TypeKind kind) noexcept;

[[nodiscard]] bool is_integer_type(TypeKind kind) noexcept;

[[nodiscard]] bool is_numeric_type(TypeKind kind) noexcept;

// Returns true only when every value of source has an exact representation in
// target. Equal types are deliberately excluded because no conversion is
// required.
[[nodiscard]] bool can_widen_numeric(TypeKind source, TypeKind target) noexcept;

// Parse a decimal magnitude with a separately supplied sign, checking the
// declared width. The result is the canonical, zero-extended bit pattern.
[[nodiscard]] std::optional<std::uint64_t> integer_constant_bits(
    std::string_view magnitude, bool negative, TypeKind type) noexcept;
[[nodiscard]] bool is_valid_integer_bits(std::uint64_t bits,
                                         TypeKind type) noexcept;
[[nodiscard]] std::optional<std::uint64_t> widen_integer_constant(
    std::uint64_t bits, TypeKind source, TypeKind target) noexcept;

}  // namespace cloth

#endif  // CLOTH_SEMA_NUMERIC_TYPES_H_
