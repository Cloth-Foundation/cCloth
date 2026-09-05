// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/identity/package_identity.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace cloth {
namespace {

bool valid_semver_identifier(std::string_view value, bool reject_leading_zero) {
  if (value.empty()) {
    return false;
  }
  const bool numeric = std::ranges::all_of(value, [](char character) {
    return character >= '0' && character <= '9';
  });
  if (numeric && reject_leading_zero && value.size() > 1 &&
      value.front() == '0') {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '-';
  });
}

bool valid_semver_identifiers(std::string_view value,
                              bool reject_leading_zero) {
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t end = value.find('.', begin);
    const std::string_view part =
        value.substr(begin, end == std::string_view::npos ? value.size() - begin
                                                          : end - begin);
    if (!valid_semver_identifier(part, reject_leading_zero)) {
      return false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
    begin = end + 1;
  }
  return false;
}

}  // namespace

bool is_valid_package_name(std::string_view value) {
  const auto ascii_lower = [](unsigned char character) {
    return character >= 'a' && character <= 'z';
  };
  const auto ascii_digit = [](unsigned char character) {
    return character >= '0' && character <= '9';
  };
  if (value.empty() || value.size() > 64 ||
      !ascii_lower(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  bool after_separator = false;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character == '-') {
      if (after_separator) {
        return false;
      }
      after_separator = true;
    } else if (ascii_lower(character) || ascii_digit(character)) {
      after_separator = false;
    } else {
      return false;
    }
  }
  return !after_separator;
}

bool is_valid_package_version(std::string_view value) {
  const std::size_t build_separator = value.find('+');
  if (build_separator != std::string_view::npos) {
    if (value.find('+', build_separator + 1) != std::string_view::npos ||
        !valid_semver_identifiers(value.substr(build_separator + 1), false)) {
      return false;
    }
    value = value.substr(0, build_separator);
  }
  const std::size_t prerelease_separator = value.find('-');
  if (prerelease_separator != std::string_view::npos) {
    if (!valid_semver_identifiers(value.substr(prerelease_separator + 1),
                                  true)) {
      return false;
    }
    value = value.substr(0, prerelease_separator);
  }

  std::array<std::string_view, 3> core;
  std::size_t begin = 0;
  for (std::string_view& part : core) {
    if (begin > value.size()) {
      return false;
    }
    const std::size_t end = value.find('.', begin);
    part =
        value.substr(begin, end == std::string_view::npos ? value.size() - begin
                                                          : end - begin);
    begin = end == std::string_view::npos ? value.size() + 1 : end + 1;
  }
  if (begin <= value.size()) {
    return false;
  }
  return std::ranges::all_of(core, [](std::string_view part) {
    return !part.empty() && (part.size() == 1 || part.front() != '0') &&
           std::ranges::all_of(part, [](char character) {
             return character >= '0' && character <= '9';
           });
  });
}

bool has_reserved_standard_library_root(std::string_view value) {
  const std::size_t separator = value.find('.');
  value = value.substr(0, separator);
  if (value.size() != kStandardLibraryPackageName.size()) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    char character = value[index];
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
    if (character != kStandardLibraryPackageName[index]) {
      return false;
    }
  }
  return true;
}

bool has_standard_library_dependency_conflict(std::string_view alias,
                                              std::string_view package) {
  const bool claims_standard_library =
      has_reserved_standard_library_root(alias) ||
      has_reserved_standard_library_root(package);
  return claims_standard_library && (alias != kStandardLibraryPackageName ||
                                     package != kStandardLibraryPackageName);
}

}  // namespace cloth
