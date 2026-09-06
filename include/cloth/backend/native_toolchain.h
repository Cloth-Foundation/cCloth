// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_BACKEND_NATIVE_TOOLCHAIN_H_
#define CLOTH_BACKEND_NATIVE_TOOLCHAIN_H_

#include "cloth/backend/llvm_ir.h"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace cloth {

enum class NativeLinkerFlavor {
  kGnu,
  kMsvc,
};

struct NativeToolchain {
  std::filesystem::path llc;
  std::filesystem::path linker;
  std::filesystem::path runtime_library;
  std::string target_triple;
  std::string cpu{"x86-64"};
  std::vector<std::string> features{"+sse2"};
  std::string relocation_model{"pic"};
  std::string code_model{"small"};
  NativeLinkerFlavor linker_flavor{NativeLinkerFlavor::kGnu};
  bool link_static_runtime{false};
};

struct NativeBuildError {
  std::string message;
};

// Emits one relocatable object without linking it. The caller owns atomic
// publication and cleanup of output_path.
[[nodiscard]] std::expected<void, NativeBuildError> build_native_object(
    const LlvmIrModule& module, const std::filesystem::path& output_path,
    const NativeToolchain& toolchain);

// Links already verified object files with the configured runtime exactly once.
// The caller supplies objects in deterministic order and owns output staging.
[[nodiscard]] std::expected<void, NativeBuildError> link_native_objects(
    std::span<const std::filesystem::path> object_paths,
    const std::filesystem::path& output_path, const NativeToolchain& toolchain,
    bool uses_wide_native_arguments = false);

// Uses LLVM's llc and the configured host linker driver. Temporary IR and
// object files are removed before this function returns.
[[nodiscard]] std::expected<void, NativeBuildError> build_native_executable(
    const LlvmIrModule& module, const std::filesystem::path& output_path,
    const NativeToolchain& toolchain);

}  // namespace cloth

#endif  // CLOTH_BACKEND_NATIVE_TOOLCHAIN_H_
