// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SEMA_VISIBILITY_H_
#define CLOTH_SEMA_VISIBILITY_H_

#include <string_view>

namespace cloth {

enum class Visibility {
  kPublic,
  kPrivate,
};

[[nodiscard]] bool is_valid_identifier(std::string_view name) noexcept;
[[nodiscard]] Visibility infer_visibility(std::string_view name) noexcept;
[[nodiscard]] std::string_view visibility_name(Visibility visibility) noexcept;

}  // namespace cloth

#endif  // CLOTH_SEMA_VISIBILITY_H_
