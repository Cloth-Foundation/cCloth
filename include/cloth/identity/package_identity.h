// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_IDENTITY_PACKAGE_IDENTITY_H_
#define CLOTH_IDENTITY_PACKAGE_IDENTITY_H_

#include <string_view>

namespace cloth {

[[nodiscard]] bool is_valid_package_name(std::string_view value);
[[nodiscard]] bool is_valid_package_version(std::string_view value);

}  // namespace cloth

#endif  // CLOTH_IDENTITY_PACKAGE_IDENTITY_H_
