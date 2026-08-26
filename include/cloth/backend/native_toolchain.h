#ifndef CLOTH_BACKEND_NATIVE_TOOLCHAIN_H_
#define CLOTH_BACKEND_NATIVE_TOOLCHAIN_H_

#include "cloth/backend/llvm_ir.h"

#include <expected>
#include <filesystem>
#include <string>

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
  NativeLinkerFlavor linker_flavor{NativeLinkerFlavor::kGnu};
  bool link_static_runtime{false};
};

struct NativeBuildError {
  std::string message;
};

// Uses LLVM's llc and the configured host linker driver. Temporary IR and
// object files are removed before this function returns.
[[nodiscard]] std::expected<void, NativeBuildError> build_native_executable(
    const LlvmIrModule& module, const std::filesystem::path& output_path,
    const NativeToolchain& toolchain);

}  // namespace cloth

#endif  // CLOTH_BACKEND_NATIVE_TOOLCHAIN_H_
