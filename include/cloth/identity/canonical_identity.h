// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_IDENTITY_CANONICAL_IDENTITY_H_
#define CLOTH_IDENTITY_CANONICAL_IDENTITY_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cloth {

inline constexpr std::uint32_t kCompilerAbiVersion = 3;

struct PackageIdentity {
  // Both fields are empty only for the distinct standalone compilation domain.
  std::string name;
  std::string version;

  friend bool operator==(const PackageIdentity&,
                         const PackageIdentity&) = default;
};

enum class NominalKind { kClass, kInterface, kEnum };

struct NominalIdentity {
  PackageIdentity package;
  std::string source_package;
  std::string name;
  NominalKind kind{NominalKind::kClass};

  friend bool operator==(const NominalIdentity&,
                         const NominalIdentity&) = default;
};

enum class CanonicalMemberKind {
  kFunction,
  kConstructor,
  kConstructorInitializer,
  kStaticField,
  kInstanceField,
  kDescriptor,
  kEnumCase,
};

// These functions encode trusted identities, not filesystem paths or display
// names. Returned strings contain binary bytes, including NUL. Wire readers
// must validate records before constructing trusted identities.
[[nodiscard]] std::string canonical_nominal_identity(
    const NominalIdentity& identity);
[[nodiscard]] std::string canonical_primitive_identity(std::string_view name);
[[nodiscard]] std::string canonical_array_identity(std::string_view element);
[[nodiscard]] std::string canonical_nullable_identity(std::string_view element);
[[nodiscard]] std::string canonical_member_identity(
    const NominalIdentity& owner, CanonicalMemberKind kind,
    std::string_view name, std::span<const std::string> parameter_types = {});
[[nodiscard]] std::string mangle_canonical_identity(std::string_view identity);
[[nodiscard]] std::uint64_t canonical_interface_id(
    const NominalIdentity& identity);

}  // namespace cloth

#endif  // CLOTH_IDENTITY_CANONICAL_IDENTITY_H_
