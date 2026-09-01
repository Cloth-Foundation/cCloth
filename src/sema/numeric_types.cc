// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/numeric_types.h"

#include "cloth/sema/semantic_model.h"

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

}  // namespace cloth
