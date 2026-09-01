// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_COMPILER_PACKAGE_PIPELINE_H_
#define CLOTH_COMPILER_PACKAGE_PIPELINE_H_

#include "cloth/backend/native_toolchain.h"
#include "cloth/compiler/shuttle_protocol_v2.h"

#include <filesystem>
#include <string>

namespace cloth {

struct ShuttleV2ExecutionResult {
  int exit_code;
  std::string standard_output;
  std::string standard_error;

  [[nodiscard]] bool succeeded() const noexcept { return exit_code == 0; }
};

[[nodiscard]] ShuttleV2ExecutionResult execute_shuttle_v2_request(
    const ShuttleV2Request& request,
    const std::filesystem::path& compiler_executable,
    const NativeToolchain& toolchain);

}  // namespace cloth

#endif  // CLOTH_COMPILER_PACKAGE_PIPELINE_H_
