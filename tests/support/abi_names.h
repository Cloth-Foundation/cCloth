// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_TESTS_SUPPORT_ABI_NAMES_H_
#define CLOTH_TESTS_SUPPORT_ABI_NAMES_H_

#include "cloth/identity/canonical_identity.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace cloth::test {

// Backend expectations name the intended source declaration independently of
// the emitted ABI tables. Byte-level encoding is tested by identity_tests.cc.
inline NominalIdentity standalone_identity(std::string_view name) {
  const auto separator = name.rfind('.');
  return NominalIdentity{
      {},
      separator == std::string_view::npos
          ? std::string{}
          : std::string{name.substr(0, separator)},
      std::string{name.substr(
          separator == std::string_view::npos ? 0 : separator + 1)}};
}

inline std::string abi_type(std::string_view name) {
  if (name.ends_with('?')) {
    return abi_type(name.substr(0, name.size() - 1));
  }
  if (name.ends_with("[]")) {
    return canonical_array_identity(abi_type(name.substr(0, name.size() - 2)));
  }
  constexpr std::array<std::string_view, 16> kPrimitives{
      "void",    "bool",    "char",   "byte",   "int8",   "int16",
      "int32",   "int64",   "uint8",  "uint16", "uint32", "uint64",
      "float32", "float64", "string", "object"};
  if (std::ranges::find(kPrimitives, name) != kPrimitives.end()) {
    return canonical_primitive_identity(name);
  }
  return canonical_nominal_identity(standalone_identity(name));
}

inline std::string member_name(
    CanonicalMemberKind kind, std::string_view owner, std::string_view name,
    std::initializer_list<std::string_view> parameters = {}) {
  std::vector<std::string> types;
  for (const auto parameter : parameters) {
    types.push_back(abi_type(parameter));
  }
  return mangle_canonical_identity(
      canonical_member_identity(standalone_identity(owner), kind, name, types));
}

inline std::string function_name(
    std::string_view owner, std::string_view name,
    std::initializer_list<std::string_view> parameters = {}) {
  return member_name(CanonicalMemberKind::kFunction, owner, name, parameters);
}

inline std::string constructor_name(
    std::string_view owner, std::string_view name,
    std::initializer_list<std::string_view> parameters = {}) {
  return member_name(CanonicalMemberKind::kConstructor, owner, name,
                     parameters);
}

inline std::string initializer_name(
    std::string_view owner, std::string_view name,
    std::initializer_list<std::string_view> parameters = {}) {
  return member_name(CanonicalMemberKind::kConstructorInitializer, owner, name,
                     parameters);
}

inline std::string static_field_name(std::string_view owner,
                                     std::string_view name) {
  return member_name(CanonicalMemberKind::kStaticField, owner, name);
}

inline std::string descriptor_name(std::string_view owner) {
  return member_name(CanonicalMemberKind::kDescriptor, owner, "");
}

}  // namespace cloth::test

#endif  // CLOTH_TESTS_SUPPORT_ABI_NAMES_H_
