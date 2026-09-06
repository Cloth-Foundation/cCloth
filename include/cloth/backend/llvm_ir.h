// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_BACKEND_LLVM_IR_H_
#define CLOTH_BACKEND_LLVM_IR_H_

#include "cloth/abi/abi.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

#include <optional>
#include <string>
#include <string_view>

namespace cloth {

struct LlvmIrModule {
  std::string text;
  bool uses_wide_native_arguments{false};
};

struct LlvmIrOptions {
  bool emit_native_entry_point{false};
  std::optional<std::string> entry_file{};
  // Restricts definitions to one package; dependencies are ABI declarations.
  // Package modules never contain a native entry wrapper.
  std::optional<PackageIdentity> package{};
  bool use_wide_native_arguments{false};
};

// Selects and validates the executable entry independently of IR emission.
// The returned callable borrows abi; failures are reported to diagnostics.
[[nodiscard]] const AbiCallable* find_native_entry_point(
    const AbiModule& abi, const SemanticModel& semantics,
    DiagnosticEngine& diagnostics,
    std::optional<std::string_view> entry_file = std::nullopt);

// Emits opaque-pointer LLVM IR from verified MIR and ABI. The emitter has no
// link-time dependency on LLVM; LLVM's opt tool verifies the result in tests.
[[nodiscard]] std::optional<LlvmIrModule> emit_llvm_ir(
    const MirModule& mir, const AbiModule& abi, const SemanticModel& semantics,
    DiagnosticEngine& diagnostics, LlvmIrOptions options = {});

}  // namespace cloth

#endif  // CLOTH_BACKEND_LLVM_IR_H_
