// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SOURCE_SOURCE_RANGE_H_
#define CLOTH_SOURCE_SOURCE_RANGE_H_

#include "cloth/source/source_location.h"

namespace cloth {

// A half-open source span: [begin, end).
struct SourceRange {
  SourceLocation begin;
  SourceLocation end;

  friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

[[nodiscard]] constexpr SourceRange point_range(
    SourceLocation location) noexcept {
  return SourceRange{location, location};
}

}  // namespace cloth

#endif  // CLOTH_SOURCE_SOURCE_RANGE_H_
