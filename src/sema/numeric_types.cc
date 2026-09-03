// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/numeric_types.h"

#include "cloth/sema/semantic_model.h"

#include <charconv>
#include <limits>
#include <optional>

namespace cloth {

std::optional<NumericTypeProperties> numeric_type_properties(
    TypeKind kind) noexcept {
  switch (kind) {
    case TypeKind::kByte:
    case TypeKind::kUint8:
      return NumericTypeProperties{NumericCategory::kUnsignedInteger, 8};
    case TypeKind::kInt8:
      return NumericTypeProperties{NumericCategory::kSignedInteger, 8};
    case TypeKind::kUint16:
      return NumericTypeProperties{NumericCategory::kUnsignedInteger, 16};
    case TypeKind::kInt16:
      return NumericTypeProperties{NumericCategory::kSignedInteger, 16};
    case TypeKind::kUint32:
      return NumericTypeProperties{NumericCategory::kUnsignedInteger, 32};
    case TypeKind::kInt32:
      return NumericTypeProperties{NumericCategory::kSignedInteger, 32};
    case TypeKind::kUint64:
      return NumericTypeProperties{NumericCategory::kUnsignedInteger, 64};
    case TypeKind::kInt64:
      return NumericTypeProperties{NumericCategory::kSignedInteger, 64};
    case TypeKind::kFloat32:
      return NumericTypeProperties{NumericCategory::kFloatingPoint, 32};
    case TypeKind::kFloat64:
      return NumericTypeProperties{NumericCategory::kFloatingPoint, 64};
    default:
      return std::nullopt;
  }
}

bool is_integer_type(TypeKind kind) noexcept {
  const std::optional<NumericTypeProperties> properties =
      numeric_type_properties(kind);
  return properties && properties->category != NumericCategory::kFloatingPoint;
}

bool is_numeric_type(TypeKind kind) noexcept {
  return numeric_type_properties(kind).has_value();
}

bool can_widen_numeric(TypeKind source, TypeKind target) noexcept {
  if (source == target) {
    return false;
  }
  const std::optional<NumericTypeProperties> source_properties =
      numeric_type_properties(source);
  const std::optional<NumericTypeProperties> target_properties =
      numeric_type_properties(target);
  if (!source_properties || !target_properties) {
    return false;
  }

  if (source_properties->category == NumericCategory::kFloatingPoint ||
      target_properties->category == NumericCategory::kFloatingPoint) {
    return source_properties->category == NumericCategory::kFloatingPoint &&
           target_properties->category == NumericCategory::kFloatingPoint &&
           source_properties->bit_width < target_properties->bit_width;
  }
  if (source_properties->category == NumericCategory::kSignedInteger &&
      target_properties->category == NumericCategory::kUnsignedInteger) {
    return false;
  }
  return source_properties->bit_width < target_properties->bit_width;
}

namespace {

std::uint64_t integer_mask(std::uint32_t width) noexcept {
  return width == 64 ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << width) - 1;
}

}  // namespace

std::optional<std::uint64_t> integer_constant_bits(std::string_view magnitude,
                                                   bool negative,
                                                   TypeKind type) noexcept {
  const auto properties = numeric_type_properties(type);
  if (!properties || !is_integer_type(type) || magnitude.empty() ||
      magnitude.front() < '0' || magnitude.front() > '9' ||
      (negative && properties->category == NumericCategory::kUnsignedInteger)) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(
      magnitude.data(), magnitude.data() + magnitude.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != magnitude.data() + magnitude.size())
    return std::nullopt;
  std::uint64_t maximum = integer_mask(properties->bit_width);
  if (properties->category == NumericCategory::kSignedInteger) {
    maximum =
        (std::uint64_t{1} << (properties->bit_width - 1)) - (negative ? 0 : 1);
  }
  if (value > maximum) return std::nullopt;
  return (negative ? std::uint64_t{0} - value : value) &
         integer_mask(properties->bit_width);
}

bool is_valid_integer_bits(std::uint64_t bits, TypeKind type) noexcept {
  const auto properties = numeric_type_properties(type);
  return properties && is_integer_type(type) &&
         bits <= integer_mask(properties->bit_width);
}

std::optional<std::uint64_t> widen_integer_constant(std::uint64_t bits,
                                                    TypeKind source,
                                                    TypeKind target) noexcept {
  if (!is_valid_integer_bits(bits, source) || !is_integer_type(target) ||
      (source != target && !can_widen_numeric(source, target)))
    return std::nullopt;
  const auto properties = *numeric_type_properties(source);
  if (properties.category == NumericCategory::kSignedInteger &&
      (bits & (std::uint64_t{1} << (properties.bit_width - 1))) != 0) {
    bits |= ~integer_mask(properties.bit_width);
  }
  return bits & integer_mask(numeric_type_properties(target)->bit_width);
}

}  // namespace cloth
