#ifndef CLOTH_BACKEND_LLVM_IR_H_
#define CLOTH_BACKEND_LLVM_IR_H_

#include "cloth/abi/abi.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

#include <optional>
#include <string>

namespace cloth {

struct LlvmIrModule {
  std::string text;
};

struct LlvmIrOptions {
  bool emit_native_entry_point{false};
};

// Emits opaque-pointer LLVM IR from verified MIR and ABI. The emitter has no
// link-time dependency on LLVM; LLVM's opt tool verifies the result in tests.
[[nodiscard]] std::optional<LlvmIrModule> emit_llvm_ir(
    const MirModule& mir, const AbiModule& abi, const SemanticModel& semantics,
    DiagnosticEngine& diagnostics, LlvmIrOptions options = {});

}  // namespace cloth

#endif  // CLOTH_BACKEND_LLVM_IR_H_
