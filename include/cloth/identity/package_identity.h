// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_IDENTITY_PACKAGE_IDENTITY_H_
#define CLOTH_IDENTITY_PACKAGE_IDENTITY_H_

#include <string_view>

namespace cloth {

inline constexpr std::string_view kStandardLibraryPackageName = "cloth";
inline constexpr std::string_view kStandardLibraryPackageVersion = "0.3.0";

[[nodiscard]] bool is_valid_package_name(std::string_view value);
[[nodiscard]] bool is_valid_package_version(std::string_view value);
[[nodiscard]] bool has_reserved_standard_library_root(std::string_view value);
[[nodiscard]] bool has_standard_library_dependency_conflict(
    std::string_view alias, std::string_view package);

}  // namespace cloth

#endif  // CLOTH_IDENTITY_PACKAGE_IDENTITY_H_
