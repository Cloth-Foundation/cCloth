// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_ABI_AGGREGATE_LIMITS_H_
#define CLOTH_ABI_AGGREGATE_LIMITS_H_

#include <cstddef>
#include <cstdint>

namespace cloth {

inline constexpr std::size_t kMaxStructFields = 65'536;
inline constexpr std::size_t kMaxStructDepth = 128;
inline constexpr std::uint64_t kMaxStructSize = 1'048'576;
inline constexpr std::size_t kMaxLayoutReferences = 65'536;
inline constexpr std::size_t kMaxAggregateMapEntries = 1'048'576;
inline constexpr std::uint64_t kMaxAggregateFrameSize = 262'144;

}  // namespace cloth

#endif  // CLOTH_ABI_AGGREGATE_LIMITS_H_
