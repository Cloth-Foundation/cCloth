// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/canonical_identity.h"

#include <string>
#include <vector>

namespace cloth {

std::string canonical_type_identity(TypeId type, const SemanticModel& semantics,
                                    TypeIdentityMode mode) {
  const SemanticType& value = semantics.type(type);
  if (value.kind == TypeKind::kFileClass ||
      value.kind == TypeKind::kInterface || value.kind == TypeKind::kEnum) {
    return canonical_nominal_identity(semantics.file(*value.file).identity);
  }
  if (value.kind == TypeKind::kArray || value.kind == TypeKind::kNullable) {
    const std::string element =
        canonical_type_identity(*value.element_type, semantics, mode);
    if (value.kind == TypeKind::kArray) {
      return canonical_array_identity(element);
    }
    return mode == TypeIdentityMode::kOverload
               ? element
               : canonical_nullable_identity(element);
  }
  return canonical_primitive_identity(type_kind_name(value.kind));
}

std::string canonical_symbol_identity(const SemanticSymbol& symbol,
                                      const SemanticModel& semantics,
                                      CanonicalMemberKind kind) {
  std::vector<std::string> parameters;
  parameters.reserve(symbol.parameter_types.size());
  for (const TypeId parameter : symbol.parameter_types) {
    parameters.push_back(canonical_type_identity(parameter, semantics,
                                                 TypeIdentityMode::kOverload));
  }
  return canonical_member_identity(semantics.file(*symbol.file).identity, kind,
                                   symbol.name, parameters);
}

}  // namespace cloth
