#ifndef CLOTH_SEMA_CANONICAL_IDENTITY_H_
#define CLOTH_SEMA_CANONICAL_IDENTITY_H_

#include "cloth/identity/canonical_identity.h"
#include "cloth/sema/semantic_model.h"

#include <string>

namespace cloth {

enum class TypeIdentityMode { kSemantic, kOverload };

// Requires a verified semantic model. Symbols must be file-owned declarations;
// intrinsic/local symbols do not have a persistent member identity. Untrusted
// artifact records must be verified before they enter the semantic model.
[[nodiscard]] std::string canonical_type_identity(
    TypeId type, const SemanticModel& semantics,
    TypeIdentityMode mode = TypeIdentityMode::kSemantic);
[[nodiscard]] std::string canonical_symbol_identity(
    const SemanticSymbol& symbol, const SemanticModel& semantics,
    CanonicalMemberKind kind);

}  // namespace cloth

#endif  // CLOTH_SEMA_CANONICAL_IDENTITY_H_
