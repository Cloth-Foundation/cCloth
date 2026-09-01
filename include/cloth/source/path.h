// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SOURCE_PATH_H_
#define CLOTH_SOURCE_PATH_H_

#include <filesystem>
#include <string>

namespace cloth {

// Convert native paths without the Windows active code page. Use this for
// logical keys and UTF-8 diagnostics, not to round-trip process arguments.
inline std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string encoded = path.generic_u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

}  // namespace cloth

#endif  // CLOTH_SOURCE_PATH_H_
