// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/identity/canonical_identity.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace cloth {
namespace {

void append_count(std::string& output, std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

void append_component(std::string& output, std::string_view value) {
  append_count(output, value.size());
  output.append(value);
}

std::string_view member_tag(CanonicalMemberKind kind) {
  switch (kind) {
    case CanonicalMemberKind::kFunction:
      return "function";
    case CanonicalMemberKind::kConstructor:
      return "constructor";
    case CanonicalMemberKind::kConstructorInitializer:
      return "constructor-initializer";
    case CanonicalMemberKind::kStaticField:
      return "static-field";
    case CanonicalMemberKind::kInstanceField:
      return "instance-field";
    case CanonicalMemberKind::kDescriptor:
      return "descriptor";
  }
  return "invalid";
}

std::string wrapped_identity(std::string_view tag, std::string_view element) {
  std::string result;
  append_component(result, tag);
  append_component(result, element);
  return result;
}

}  // namespace

std::string canonical_nominal_identity(const NominalIdentity& identity) {
  std::string result;
  append_component(result, "nominal");
  append_component(result,
                   identity.package.name.empty() ? "standalone" : "package");
  append_component(result, identity.package.name);
  append_component(result, identity.package.version);
  std::uint64_t component_count = identity.source_package.empty() ? 0 : 1;
  for (const char character : identity.source_package) {
    if (character == '.') {
      ++component_count;
    }
  }
  append_count(result, component_count);
  std::size_t begin = 0;
  for (std::uint64_t index = 0; index < component_count; ++index) {
    const std::size_t end = identity.source_package.find('.', begin);
    append_component(result,
                     std::string_view{identity.source_package}.substr(
                         begin, end == std::string::npos
                                    ? identity.source_package.size() - begin
                                    : end - begin));
    if (end != std::string::npos) {
      begin = end + 1;
    }
  }
  append_component(result, identity.name);
  append_component(
      result, identity.kind == NominalKind::kClass ? "class" : "interface");
  return result;
}

std::string canonical_primitive_identity(std::string_view name) {
  return wrapped_identity("primitive", name);
}

std::string canonical_array_identity(std::string_view element) {
  return wrapped_identity("array", element);
}

std::string canonical_nullable_identity(std::string_view element) {
  return wrapped_identity("nullable", element);
}

std::string canonical_member_identity(
    const NominalIdentity& owner, CanonicalMemberKind kind,
    std::string_view name, std::span<const std::string> parameter_types) {
  std::string result;
  append_component(result, member_tag(kind));
  append_component(result, canonical_nominal_identity(owner));
  append_component(result, name);
  append_count(result, parameter_types.size());
  for (const std::string& parameter : parameter_types) {
    append_component(result, parameter);
  }
  return result;
}

std::string mangle_canonical_identity(std::string_view identity) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string result = "_C2";
  for (const char character : identity) {
    const auto byte = static_cast<unsigned char>(character);
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0xfU]);
  }
  return result;
}

std::uint64_t canonical_interface_id(const NominalIdentity& identity) {
  std::uint64_t result = 14695981039346656037ULL;
  for (const char character : canonical_nominal_identity(identity)) {
    result ^= static_cast<unsigned char>(character);
    result *= 1099511628211ULL;
  }
  return result;
}

}  // namespace cloth
