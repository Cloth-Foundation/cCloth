// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SOURCE_SOURCE_LOCATION_H_
#define CLOTH_SOURCE_SOURCE_LOCATION_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cloth {

// Lines and columns are 1-based. byte_offset is 0-based and counts source
// bytes.
struct SourceLocation {
  std::string_view file;
  std::size_t byte_offset{0};
  std::uint32_t line{1};
  std::uint32_t column{1};

  friend bool operator==(const SourceLocation&,
                         const SourceLocation&) = default;
};

}  // namespace cloth

#endif  // CLOTH_SOURCE_SOURCE_LOCATION_H_
