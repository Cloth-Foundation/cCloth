#ifndef CLOTH_IDENTITY_PACKAGE_IDENTITY_H_
#define CLOTH_IDENTITY_PACKAGE_IDENTITY_H_

#include <string_view>

namespace cloth {

[[nodiscard]] bool is_valid_package_name(std::string_view value);
[[nodiscard]] bool is_valid_package_version(std::string_view value);

}  // namespace cloth

#endif  // CLOTH_IDENTITY_PACKAGE_IDENTITY_H_
