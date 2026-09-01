// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_COMPILER_SHUTTLE_PROTOCOL_H_
#define CLOTH_COMPILER_SHUTTLE_PROTOCOL_H_

#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cloth {

inline constexpr int kShuttleProtocolVersion = 1;

enum class ShuttleOutputKind {
  kCheck,
  kLlvmIr,
  kExecutable,
};

struct ShuttlePackageInput {
  std::string name;
  std::string version;
  std::filesystem::path source_root;
};

struct ShuttleDependencyInput {
  std::string owner;
  std::string alias;
  std::string target;
};

struct ShuttleBuildRequest {
  TargetDataLayout target;
  ShuttleOutputKind output_kind;
  std::optional<std::filesystem::path> output;
  std::string root_package;
  std::optional<std::filesystem::path> entry;
  std::vector<ShuttlePackageInput> packages;
  std::vector<ShuttleDependencyInput> dependencies;
};

struct ShuttleSourceInput {
  SourceFile source;
  std::string package;
  std::string source_package;
  std::string version;
};

struct ShuttleBuildPlan {
  ShuttleBuildRequest request;
  std::vector<ShuttleSourceInput> sources;
  std::optional<std::string> entry_file;
};

// Parses, validates, and resolves one protocol-mode compiler request. Arguments
// include the leading --shuttle-protocol option but not the executable name.
[[nodiscard]] std::expected<ShuttleBuildPlan, std::string>
prepare_shuttle_build(std::span<const std::filesystem::path> arguments);

// Shared source-snapshot boundary used by protocol v1 whole-graph builds and
// protocol v2 single-package compilation.
[[nodiscard]] std::expected<std::vector<ShuttleSourceInput>, std::string>
load_shuttle_sources(const ShuttleBuildRequest& request);

[[nodiscard]] std::expected<std::optional<std::string>, std::string>
resolve_shuttle_entry(const ShuttleBuildRequest& request,
                      std::span<const ShuttleSourceInput> sources);

}  // namespace cloth

#endif  // CLOTH_COMPILER_SHUTTLE_PROTOCOL_H_
