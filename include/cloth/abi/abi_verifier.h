#ifndef CLOTH_ABI_ABI_VERIFIER_H_
#define CLOTH_ABI_ABI_VERIFIER_H_

#include "cloth/abi/abi.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

namespace cloth {

[[nodiscard]] bool verify_abi(const AbiModule& abi, const MirModule& mir,
                              const SemanticModel& semantics,
                              DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_ABI_ABI_VERIFIER_H_
