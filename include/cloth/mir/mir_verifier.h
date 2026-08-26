#ifndef CLOTH_MIR_MIR_VERIFIER_H_
#define CLOTH_MIR_MIR_VERIFIER_H_

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

namespace cloth {

[[nodiscard]] bool verify_mir(const MirModule& mir,
                              const SemanticModel& semantics,
                              DiagnosticEngine& diagnostics);

}  // namespace cloth

#endif  // CLOTH_MIR_MIR_VERIFIER_H_
